# Verdandi Native Build Guide

Verdandi provides one platform entry point in this directory:

- Windows x64: `build.ps1`;
- Linux x64: `build.sh`.

These scripts build and test only the C++23 core, C ABI, and C++11/14/17 Legacy
consumers. Go, Rust, and C# use their own language toolchains and are never
compiled by these native scripts. A C# application only needs the shared
`verdandi_cpp` runtime placed in one of its documented native-library search
locations.

The scripts contain detailed Chinese maintainer comments for the current review
phase. Help, progress, diagnostics, warnings, errors, and successful command
summaries are emitted in standard English so local and CI logs are consistent.

## Quick start

Run commands from the repository root. The scripts resolve every source path
from their own location, so another working directory is also safe.

Windows:

```powershell
./sdk/cpp/build.ps1 doctor
./sdk/cpp/build.ps1 all -Profile dev
./sdk/cpp/build.ps1 all -Profile release -Linkage shared
```

Linux:

```bash
bash sdk/cpp/build.sh doctor
bash sdk/cpp/build.sh all --profile dev
bash sdk/cpp/build.sh all --profile release --linkage shared
```

## Commands

| Command | Contract |
| --- | --- |
| `doctor` | Detect required tools and compile minimal C++23 and OpenSSL probes. It does not restore project dependencies. |
| `configure` | Resolve/fetch native dependencies and generate the CMake build tree. |
| `build` | Compile an already configured tree. It does not configure implicitly. |
| `test` | Run tests from an already built tree. It does not build implicitly. |
| `all` | Run `configure`, `build`, and `test` in order. |

The explicit stage boundaries are useful for offline validation and build
systems that manage dependency restoration separately. For ordinary local
verification, use `all`.

## Shared options

PowerShell uses conventional parameter names such as `-Profile` and
`-Linkage`. Bash uses `--profile` and `--linkage`.

### Profiles

- `dev`: Debug native builds for fast iteration;
- `check`: Release builds plus mandatory formatting/static-analysis checks;
- `release`: Release builds and tests without packaging or publication.

On Windows, `check` requires `clang-format`; Linux additionally requires
`run-clang-tidy`. Missing check tools are errors, not silent skips. Sanitizer
qualification remains available through the existing explicit CMake preset and
is not silently enabled by these profiles.

### Linkage

- `auto`: use the normal static C++ development build;
- `static`: build a static C++ core;
- `shared`: build a shared C++ core.

Choose `shared` to produce `verdandi_cpp.dll` or `libverdandi_cpp.so` for C
ABI, dynamically linked Legacy, C#, or another foreign-language consumer. The
script prints the exact runtime path and records it in
`build/environment.json`.

### C# consumption boundary

C# has no separate PowerShell or Bash build orchestration. Its managed source
is compiled normally by the consuming `dotnet build`, project reference, or
eventual NuGet package. Build the native runtime with `-Linkage shared` or
`--linkage shared`, then make the resulting file available through one of the
C# resolver's supported locations:

1. the exact path in `VERDANDI_NATIVE_LIBRARY`;
2. `runtimes/win-x64/native/verdandi_cpp.dll` or
   `runtimes/linux-x64/native/libverdandi_cpp.so` below the application output
   directory;
3. `verdandi_cpp.dll` or `libverdandi_cpp.so` beside the application
   executable.

Ordinary C# compilation and C#-owned tests remain `dotnet` responsibilities;
copying the native runtime does not require these native scripts to detect or
invoke the .NET SDK.

### Dependency policy

- `system`: require every dependency to be visible to CMake and forbid
  FetchContent/vcpkg downloads;
- `auto`: prefer compatible system packages, use an existing vcpkg for OpenSSL
  when necessary, and use locked FetchContent fallbacks for Boost, SQLite, and
  yyjson;
- `managed`: require an existing vcpkg for OpenSSL and use Verdandi's locked
  FetchContent revisions for Boost, SQLite, and yyjson.

Verdandi never installs a compiler, Windows/Linux SDK, CMake, Ninja, Make,
LLVM, or vcpkg. It also never downloads and builds OpenSSL directly.
When OpenSSL 3.0 or newer is not usable as a system package, `auto` can invoke
an already installed vcpkg using `vcpkg.json`. If neither provider exists, the
script stops with installation guidance.

Every downloaded source has a pinned revision and checksum or a pinned vcpkg
baseline. Network operations have bounded total and inactivity timeouts.

### Generator and compiler

Windows supports the validated MSVC toolchain:

- `auto` and `visual-studio` use the CMake generator matching the newest
  installed Visual Studio instance and do not require a pre-initialized
  developer shell;
- `ninja` requires Ninja and an already initialized MSVC Developer PowerShell.

Linux supports GCC and Clang:

- compiler `auto` honors explicit `CC`/`CXX`, otherwise prefers GCC and falls
  back to Clang;
- generator `auto` prefers Ninja and falls back to GNU Make.

Ninja is therefore optional on both platforms.

### Offline and dry-run

`-Offline`/`--offline` forbids new FetchContent and vcpkg downloads.
Fallback dependencies are populated from checksum-verified local archive URLs,
so a new build tree can be configured from the shared cache without network
access. A missing or corrupt archive is a terminal error and never falls back
to its remote URL. vcpkg receives `--no-downloads`. A valid cached archive is
also preferred online,
which keeps FetchContent metadata stable when developers switch between online
and offline invocations. In online mode only, a corrupt cache entry falls back
to the locked remote URL so FetchContent can replace it.

`-DryRun`/`--dry-run` validates surface-level tool discovery and prints the
planned paths/commands without writing build output. It intentionally skips
compilation probes, so `doctor` without dry-run is the authoritative toolchain
check.

### Parallelism

`-Jobs 0`/`--jobs 0` uses the logical processor count. An explicit value must
be in `1..256`.

## Tool requirements

Common:

- CMake 3.28 or newer;
- an actual C++23 compiler/standard library capable of `std::expected`;
- OpenSSL 3.0 or newer, provided by the system or an existing vcpkg;
- network access for the first non-system dependency restore, unless all
  artifacts are already cached.

Windows x64:

- PowerShell;
- Visual Studio with Desktop development with C++ and a Windows SDK;
- `clang-format` when the `check` profile is selected.

Linux x64:

- Bash;
- GCC or Clang with C++23 library support;
- Ninja or GNU Make;
- an OpenSSL development package, or an existing Linux vcpkg;
- `clang-format` and `run-clang-tidy` when `check` is selected.

macOS and Redis Cluster are intentionally outside the supported build/runtime
matrix.

## vcpkg discovery

An explicit `-VcpkgRoot`/`--vcpkg-root` wins. Otherwise the scripts inspect:

1. `VCPKG_ROOT`;
2. a `CMAKE_TOOLCHAIN_FILE` that points to `vcpkg.cmake`;
3. `vcpkg` on `PATH`;
4. platform-specific bounded common locations;
5. the Visual Studio bundled vcpkg on Windows, as the final fallback.

Windows checks exact common paths on each mounted file-system drive, including
`<drive>:\vcpkg` and `<drive>:\Program Files\vcpkg`; it does not recursively
scan disks. This detects installations such as `D:\Program Files\vcpkg`
without making discovery unbounded.

An explicit path is strict: a missing directory, incomplete layout, or failed
`vcpkg version` query is an immediate error rather than a silent fallback.
Automatically discovered broken candidates are skipped. Linux accepts only a
native executable named `vcpkg`; it never attempts to use a Windows
`vcpkg.exe` exposed through WSL.

## Console and diagnostic contract

Every message owned by the scripts is detailed standard English. This includes
help, selected platform, dependency/offline policy, exact tool paths and
versions, generator and linkage, output/cache paths, complete commands,
per-command results and elapsed time, warnings, terminal errors, and the final
summary. Linux forces the C locale for compiler and CMake diagnostics. Windows
requests the English MSBuild UI language; successful Visual Studio build
chatter is kept quiet because some lower-level tools can otherwise emit
localized text despite that request. The wrapper still reports the complete
command, selected configuration, result, and elapsed time, while failures
retain the underlying tool diagnostics.

The source comments in `build.ps1` and `build.sh` are intentionally detailed
Chinese during the maintainer-review phase. Comments never appear in console
output and do not change the English logging contract.

## Output and cache layout

Generated content is ignored by Git and remains below the repository-level
`build/` directory:

```text
build/
  cpp/<platform>/x64/<compiler>/<generator>/<openssl>/<dependency-policy>/<profile>-<linkage>/
  deps/<platform>/x64/<compiler>/<generator>/<openssl>/<dependency-policy>/<profile>-<linkage>/
  deps/common/downloads/fetchcontent/
  deps/common/downloads/vcpkg/
  deps/common/vcpkg-binary-cache/<platform>/x64/<compiler>/
  probes/<platform>/x64/<compiler>/<generator>/
  environment.json
```

Downloaded archives are immutable and checksum-verified, so they are shared
across dependency policies, profiles, and linkage modes. Extracted sources and
compiled dependency objects remain isolated per native build tree. The policy
dimension prevents `auto`, `system`, and `managed` from reusing a CMake cache
whose dependency sources were resolved under different rules. vcpkg downloads
and binary packages are shared by compatible platform/compiler ABI, while each
CMake tree gets its own manifest installation.

vcpkg's transient `buildtrees` and `packages` directories remain at the vcpkg
installation's stable default location. Experimental redirection changes its
package ABI and defeats cross-profile binary-cache reuse. They are not runtime
or distribution inputs.

`build/environment.json` records the most recent invocation's selected paths,
tool versions, provider, profile, linkage, and dependency policy. It must not be
treated as a release manifest; formal installation, export, packaging, signing,
and artifact publication remain separate release work.

## Existing CMake presets

The existing presets remain supported for focused qualification, sanitizer
runs, and historical test harnesses. They continue to use `sdk/cpp/build/` and
do not share a CMake cache with the unified scripts. The scripts are the normal
developer entry point; presets remain the explicit specialist path.
