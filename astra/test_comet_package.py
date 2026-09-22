"""Explicit SDK-only and installed-package consumption; no downloads or service launch."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import socket
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from testkit.support import run_command, temporary_directory
from build import parallel, resources


def main():
    """仅由获准的测试入口调用, 所有配置、编译和消费产物归独占项目临时目录."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--sanitizer", default="")
    options = parser.parse_args()
    if sys.platform != "linux" or not options.compiler.is_file():
        raise RuntimeError(
            "Comet package qualification requires the approved Linux compiler"
        )
    cpus, available = resources()
    jobs, _ = parallel(
        cpus,
        available,
        (
            "tsan"
            if options.sanitizer == "thread"
            else "asan" if options.sanitizer else "release"
        ),
    )
    consumer = ROOT / "astra/comet/cpp/tests/consumer"
    source = ROOT / "astra/comet/cpp"
    with temporary_directory(
        "comet-package-"
    ) as directory, socket.socket() as reservation:
        temporary = Path(directory)
        # 占有但不监听此端口, 配置验收不会误连开发者已有服务; 不需要额外测试服务器.
        reservation.bind(("127.0.0.1", 0))
        endpoint = f"127.0.0.1:{reservation.getsockname()[1]}"
        arguments = [endpoint, str(temporary / "missing-ca.pem")]
        env = dict(os.environ)
        env["VERDANDI_TEST_LOG_DIR"] = str(temporary / "logs")
        built, installed, packaged = (
            temporary / name for name in ("source", "installed", "consumer")
        )
        common = [
            f"-DCMAKE_CXX_COMPILER={options.compiler}",
            f"-DCMAKE_PREFIX_PATH={options.prefix}",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCOMET_SANITIZER={options.sanitizer}",
            "-DCOMET_BUILD_TESTS=OFF",
            f"-DCOMET_CA_FILE={ROOT / 'cluster/tests/fixtures/star-a/ca.pem'}",
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        ]
        run_command(
            "comet source configure",
            ["cmake", "-S", consumer, "-B", built, f"-DCOMET_SOURCE={source}", *common],
            env=env,
            timeout=120,
        )
        run_command(
            "comet source consume",
            ["cmake", "--build", built, "--target", "consumer", "--parallel", jobs],
            env=env,
            timeout=600,
        )
        run_command(
            "comet source executable",
            [built / "consumer", *arguments],
            env=env,
            timeout=20,
        )
        run_command(
            "comet install",
            [
                "cmake",
                "--install",
                built,
                "--prefix",
                installed,
                "--component",
                "Comet",
            ],
            env=env,
            timeout=60,
        )
        if (
            any(installed.rglob("*.proto"))
            or any(installed.rglob("*key.pem"))
            or (installed / "bin").exists()
        ):
            raise RuntimeError(
                "SDK installation unexpectedly contains service/protocol/private material"
            )
        # 新消费工程不提供源码目录/私有 include, 只通过安装前缀发现 SDK 和既有第三方库.
        run_command(
            "comet installed configure",
            [
                "cmake",
                "-S",
                consumer,
                "-B",
                packaged,
                *common,
                f"-DCMAKE_PREFIX_PATH={installed};{options.prefix}",
            ],
            env=env,
            timeout=120,
        )
        run_command(
            "comet installed consume",
            ["cmake", "--build", packaged, "--parallel", jobs],
            env=env,
            timeout=120,
        )
        run_command(
            "comet installed executable",
            [packaged / "consumer", *arguments],
            env=env,
            timeout=20,
        )
        for path in installed.rglob("*.cmake"):
            text = path.read_text(encoding="utf-8")
            if str(temporary) in text or str(ROOT) in text:
                raise RuntimeError("Installed CMake export leaks build/source paths")


if __name__ == "__main__":
    # CTest 取消必须走资源清理, 不能在临时目录销毁后留下编译器子进程.
    def interrupted(number, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    main()
