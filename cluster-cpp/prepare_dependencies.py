"""显式准备固定 C++ 依赖. fetch 才允许联网, build 仅使用已验证的项目源码缓存."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

PROJECT = Path(__file__).resolve().parent.parent
CACHE = PROJECT / "build/deps/cluster-cpp"
LOCK = json.loads((PROJECT / "cluster-cpp/dependencies.lock.json").read_text())
GCC = PROJECT / "build/tools/gcc-16.2.0"
# 该 gRPC/BoringSSL 版本的安装目标. BoringSSL 没有关闭所有自测的总开关, 因而不构建默认 all.
# 清单覆盖导出包引用的库、C++ 生成器和 BoringSSL 自己安装的 bssl, 不修改上游源文件或警告规则.
GRPC_INSTALL_TARGETS = [
    "ssl",
    "crypto",
    "address_sorting",
    "gpr",
    "grpc",
    "grpc_unsecure",
    *[
        f"upb_{name}_lib"
        for name in ("base", "descriptor", "hash", "json", "lex", "mem", "message", "mini_descriptor", "mini_table", "reflection", "textformat", "wire")
    ],
    "utf8_range_lib",
    "grpc++",
    "grpc++_alts",
    "grpc++_error_details",
    "grpc++_reflection",
    "grpc++_unsecure",
    "grpc_authorization_provider",
    "grpc_plugin_support",
    "grpcpp_channelz",
    "grpc_cpp_plugin",
    "bssl",
]


def source(item):
    return CACHE / "src" / f"{item['name']}-{item['commit']}"


def archive(item):
    return CACHE / f"{item['name']}-{item['commit']}.tar.gz"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(item):
    if not archive(item).is_file() or digest(archive(item)) != item["sha256"]:
        raise RuntimeError("Missing or mismatched source archive: " + item["name"])


def fetch(proxy):
    # 下载入口与正常构建分离, URL/校验值只来自提交的来源锁, 不查询 latest.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({"https": proxy} if proxy else {}))
    for item in LOCK["sources"]:
        path = archive(item)
        if not path.exists():
            request = urllib.request.Request(
                f"https://codeload.github.com/{item['repository']}/tar.gz/{item['commit']}", headers={"User-Agent": "Verdandi-dependency-prepare"}
            )
            temporary = path.with_suffix(".partial")
            with opener.open(request, timeout=60) as response, temporary.open("wb") as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
                    if output.tell() > 512 * 1024 * 1024:
                        raise RuntimeError("Source archive exceeds 512 MiB budget")
            if digest(temporary) != item["sha256"]:
                raise RuntimeError("Downloaded archive checksum mismatch: " + item["name"])
            temporary.replace(path)
        verify(item)
        target = source(item)
        marker = target / ".verdandi-extracted"
        if marker.exists() and marker.read_text().strip() != item["commit"]:
            raise RuntimeError("Extracted source marker mismatch: " + item["name"])
        if not marker.exists():
            target.mkdir(parents=True, exist_ok=True)
            with tarfile.open(path) as bundle:
                for member in bundle.getmembers():
                    parts = Path(member.name).parts
                    if len(parts) > 1:
                        member.name = str(Path(*parts[1:]))
                        bundle.extract(member, target, filter="data")
            marker.write_text(item["commit"] + "\n")
        print("Verified source: " + item["name"], flush=True)
    sources = {item["name"]: source(item) for item in LOCK["sources"]}
    # gRPC 自己的协议子模块随其固定版本展开, 不开启上游的通用归档下载器.
    shutil.copytree(sources["grpc-proto"], sources["grpc"] / "third_party/grpc-proto", dirs_exist_ok=True)


def limits(tsan=False):
    import resource

    # TSan 生成器需要映射大块虚拟 shadow 地址, 不能沿用普通构建的 2 GiB 地址空间限制.
    # 两种配置均保持一个编译任务且关闭 core dump, TSan 的物理内存另由测试资源采样检查.
    if not tsan:
        resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def build(profile):
    tsan = profile == "tsan"
    prefix = CACHE / ("linux-gcc16-tsan" if tsan else "linux-gcc16") / "install"
    for item in LOCK["sources"]:
        verify(item)
        marker = source(item) / ".verdandi-extracted"
        if not marker.is_file() or marker.read_text().strip() != item["commit"]:
            raise RuntimeError("Source not extracted; use the explicit fetch step: " + item["name"])
    sources = {item["name"]: source(item) for item in LOCK["sources"]}
    env = dict(os.environ)
    # 空继承值不能变成末尾分隔符, 否则动态链接器还会搜索当前目录.
    env["PATH"] = os.pathsep.join(path for path in (str(GCC / "bin"), env.get("PATH", "")) if path)
    env["LD_LIBRARY_PATH"] = os.pathsep.join(path for path in (str(GCC / "lib64"), env.get("LD_LIBRARY_PATH", "")) if path)
    env["CMAKE_BUILD_PARALLEL_LEVEL"] = "1"
    env["TMPDIR"] = str(CACHE / "tmp")
    Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([str(GCC / "bin/g++"), "-dumpfullversion"], text=True, env=env).strip()
    if version != "16.2.0":
        raise RuntimeError("Expected GCC 16.2.0, found " + version)
    common = [
        f"-DCMAKE_C_COMPILER={GCC}/bin/gcc",
        f"-DCMAKE_CXX_COMPILER={GCC}/bin/g++",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG",
        "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
        "-DCMAKE_CXX_STANDARD=17",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DZLIB_USE_STATIC_LIBS=ON",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
        "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
    ]
    if tsan:
        # 同步原子与锁必须一同插桩. 隔离前缀保留正常运行库, 不使用 suppression 掩盖依赖边界.
        common.extend(
            [
                "-DCMAKE_C_FLAGS=-fsanitize=thread -g1 -fno-omit-frame-pointer",
                "-DCMAKE_CXX_FLAGS=-fsanitize=thread -g1 -fno-omit-frame-pointer",
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread",
            ]
        )
    packages = [
        ("abseil", ["-DABSL_BUILD_TESTING=OFF", "-DABSL_ENABLE_INSTALL=ON"]),
        ("zlib", ["-DZLIB_BUILD_TESTING=OFF"]),
        ("cares", ["-DCARES_SHARED=OFF", "-DCARES_STATIC=ON", "-DCARES_BUILD_TESTS=OFF", "-DCARES_BUILD_TOOLS=OFF"]),
        ("re2", ["-DRE2_BUILD_TESTING=OFF"]),
        ("yyjson", ["-DYYJSON_BUILD_TESTS=OFF", "-DYYJSON_BUILD_MISC=OFF"]),
        ("protobuf", ["-Dprotobuf_BUILD_TESTS=OFF", "-Dprotobuf_ABSL_PROVIDER=package", "-Dprotobuf_WITH_ZLIB=OFF", "-Dprotobuf_BUILD_SHARED_LIBS=OFF"]),
        (
            "grpc",
            [
                "-DgRPC_BUILD_TESTS=OFF",
                "-DgRPC_DOWNLOAD_ARCHIVES=OFF",
                "-DgRPC_INSTALL=ON",
                "-DgRPC_BUILD_CODEGEN=ON",
                "-DgRPC_SSL_PROVIDER=module",
                f"-DBORINGSSL_ROOT_DIR={sources['boringssl']}",
                *[f"-DgRPC_{name}_PROVIDER=package" for name in ("ABSL", "PROTOBUF", "CARES", "RE2", "ZLIB")],
                *[f"-DgRPC_BUILD_GRPC_{name}_PLUGIN=OFF" for name in ("CSHARP", "NODE", "OBJECTIVE_C", "PHP", "PYTHON", "RUBY")],
                "-DgRPC_BUILD_GRPCPP_OTEL_PLUGIN=OFF",
                f"-DZLIB_LIBRARY_RELEASE={prefix}/lib/libz.a",
                f"-DZLIB_LIBRARY_DEBUG={prefix}/lib/libz.a",
                f"-DZLIB_INCLUDE_DIR={prefix}/include",
            ],
        ),
    ]
    for name, options in packages:
        directory = prefix.parent / "build" / name
        directory.mkdir(parents=True, exist_ok=True)
        build_command = ["cmake", "--build", directory, "--parallel", "1"]
        if name == "grpc":
            build_command.extend(["--target", *GRPC_INSTALL_TARGETS])
        # 重新配置校验锁定参数; CMake/Make 复用对象文件, 不以一个旧 marker 跳过配置验证.
        with (directory / "build.log").open("a") as log:
            for command in (
                ["cmake", "-S", sources[name], "-B", directory, *common, *options],
                build_command,
                ["cmake", "--install", directory, "--prefix", prefix],
            ):
                print("Running: " + subprocess.list2cmdline(list(map(str, command))), flush=True)
                result = subprocess.run(list(map(str, command)), env=env, stdout=log, stderr=subprocess.STDOUT, preexec_fn=lambda: limits(tsan))
                if result.returncode:
                    raise RuntimeError(f"{name} preparation failed; inspect {directory / 'build.log'}")
    for executable, tool_version, folder in ([] if tsan else [("protoc", "36.1", "protoc"), ("grpc_cpp_plugin", "1.84.0", "grpc-cpp-plugin")]):
        target = PROJECT / "build/tools" / folder / tool_version / "bin" / executable
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(prefix / "bin" / executable, target)
    # 记录静态库和生成工具摘要, 不把源码 commit 当作二进制校验值.
    artifacts = {
        str(path.relative_to(prefix)): digest(path)
        for path in sorted(prefix.rglob("*"))
        if path.is_file() and (path.suffix == ".a" or path.parent == prefix / "bin")
    }
    (CACHE / ("artifacts-tsan.json" if tsan else "artifacts.json")).write_text(
        json.dumps({"compiler": version, "profile": profile, "sources": LOCK, "artifacts": artifacts}, indent=2) + "\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["fetch", "build"])
    parser.add_argument("--profile", choices=["release", "tsan"], default="release")
    parser.add_argument("--proxy", help="Explicit HTTPS proxy, only used by fetch")
    options = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Dependency preparation is currently qualified only for Linux/GCC 16.2")
    if options.command == "build" and options.proxy:
        parser.error("build is offline and does not accept a proxy")
    CACHE.mkdir(parents=True, exist_ok=True)
    # 锁只保护这套项目依赖, 避免两个准备进程同时写入归档或构建目录.
    import fcntl

    with (CACHE / "prepare.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fetch(options.proxy) if options.command == "fetch" else build(options.profile)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1)
