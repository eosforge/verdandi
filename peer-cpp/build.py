"""Offline C++26 service build, explicit generation and owned process regression."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "peer-cpp"
PREFIX = ROOT / "build/deps/peer-cpp/linux-gcc16/install"


def environment(prefix=PREFIX):
    """仅为子进程选择项目工具和运行库, 不修改调用者或系统环境."""
    result = dict(os.environ)
    gcc = ROOT / "build/tools/gcc-16.2.0"
    result["PATH"] = os.pathsep.join([str(gcc / "bin"), str(prefix / "bin"), result.get("PATH", "")])
    result["LD_LIBRARY_PATH"] = os.pathsep.join([str(gcc / "lib64"), str(prefix / "lib"), result.get("LD_LIBRARY_PATH", "")])
    result["CMAKE_BUILD_PARALLEL_LEVEL"] = "1"
    result["PYTHONDONTWRITEBYTECODE"] = "1"
    result["TMPDIR"] = str(ROOT / "build/tmp")
    Path(result["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    return result


def run(command, env):
    """参数以列表传递, 不拼接 shell; 失败直接保留退出码, 不触发包管理器修复."""
    print("Running: " + subprocess.list2cmdline(list(map(str, command))), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)


def generated(check, env):
    """显式生成到项目临时目录, 检查模式逐字节比较且不修改源码."""
    protoc = ROOT / "build/tools/protoc/36.1/bin/protoc"
    if not protoc.is_file():
        protoc = PREFIX / "bin/protoc"
    plugin = ROOT / "build/tools/grpc-cpp-plugin/1.84.0/bin/grpc_cpp_plugin"
    if not plugin.is_file():
        plugin = PREFIX / "bin/grpc_cpp_plugin"
    if not protoc.is_file() or not plugin.is_file():
        raise RuntimeError("Missing approved protoc 36.1 or grpc_cpp_plugin 1.84.0; generation never downloads tools")
    version = subprocess.check_output([str(protoc), "--version"], env=env, text=True).strip()
    if version != "libprotoc 36.1":
        raise RuntimeError("Unexpected protoc version: " + version)
    destination = SOURCE / "common/src/generated"
    with tempfile.TemporaryDirectory(prefix="peer-cpp-proto-", dir=ROOT / "build/tmp") as temporary:
        run(
            [
                protoc,
                "--proto_path=" + str(ROOT / "proto"),
                "--cpp_out=" + temporary,
                "--grpc_out=" + temporary,
                "--plugin=protoc-gen-grpc=" + str(plugin),
                "peer.proto",
                "admission.proto",
                "peer_transport.proto",
            ],
            env,
        )
        files = sorted(Path(temporary).iterdir())
        if check:
            if not destination.exists() or {p.name for p in destination.iterdir()} != {p.name for p in files}:
                raise RuntimeError("Generated protocol file set differs")
            for path in files:
                if path.read_bytes() != (destination / path.name).read_bytes():
                    raise RuntimeError("Generated protocol differs: " + path.name)
        else:
            destination.mkdir(parents=True, exist_ok=True)
            for path in files:
                shutil.copyfile(path, destination / path.name)
    print("Protocol generation comparison passed" if check else "Generated C++ protocol sources", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["configure", "build", "test", "regression", "soak", "generate", "check-generated"])
    parser.add_argument("--profile", choices=["debug", "release", "asan", "tsan"], default="debug")
    parser.add_argument("--duration", type=int, default=3600)
    parser.add_argument("--core-only", action="store_true", help="Only offline core tests; no service qualification")
    options = parser.parse_args()
    if sys.platform != "linux":
        parser.error("The C++26 runtime currently requires Linux and the project GCC 16.2 toolchain")
    if not 1 <= options.duration <= 604800:
        parser.error("duration must be 1..604800 seconds")
    if options.core_only and options.command in {"regression", "soak"}:
        parser.error("Core-only mode cannot qualify service processes")
    prefix = ROOT / "build/deps/peer-cpp/linux-gcc16-tsan/install" if options.profile == "tsan" and not options.core_only else PREFIX
    env = environment(prefix)
    # Sanitizer 发现问题必须返回失败, 不能让可恢复的 UBSan 诊断被 CTest 成功输出折叠.
    if options.profile == "asan":
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    elif options.profile == "tsan":
        env["TSAN_OPTIONS"] = "halt_on_error=1:exitcode=66"
    if options.command in {"generate", "check-generated"}:
        generated(options.command == "check-generated", env)
        return
    if options.profile == "tsan" and not options.core_only and not (ROOT / "build/deps/peer-cpp/artifacts-tsan.json").is_file():
        raise RuntimeError("TSan requires the explicit offline dependency build: prepare_dependencies.py build --profile tsan")
    gcc = ROOT / "build/tools/gcc-16.2.0/bin"
    if not (gcc / "g++").is_file():
        raise RuntimeError("Missing project-local GCC 16.2 toolchain")
    output = ROOT / "build/peer-cpp" / (("core-" if options.core_only else "") + options.profile)
    configure = [
        "cmake",
        # 明确切换固定前缀, 避免旧 CMakeCache 中的库位置把 TSan 服务链接回未插桩依赖.
        *[
            "-U" + name
            for name in ("gRPC_DIR", "Protobuf_DIR", "absl_DIR", "utf8_range_DIR", "re2_DIR", "c-ares_DIR", "yyjson_DIR", "ZLIB_*", "peer_yyjson_headers")
        ],
        "-S",
        SOURCE,
        "-B",
        output,
        "-DCMAKE_C_COMPILER=" + str(gcc / "gcc"),
        "-DCMAKE_CXX_COMPILER=" + str(gcc / "g++"),
        "-DCMAKE_PREFIX_PATH=" + str(prefix),
        "-DCMAKE_BUILD_TYPE=" + ("Release" if options.profile == "release" else "Debug"),
        "-DVERDANDI_CORE_ONLY=" + ("ON" if options.core_only else "OFF"),
        "-DVERDANDI_SANITIZER=" + {"asan": "address,undefined", "tsan": "thread"}.get(options.profile, ""),
    ]
    run(configure, env)
    if options.command == "configure":
        return
    run(["cmake", "--build", output, "--parallel", "1"], env)
    if options.command == "build":
        return
    run(["ctest", "--test-dir", output, "--output-on-failure"], env)
    if options.command in {"regression", "soak"}:
        generated(True, env)
        run([sys.executable, "-B", SOURCE / "test_processes.py", "--binaries", output], env)
        run([sys.executable, "-B", "-m", "testkit.services", "--mode", options.command, "--duration", options.duration, "--peer-binaries", output], env)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1)
