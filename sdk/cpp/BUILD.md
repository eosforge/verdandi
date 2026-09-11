# Verdandi Native Build Guide

Verdandi uses one standard-library Python implementation in this directory:

- `build.py` owns dependency policy, probes, cache layout and build/test stages;
- `build_support.py` owns process cleanup and Windows/Linux tool discovery;
- Windows `build.ps1` and Linux `build.sh` preserve convenient platform entry
  points and forward to the same implementation.

These scripts build and test only the C++23 core, C ABI, and C++11/14/17 Legacy
consumers. Go, Rust, and C# use their own language toolchains and are never
compiled by these native scripts. A C# application only needs the shared
`verdandi_cpp` runtime placed in one of its documented native-library search
locations.

Python 3.10 or newer is required. Ordinary builds use only its standard library;
Black is a maintainer formatting tool and is not a build-time package dependency.
Help, progress, diagnostics, warnings, errors, and successful command
summaries are emitted in standard English so local and CI logs are consistent.

## Quick start

Run commands from the repository root. The scripts resolve every source path
from their own location, so another working directory is also safe.
Prepare OpenSSL externally first. Obtain approval for each specific dependency
download before an online configure; use `-Offline`/`--offline` for local caches.

The same command works on Windows and Linux when an existing interpreter is
available as `python` (use `python3` where appropriate):

```text
python -B sdk/cpp/build.py all --profile dev --linkage shared --offline --jobs 1
```

Windows:

```powershell
./sdk/cpp/build.ps1 doctor
./sdk/cpp/build.ps1 all -Profile dev
./sdk/cpp/build.ps1 all -Profile release -Linkage shared
./sdk/cpp/build.ps1 doctor -Python 'C:\path to existing Python\python.exe'
```

Linux:

```bash
bash sdk/cpp/build.sh doctor
bash sdk/cpp/build.sh all --profile dev
bash sdk/cpp/build.sh all --profile release --linkage shared
bash sdk/cpp/build.sh --python /path/to/existing/python3 doctor
```

The Windows wrapper checks existing Python commands and the standard
`~/.local/bin/python.exe` location used by uv, and skips empty Windows Store
placeholders. The Linux wrapper uses `python3` by default. Neither wrapper runs
a Python package manager, installs Python, activates an environment, or changes
the parent terminal. An explicit interpreter selection overrides discovery.

## Commands

### Catalog checks without external SDK dependencies

The standalone test project below requires only an existing C++23 compiler,
its standard library, and CMake. It neither includes the full SDK project nor
declares OpenSSL, Boost, SQLite, yyjson, or FetchContent dependencies. Use it
to check Catalog shapes, byte limits, UTF-8, numeric Array ordering, the
Replace argument ABI, and detached argument ownership before restoring SDK
dependencies:

```text
cmake -S sdk/cpp/tests/offline -B build/catalog-offline -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=
cmake --build build/catalog-offline --config Release
ctest --test-dir build/catalog-offline -C Release --output-on-failure
```

On Windows, use a configured x64 MSVC developer shell; `-G "NMake Makefiles"`
is available when the Visual Studio generator cannot discover an installed
instance. Keep externally supplied package-manager toolchain files disabled
for this dependency-free project.

Add `-DVERDANDI_BUILD_BENCHMARKS=ON` at configure time to build
`verdandi_cpp_catalog_value_benchmark`, which prints JSON timing and allocation
measurements for successful Map and Patch validation. Input construction and
JSON output are outside the measured region. The benchmark replaces the
executable's allocation functions to count requests; it is not a Redis or
end-to-end SDK benchmark.

The same regression source is part of the normal SDK CTest matrix. Passing
the standalone project does not qualify the full runtime, bindings, Redis,
Sentinel, TLS, or checkpoint I/O.

The standalone project also tests the production MessagePack notification
field decoder. It verifies numeric Array Replace order, lexical Map/Patch
order, decimal-width transitions, truncated fields, count limits, and owned
binary values. This directly checks the field reader used by the Subscriber;
it does not execute the complete notification envelope or recovery loop.

### Full native build commands

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

The shared resolver has standard-library offline regressions. Optional shell
checks exercise the real adapters with argument-reporting fixtures; CMake
checks retain the early installation guards and package-boundary regressions.

```text
python sdk/cpp/tests/dependency_policy_test.py --work-dir build/dependency-policy-tests --cmake cmake --powershell powershell
python sdk/cpp/tests/dependency_policy_test.py --work-dir build/dependency-policy-tests --cmake cmake --bash bash
```

Repeat `--powershell` to check both Windows PowerShell 5.1 and PowerShell 7.
The shared tests also cover failure short-circuiting, child environment/cwd,
argument boundaries, dry-run writes, native tool selection and timeout cleanup
of compiler grandchildren. Each run keeps isolated fixtures under `--work-dir`.

These check search order, failure handling, and installation guards; they do
not replace real OpenSSL linking or platform runtime qualification.

- `system`: require every dependency to be visible to CMake and forbid
  FetchContent/vcpkg downloads;
- `auto`: try system OpenSSL, an already installed vcpkg OpenSSL package, then
  the project prebuilt cache. Other libraries prefer system packages and retain
  locked FetchContent source fallbacks;
- `managed`: skip system OpenSSL and try installed vcpkg packages followed by
  the prebuilt cache; use locked source revisions for Boost, SQLite, and yyjson.

Verdandi never installs a compiler, Windows/Linux SDK, CMake, Ninja, Make,
LLVM, or vcpkg. OpenSSL acquisition and compilation are external responsibilities:
even cached sources must never trigger an automatic OpenSSL build. Both CMake
entry points disable `VCPKG_MANIFEST_INSTALL` before loading a toolchain. The
native scripts also disable vcpkg app-local deployment; prepare matching DLLs
beside consuming executables or on their runtime search path externally.

OpenSSL lookup uses these concrete locations:

1. System discovery, including `OPENSSL_ROOT_DIR` and `CMAKE_PREFIX_PATH`.
2. `<vcpkg-root>/installed/x64-windows` or `installed/x64-linux`. Set the
   `VCPKG_INSTALLED_DIR` environment variable to reuse another existing install
   tree, including one prepared externally from the retained `vcpkg.json`.
3. An extracted development package at `build/deps/openssl/windows/x64` or
   `build/deps/openssl/linux/x64`, with `include/openssl` and libraries in the
   package's CMake-discoverable library layout. Other locations can be supplied
   explicitly through `OPENSSL_ROOT_DIR`.
4. If unavailable, report the required platform, development files, and cache
   path. Obtain explicit approval for a specific binary package before download.
5. If no compatible binary package exists, report that OpenSSL must be built
   externally; do not start source compilation or install Perl/NASM/tools.

Each available candidate must pass an actual C++23/OpenSSL compile/link probe
for the selected Debug or Release profile. vcpkg/cache headers and Crypto/SSL
libraries must resolve inside the selected package. A package-manager executable,
source archive, or runtime-only DLL/SO is insufficient. Probes do not execute the
binary, certify package provenance, or qualify runtime ABI/TLS behavior. Linux
packages must match the target libc/distribution. Use a fresh CMake tree when
replacing a package or changing its ABI; the fixed cache path holds one externally
prepared package per platform/architecture.

SQLite's amalgamated C source and yyjson's C sources still compile with the
project; the selected Boost headers/Redis implementation compile in project
translation units. This OpenSSL policy does not require external builds for
those dependencies. Their source downloads still require prior approval.

Every FetchContent source has a pinned revision and checksum. Network operations
have bounded total and inactivity timeouts. The vcpkg manifest remains an input
for external preparation, not an automatic restore instruction.

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

`-Offline`/`--offline` forbids new FetchContent downloads. OpenSSL acquisition is
always external, regardless of this flag.
Fallback dependencies are populated from checksum-verified local archive URLs,
so a new build tree can be configured from the shared cache without network
access. A missing or corrupt archive is a terminal error and never falls back
to its remote URL. vcpkg manifest installation is always disabled. A valid cached archive is
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

- Python 3.10 or newer (standard library only);
- CMake 3.28 or newer;
- an actual C++23 compiler/standard library capable of `std::expected`;
- an already compiled OpenSSL 3.0+ development package;
- network access for the first non-system dependency restore, unless all
  artifacts are already cached.

Windows x64:

- PowerShell when using the `.ps1` compatibility entry;
- Visual Studio with Desktop development with C++ and a Windows SDK;
- `clang-format` when the `check` profile is selected.

Linux x64:

- Bash when using the `.sh` compatibility entry;
- GCC or Clang with C++23 library support;
- Ninja or GNU Make;
- an OpenSSL development package matching the Linux target;
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
Finding vcpkg does not establish OpenSSL availability: its installed triplet
must contain the headers and linkable libraries. No install, binary-cache
restore, or source build is invoked by the native entry points.

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

The Python implementation passes diagnostic language settings only to child
processes. Commands are launched with argument arrays and inherit streaming
output; probe logs stream to files instead of accumulating compilation output
in memory. Failed commands stop the sequence and preserve their nonzero exit
code. Interrupts return 130 and stop the active command's owned process tree.
PowerShell reuses `sdk/run-tool.ps1` for lossless native argument forwarding,
including Windows PowerShell 5.1. Its entry file retains a UTF-8 BOM for 5.1.

## Output and cache layout

Generated content is ignored by Git and remains below the repository-level
`build/` directory:

```text
build/
  cpp/<platform>/x64/<compiler>/<generator>/<openssl>/<dependency-policy>/<profile>-<linkage>/
  deps/<platform>/x64/<compiler>/<generator>/<openssl>/<dependency-policy>/<profile>-<linkage>/
  deps/common/downloads/fetchcontent/
  deps/openssl/<platform>/x64/  # externally prepared development package
  probes/<platform>/x64/<compiler>/<generator>/
  environment.json
```

Downloaded archives are immutable and checksum-verified, so they are shared
across dependency policies, profiles, and linkage modes. Extracted sources and
compiled dependency objects remain isolated per native build tree. The policy
dimension prevents `auto`, `system`, and `managed` from reusing a CMake cache
whose dependency sources were resolved under different rules. vcpkg installed
packages are consumed in place; the scripts do not populate vcpkg downloads,
binary caches, buildtrees, packages, or per-build manifest installations.

`build/environment.json` records the most recent invocation's selected paths,
tool versions (including the actual Python executable), provider, profile,
linkage, and dependency policy. It must not be
treated as a release manifest; formal installation, export, packaging, signing,
and artifact publication remain separate release work.

## Maintaining the Python entry

Use the repository's Black configuration for the three handwritten Python files:

```text
python -m black --config testkit/pyproject.toml sdk/cpp/build.py sdk/cpp/build_support.py sdk/cpp/tests/dependency_policy_test.py
```

The formatter is pinned in `sdk/cpp/requirements-dev.txt`.
The maintainer-approved local Black environment is `build/tools/python-build`;
its download cache is `build/deps/pip` and formatter cache is
`build/cache/black`. These paths are ignored by Git. Python package acquisition
still requires separate approval. No Redis, SSH or cryptography Python package
is required for the build entry or its offline policy tests.
For this local environment, replace `python` in the formatting command with
`build/tools/python-build/Scripts/python.exe` on Windows (or the environment's
`bin/python` on Linux). No environment activation is necessary.

## Existing CMake presets

The existing presets remain supported for focused qualification, sanitizer
runs, and historical test harnesses. They continue to use `sdk/cpp/build/` and
do not share a CMake cache with the unified scripts. The scripts are the normal
developer entry point; presets remain the explicit specialist path.
