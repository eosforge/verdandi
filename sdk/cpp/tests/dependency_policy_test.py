"""Offline regressions for the shared build policy and native entry adapters."""

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
CPP = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CPP))
import build as core
import build_support as support

WORK = None


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix="case-", dir=WORK))
        self.cpp = self.directory / "repo space/sdk/cpp"
        self.cpp.mkdir(parents=True)

    def build(self, host="linux", *arguments):
        native = support.Toolchain(host, sys.executable, "4.0.0", compiler_label="test", generator_name="test", generator_slug="test")
        return core.Build(core.parse_options(["all", "--offline", "--jobs", "1", *arguments]), native, dict(os.environ), self.cpp)

    def test_provider_order_and_system_managed_boundaries(self):
        for host in ("linux", "windows"):
            for scenario in ("system", "vcpkg", "cache", "missing", "managed", "system-only"):
                with self.subTest(host=host, scenario=scenario):
                    build = self.build(host)
                    root = self.directory / f"vcpkg-{host}"
                    package = root / f"installed/x64-{host}"
                    cache = build.build_root / f"deps/openssl/{host}/x64"
                    for prefix in (package, cache):
                        (prefix / "include/openssl").mkdir(parents=True, exist_ok=True)
                        (prefix / "include/openssl/ssl.h").touch()
                    vcpkg = core.Vcpkg(root, root / "vcpkg", root / "scripts/buildsystems/vcpkg.cmake", "test")
                    expected = scenario
                    success = f"openssl-{scenario}"
                    calls = ["cpp23", "openssl-system", "openssl-vcpkg", "openssl-cache"]
                    if scenario == "system":
                        calls = calls[:2]
                    elif scenario == "vcpkg":
                        calls = calls[:3]
                    elif scenario == "managed":
                        build.options.deps, expected, success = "managed", "cache", "openssl-cache"
                        calls = ["cpp23", "openssl-vcpkg", "openssl-cache"]
                    elif scenario == "system-only":
                        build.options.deps, expected, success = "system", "missing", "openssl-cache"
                        calls = calls[:2]
                    with (
                        patch.object(core, "resolve_vcpkg", return_value=vcpkg),
                        patch.object(build, "probe", side_effect=lambda name, *_: name in ("cpp23", success)) as probe,
                    ):
                        if expected == "missing":
                            with self.assertRaises(core.BuildError):
                                build.resolve_openssl()
                        else:
                            build.resolve_openssl()
                            self.assertEqual(build.provider, expected)
                            if expected != "system":
                                self.assertTrue(build.openssl_root)
                        self.assertEqual([call.args[0] for call in probe.call_args_list], calls)
                    arguments = build.openssl_arguments(build.openssl_root, build.installed)
                    self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", arguments)
                    self.assertIn("-DVCPKG_APPLOCAL_DEPS=OFF", arguments)

    def test_failed_probe_configuration_does_not_build_stale_tree(self):
        for host in ("linux", "windows"):
            with self.subTest(host=host):
                build = self.build(host)
                with patch.object(core, "run_process", return_value=subprocess.CompletedProcess([], 17)) as run:
                    self.assertFalse(build.probe("cpp23"))
                self.assertEqual(run.call_count, 1)
                self.assertIn("--fresh", run.call_args.args[0])
                self.assertTrue(build.probe_log.is_file())

    def test_all_stops_at_first_failure_and_preserves_code(self):
        build = self.build()
        with (
            patch.object(build, "diagnose"),
            patch.object(build, "configure", side_effect=core.BuildError("failed", 17)),
            patch.object(build, "build") as compile_stage,
            patch.object(build, "test") as tests,
        ):
            with self.assertRaises(core.BuildError) as failure:
                build.execute()
            self.assertEqual(failure.exception.returncode, 17)
            compile_stage.assert_not_called()
            tests.assert_not_called()

    def test_offline_flags_cache_paths_and_shared_layout(self):
        for host in ("linux", "windows"):
            build = self.build(host, "--linkage", "shared")
            build.native.multi_config = host == "windows"
            build.provider = "cache"
            arguments = build.configure_arguments()
            self.assertIn("-DVERDANDI_OFFLINE_DEPENDENCIES=ON", arguments)
            self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=OFF", arguments)
            self.assertIn("-DFETCHCONTENT_UPDATES_DISCONNECTED=ON", arguments)
            self.assertIn("-DCMAKE_TOOLCHAIN_FILE=", arguments)
            expected = "test/test/cache/auto/dev-shared"
            self.assertTrue(build.directory.as_posix().endswith(f"cpp/{host}/x64/{expected}"))
            self.assertTrue(build.dependencies.as_posix().endswith(f"deps/{host}/x64/{expected}"))
            self.assertEqual(build.runtime.name, "verdandi_cpp.dll" if host == "windows" else "libverdandi_cpp.so")
            if host == "windows":
                self.assertEqual(build.runtime.parent.name, "Debug")
            original = build.directory
            build.options.deps = "managed"
            self.assertNotEqual(original, build.directory)
            self.assertIn("-DVERDANDI_USE_MANAGED_DEPENDENCIES=ON", build.configure_arguments())

    def test_dry_run_creates_no_build_output(self):
        build = self.build("linux", "--dry-run")
        with patch.object(core, "resolve_vcpkg", return_value=None), patch.object(core, "run_process") as run:
            build.diagnose()
            build.configure()
            build.build()
        run.assert_not_called()
        self.assertFalse(build.build_root.exists())

    def test_vcpkg_explicit_invalid_selection_never_falls_back(self):
        for host in ("linux", "windows"):
            build = self.build(host, "--vcpkg-root", str(self.directory / "missing"))
            with self.assertRaises(core.BuildError):
                core.resolve_vcpkg(build.native, build.options, {})
            root = self.directory / f"incomplete-{host}"
            root.mkdir()
            build.options.vcpkg_root = str(root)
            with patch.object(core, "vcpkg_candidates", return_value=[]), self.assertRaises(core.BuildError):
                core.resolve_vcpkg(build.native, build.options, {})

    def test_vcpkg_broken_automatic_candidate_is_skipped(self):
        build = self.build()
        roots = [self.directory / name for name in ("bad package", "good package")]
        for root in roots:
            (root / "scripts/buildsystems").mkdir(parents=True)
            (root / "scripts/buildsystems/vcpkg.cmake").touch()
            (root / "vcpkg").touch()
            (root / "vcpkg").chmod(0o755)
        with patch.object(core, "vcpkg_candidates", return_value=roots), patch.object(core, "query", side_effect=[core.BuildError("broken"), "version 1"]):
            self.assertEqual(core.resolve_vcpkg(build.native, build.options, {}).root, roots[1])
        build.options.vcpkg_root = str(roots[0])
        with (
            patch.object(core, "vcpkg_candidates", return_value=roots),
            patch.object(core, "query", side_effect=core.BuildError("broken")),
            self.assertRaises(core.BuildError),
        ):
            core.resolve_vcpkg(build.native, build.options, {})

    def test_cli_rejects_invalid_and_abbreviated_options(self):
        for arguments in (["--jobs", "-1"], ["--jobs", "257"], ["--jobs", "1.5"], ["--off"], ["--profile", "oops"], ["clean"]):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failure:
                core.parse_options(arguments)
            self.assertEqual(failure.exception.code, 2)

    def test_child_argument_cwd_environment_and_exit_contract(self):
        original_env, original_cwd = dict(os.environ), Path.cwd()
        env = {**os.environ, "VERDANDI_TEST_CHILD_ONLY": "child"}
        arguments = ["", "space path/", 'quote"literal', "backslash\\", "中文", "$()", chr(96)]
        program = "import json, os, sys; print(json.dumps([sys.argv[1:], os.getcwd(), os.getenv('VERDANDI_TEST_CHILD_ONLY')])); sys.exit(17)"
        result = support.run_process([sys.executable, "-I", "-S", "-B", "-c", program, *arguments], cwd=self.directory, env=env, capture=True)
        received, cwd, value = json.loads(result.stdout)
        self.assertEqual(received, arguments)
        self.assertEqual(Path(cwd), self.directory)
        self.assertEqual(value, "child")
        self.assertEqual(result.returncode, 17)
        self.assertEqual(dict(os.environ), original_env)
        self.assertEqual(Path.cwd(), original_cwd)

    def test_windows_generator_matches_installed_vs_not_newest_known(self):
        build = self.build("windows")
        root = self.directory / "Program Files/Visual Studio"
        toolset = root / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
        toolset.parent.mkdir(parents=True)
        toolset.write_text("14.40.1", encoding="utf-8")
        compiler = root / "VC/Tools/MSVC/14.40.1/bin/Hostx64/x64/cl.exe"
        compiler.parent.mkdir(parents=True)
        compiler.touch()
        env = {"PROGRAMFILES": str(self.directory / "Program Files")}
        observed = []

        def find(*names, required=False):
            observed.extend(map(str, names))
            return "vswhere.exe" if names[0] == "vswhere.exe" else ""

        responses = [json.dumps([{"installationPath": str(root), "installationVersion": "17.9.0"}]), "Visual Studio 18 2026\nVisual Studio 17 2022"]
        with patch.object(support, "executable", side_effect=find), patch.object(support, "query", side_effect=responses):
            support.windows_toolchain(build.native, build.options, env)
        self.assertEqual(build.native.generator_name, "Visual Studio 17 2022")
        self.assertEqual(build.native.compiler_label, "msvc-19.40")
        self.assertTrue(any(Path(name).as_posix().endswith("Microsoft Visual Studio/Installer/vswhere.exe") for name in observed))
        self.assertIn(f"-DCMAKE_GENERATOR_INSTANCE={root.as_posix()}", build.native.generator_arguments)

    def test_linux_versioned_compiler_pair_and_unknown_wrapper(self):
        build = self.build()
        cxx = self.directory / "g++-15"
        cc = self.directory / "gcc-15"
        for path in (cxx, cc):
            path.touch()
            path.chmod(0o755)

        def find(*names, required=False):
            for name in names:
                if Path(name) in (cxx, cc):
                    return str(name)
                if str(name) in ("make", "gmake"):
                    return "/usr/bin/make"
            return ""

        with patch.object(support, "executable", side_effect=find), patch.object(support, "query", return_value="compiler 15.2.0"):
            support.linux_toolchain(build.native, build.options, {"CXX": str(cxx)})
        self.assertEqual(build.native.tools["c"]["path"], str(cc))
        with patch.object(support, "executable", return_value=str(self.directory / "custom-wrapper")), self.assertRaises(core.BuildError):
            support.linux_toolchain(build.native, build.options, {"CXX": "custom-wrapper"})

    def test_timeout_stops_owned_grandchild(self):
        marker = self.directory / "child.pid"
        program = (
            "import pathlib, subprocess, sys, time; "
            "child = subprocess.Popen([sys.executable, '-I', '-S', '-c', 'import time; time.sleep(60)']); "
            "pathlib.Path(sys.argv[1]).write_text(str(child.pid)); time.sleep(60)"
        )
        with self.assertRaises(subprocess.TimeoutExpired):
            support.run_process([sys.executable, "-I", "-S", "-B", "-c", program, str(marker)], capture=True, timeout=2)
        self.assertTrue(marker.is_file(), "Child did not start before the timeout")
        pid = int(marker.read_text())
        if os.name == "nt":
            import ctypes

            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x100000, False, pid)
            if handle:
                try:
                    alive = kernel.WaitForSingleObject(handle, 0) == 258
                finally:
                    kernel.CloseHandle(handle)
            else:
                alive = False
            if alive:
                subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32/taskkill.exe"), "/PID", str(pid), "/T", "/F"], check=False)
        else:
            status = Path(f"/proc/{pid}/stat")
            try:
                alive = status.read_text().split(") ", 1)[1].split()[0] != "Z"
            except FileNotFoundError:
                alive = False
            if alive:
                import signal

                os.kill(pid, signal.SIGKILL)
        self.assertFalse(alive, "Timed-out command left a running grandchild")


def entry_test(shell: str, directory: Path, powershell: bool) -> None:
    """Exercise the real shell transport using a generated argument-reporting fixture."""
    cpp = directory / ("PowerShell space 中文/sdk/cpp" if powershell else "Bash space 中文/sdk/cpp")
    cpp.mkdir(parents=True)
    entry = cpp / ("build.ps1" if powershell else "build.sh")
    shutil.copyfile(CPP / entry.name, entry)
    if powershell:
        shutil.copyfile(CPP.parent / "run-tool.ps1", cpp.parent / "run-tool.ps1")
    (cpp / "build.py").write_text(
        "import json, os, sys\nprint(json.dumps([sys.argv[1:], os.getcwd()], ensure_ascii=False))\nraise SystemExit(17)\n", encoding="utf-8"
    )
    package = str(directory / "package with space 中文") + "\\"
    if powershell:
        command = [
            shell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(entry),
            "doctor",
            "-Python",
            sys.executable,
            "-Offline",
            "-DryRun",
            "-Profile",
            "check",
            "-Linkage",
            "shared",
            "-Deps",
            "managed",
            "-Jobs",
            "1",
            "-VcpkgRoot",
            package,
        ]
        expected = [
            "doctor",
            "--profile",
            "check",
            "--linkage",
            "shared",
            "--deps",
            "managed",
            "--generator",
            "auto",
            "--compiler",
            "auto",
            "--jobs",
            "1",
            "--vcpkg-root",
            package,
            "--offline",
            "--dry-run",
        ]
    else:
        expected = [
            "doctor",
            "--offline",
            "--dry-run",
            "--profile",
            "check",
            "--linkage",
            "shared",
            "--deps",
            "managed",
            "--jobs",
            "1",
            "--vcpkg-root",
            package,
            "",
            'quote"literal',
            "$()",
            chr(96),
        ]
        command = [shell, str(entry), "--python", sys.executable, *expected]
    result = subprocess.run(command, cwd=directory, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    if result.returncode != 17:
        raise AssertionError(f"Entry did not preserve exit code: {result.returncode}\n{result.stdout}\n{result.stderr}")
    arguments, cwd = json.loads(result.stdout)
    if arguments != expected or Path(cwd) != directory:
        raise AssertionError(f"Entry changed arguments or cwd:\n{arguments!r}\n{expected!r}\n{cwd}")
    print(f"Entry transport passed: {shell}")


def cmake_test(cmake: str, directory: Path) -> None:
    # 在真正的两个入口加载 toolchain 时截断；无需编译器/依赖即可验证前置保护。
    toolchain = directory / "guard.cmake"
    toolchain.write_text(
        'if(VCPKG_MANIFEST_INSTALL)\nmessage(FATAL_ERROR "UNEXPECTED_INSTALL")\nendif()\n' 'message(FATAL_ERROR "READ_ONLY_GUARD_PASSED")\n',
        encoding="utf-8",
    )
    for name, source in (("sdk", CPP), ("probe", CPP / "cmake/probe")):
        result = subprocess.run(
            [
                cmake,
                "--fresh",
                "-S",
                str(source),
                "-B",
                str(directory / f"guard-{name}"),
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
                "-DVCPKG_MANIFEST_INSTALL=ON",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0 or "READ_ONLY_GUARD_PASSED" not in result.stderr:
            raise AssertionError(f"Early install guard failed: {name}\n{result.stdout}\n{result.stderr}")

    # 伪造发现结果只检查来源隔离，绝不把空 .lib 文件当成可链接的真实开发包。
    package = directory / "mock-package"
    include = package / "include/openssl"
    include.mkdir(parents=True, exist_ok=True)
    (include / "ssl.h").write_text("", encoding="utf-8")
    for name in ("crypto.lib", "ssl.lib"):
        (package / name).write_text("", encoding="utf-8")
    (directory / "FindOpenSSL.cmake").write_text(
        """
set(OPENSSL_FOUND TRUE)
set(OPENSSL_INCLUDE_DIR "${VERDANDI_OPENSSL_ROOT}/include")
set(OPENSSL_CRYPTO_LIBRARY "${VERDANDI_OPENSSL_ROOT}/crypto.lib")
set(OPENSSL_SSL_LIBRARY "${VERDANDI_OPENSSL_ROOT}/ssl.lib")
if(MIXED_PACKAGE)
    set(OPENSSL_SSL_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/outside.lib")
endif()
""",
        encoding="utf-8",
    )
    driver = directory / "find.cmake"
    driver.write_text(
        'list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")\n' f'include("{(CPP / "cmake/OpenSSL.cmake").as_posix()}")\n',
        encoding="utf-8",
    )
    for mixed in (False, True):
        result = subprocess.run(
            [cmake, f"-DVERDANDI_OPENSSL_ROOT={package.as_posix()}", f"-DMIXED_PACKAGE={'ON' if mixed else 'OFF'}", "-P", str(driver)],
            capture_output=True,
            text=True,
            check=False,
        )
        if (result.returncode == 0) == mixed:
            raise AssertionError(f"Package boundary check failed: mixed={mixed}\n{result.stderr}")
        if mixed and "outside the selected package" not in result.stderr:
            raise AssertionError(result.stderr)
    print("CMake: 2 early install guards and 2 package-boundary scenarios passed.")


def main() -> None:
    global WORK
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--bash")
    parser.add_argument("--powershell", action="append", default=[])
    parser.add_argument("--cmake")
    options = parser.parse_args()
    WORK = options.work_dir.resolve()
    WORK.mkdir(parents=True, exist_ok=True)
    WORK = Path(tempfile.mkdtemp(prefix="run-", dir=WORK))
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(PolicyTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    for shell in options.powershell:
        entry_test(shell, Path(tempfile.mkdtemp(prefix="entry-", dir=WORK)), True)
    if options.bash:
        entry_test(options.bash, Path(tempfile.mkdtemp(prefix="entry-", dir=WORK)), False)
    if options.cmake:
        cmake_test(options.cmake, Path(tempfile.mkdtemp(prefix="cmake-", dir=WORK)))


if __name__ == "__main__":
    main()
