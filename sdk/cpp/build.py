#!/usr/bin/env python3
"""Build and test the Verdandi C++23/C ABI runtime on Windows x64 and Linux x64."""

from __future__ import annotations

import argparse
from collections import deque
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

# 直接运行和薄入口均不在源码目录产生 __pycache__。
sys.dont_write_bytecode = True
if sys.version_info < (3, 10):
    raise SystemExit("Python 3.10+ is required; select an existing interpreter.")
from build_support import BuildError, Toolchain, executable, query, resolve_toolchain, run_process

CPP = Path(__file__).resolve().parent


def note(message: str) -> None:
    print(f"[verdandi] {message}", flush=True)


def section(title: str) -> None:
    print(f"\n== {title} ==", flush=True)


def display(command) -> str:
    arguments = list(map(str, command))
    return subprocess.list2cmdline(arguments) if os.name == "nt" else shlex.join(arguments)


def parse_options(arguments=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Toolchains, vcpkg and OpenSSL are prepared externally. Other locked dependency downloads require prior approval. Go/Rust/C# build independently.",
        allow_abbrev=False,
    )
    parser.add_argument("command", nargs="?", choices=("doctor", "configure", "build", "test", "all"), default="all", type=str.lower)
    for name, choices, default in (
        ("profile", ("dev", "check", "release"), "dev"),
        ("linkage", ("auto", "static", "shared"), "auto"),
        ("deps", ("auto", "system", "managed"), "auto"),
        ("generator", ("auto", "visual-studio", "ninja", "make"), "auto"),
        ("compiler", ("auto", "msvc", "gcc", "clang"), "auto"),
    ):
        parser.add_argument(f"--{name}", choices=choices, default=default, type=str.lower)
    parser.add_argument("--vcpkg-root", default="", help="Strict selection of an existing vcpkg installation")
    parser.add_argument("--jobs", type=int, default=0, help="Parallel jobs, 0..256; zero uses the logical processor count")
    parser.add_argument("--offline", action="store_true", help="Use verified local dependency archives only")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan without writing output or compiling probes")
    options = parser.parse_args(arguments)
    if not 0 <= options.jobs <= 256:
        parser.error("--jobs must be in the range 0..256")
    return options


@dataclass(frozen=True)
class Vcpkg:
    root: Path
    executable: Path
    toolchain: Path
    version: str


def vcpkg_candidates(native: Toolchain, env: dict) -> list[Path]:
    candidates = [Path(env["VCPKG_ROOT"])] if env.get("VCPKG_ROOT") else []
    toolchain = Path(env.get("CMAKE_TOOLCHAIN_FILE", "")).absolute()
    if toolchain.name.lower() == "vcpkg.cmake":
        candidates.append(toolchain.parents[2])
    if found := executable("vcpkg.exe" if native.platform == "windows" else "vcpkg"):
        candidates.append(Path(found).parent)
    candidates.append(Path.home() / "vcpkg")
    if native.platform == "windows":
        import ctypes

        if env.get("LOCALAPPDATA"):
            candidates.append(Path(env["LOCALAPPDATA"]) / "vcpkg")
        # 仅检查已挂载盘符的两个精确目录，不递归搜索磁盘。
        drives = ctypes.windll.kernel32.GetLogicalDrives()
        for index in range(26):
            if drives & (1 << index):
                drive = Path(f"{chr(65 + index)}:/")
                candidates.extend((drive / "vcpkg", drive / "Program Files/vcpkg"))
        if native.visual_studio:
            candidates.append(Path(native.visual_studio["path"]) / "VC/vcpkg")
    else:
        candidates.extend(map(Path, ("/opt/vcpkg", "/usr/local/vcpkg", "/usr/local/share/vcpkg")))
    return candidates


def resolve_vcpkg(native: Toolchain, options, env: dict) -> Vcpkg | None:
    candidates = []
    explicit = None
    if options.vcpkg_root:
        explicit = Path(options.vcpkg_root).resolve()
        if explicit.is_file():
            explicit = explicit.parent
        if not explicit.is_dir():
            raise BuildError(f"The explicit vcpkg root does not exist or is not a directory: {options.vcpkg_root}")
        candidates.append(explicit)
    candidates.extend(vcpkg_candidates(native, env))
    seen = set()
    for candidate in candidates:
        root = candidate.resolve()
        if root.is_file():
            root = root.parent
        if root in seen:
            continue
        seen.add(root)
        program = root / ("vcpkg.exe" if native.platform == "windows" else "vcpkg")
        toolchain = root / "scripts/buildsystems/vcpkg.cmake"
        try:
            if not program.is_file() or not os.access(program, os.X_OK) or not toolchain.is_file():
                raise BuildError(f"Incomplete vcpkg installation: {root}")
            return Vcpkg(root, program, toolchain, query([program, "version"], env).partition("\n")[0])
        except (BuildError, OSError, subprocess.TimeoutExpired) as error:
            if root == explicit:
                raise BuildError(f"The explicit vcpkg installation is unusable: {root}\n{error}") from error
    return None


class Build:
    def __init__(self, options, native: Toolchain, env: dict, cpp: Path = CPP):
        self.options, self.native, self.env = options, native, dict(env)
        self.cpp = cpp
        self.root = cpp.parents[1]
        self.build_root = self.root / "build"
        self.jobs = options.jobs or max(1, os.cpu_count() or 1)
        self.linkage = "static" if options.linkage == "auto" else options.linkage
        self.configuration = "Debug" if options.profile == "dev" else "Release"
        self.vcpkg = None
        self.provider = "unresolved"
        self.openssl_root = self.installed = ""
        self.probe_log = None

    @property
    def dimensions(self) -> Path:
        return Path(self.native.platform, "x64", self.native.compiler_label, self.native.generator_slug)

    @property
    def variant(self) -> Path:
        return self.dimensions / self.provider / self.options.deps / f"{self.options.profile}-{self.linkage}"

    @property
    def directory(self) -> Path:
        return self.build_root / "cpp" / self.variant

    @property
    def dependencies(self) -> Path:
        return self.build_root / "deps" / self.variant

    @property
    def runtime(self) -> Path | None:
        if self.linkage != "shared":
            return None
        directory = self.directory / self.configuration if self.native.multi_config else self.directory
        return directory / ("verdandi_cpp.dll" if self.native.platform == "windows" else "libverdandi_cpp.so")

    def openssl_arguments(self, root="", installed="") -> list[str]:
        arguments = ["-DVCPKG_MANIFEST_INSTALL=OFF", "-DVCPKG_APPLOCAL_DEPS=OFF", f"-DVERDANDI_OPENSSL_ROOT={root}"]
        if installed:
            arguments += [
                f"-DCMAKE_TOOLCHAIN_FILE={self.vcpkg.toolchain.as_posix()}",
                f"-DVCPKG_TARGET_TRIPLET=x64-{self.native.platform}",
                f"-DVCPKG_HOST_TRIPLET=x64-{self.native.platform}",
                f"-DVCPKG_INSTALLED_DIR={installed}",
            ]
        else:
            arguments.append("-DCMAKE_TOOLCHAIN_FILE=")
        return arguments

    def build_arguments(self, directory: Path, target: str = "") -> list[str]:
        arguments = ["--build", str(directory), "--parallel", str(self.jobs)]
        if target:
            arguments += ["--target", target]
        if self.native.multi_config:
            arguments += ["--config", self.configuration, "--", "/nologo", "/verbosity:quiet"]
        return arguments

    def probe(self, name: str, root="", installed="") -> bool:
        if self.options.dry_run:
            note(f"Skipping the {name} compilation probe in dry-run mode.")
            return True
        directory = self.build_root / "probes" / self.dimensions / name
        directory.mkdir(parents=True, exist_ok=True)
        self.probe_log = directory / "probe.log"
        arguments = ["--fresh", "-S", str(self.cpp / "cmake/probe"), "-B", str(directory), "-G", self.native.generator_name]
        arguments += self.native.generator_arguments
        arguments += [f"-DVERDANDI_PROBE_OPENSSL={'OFF' if name == 'cpp23' else 'ON'}", f"-DCMAKE_BUILD_TYPE={self.configuration}"]
        arguments += self.openssl_arguments(root, installed)
        # 流式写探针日志，配置失败立即停止，既不缓存完整编译输出，也不构建旧工程。
        with self.probe_log.open("w", encoding="utf-8") as log:
            for stage in (arguments, self.build_arguments(directory)):
                command = [self.native.cmake, *stage]
                log.write(display(command) + "\n")
                log.flush()
                if run_process(command, cwd=self.root, env=self.env, stdout=log).returncode:
                    return False
        return True

    def show_probe_log(self) -> None:
        if self.probe_log and self.probe_log.is_file():
            print(f"[verdandi] Probe log: {self.probe_log}", file=sys.stderr)
            with self.probe_log.open(encoding="utf-8", errors="replace") as log:
                print("".join(deque(log, maxlen=30)), file=sys.stderr)

    def resolve_openssl(self) -> None:
        self.vcpkg = resolve_vcpkg(self.native, self.options, self.env)
        if not self.probe("cpp23"):
            self.show_probe_log()
            raise BuildError("The C++23 compile/link probe failed. Check the compiler, standard library, SDK and CMake generator.")
        if self.options.dry_run and self.options.deps != "managed":
            note("Dry-run plans system OpenSSL first; doctor checks system, installed vcpkg packages, then the prebuilt cache.")
            self.provider = "system"
            return
        if self.options.deps != "managed" and self.probe("openssl-system"):
            self.provider = "system"
            return
        if self.options.deps == "system":
            self.show_probe_log()
            raise BuildError("System dependency mode could not compile and link OpenSSL 3.0+. Set OPENSSL_ROOT_DIR/CMAKE_PREFIX_PATH or prepare it externally.")
        candidates = []
        if self.vcpkg:
            installed = Path(self.env.get("VCPKG_INSTALLED_DIR") or self.vcpkg.root / "installed").resolve()
            candidates.append(("vcpkg", installed / f"x64-{self.native.platform}", installed.as_posix()))
        cache = self.build_root / "deps/openssl" / self.native.platform / "x64"
        candidates.append(("cache", cache, ""))
        for provider, prefix, installed in candidates:
            if (prefix / "include/openssl/ssl.h").is_file() and self.probe(f"openssl-{provider}", prefix.as_posix(), installed):
                self.provider, self.openssl_root, self.installed = provider, prefix.as_posix(), installed
                note(f"Using the existing OpenSSL development package: {prefix}")
                return
            note(f"No usable prebuilt OpenSSL package in: {prefix}")
        raise BuildError(
            f"No compatible prebuilt OpenSSL 3.0+ development package was found for {self.native.platform}/x64.\n"
            "Required: matching headers and Crypto/SSL link libraries, plus runtime files for dynamic linking.\n"
            "Windows packages must be MSVC-compatible; Linux packages must match the target libc/distribution.\n"
            f"Provide OPENSSL_ROOT_DIR, an existing vcpkg installed/x64-{self.native.platform} package, or an extracted package at: {cache}\n"
            "Obtain approval for a specific binary package before downloading it. If none is suitable, build OpenSSL externally.\n"
            "Verdandi will not install packages/tools or build OpenSSL, even from cached sources. See build/probes for candidate logs."
        )

    def manifest(self) -> dict:
        return {
            "schema": "v1",
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "platform": self.native.platform,
            "architecture": "x64",
            "command": self.options.command,
            "profile": self.options.profile,
            "linkage": self.linkage,
            "dependencies": self.options.deps,
            "offline": self.options.offline,
            "jobs": self.jobs,
            "openssl": {"minimum": "3.0.0", "provider": self.provider, "root": self.openssl_root},
            "vcpkg": asdict(self.vcpkg) if self.vcpkg else None,
            "paths": {"cpp": str(self.directory), "cpp_dependencies": str(self.dependencies), "native_runtime": str(self.runtime) if self.runtime else None},
            "tools": {**self.native.tools, "python": {"path": sys.executable, "version": sys.version.split()[0]}},
        }

    def diagnose(self) -> None:
        self.resolve_openssl()
        note(f"Platform: {self.native.platform}/x64; command: {self.options.command}; profile: {self.options.profile}; jobs: {self.jobs}")
        note(f"Dependency policy: {self.options.deps}; offline mode: {str(self.options.offline).lower()}")
        for name, tool in self.native.tools.items():
            note(f"{name}: {tool['path']} (version {tool['version']})")
        note(f"CMake generator: {self.native.generator_name}; linkage: {self.linkage}; OpenSSL provider: {self.provider}")
        if self.vcpkg:
            note(f"vcpkg: {self.vcpkg.executable} ({self.vcpkg.version})")
        note(f"C++ build directory: {self.directory}")
        note(f"C++ dependency directory: {self.dependencies}")
        if self.runtime:
            note(f"Shared runtime output: {self.runtime}")
        if not self.options.dry_run:
            self.build_root.mkdir(parents=True, exist_ok=True)
            # 同目录原子替换，读者始终看到完整 JSON；失败日志留在 build 内。
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.build_root, prefix="environment-", suffix=".tmp", delete=False) as temporary:
                json.dump(self.manifest(), temporary, ensure_ascii=True, indent=2, default=str)
                temporary.write("\n")
            os.replace(temporary.name, self.build_root / "environment.json")

    def configure_arguments(self) -> list[str]:
        arguments = ["-S", str(self.cpp), "-B", str(self.directory), "-G", self.native.generator_name, *self.native.generator_arguments]
        if not self.native.multi_config:
            arguments.append(f"-DCMAKE_BUILD_TYPE={self.configuration}")
        definitions = {
            "BUILD_SHARED_LIBS": self.linkage == "shared",
            "VERDANDI_BUILD_TESTS": True,
            "VERDANDI_FETCH_DEPENDENCIES": self.options.deps != "system",
            "VERDANDI_USE_MANAGED_DEPENDENCIES": self.options.deps == "managed",
            "VERDANDI_OFFLINE_DEPENDENCIES": self.options.offline,
            "VERDANDI_ENABLE_SANITIZERS": False,
            # FULLY_DISCONNECTED 会阻止首次解压本地包；离线边界由 CMake 的本地 URL 校验负责。
            "FETCHCONTENT_FULLY_DISCONNECTED": False,
            "FETCHCONTENT_UPDATES_DISCONNECTED": self.options.offline,
            "FETCHCONTENT_BASE_DIR": (self.dependencies / "fetchcontent").as_posix(),
            "VERDANDI_DOWNLOAD_CACHE": (self.build_root / "deps/common/downloads/fetchcontent").as_posix(),
            "VERDANDI_CLANG_FORMAT": self.native.clang_format.replace("\\", "/"),
            "VERDANDI_RUN_CLANG_TIDY": self.native.run_clang_tidy.replace("\\", "/"),
        }
        arguments.extend(f"-D{name}={('ON' if value else 'OFF') if isinstance(value, bool) else value}" for name, value in definitions.items())
        return arguments + self.openssl_arguments(self.openssl_root, self.installed)

    def run(self, label: str, command) -> None:
        note(f"{label}: {display(command)}")
        if self.options.dry_run:
            return
        started = time.monotonic()
        result = run_process(command, cwd=self.root, env=self.env)
        if result.returncode:
            raise BuildError(f"{label} failed with exit code {result.returncode}.", result.returncode)
        note(f"{label} completed successfully in {time.monotonic() - started:.2f} seconds.")

    def configure(self) -> None:
        section("Configure the C++23/C ABI runtime")
        if not self.options.dry_run:
            self.dependencies.mkdir(parents=True, exist_ok=True)
            (self.build_root / "deps/common/downloads/fetchcontent").mkdir(parents=True, exist_ok=True)
        self.run("CMake configure", [self.native.cmake, *self.configure_arguments()])

    def build(self) -> None:
        section("Build the C++23/C ABI runtime")
        self.run("CMake build", [self.native.cmake, *self.build_arguments(self.directory)])
        if not self.options.dry_run and self.runtime:
            if not self.runtime.is_file():
                raise BuildError(f"The shared runtime was not produced at the expected path: {self.runtime}")
            note(f"Shared runtime verified: {self.runtime} ({self.runtime.stat().st_size} bytes)")

    def test(self) -> None:
        section("Test C++23, C ABI, and Legacy consumers")
        ctest = executable(Path(self.native.cmake).with_name("ctest.exe" if self.native.platform == "windows" else "ctest"), "ctest", required=True)
        arguments = [ctest, "--test-dir", str(self.directory), "--output-on-failure", "--no-tests=error", "--parallel", str(self.jobs)]
        if self.native.multi_config:
            arguments += ["-C", self.configuration]
        self.run("CTest", arguments)
        if self.options.profile == "check":
            section("Check the C++ source")
            targets = ["verdandi_cpp_format_check"]
            if self.native.platform == "linux":
                targets.append("verdandi_cpp_clang_tidy")
            for target in targets:
                self.run(target, [self.native.cmake, *self.build_arguments(self.directory, target)])

    def execute(self) -> None:
        self.diagnose()
        stages = ("configure", "build", "test") if self.options.command == "all" else (self.options.command,)
        for stage in stages:
            if stage != "doctor":
                getattr(self, stage)()
        section("Dry-run complete" if self.options.dry_run else "Completed")
        note(
            "Surface-level plan completed; compilation probes were skipped and no files were written."
            if self.options.dry_run
            else f"Verdandi {self.options.command} completed successfully."
        )


def main(arguments=None) -> int:
    options = parse_options(arguments)
    # 仅给子进程覆盖诊断语言，保留 SystemRoot、PATH 和调用者其他必需环境。
    env = dict(os.environ)
    env.update({"VSLANG": "1033"} if os.name == "nt" else {"LC_ALL": "C", "LANG": "C"})
    try:
        section("Toolchain diagnostics")
        Build(options, resolve_toolchain(options, env), env).execute()
        return 0
    except KeyboardInterrupt:
        print("[verdandi] Interrupted; the active command's process tree was stopped.", file=sys.stderr)
        return 130
    except (BuildError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"[verdandi] error: {error}", file=sys.stderr)
        code = getattr(error, "returncode", 1)
        return 128 - code if code < 0 else code


if __name__ == "__main__":
    raise SystemExit(main())
