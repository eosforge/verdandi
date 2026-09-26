"""探针离线解析负例及可选 C++ 开关集成; 测试必须另行授权后运行."""

import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("astra_profile", Path(__file__).with_name("profile.py"))
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class Format(unittest.TestCase):
    """不依赖服务和工具链的格式/完整性边界."""

    def setUp(self):
        # 临时文件限定在项目 build 下, 不污染系统工具和运行目录.
        root = profile.ROOT / "build" / "profile-tests"
        root.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "1.profile"
        self.site = profile.identify("example")
        self.sites = {self.site: {"name": "example"}}

    def write(self, records, *, missed=0, errors=0, cpu=0):
        """固定一块, 尾部零槽模拟各线程尚未用完的预留区域."""
        header = profile.HEADER.pack(profile.MAGIC, 1, 80, 64, 64, missed, errors, 1, cpu, *([0] * 7))
        entries = b"".join(profile.ENTRY.pack(*record) for record in records)
        self.path.write_bytes(header + entries + bytes((64 - len(records)) * 80))

    def record(self, **changes):
        fields = dict(start=100, elapsed=100, own=70, cpu=0, exclusive=0, id=1, parent=0, site=self.site, value=0, thread=1, flags=0)
        fields.update(changes)
        return tuple(fields.values())

    def test_nested_and_holes(self):
        self.write([self.record(), self.record(start=110, elapsed=30, own=30, id=2, parent=1)])
        result = profile.analyze(self.path, self.sites)
        self.assertTrue(result["complete"])
        self.assertEqual(result["spans"], 2)
        self.assertEqual(result["unused_slots"], 62)
        self.assertEqual(result["groups"][0]["own_ns"]["sum"], 70)

    def test_counter_and_async(self):
        self.write(
            [
                self.record(),
                self.record(start=110, elapsed=0, own=0, id=0, parent=1, value=17, flags=2),
                self.record(start=300, elapsed=50, own=50, id=0, flags=4),
            ]
        )
        result = profile.analyze(self.path, self.sites)
        self.assertEqual((result["counters"], result["intervals"]), (1, 1))

    def test_cpu_mode(self):
        self.write([self.record(cpu=90, exclusive=50, flags=1)], cpu=1)
        result = profile.analyze(self.path, self.sites)
        self.assertEqual(result["groups"][0]["cpu_ns"]["sum"], 90)

    def test_drop_and_clock_error(self):
        for field in ("missed", "errors"):
            with self.subTest(field=field):
                self.write([self.record()], **{field: 1})
                self.assertFalse(profile.analyze(self.path, self.sites)["complete"])

    def test_invalid_record(self):
        for change in ({"id": 9}, {"flags": 8}, {"own": 101}, {"cpu": 1}, {"parent": 1}, {"site": 0}, {"thread": 0}):
            with self.subTest(change=change):
                self.write([self.record(**change)])
                with self.assertRaises(ValueError):
                    profile.analyze(self.path, self.sites)

    def test_invalid_parent(self):
        for change in ({"start": 50}, {"thread": 2}, {"elapsed": 101}):
            with self.subTest(change=change):
                self.write([self.record(), self.record(id=2, parent=1, **change)])
                with self.assertRaises(ValueError):
                    profile.analyze(self.path, self.sites)

    def test_truncated(self):
        for data in (b"", bytes(128)):
            self.path.write_bytes(data)
            with self.assertRaises(ValueError):
                profile.analyze(self.path, self.sites)

    def test_quantiles_and_hash(self):
        self.assertEqual(profile.identify(""), 14695981039346656037)
        self.assertEqual(profile.identify("hello"), 0xA430D84680AABD0B)
        result = profile.distribution([10, 1, 5])
        self.assertEqual((result["p50"], result["p99"]), (5, 10))
        self.assertFalse(result["tail_samples_sufficient"])


def binaries(enabled, disabled, root):
    """只运行 CMake 已构建的测试对象, 不编译/下载, 每个场景独立目录且退出后才解析."""
    sites = profile.inventory(profile.ROOT)
    environment = {key: value for key, value in os.environ.items() if not key.startswith("ASTRA_PROFILE_")}
    with tempfile.TemporaryDirectory(prefix="profile-", dir=root) as directory:

        def run(name, binary, mode="normal", **settings):
            destination = Path(directory) / name
            destination.mkdir()
            env = dict(environment, ASTRA_PROFILE_DIR=str(destination), ASTRA_PROFILE_SAMPLE="1", ASTRA_PROFILE_MIB="1", ASTRA_PROFILE_CPU="0")
            env.update(settings)
            for key in tuple(env):
                if env[key] is None:
                    del env[key]
            subprocess.run([str(binary), mode], check=True, timeout=30, env=env, capture_output=True, text=True)
            return [profile.analyze(path, sites) for path in destination.glob("*.profile")]

        if run("disabled", disabled):
            raise AssertionError("Disabled macros created a file")
        # 名称和初始化错误字符串都不应出现在关闭二进制, 不靠 Release 优化器侥幸消除运行期分支.
        raw = disabled.read_bytes()
        if b"ASTRA_PROFILE_DIR" in raw or b"profile.test.work" in raw:
            raise AssertionError("Disabled binary retains probe strings")
        if run("unset", enabled, ASTRA_PROFILE_DIR=None):
            raise AssertionError("Unset directory created records")
        if run("invalid", enabled, ASTRA_PROFILE_SAMPLE="0"):
            raise AssertionError("Invalid sampling accepted")
        normal = run("normal", enabled)
        if len(normal) != 1 or not normal[0]["complete"] or normal[0]["spans"] != 402 or normal[0]["counters"] != 81 or normal[0]["intervals"] != 1:
            raise AssertionError("Nested/thread/unwind record accounting changed")
        cpu = run("cpu", enabled, ASTRA_PROFILE_CPU="1")
        if len(cpu) != 1 or not cpu[0]["complete"] or not cpu[0]["cpu"]:
            raise AssertionError("CPU recording failed")
        sampled = run("sampled", enabled, ASTRA_PROFILE_SAMPLE="4")
        if len(sampled) != 1 or not sampled[0]["complete"] or not 0 < sampled[0]["spans"] < normal[0]["spans"]:
            raise AssertionError("Sampling did not retain whole trees")
        phase = run("phase", enabled, mode="phase", ASTRA_PROFILE_SAMPLE="64")
        if len(phase) != 1 or not phase[0]["complete"] or {group["name"] for group in phase[0]["groups"]} != {"profile.test.first", "profile.test.second"}:
            raise AssertionError("Sampling aliases a fixed alternating call sequence")
        full = run("full", enabled, mode="overflow")
        if len(full) != 1 or full[0]["complete"] or not full[0]["missed"]:
            raise AssertionError("Bounded recording did not report overflow")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--enabled", type=Path)
    parser.add_argument("--disabled", type=Path)
    parser.add_argument("--temporary-root", type=Path)
    options = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Format))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if options.enabled or options.disabled:
        if not (options.enabled and options.disabled and options.temporary_root):
            parser.error("both binaries and temporary root are required")
        binaries(options.enabled, options.disabled, options.temporary_root)
