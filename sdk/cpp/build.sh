#!/usr/bin/env bash

# Verdandi C++23 核心、C ABI 与 Legacy facade 的 Linux x64 开发构建入口。
#
# 设计边界：
#   1. 只探测已经存在的编译器、CMake、.NET、Ninja/Make 和 vcpkg，绝不运行
#      apt、dnf、pacman 或其他工具链安装器。
#   2. C++ 依赖优先复用兼容的系统包；Boost、yyjson、SQLite 缺失时可由
#      FetchContent 获取锁定版本。OpenSSL 不由 Verdandi 自行编译，系统中找不到
#      OpenSSL 3.0+ 时只使用已经安装的 vcpkg，否则给出明确安装提示。
#   3. 所有统一构建输出进入仓库根目录 build/，并按平台、架构、编译器、生成器、
#      OpenSSL 来源、Profile 和链接方式隔离，避免 Debug/Release、静态/共享或不同
#      工具链之间复用不安全的二进制对象。
#   4. C ABI 和 C++11/14/17 Legacy 是同一个 C++23 核心的消费边界，不单独构建
#      第二套运行时；C# 等使用方语言只需加载 shared 产物，由各自工具链编译。
#
# 常用示例：
#   bash sdk/cpp/build.sh doctor
#   bash sdk/cpp/build.sh all --profile check --linkage static
#   bash sdk/cpp/build.sh all --profile release --linkage shared
#   bash sdk/cpp/build.sh configure --deps system --offline

set -Eeuo pipefail

# 构建日志是跨发行版、CI 和缺陷检索的协议面。C locale 强制 GCC/Clang/CMake 的
# 诊断使用英文；脚本作为独立进程运行，不会改变父 shell。
export LANG=C
export LC_ALL=C

# 被 source 时脚本为 vcpkg 设置的环境变量会泄漏到调用者。统一入口必须作为独立
# 进程运行，因此明确拒绝 source。
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' '[verdandi] error: Execute bash sdk/cpp/build.sh; do not source this script.' >&2
    return 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd -P)"
BUILD_ROOT="$REPOSITORY_ROOT/build"
CPP_ROOT="$SCRIPT_DIR"
PROBE_ROOT="$CPP_ROOT/cmake/probe"

COMMAND=all
PROFILE=dev
LINKAGE=auto
DEPENDENCIES=auto
GENERATOR=auto
COMPILER=auto
VCPKG_ROOT_ARGUMENT=
JOBS=0
OFFLINE=0
DRY_RUN=0

# 输出与 PowerShell 入口等价的英文命令帮助。函数不读取环境、不探测工具，调用方
# 可在缺失任何编译器的机器上安全查看参数和阶段语义。
usage() {
    # Help 保持参数语义与 build.ps1 对齐，但采用 Bash 惯例的长选项形式。
    printf '%s\n' \
        'Usage: bash sdk/cpp/build.sh [doctor|configure|build|test|all] [options]' \
        '' \
        'Options:' \
        '  --profile dev|check|release        Build/check profile; default: dev' \
        '  --linkage auto|static|shared       C++ linkage; default: auto' \
        '  --deps auto|system|managed         C++ dependency policy; default: auto' \
        '  --generator auto|ninja|make        CMake generator; default: auto' \
        '  --compiler auto|gcc|clang          Linux C/C++ compiler; default: auto' \
        '  --vcpkg-root PATH                  Existing vcpkg root' \
        '  --jobs N                           Parallel jobs; 0 uses logical CPUs' \
        '  --offline                          Forbid dependency downloads' \
        '  --dry-run                          Print the plan without writing build/' \
        '  -h, --help                         Show this help' \
        '' \
        'Commands:' \
        '  doctor     Detect tools and compile minimal C++23/OpenSSL probes' \
        '  configure  Restore dependencies and generate the CMake build tree' \
        '  build      Compile an existing configured build tree' \
        '  test       Run tests from an existing built tree' \
        '  all        Run configure, build, and test in sequence' \
        '' \
        'Profiles:' \
        '  dev        Debug build for normal iteration' \
        '  check      Release build with mandatory format and static-analysis gates' \
        '  release    Release build and tests without packaging or publication' \
        '' \
        'Dependency policies:' \
        '  auto       Prefer compatible system packages and use locked fallbacks' \
        '  system     Require system packages and forbid dependency downloads' \
        '  managed    Require existing vcpkg for OpenSSL and use locked fallback sources' \
        '' \
        'Behavior:' \
        '  Toolchains and vcpkg are detected but never installed by this script.' \
        '  Auto linkage is static; use shared to produce a runtime for dynamic consumers.' \
        '  C# and other language projects compile independently and only load the shared runtime.' \
        '  Offline mode accepts only existing verified caches and never falls back to the network.' \
        '  All generated files remain below the repository-level build/ directory.' \
        '  Go, Rust, and C# are not compiled by this native entry point.' \
        '' \
        'Examples:' \
        '  bash sdk/cpp/build.sh doctor' \
        '  bash sdk/cpp/build.sh all --profile check --linkage static' \
        '  bash sdk/cpp/build.sh all --profile release --linkage shared --offline'
}

# 输出稳定英文错误并终止整个脚本。参数应是不含凭据的最终诊断；退出码固定为 1，
# 外部工具的具体非零码会作为文本保留供日志分析。
die() {
    printf '[verdandi] error: %s\n' "$*" >&2
    exit 1
}

# 输出脚本拥有的普通英文进度信息，不参与控制流，也不写持久状态。
note() {
    printf '[verdandi] %s\n' "$*"
}

# 向标准错误输出可继续执行的英文警告；必需能力缺失必须使用 die 而不是降级。
warn() {
    printf '[verdandi] warning: %s\n' "$*" >&2
}

# 输出一个稳定英文阶段标题，便于本地和 CI 日志按生命周期分段。
section() {
    printf '\n== %s ==\n' "$*"
}

# 检查第一个值是否等于后续任一候选项。匹配返回 0，否则返回 1；只用于参数白名单，
# 不执行正则或模糊匹配，防止拼写错误被静默接受。
is_one_of() {
    local value=$1
    shift
    local candidate
    for candidate in "$@"; do
        [[ "$value" == "$candidate" ]] && return 0
    done
    return 1
}

# 第一项非选项参数是阶段名；其余参数使用显式值，未知参数一律失败，避免拼写错误
# 被静默忽略后构建出与调用者预期不同的产物。
if (($# > 0)) && [[ "$1" != -* ]]; then
    COMMAND=$1
    shift
fi
while (($# > 0)); do
    case "$1" in
        --profile)
            (($# >= 2)) || die '--profile requires a value'
            PROFILE=$2
            shift 2
            ;;
        --linkage)
            (($# >= 2)) || die '--linkage requires a value'
            LINKAGE=$2
            shift 2
            ;;
        --deps)
            (($# >= 2)) || die '--deps requires a value'
            DEPENDENCIES=$2
            shift 2
            ;;
        --generator)
            (($# >= 2)) || die '--generator requires a value'
            GENERATOR=$2
            shift 2
            ;;
        --compiler)
            (($# >= 2)) || die '--compiler requires a value'
            COMPILER=$2
            shift 2
            ;;
        --vcpkg-root)
            (($# >= 2)) || die '--vcpkg-root requires a value'
            VCPKG_ROOT_ARGUMENT=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || die '--jobs requires a value'
            JOBS=$2
            shift 2
            ;;
        --offline)
            OFFLINE=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unknown option: $1"
            ;;
    esac
done

is_one_of "$COMMAND" doctor configure build test all || die "Unknown command: $COMMAND"
is_one_of "$PROFILE" dev check release || die "Unknown profile: $PROFILE"
is_one_of "$LINKAGE" auto static shared || die "Unknown linkage: $LINKAGE"
is_one_of "$DEPENDENCIES" auto system managed || die "Unknown dependency policy: $DEPENDENCIES"
is_one_of "$GENERATOR" auto ninja make || die "Unknown CMake generator: $GENERATOR"
is_one_of "$COMPILER" auto gcc clang || die "Unknown compiler: $COMPILER"
[[ "$JOBS" =~ ^[0-9]+$ ]] || die '--jobs must be an integer from 0 through 256'
((JOBS >= 0 && JOBS <= 256)) || die '--jobs must be in the range 0..256'

[[ "$(uname -s)" == Linux ]] || die 'sdk/cpp/build.sh supports Linux only. Use sdk/cpp/build.ps1 on Windows.'
case "$(uname -m)" in
    x86_64|amd64) ;;
    *) die "Verdandi currently supports Linux x64 only; detected architecture: $(uname -m)" ;;
esac

if ((JOBS == 0)); then
    if command -v nproc >/dev/null 2>&1; then
        JOBS=$(nproc)
    else
        JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
    fi
    ((JOBS > 0)) || JOBS=1
fi

# auto 保留 CMake 的静态开发基线；需要 SO 的 C ABI、Legacy 或 C# 使用者显式选择
# shared。原生脚本只生成运行库，不探测或编译使用方语言工程。
RESOLVED_LINKAGE=$LINKAGE
[[ "$RESOLVED_LINKAGE" == auto ]] && RESOLVED_LINKAGE=static

if [[ "$PROFILE" == dev ]]; then
    NATIVE_CONFIGURATION=Debug
else
    # check/release 使用 Release 以覆盖优化期编译诊断并生成正式原生产物。
    NATIVE_CONFIGURATION=Release
fi

# 按参数顺序使用 shell 命令解析/PATH 查找可执行程序。成功输出首个路径并返回 0，
# 全部缺失返回 1；不启动候选程序，也不修改 PATH。
find_program() {
    local name
    for name in "$@"; do
        if command -v "$name" >/dev/null 2>&1; then
            command -v "$name"
            return 0
        fi
    done
    return 1
}

# 为 find_program 增加必需语义。第一个参数是英文用途，其余参数是候选命令名；
# 成功输出路径，失败通过 die 明确声明脚本不会安装工具链。
require_program() {
    local purpose=$1
    shift
    local path
    path=$(find_program "$@") || die "Missing $purpose (searched for: $*). This script never installs toolchains; install it, ensure it is on PATH, and retry."
    printf '%s' "$path"
}

# 返回多行文本的第一行供环境清单使用。完整输出仍由调用命令拥有，本函数不用于
# 版本比较或错误判断，避免摘要截断改变验证结果。
first_line() {
    # 版本输出可能包含多行说明；环境清单只保留第一行，完整失败输出仍由具体命令显示。
    printf '%s\n' "$1" | sed -n '1p'
}

# 从任意工具版本文本中提取首个 major.minor[.patch] 数值。缺失 patch 补 0；无法
# 解析直接失败，不依赖 lexicographic 字符串顺序。
extract_version() {
    local text=$1
    local version
    version=$(printf '%s\n' "$text" | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n 1 || true)
    [[ -n "$version" ]] || die "Could not parse a tool version from: $text"
    [[ "$version" == *.*.* ]] || version="$version.0"
    printf '%s' "$version"
}

# 对 actual 与 minimum 的三段整数版本逐段比较。满足返回 0，过低返回 1；输入必须
# 已由 extract_version 规范化，函数不输出诊断。
version_at_least() {
    local actual=$1
    local minimum=$2
    local a_major=0 a_minor=0 a_patch=0 m_major=0 m_minor=0 m_patch=0
    IFS=. read -r a_major a_minor a_patch <<<"$actual"
    IFS=. read -r m_major m_minor m_patch <<<"$minimum"
    ((a_major > m_major)) && return 0
    ((a_major < m_major)) && return 1
    ((a_minor > m_minor)) && return 0
    ((a_minor < m_minor)) && return 1
    ((a_patch >= m_patch))
}

# 验证指定工具输出达到最低版本。name 进入英文错误，output 是原始版本文本，
# minimum 是三段下限；成功输出规范化实际版本，失败终止且不尝试升级。
confirm_minimum() {
    local name=$1
    local output=$2
    local minimum=$3
    local actual
    actual=$(extract_version "$output")
    version_at_least "$actual" "$minimum" || die "$name $actual is too old; version $minimum or newer is required"
    printf '%s' "$actual"
}

# 以 Bash %q 逐参数打印即将执行的命令。首参是英文 Label，仅供显示；其余参数保持
# 原始边界，所以带空格路径可审阅但不会被日志字符串二次执行。
print_command() {
    local argument
    printf '[verdandi] %s:' "$1"
    shift
    for argument in "$@"; do
        printf ' %q' "$argument"
    done
    printf '\n'
}

# 在指定目录同步执行外部命令。label 驱动英文开始/完成/失败消息，directory 不泄漏
# 到父 shell；DryRun 只显示。函数保留退出码并报告整数秒耗时。
run_in() {
    local label=$1
    local directory=$2
    shift 2
    print_command "$label" "$@"
    ((DRY_RUN)) && return 0
    local started=$SECONDS
    if (
        cd -- "$directory"
        "$@"
    ); then
        note "$label completed successfully in $((SECONDS - started)) seconds."
    else
        local status=$?
        die "$label failed with exit code $status."
    fi
}

# 把工具版本标签规范为可移植的构建路径片段：转小写并只保留安全 ASCII 字符。
# 输入只用于目录命名，输出不会用于命令选择或安全判定。
sanitize_component() {
    # 路径标签只保留常见安全字符，避免厂商版本文本中的空格或括号制造不同平台的
    # 路径解析差异。
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9._-]+/-/g; s/^-+//; s/-+$//'
}

# 解析 CMake、GCC/Clang、Ninja/Make 及 check 工具并验证最低能力。成功填充调用级
# 全局上下文；缺失必需工具立即失败，不安装软件也不永久修改用户环境。
resolve_native_toolchain() {
    CMAKE=$(require_program 'CMake 3.28 or newer' cmake)
    CMAKE_VERSION_TEXT=$($CMAKE --version)
    CMAKE_VERSION=$(confirm_minimum CMake "$CMAKE_VERSION_TEXT" 3.28.0)

    # auto 尊重明确的 CC/CXX；否则优先项目已资格验证的 GCC，再回退到 Clang。
    if [[ "$COMPILER" == auto && -n "${CXX:-}" ]]; then
        CXX_PATH=$(require_program 'the C++23 compiler selected by CXX' "$CXX")
        if [[ -n "${CC:-}" ]]; then
            CC_PATH=$(require_program 'the C compiler selected by CC' "$CC")
        else
            # CXX 使用带版本后缀的编译器时必须推导同版本 C 编译器，不能把 g++-14
            # 与 PATH 中的 gcc-13 配成一组。自定义 wrapper 无法可靠推导，要求显式 CC。
            local cxx_name c_name sibling
            cxx_name=$(basename -- "$CXX_PATH")
            case "$cxx_name" in
                clang++*) c_name=${cxx_name/clang++/clang} ;;
                g++*) c_name=${cxx_name/g++/gcc} ;;
                c++) c_name=cc ;;
                *) die "CXX resolves to '$CXX_PATH', but its matching C compiler cannot be inferred. Set CC explicitly and retry." ;;
            esac
            sibling="$(dirname -- "$CXX_PATH")/$c_name"
            if [[ -x "$sibling" ]]; then
                CC_PATH=$sibling
            else
                CC_PATH=$(require_program "the C compiler matching $CXX_PATH" "$c_name")
            fi
        fi
    elif [[ "$COMPILER" == gcc ]] || { [[ "$COMPILER" == auto ]] && command -v g++ >/dev/null 2>&1; }; then
        CXX_PATH=$(require_program 'the G++ C++23 compiler' g++)
        CC_PATH=$(require_program 'the GCC C compiler' gcc)
    else
        CXX_PATH=$(require_program 'the Clang++ C++23 compiler' clang++)
        CC_PATH=$(require_program 'the Clang C compiler' clang)
    fi

    CXX_VERSION_TEXT=$($CXX_PATH --version)
    CXX_VERSION=$(extract_version "$CXX_VERSION_TEXT")
    local compiler_base
    compiler_base=$(basename -- "$CXX_PATH")
    if [[ "$compiler_base" == *clang* ]]; then
        COMPILER_LABEL="clang-${CXX_VERSION%%.*}"
    else
        # GCC 的主版本定义 C++ 标准库 ABI/能力边界；完整补丁版本仍写入环境清单。
        COMPILER_LABEL="gcc-${CXX_VERSION%%.*}"
    fi
    COMPILER_LABEL=$(sanitize_component "$COMPILER_LABEL")

    NINJA=
    if [[ "$GENERATOR" == ninja ]] || { [[ "$GENERATOR" == auto ]] && command -v ninja >/dev/null 2>&1; }; then
        NINJA=$(require_program Ninja ninja)
        CMAKE_GENERATOR=Ninja
        GENERATOR_SLUG=ninja
        CMAKE_GENERATOR_ARGS=("-DCMAKE_MAKE_PROGRAM=$NINJA")
    else
        MAKE=$(require_program 'GNU Make (fallback when Ninja is unavailable)' make gmake)
        CMAKE_GENERATOR='Unix Makefiles'
        GENERATOR_SLUG=make
        CMAKE_GENERATOR_ARGS=("-DCMAKE_MAKE_PROGRAM=$MAKE")
    fi

    CLANG_FORMAT=$(find_program \
        clang-format clang-format-22 clang-format-21 clang-format-20 clang-format-19 clang-format-18 || true)
    RUN_CLANG_TIDY=$(find_program \
        run-clang-tidy run-clang-tidy-22 run-clang-tidy-21 run-clang-tidy-20 run-clang-tidy-19 run-clang-tidy-18 || true)
    if [[ "$PROFILE" == check ]]; then
        [[ -n "$CLANG_FORMAT" ]] || die 'The check profile requires clang-format, but it was not found.'
        [[ -n "$RUN_CLANG_TIDY" ]] || die 'The Linux check profile requires run-clang-tidy, but it was not found.'
    fi
}

# 按显式参数、环境、PATH 与少量标准目录查找现有 vcpkg。候选必须是 Linux 原生程序
# 且通过 version 查询；成功填充 root/executable/toolchain/version，未找到返回 1。
resolve_vcpkg() {
    VCPKG_ROOT_RESOLVED=
    VCPKG_EXECUTABLE=
    VCPKG_TOOLCHAIN=
    VCPKG_VERSION_TEXT=
    local -a candidates=()
    local explicit_root=
    if [[ -n "$VCPKG_ROOT_ARGUMENT" ]]; then
        # 显式参数是调用者的确定选择。无效路径必须立即失败，不能静默换用 PATH 或
        # 标准目录中的另一份 vcpkg，否则依赖来源将偏离可复现配置。
        local explicit_candidate=$VCPKG_ROOT_ARGUMENT
        [[ -f "$explicit_candidate" ]] && explicit_candidate=$(dirname -- "$explicit_candidate")
        [[ -d "$explicit_candidate" ]] || die "The explicit vcpkg root does not exist or is not a directory: $VCPKG_ROOT_ARGUMENT"
        explicit_root=$(cd -- "$explicit_candidate" && pwd -P)
        candidates+=("$explicit_root")
    fi
    [[ -n "${VCPKG_ROOT:-}" ]] && candidates+=("$VCPKG_ROOT")

    if [[ -n "${CMAKE_TOOLCHAIN_FILE:-}" && "$(basename -- "$CMAKE_TOOLCHAIN_FILE")" == vcpkg.cmake ]]; then
        candidates+=("$(cd -- "$(dirname -- "$CMAKE_TOOLCHAIN_FILE")/../.." 2>/dev/null && pwd -P || true)")
    fi
    local from_path
    from_path=$(find_program vcpkg || true)
    [[ -n "$from_path" ]] && candidates+=("$(dirname -- "$from_path")")
    candidates+=("$HOME/vcpkg" /opt/vcpkg /usr/local/vcpkg /usr/local/share/vcpkg)

    local candidate executable toolchain version_output resolved
    for candidate in "${candidates[@]}"; do
        [[ -n "$candidate" ]] || continue
        if [[ -f "$candidate" ]]; then
            candidate=$(dirname -- "$candidate")
        fi
        [[ -d "$candidate" ]] || continue
        resolved=$(cd -- "$candidate" && pwd -P)
        executable="$candidate/vcpkg"
        toolchain="$candidate/scripts/buildsystems/vcpkg.cmake"
        if [[ -x "$executable" && -f "$toolchain" ]]; then
            # Linux 只接受原生可执行的 vcpkg，不回退到 vcpkg.exe。版本查询同时发现
            # 损坏安装、错误架构和动态加载失败；坏的自动候选可跳过，显式候选失败。
            if ! version_output=$("$executable" version 2>&1); then
                if [[ -n "$explicit_root" && "$resolved" == "$explicit_root" ]]; then
                    die "The explicit vcpkg installation could not run '$executable version': $version_output"
                fi
                continue
            fi
            VCPKG_ROOT_RESOLVED=$resolved
            VCPKG_EXECUTABLE="$VCPKG_ROOT_RESOLVED/vcpkg"
            VCPKG_TOOLCHAIN="$VCPKG_ROOT_RESOLVED/scripts/buildsystems/vcpkg.cmake"
            VCPKG_VERSION_TEXT=$(first_line "$version_output")
            return 0
        fi
        if [[ -n "$explicit_root" && "$resolved" == "$explicit_root" ]]; then
            die "The explicit vcpkg root is incomplete. Expected an executable vcpkg and scripts/buildsystems/vcpkg.cmake below: $explicit_root"
        fi
    done
    if [[ -n "$explicit_root" ]]; then
        die "The explicit vcpkg root is incomplete. Expected an executable vcpkg and scripts/buildsystems/vcpkg.cmake below: $explicit_root"
    fi
    return 1
}

# 配置并编译隔离的最小 CMake 探针。with_openssl 控制是否验证 OpenSSL，name 决定
# 已知日志目录；原始输出写入 probe.log，成功返回 0，失败返回非零供上层分类。
cmake_probe() {
    local with_openssl=$1
    local name=$2
    local probe_directory="$BUILD_ROOT/probes/linux/x64/$COMPILER_LABEL/$GENERATOR_SLUG/$name"
    local log_path="$probe_directory/probe.log"
    local -a configure_args=(
        --fresh
        -S "$PROBE_ROOT"
        -B "$probe_directory"
        -G "$CMAKE_GENERATOR"
        "${CMAKE_GENERATOR_ARGS[@]}"
        "-DCMAKE_C_COMPILER=$CC_PATH"
        "-DCMAKE_CXX_COMPILER=$CXX_PATH"
        "-DVERDANDI_PROBE_OPENSSL=$([[ "$with_openssl" == 1 ]] && printf ON || printf OFF)"
        -DCMAKE_TOOLCHAIN_FILE=
        -DCMAKE_BUILD_TYPE=Release
    )
    local -a build_args=(--build "$probe_directory" --parallel "$JOBS")

    if ((DRY_RUN)); then
        note "Skipping the $name compilation probe in dry-run mode."
        return 0
    fi

    mkdir -p -- "$probe_directory"
    {
        print_command "$name configure" "$CMAKE" "${configure_args[@]}"
        "$CMAKE" "${configure_args[@]}"
        print_command "$name build" "$CMAKE" "${build_args[@]}"
        "$CMAKE" "${build_args[@]}"
    } >"$log_path" 2>&1
}

# 在探针失败时把固定路径及末尾 30 行写到标准错误。缺少日志时安静返回；函数只读
# 构建产物，不覆盖真正退出状态。
show_probe_log() {
    local name=$1
    local path="$BUILD_ROOT/probes/linux/x64/$COMPILER_LABEL/$GENERATOR_SLUG/$name/probe.log"
    [[ -f "$path" ]] || return 0
    warn "Probe log: $path"
    tail -n 30 -- "$path" >&2
}

# 先证明 C++23 编译能力，再按依赖策略选择 system/vcpkg OpenSSL。成功填充稳定的
# OPENSSL_PROVIDER；system 必须通过真实链接探针，vcpkg 必须预先存在。
resolve_openssl_provider() {
    resolve_vcpkg || true

    if ! cmake_probe 0 cpp23; then
        show_probe_log cpp23
        die 'The C++23 compile/link probe failed. Check the compiler, standard library, and CMake generator.'
    fi
    ((DRY_RUN)) || note 'The C++23 compile/link probe passed.'

    if [[ "$DEPENDENCIES" == managed ]]; then
        [[ -n "$VCPKG_ROOT_RESOLVED" ]] || die 'Managed dependency mode requires vcpkg. Install it and set VCPKG_ROOT or --vcpkg-root.'
        OPENSSL_PROVIDER=vcpkg
        return
    fi

    if ((DRY_RUN)); then
        if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
            note 'Dry-run selected system OpenSSL from OPENSSL_ROOT_DIR; doctor performs the actual link probe.'
            OPENSSL_PROVIDER=system
        elif [[ -n "$VCPKG_ROOT_RESOLVED" && "$DEPENDENCIES" == auto ]]; then
            note 'Dry-run skipped the system OpenSSL probe and conservatively selected the available vcpkg fallback.'
            OPENSSL_PROVIDER=vcpkg
        else
            note 'Dry-run plans to use system OpenSSL; run doctor without --dry-run to perform the required compile/link probe.'
            OPENSSL_PROVIDER=system
        fi
        return
    fi

    if cmake_probe 1 openssl-system; then
        note 'The system OpenSSL 3.0+ compile/link probe passed.'
        OPENSSL_PROVIDER=system
        return
    fi
    if [[ "$DEPENDENCIES" == system ]]; then
        show_probe_log openssl-system
        die 'System dependency mode could not compile and link OpenSSL 3.0+. Install its development package or set OPENSSL_ROOT_DIR/CMAKE_PREFIX_PATH.'
    fi
    if [[ -n "$VCPKG_ROOT_RESOLVED" ]]; then
        note "System OpenSSL is unavailable; using the existing vcpkg installation: $VCPKG_ROOT_RESOLVED"
        OPENSSL_PROVIDER=vcpkg
        return
    fi

    die 'No usable OpenSSL 3.0+ or vcpkg installation was found. Install the distribution OpenSSL development package, or install vcpkg and set VCPKG_ROOT.'
}

# 完成本次调用的全部只读诊断，计算 ABI 隔离路径，并设置仅对子进程有效的 vcpkg
# 环境。任何缺失在依赖下载前失败，后续阶段可直接消费已解析上下文。
initialize_context() {
    section 'Toolchain diagnostics'

    CMAKE=
    CMAKE_VERSION=
    CC_VERSION=
    CXX_PATH=
    CXX_VERSION=
    COMPILER_LABEL=
    CMAKE_GENERATOR=
    GENERATOR_SLUG=
    NINJA=
    MAKE=
    CLANG_FORMAT=
    RUN_CLANG_TIDY=
    VCPKG_ROOT_RESOLVED=
    VCPKG_VERSION_TEXT=
    # 本入口只拥有 C++23 核心、C ABI 和 Legacy 原生边界，因此一次性执行真实
    # C++23/OpenSSL 探针；使用方语言编译器不属于此处依赖。
    OPENSSL_PROVIDER=unresolved
    resolve_native_toolchain
    CC_VERSION=$(extract_version "$($CC_PATH --version)")
    resolve_openssl_provider
    # dependency policy 会改变 Boost/SQLite/yyjson 的 system/fetched 来源，即使
    # OpenSSL provider 相同也必须使用独立 CMake cache，防止重配顺序影响结果。
    local native_path="linux/x64/$COMPILER_LABEL/$GENERATOR_SLUG/$OPENSSL_PROVIDER/$DEPENDENCIES/$PROFILE-$RESOLVED_LINKAGE"
    CPP_BUILD_DIRECTORY="$BUILD_ROOT/cpp/$native_path"
    CPP_DEPENDENCY_DIRECTORY="$BUILD_ROOT/deps/$native_path"

    if [[ "$RESOLVED_LINKAGE" == shared ]]; then
        NATIVE_RUNTIME_PATH="$CPP_BUILD_DIRECTORY/libverdandi_cpp.so"
    else
        NATIVE_RUNTIME_PATH=
    fi

    note "Platform: linux/x64; command: $COMMAND; profile: $PROFILE; jobs: $JOBS"
    note "Dependency policy: $DEPENDENCIES; offline mode: $([[ "$OFFLINE" == 1 ]] && printf true || printf false)"
    note "CMake: $CMAKE (version $CMAKE_VERSION)"
    note "C compiler: $CC_PATH (version $CC_VERSION)"
    note "C++ compiler: $CXX_PATH (version $CXX_VERSION)"
    note "CMake generator: $CMAKE_GENERATOR; linkage: $RESOLVED_LINKAGE"
    note "OpenSSL provider: $OPENSSL_PROVIDER (minimum supported version 3.0.0)"
    if [[ -n "$VCPKG_ROOT_RESOLVED" ]]; then
        note "vcpkg: $VCPKG_EXECUTABLE ($VCPKG_VERSION_TEXT)"
    fi
    note "C++ build directory: $CPP_BUILD_DIRECTORY"
    note "C++ dependency directory: $CPP_DEPENDENCY_DIRECTORY"
    if [[ -n "$NATIVE_RUNTIME_PATH" ]]; then
        note "Shared runtime output: $NATIVE_RUNTIME_PATH"
        note 'Copy this SO beside the consuming executable or into its runtimes/linux-x64/native directory.'
    fi
}

# 对一个 Bash 字符串执行 JSON 必需转义并输出结果，不添加引号。调用方保证输入不含
# NUL（shell 本身无法保存 NUL）；路径、制表符和换行均不会破坏清单结构。
json_escape() {
    local value=$1
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//$'\n'/\\n}
    value=${value//$'\r'/\\r}
    value=${value//$'\t'/\\t}
    printf '%s' "$value"
}

# 在 json_escape 结果外增加 JSON 双引号。该函数只负责字符串标量，不接受原始
# JSON 片段，从而避免工具输出被解释为结构。
json_string() {
    printf '"%s"' "$(json_escape "$1")"
}

# 将已解析平台、工具、依赖策略和路径写入 UTF-8/ASCII 兼容 JSON。DryRun 不写文件；
# 清单不含凭据，只用于复现本次调用，不构成签名或发布资格证明。
write_environment_manifest() {
    ((DRY_RUN)) && return 0
    mkdir -p -- "$BUILD_ROOT"
    local manifest="$BUILD_ROOT/environment.json"
    local timestamp
    timestamp=$(date -u +'%Y-%m-%dT%H:%M:%SZ')

    # 不引入 jq/Python 作为只写少量诊断 JSON 的额外构建依赖；所有字符串先经过
    # 上面的 JSON 转义，路径和工具输出不会破坏清单语法。
    {
        printf '{\n'
        printf '  "schema": "v1",\n'
        printf '  "generated_at_utc": '; json_string "$timestamp"; printf ',\n'
        printf '  "platform": "linux",\n'
        printf '  "architecture": "x64",\n'
        printf '  "command": '; json_string "$COMMAND"; printf ',\n'
        printf '  "profile": '; json_string "$PROFILE"; printf ',\n'
        printf '  "linkage": '; json_string "$RESOLVED_LINKAGE"; printf ',\n'
        printf '  "dependencies": '; json_string "$DEPENDENCIES"; printf ',\n'
        printf '  "offline": %s,\n' "$([[ "$OFFLINE" == 1 ]] && printf true || printf false)"
        printf '  "jobs": %s,\n' "$JOBS"
        printf '  "openssl": {"minimum": "3.0.0", "provider": '; json_string "$OPENSSL_PROVIDER"; printf '},\n'
        printf '  "vcpkg": '
        if [[ -n "$VCPKG_ROOT_RESOLVED" ]]; then
            printf '{"root": '; json_string "$VCPKG_ROOT_RESOLVED"
            printf ', "executable": '; json_string "$VCPKG_EXECUTABLE"
            printf ', "toolchain": '; json_string "$VCPKG_TOOLCHAIN"
            printf ', "version": '; json_string "$VCPKG_VERSION_TEXT"
            printf '},\n'
        else
            printf 'null,\n'
        fi
        printf '  "paths": {\n'
        printf '    "cpp": '; json_string "${CPP_BUILD_DIRECTORY:-}"; printf ',\n'
        printf '    "cpp_dependencies": '; json_string "${CPP_DEPENDENCY_DIRECTORY:-}"; printf ',\n'
        printf '    "native_runtime": '
        if [[ -n "${NATIVE_RUNTIME_PATH:-}" ]]; then json_string "$NATIVE_RUNTIME_PATH"; else printf 'null'; fi
        printf '\n'
        printf '  },\n'
        printf '  "tools": {\n'
        printf '    "cmake": {"path": '; json_string "${CMAKE:-}"; printf ', "version": '; json_string "${CMAKE_VERSION:-}"; printf '},\n'
        printf '    "c": {"path": '; json_string "${CC_PATH:-}"; printf ', "version": '; json_string "${CC_VERSION:-}"; printf '},\n'
        printf '    "cpp": {"path": '; json_string "${CXX_PATH:-}"; printf ', "version": '; json_string "${CXX_VERSION:-}"; printf '}\n'
        printf '  }\n'
        printf '}\n'
    } >"$manifest.$$.tmp"
    # 同目录 rename 保证并发读者只看到上一份或下一份完整 JSON；失败的进程不会
    # 截断已经存在的最近调用清单。
    mv -f -- "$manifest.$$.tmp" "$manifest"
    note "Environment manifest written to: $manifest"
}

# 根据冻结的 CLI/探测结果构造完整 CMake 参数数组并设置 vcpkg 缓存环境。数组保留
# 参数边界；offline 同时禁止 FetchContent 和 vcpkg 下载，避免半离线行为。
cpp_configure_arguments() {
    CPP_CONFIGURE_ARGS=(
        -S "$CPP_ROOT"
        -B "$CPP_BUILD_DIRECTORY"
        -G "$CMAKE_GENERATOR"
        "${CMAKE_GENERATOR_ARGS[@]}"
        "-DCMAKE_C_COMPILER=$CC_PATH"
        "-DCMAKE_CXX_COMPILER=$CXX_PATH"
        "-DCMAKE_BUILD_TYPE=$NATIVE_CONFIGURATION"
        "-DBUILD_SHARED_LIBS=$([[ "$RESOLVED_LINKAGE" == shared ]] && printf ON || printf OFF)"
        # 统一入口始终配置原生测试 target，使 configure/build/test 阶段共享完全
        # 相同的 CMake 形状，不因命令顺序改写 cache 或重新生成工程。
        -DVERDANDI_BUILD_TESTS=ON
        "-DVERDANDI_FETCH_DEPENDENCIES=$([[ "$DEPENDENCIES" == system ]] && printf OFF || printf ON)"
        "-DVERDANDI_USE_MANAGED_DEPENDENCIES=$([[ "$DEPENDENCIES" == managed ]] && printf ON || printf OFF)"
        "-DVERDANDI_OFFLINE_DEPENDENCIES=$([[ "$OFFLINE" == 1 ]] && printf ON || printf OFF)"
        -DVERDANDI_ENABLE_SANITIZERS=OFF
        # FULLY_DISCONNECTED 会连本地缓存包的首次解压也禁止。离线网络边界由经过
        # 摘要复核的本地 URL 与 vcpkg --no-downloads 共同保证。
        -DFETCHCONTENT_FULLY_DISCONNECTED=OFF
        "-DFETCHCONTENT_UPDATES_DISCONNECTED=$([[ "$OFFLINE" == 1 ]] && printf ON || printf OFF)"
        "-DFETCHCONTENT_BASE_DIR=$CPP_DEPENDENCY_DIRECTORY/fetchcontent"
        "-DVERDANDI_DOWNLOAD_CACHE=$BUILD_ROOT/deps/common/downloads/fetchcontent"
        "-DVERDANDI_CLANG_FORMAT=$CLANG_FORMAT"
        "-DVERDANDI_RUN_CLANG_TIDY=$RUN_CLANG_TIDY"
    )

    if [[ "$OPENSSL_PROVIDER" == vcpkg ]]; then
        export VCPKG_DOWNLOADS="$BUILD_ROOT/deps/common/downloads/vcpkg"
        # vcpkg 的 ABI 哈希已经编码编译器与 triplet；同平台/编译器下跨 Verdandi
        # Profile 和链接方式共享二进制包，避免重复编译 OpenSSL。
        export VCPKG_DEFAULT_BINARY_CACHE="$BUILD_ROOT/deps/common/vcpkg-binary-cache/linux/x64/$COMPILER_LABEL"
        CPP_CONFIGURE_ARGS+=(
            "-DCMAKE_TOOLCHAIN_FILE=$VCPKG_TOOLCHAIN"
            -DVCPKG_TARGET_TRIPLET=x64-linux
            -DVCPKG_HOST_TRIPLET=x64-linux
            "-DVCPKG_MANIFEST_DIR=$CPP_ROOT"
            "-DVCPKG_INSTALLED_DIR=$CPP_DEPENDENCY_DIRECTORY/vcpkg_installed"
        )
        # 实验性 buildtrees/packages 重定向会改变包 ABI 并降低缓存命中率，因此保留
        # vcpkg 默认工作目录；严格离线只使用稳定的 --no-downloads 选项。
        if ((OFFLINE)); then
            CPP_CONFIGURE_ARGS+=('-DVCPKG_INSTALL_OPTIONS=--no-downloads')
        else
            # 显式清空，避免复用构建树时继承先前的严格离线选项。
            CPP_CONFIGURE_ARGS+=('-DVCPKG_INSTALL_OPTIONS=')
        fi
    else
        # 防止外部 CMAKE_TOOLCHAIN_FILE 把明确的 system 构建悄悄切换到 vcpkg。
        CPP_CONFIGURE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE=)
    fi
}

# 创建当前依赖目录及公共不可变下载缓存，再执行 CMake configure。DryRun 不创建
# 目录；下载可跨 Profile 复用，解压源码与对象仍保持构建树隔离。
cpp_configure() {
    section 'Configure the C++23/C ABI runtime'
    if ((!DRY_RUN)); then
        mkdir -p -- "$CPP_DEPENDENCY_DIRECTORY"
        # 仅共享带摘要校验的原始下载；解压源码与编译目录继续按构建维度隔离。
        mkdir -p -- "$BUILD_ROOT/deps/common/downloads/fetchcontent"
        if [[ "$OPENSSL_PROVIDER" == vcpkg ]]; then
            mkdir -p -- \
                "$BUILD_ROOT/deps/common/downloads/vcpkg" \
                "$BUILD_ROOT/deps/common/vcpkg-binary-cache/linux/x64/$COMPILER_LABEL"
        fi
    fi
    cpp_configure_arguments
    run_in 'CMake configure' "$REPOSITORY_ROOT" "$CMAKE" "${CPP_CONFIGURE_ARGS[@]}"
}

# 编译已配置的完整 C++23/C ABI/Legacy 原生树；run_in 输出完整英文命令、结果和
# 耗时，并保留失败退出码。
cpp_build() {
    section 'Build the C++23/C ABI runtime'
    local -a arguments=(--build "$CPP_BUILD_DIRECTORY" --parallel "$JOBS")
    run_in 'CMake build' "$REPOSITORY_ROOT" "$CMAKE" "${arguments[@]}"

    # shared 构建必须实际产生前面报告的 SO；立即校验可防止目标名、生成器布局或
    # CMake 选项漂移后仍向下游报告一个不存在的复制路径。
    if ((!DRY_RUN)) && [[ "$RESOLVED_LINKAGE" == shared ]]; then
        [[ -f "$NATIVE_RUNTIME_PATH" ]] || die "The shared runtime was not produced at the expected path: $NATIVE_RUNTIME_PATH"
        note "Shared runtime verified: $NATIVE_RUNTIME_PATH ($(wc -c <"$NATIVE_RUNTIME_PATH") bytes)"
    fi
}

# 运行 CTest；check 额外强制执行 clang-format 与 clang-tidy 目标。
# 未配置 Redis 的 live 用例按项目 skip 契约处理，真实失败传播为脚本失败。
cpp_test() {
    section 'Test C++23, C ABI, and Legacy consumers'
    local ctest
    ctest=$(require_program CTest ctest)
    run_in CTest "$REPOSITORY_ROOT" "$ctest" --test-dir "$CPP_BUILD_DIRECTORY" --output-on-failure --parallel "$JOBS"
    # check 是发布前原生源码门：除运行行为测试外，还必须验证格式与静态分析目标。
    if [[ "$PROFILE" == check ]]; then
        section 'Check the C++ source'
        run_in 'C++ format check' "$REPOSITORY_ROOT" "$CMAKE" --build "$CPP_BUILD_DIRECTORY" --target verdandi_cpp_format_check
        run_in 'C++ clang-tidy' "$REPOSITORY_ROOT" "$CMAKE" --build "$CPP_BUILD_DIRECTORY" --target verdandi_cpp_clang_tidy
    fi
}

# 执行唯一的原生配置阶段；使用方语言独立编译并加载 shared 产物。任一步失败立即
# 终止，不在半配置状态继续。
configure_stage() {
    cpp_configure
    return 0
}

# 构建 C++23 核心、C ABI 和各标准消费测试；不会探测或触碰使用方语言工程。
build_stage() {
    cpp_build
    return 0
}

# 只运行已选择、已构建的测试集合，不隐式 configure/build，使独立 test 子命令的
# 前置条件与失败方式保持可预测。
test_stage() {
    cpp_test
    return 0
}

# 顶层状态机：先完成诊断和环境清单，再严格执行一个请求阶段。所有成功路径打印
# 英文总结；set -Eeuo pipefail 与 run_in 确保未处理错误无法伪装成成功。
main() {
    initialize_context
    write_environment_manifest

    case "$COMMAND" in
        doctor)
            section 'Diagnostics complete'
            if ((DRY_RUN)); then
                note 'Surface-level tool discovery passed. Compilation probes were intentionally skipped in dry-run mode.'
            else
                note 'All required tools passed validation. No dependencies were downloaded and no source files were modified.'
            fi
            ;;
        configure) configure_stage ;;
        build) build_stage ;;
        test) test_stage ;;
        all)
            configure_stage
            build_stage
            test_stage
            ;;
    esac

    if [[ "$COMMAND" != doctor ]]; then
        if ((DRY_RUN)); then
            section 'Dry-run complete'
            note "The Verdandi $COMMAND plan completed successfully. No build commands were executed and no files were written."
        else
            section 'Completed'
            note "Verdandi $COMMAND completed successfully. Environment manifest: $BUILD_ROOT/environment.json"
        fi
    fi
}

main
