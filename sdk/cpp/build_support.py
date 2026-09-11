"""Native tool discovery and process ownership for the standard-library build entry."""

from __future__ import annotations

from dataclasses import dataclass, field
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess


class BuildError(RuntimeError):
    def __init__(self, message: str, returncode: int = 1):
        super().__init__(message)
        self.returncode = returncode


def executable(*names: str | Path, required: bool = False) -> str:
    for name in names:
        if name and (found := shutil.which(str(name))):
            # 保留编译器入口名称：Linux 的 clang++ 等符号链接可能影响驱动语言选择。
            return os.path.abspath(found)
    if required:
        raise BuildError(f"Missing executable (searched for: {', '.join(map(str, names))}). Install it externally; this entry never installs tools.")
    return ""


def stop_process(process: subprocess.Popen) -> None:
    """Stop only the command's owned process tree, including compiler grandchildren."""
    if os.name == "nt":
        taskkill = Path(os.environ["SystemRoot"]) / "System32/taskkill.exe"
        subprocess.run([str(taskkill), "/PID", str(process.pid), "/T", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            # 主进程先退出时，仍清理留在同一独立进程组中的编译器。
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.kill()
    process.wait()


def run_process(command, *, cwd=None, env=None, stdout=None, capture=False, timeout=None) -> subprocess.CompletedProcess:
    arguments = list(map(str, command))
    with subprocess.Popen(
        arguments,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE if capture else stdout,
        stderr=subprocess.STDOUT if capture or stdout is not None else None,
        text=True,
        encoding="utf-8",
        errors="replace",
        start_new_session=os.name != "nt",
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
    ) as process:
        try:
            output, _ = process.communicate(timeout=timeout)
        except (KeyboardInterrupt, subprocess.TimeoutExpired):
            stop_process(process)
            raise
        return subprocess.CompletedProcess(arguments, process.returncode, output)


def query(command, env=None) -> str:
    result = run_process(command, env=env, capture=True, timeout=30)
    if result.returncode:
        raise BuildError(f"Tool query failed with exit code {result.returncode}: {command[0]}\n{result.stdout}", result.returncode)
    return result.stdout.strip()


def version(text: str, minimum: tuple[int, int, int] = (0, 0, 0)) -> str:
    match = re.search(r"(?<!\d)(\d+)\.(\d+)(?:\.(\d+))?", text)
    if not match:
        raise BuildError(f"Could not parse a tool version from: {text}")
    actual = tuple(int(part or 0) for part in match.groups())
    if actual < minimum:
        raise BuildError(f"Tool version {actual} is too old; {minimum} or newer is required. Upgrade it externally.")
    return ".".join(map(str, actual))


@dataclass
class Toolchain:
    platform: str
    cmake: str
    cmake_version: str
    compiler_label: str = ""
    compiler_version: str = ""
    generator_name: str = ""
    generator_slug: str = ""
    generator_arguments: list[str] = field(default_factory=list)
    multi_config: bool = False
    visual_studio: dict | None = None
    clang_format: str = ""
    run_clang_tidy: str = ""
    tools: dict = field(default_factory=dict)


def windows_toolchain(native: Toolchain, options, env) -> None:
    import json

    candidates = [Path(env[root]) / "Microsoft Visual Studio/Installer/vswhere.exe" for root in ("PROGRAMFILES(X86)", "PROGRAMFILES") if env.get(root)]
    vswhere = executable("vswhere.exe", *candidates, required=True)
    installations = json.loads(
        query([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json", "-utf8"], env)
    )
    if not installations:
        raise BuildError("Visual Studio with the MSVC x64 C++ component was not found. Add Desktop development with C++ externally.")
    installation = installations[0]
    root = Path(installation["installationPath"])
    toolset = (root / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt").read_text(encoding="utf-8-sig").strip()
    compiler = root / f"VC/Tools/MSVC/{toolset}/bin/Hostx64/x64/cl.exe"
    if not compiler.is_file():
        raise BuildError(f"The selected Visual Studio installation is missing its declared compiler: {compiler}")
    native.visual_studio = {"path": str(root), "version": installation["installationVersion"]}
    native.compiler_version = toolset
    match = re.match(r"14\.(\d+)", toolset)
    native.compiler_label = f"msvc-19.{match[1]}" if match else "msvc"
    native.tools.update(cpp={"path": str(compiler), "version": toolset}, visual_studio=native.visual_studio)
    if options.generator == "ninja":
        ninja = executable("ninja", root / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe", required=True)
        compiler = executable("cl.exe")
        if not compiler:
            raise BuildError("Ninja with MSVC requires an initialized developer environment. Use Developer PowerShell or --generator visual-studio.")
        native.generator_name, native.generator_slug = "Ninja", "ninja"
        native.generator_arguments = [f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_C_COMPILER={compiler}", f"-DCMAKE_CXX_COMPILER={compiler}"]
        native.tools["ninja"] = {"path": ninja, "version": ""}
    else:
        major = installation["installationVersion"].split(".")[0]
        matches = re.findall(r"Visual Studio\s+(\d+)\s+(\d{4})", query([native.cmake, "--help"], env))
        selected = next((f"Visual Studio {number} {year}" for number, year in matches if number == major), None)
        if not selected:
            raise BuildError(f"CMake {native.cmake_version} has no generator for installed Visual Studio {major}. Upgrade CMake externally.")
        native.generator_name, native.generator_slug, native.multi_config = selected, f"vs{major}", True
        native.generator_arguments = ["-A", "x64", f"-DCMAKE_GENERATOR_INSTANCE={root.as_posix()}"]
    native.clang_format = executable("clang-format", *(f"clang-format-{major}" for major in range(22, 17, -1)), root / "VC/Tools/Llvm/x64/bin/clang-format.exe")


def linux_toolchain(native: Toolchain, options, env) -> None:
    if options.compiler == "auto" and env.get("CXX"):
        cxx = executable(env["CXX"], required=True)
        if env.get("CC"):
            cc = executable(env["CC"], required=True)
        else:
            name = Path(cxx).name
            if name.startswith(("clang++", "g++")):
                c_name = name.replace("clang++", "clang", 1) if name.startswith("clang++") else name.replace("g++", "gcc", 1)
            elif name == "c++":
                c_name = "cc"
            else:
                raise BuildError(f"Cannot infer the C compiler matching CXX={cxx}. Set CC explicitly.")
            cc = executable(Path(cxx).with_name(c_name), c_name, required=True)
    elif options.compiler == "gcc" or (options.compiler == "auto" and executable("g++")):
        cc, cxx = executable("gcc", required=True), executable("g++", required=True)
    else:
        cc, cxx = executable("clang", required=True), executable("clang++", required=True)
    native.compiler_version = version(query([cxx, "--version"], env))
    family = "clang" if "clang" in Path(cxx).name else "gcc"
    native.compiler_label = f"{family}-{native.compiler_version.split('.')[0]}"
    native.tools.update(c={"path": cc, "version": version(query([cc, "--version"], env))}, cpp={"path": cxx, "version": native.compiler_version})
    if options.generator == "ninja" or (options.generator == "auto" and executable("ninja")):
        make = executable("ninja", required=True)
        native.generator_name, native.generator_slug = "Ninja", "ninja"
        native.tools["ninja"] = {"path": make, "version": ""}
    else:
        make = executable("make", "gmake", required=True)
        native.generator_name, native.generator_slug = "Unix Makefiles", "make"
    native.generator_arguments = [f"-DCMAKE_MAKE_PROGRAM={make}", f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"]
    native.clang_format = executable("clang-format", *(f"clang-format-{major}" for major in range(22, 17, -1)))
    native.run_clang_tidy = executable("run-clang-tidy", *(f"run-clang-tidy-{major}" for major in range(22, 17, -1)))


def resolve_toolchain(options, env) -> Toolchain:
    host = platform.system().lower()
    if host not in ("windows", "linux") or platform.machine().lower() not in ("amd64", "x86_64"):
        raise BuildError("Verdandi supports Windows x64 and Linux x64 only.")
    generators, compilers = (
        (("auto", "visual-studio", "ninja"), ("auto", "msvc")) if host == "windows" else (("auto", "ninja", "make"), ("auto", "gcc", "clang"))
    )
    if options.generator not in generators or options.compiler not in compilers:
        raise BuildError(f"Unsupported {host} selection. Generators: {', '.join(generators)}; compilers: {', '.join(compilers)}.")
    cmake = executable("cmake", required=True)
    native = Toolchain(host, cmake, version(query([cmake, "--version"], env), (3, 28, 0)))
    native.tools["cmake"] = {"path": cmake, "version": native.cmake_version}
    (windows_toolchain if host == "windows" else linux_toolchain)(native, options, env)
    if native.clang_format:
        native.tools["clang_format"] = {"path": native.clang_format, "version": query([native.clang_format, "--version"], env).splitlines()[0]}
    if options.profile == "check" and (not native.clang_format or (host == "linux" and not native.run_clang_tidy)):
        raise BuildError("The check profile requires clang-format and, on Linux, run-clang-tidy. Install missing tools externally.")
    return native
