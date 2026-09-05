<#
.SYNOPSIS
Diagnoses, configures, builds, and tests the Verdandi C++ runtime on Windows x64.

.DESCRIPTION
This is the Windows development entry point for the C++23 core, C ABI, and Legacy wrappers.
It detects existing toolchains but never installs them. CMake and vcpkg may retrieve project
dependencies according to the selected dependency policy.

All generated files are written below the repository-level build directory and isolated by
platform, architecture, compiler, generator, OpenSSL provider, profile, and linkage.

.PARAMETER Command
Selects doctor, configure, build, test, or all. The all command runs the latter three stages.

.PARAMETER Profile
dev uses Debug. check uses Release and adds strict formatting/static checks. release uses
Release and runs tests, but does not package or publish artifacts.

.PARAMETER Linkage
Selects auto, static, or shared. auto selects the normal static C++ development build. Use
shared when producing verdandi_cpp.dll for a C ABI, Legacy shared, or C# consumer.

.PARAMETER Dependencies
system forbids dependency downloads. auto prefers compatible system packages and allows
locked fallbacks. managed uses vcpkg for OpenSSL and locked sources for other dependencies.

.PARAMETER Generator
Selects auto, visual-studio, or ninja. auto uses the Visual Studio generator. Ninja requires
an MSVC developer environment in the current terminal.

.PARAMETER Compiler
Selects auto or msvc. Both currently select the validated MSVC toolchain.

.PARAMETER VcpkgRoot
Specifies an existing vcpkg root. Discovery otherwise checks explicit environment settings,
PATH, Visual Studio, and a bounded list of common locations without scanning whole drives.

.PARAMETER Jobs
Sets parallelism from 0 through 256. Zero uses the logical processor count.

.PARAMETER Offline
Forbids FetchContent and vcpkg network downloads. Required content must be cached.

.PARAMETER DryRun
Prints the plan without writing build output or running compilation probes.

.EXAMPLE
./build.ps1 doctor

.EXAMPLE
./build.ps1 all -Profile check -Linkage static

.EXAMPLE
./build.ps1 all -Profile release -Linkage shared -VcpkgRoot 'D:\Program Files\vcpkg'
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('doctor', 'configure', 'build', 'test', 'all')]
    [string]$Command = 'all',

    [ValidateSet('dev', 'check', 'release')]
    [string]$Profile = 'dev',

    [ValidateSet('auto', 'static', 'shared')]
    [string]$Linkage = 'auto',

    [Alias('Deps')]
    [ValidateSet('auto', 'system', 'managed')]
    [string]$Dependencies = 'auto',

    [ValidateSet('auto', 'visual-studio', 'ninja')]
    [string]$Generator = 'auto',

    [ValidateSet('auto', 'msvc')]
    [string]$Compiler = 'auto',

    [string]$VcpkgRoot = '',

    [ValidateRange(0, 256)]
    [int]$Jobs = 0,

    [switch]$Offline,

    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# PowerShell 5.1 的默认控制台编码可能破坏含 Unicode 的工具路径或外部诊断。这里只
# 统一当前脚本进程为 UTF-8，不修改用户配置或系统区域设置；脚本自有输出仍为英文。
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)

if ($env:OS -ne 'Windows_NT') {
    [Console]::Error.WriteLine('[verdandi] error: sdk/cpp/build.ps1 supports Windows only. Use bash sdk/cpp/build.sh on Linux.')
    exit 1
}
if (-not [Environment]::Is64BitOperatingSystem) {
    [Console]::Error.WriteLine('[verdandi] error: Verdandi currently supports Windows x64 only; the detected operating system is not 64-bit.')
    exit 1
}

$script:RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script:BuildRoot = Join-Path $script:RepositoryRoot 'build'
$script:CppRoot = $PSScriptRoot
$script:ProbeRoot = Join-Path $PSScriptRoot 'cmake\probe'
$script:DryRunEnabled = $DryRun.IsPresent
$script:OfflineEnabled = $Offline.IsPresent
$script:ResolvedJobs = if ($Jobs -eq 0) { [Math]::Max(1, [Environment]::ProcessorCount) } else { $Jobs }
$script:LastProbeLog = $null

# auto 保留 CMake 的静态开发基线。C ABI、Legacy 动态链接或 C# 使用者需要 DLL 时
# 显式选择 shared；原生脚本只生成运行库，不编译任何使用方语言的工程。
$script:ResolvedLinkage = if ($Linkage -eq 'auto') { 'static' } else { $Linkage }

# dev 保持快速 Debug 反馈；check 使用 Release 以暴露优化期诊断；release 也使用
# Release，但不会自动执行打包、安装或发布。
$script:NativeConfiguration = if ($Profile -eq 'dev') { 'Debug' } else { 'Release' }

# 脚本会临时设置少量子进程环境变量。即使用户通过 dot-source 调用，也在 finally
# 中恢复原值，避免污染后续终端会话。
$script:EnvironmentNames = @(
    'VSLANG',
    'VCPKG_DOWNLOADS',
    'VCPKG_DEFAULT_BINARY_CACHE'
)
$script:OriginalEnvironment = @{}
foreach ($name in $script:EnvironmentNames) {
    $script:OriginalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

# CMake 的 Visual Studio 生成器会启动 MSBuild 并继承界面语言。统一固定为英文可
# 让本地日志、CI 解析和问题检索得到跨机器一致文本；finally 会恢复进程原值。
$env:VSLANG = '1033'

# 输出一个稳定的英文阶段标题。Title 只用于显示，不参与控制流；该函数不写文件，
# 也不吞掉调用者错误，使日志阅读者可以快速定位当前生命周期阶段。
function Write-BuildSection {
    param([Parameter(Mandatory = $true)][string]$Title)

    Write-Host ''
    Write-Host "== $Title ==" -ForegroundColor Cyan
}

# 输出脚本拥有的普通英文诊断。Message 必须已经是可公开记录的文本，调用方不得把
# 密码、证书私钥或带凭据的连接串传入；函数没有返回值和持久副作用。
function Write-BuildNote {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Host "[verdandi] $Message"
}

# 通过 PowerShell 标准 warning 流输出非致命英文诊断。Message 描述仍可继续执行的
# 降级；真正缺失的必需工具必须抛错而不能借此函数静默跳过。
function Write-BuildWarning {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Warning "[verdandi] $Message"
}

# 按 Names 给出的优先顺序在 PowerShell 命令解析表/PATH 中查找原生可执行文件。
# 成功返回规范化绝对路径；全部缺失返回 null，不启动候选程序，也不修改 PATH。
function Find-BuildExecutable {
    param([Parameter(Mandatory = $true)][string[]]$Names)

    # Get-Command 只查询命令解析表和 PATH，不启动目标程序。限定 Application 可以
    # 避免同名函数、别名或脚本意外替代真正的工具链可执行文件。
    foreach ($name in $Names) {
        $found = Get-Command -Name $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $found) {
            if (-not [string]::IsNullOrWhiteSpace($found.Source)) {
                return [System.IO.Path]::GetFullPath($found.Source)
            }
            return [System.IO.Path]::GetFullPath($found.Path)
        }
    }
    return $null
}

# 对 Find-BuildExecutable 增加“必需”语义。Names 是候选名称，Purpose 是英文错误中
# 的工具用途；成功返回绝对路径，失败抛出包含安装边界但不自动安装任何内容。
function Require-BuildExecutable {
    param(
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][string]$Purpose
    )

    $path = Find-BuildExecutable -Names $Names
    if ($null -eq $path) {
        throw "Missing $Purpose (searched for: $($Names -join ', ')). This script never installs toolchains; install it and retry."
    }
    return $path
}

# 从 Text 中提取第一个 major.minor[.patch] 版本供最小版本比较。ToolName 仅用于
# 诊断；无法解析时抛错，缺失 patch 按 0 处理，不接受依赖字符串排序的比较方式。
function Get-NumericVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$ToolName
    )

    $match = [regex]::Match($Text, '(?<!\d)(\d+)\.(\d+)(?:\.(\d+))?')
    if (-not $match.Success) {
        throw "Could not parse a version from the $ToolName output: $Text"
    }
    $patch = if ($match.Groups[3].Success) { $match.Groups[3].Value } else { '0' }
    return [version]::new(
        [int]$match.Groups[1].Value,
        [int]$match.Groups[2].Value,
        [int]$patch
    )
}

# 以 Arguments 启动 Executable 并完整收集标准输出/错误，用于稳定的版本探测。
# 非零退出码立即抛错；成功返回去除首尾空白的文本，不改变当前工作目录。
function Get-BuildToolOutput {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @()
    )

    $output = & $Executable @Arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Tool version query failed with exit code ${exitCode}: $Executable $($Arguments -join ' ')`n$output"
    }
    return $output.Trim()
}

# 将 Text 解析成版本并与 Minimum 做数值比较。Name 进入英文错误；成功返回实际
# System.Version，过低则抛错，绝不尝试升级或切换用户工具链。
function Confirm-MinimumVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][version]$Minimum
    )

    $actual = Get-NumericVersion -Text $Text -ToolName $Name
    if ($actual -lt $Minimum) {
        throw "$Name $actual is too old; version $Minimum or newer is required. This script never upgrades toolchains."
    }
    return $actual
}

# 把单个命令参数转换为仅供日志阅读的形式。含空白或双引号时增加显示引号；返回值
# 不参与真实进程启动，因此不会引入二次解析或改变参数字节。
function Format-BuildArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + $Value.Replace('"', '\"') + '"'
}

# 在 WorkingDirectory 中执行一个外部阶段命令。Label 用于英文开始/完成/失败日志，
# Executable 与 Arguments 直接按数组传递以保留边界；DryRun 只显示。函数恢复原
# 工作目录，校验退出码并报告不受区域设置影响的耗时。
function Invoke-BuildCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    $display = @($Executable) + $Arguments | ForEach-Object { Format-BuildArgument -Value $_ }
    Write-BuildNote "${Label}: $($display -join ' ')"
    if ($script:DryRunEnabled) {
        return
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location -LiteralPath $WorkingDirectory
    try {
        & $Executable @Arguments
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            throw "$Label failed with exit code $exitCode."
        }
    } finally {
        Pop-Location
        $stopwatch.Stop()
    }
    $seconds = $stopwatch.Elapsed.TotalSeconds.ToString('F2', [Globalization.CultureInfo]::InvariantCulture)
    Write-BuildNote "$Label completed successfully in $seconds seconds."
}

# 从 PATH 及 Visual Studio Installer 的两个固定位置寻找 vswhere。成功返回绝对
# 路径，失败返回 null；搜索是有界的，不枚举 Program Files 或磁盘目录树。
function Find-VsWhere {
    $fromPath = Find-BuildExecutable -Names @('vswhere.exe', 'vswhere')
    if ($null -ne $fromPath) {
        return $fromPath
    }

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $candidates += Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        $candidates += Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

# 使用 vswhere 选择最新且包含 MSVC x64 组件的 Visual Studio。返回安装/编译器路径、
# 版本、工具集和缓存标签；缺少 vswhere 或 C++ 工作负载时抛出英文诊断。
function Get-VisualStudioInstallation {
    $vswhere = Find-VsWhere
    if ($null -eq $vswhere) {
        throw 'vswhere was not found, so the MSVC toolchain cannot be resolved reliably. Install Visual Studio with Desktop development with C++.'
    }

    $queryArguments = @(
        '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    )
    $installationPath = (& $vswhere @queryArguments -property installationPath 2>$null | Select-Object -First 1)
    $installationVersion = (& $vswhere @queryArguments -property installationVersion 2>$null | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($installationPath)) {
        throw 'Visual Studio is installed, but its MSVC x64 C++ component is missing. Add Desktop development with C++ in Visual Studio Installer.'
    }

    $toolsetFile = Join-Path $installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $toolsetVersion = if (Test-Path -LiteralPath $toolsetFile -PathType Leaf) {
        (Get-Content -LiteralPath $toolsetFile -Raw).Trim()
    } else {
        ''
    }
    $compilerExecutable = Join-Path $installationPath "VC\Tools\MSVC\$toolsetVersion\bin\Hostx64\x64\cl.exe"
    if ([string]::IsNullOrWhiteSpace($toolsetVersion) -or
        -not (Test-Path -LiteralPath $compilerExecutable -PathType Leaf)) {
        throw "The selected Visual Studio installation does not contain its declared x64 cl.exe: $compilerExecutable"
    }

    # Visual C++ 工具集目录使用 14.xx，而 cl.exe 对外报告 19.xx。两者的次版本
    # 对应，因此把 14.51 转为 msvc-19.51，既可读又能隔离不兼容工具集缓存。
    $label = 'msvc'
    $toolsetMatch = [regex]::Match($toolsetVersion, '^14\.(\d+)')
    if ($toolsetMatch.Success) {
        $label = "msvc-19.$($toolsetMatch.Groups[1].Value)"
    }

    return [pscustomobject]@{
        Path = [System.IO.Path]::GetFullPath($installationPath)
        InstallationVersion = [string]$installationVersion
        ToolsetVersion = $toolsetVersion
        CompilerExecutable = [System.IO.Path]::GetFullPath($compilerExecutable)
        Label = $label
        VsWhere = $vswhere
    }
}

# 先从 PATH，再从已解析 VisualStudio 的固定 CMake/Ninja 位置寻找 ninja.exe。
# 返回绝对路径或 null，不要求 Ninja，因为 Visual Studio 生成器是稳定默认值。
function Find-NinjaExecutable {
    param([Parameter(Mandatory = $true)]$VisualStudio)

    $fromPath = Find-BuildExecutable -Names @('ninja.exe', 'ninja')
    if ($null -ne $fromPath) {
        return $fromPath
    }

    # Visual Studio 通常携带自己的 Ninja，但不会总是把它加入 PATH。只检查已知
    # 安装内部位置，不递归扫描磁盘。
    $candidate = Join-Path $VisualStudio.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return [System.IO.Path]::GetFullPath($candidate)
    }
    return $null
}

# 解析并验证 Windows 原生工具链：CMake 最小版本、MSVC、生成器以及可选检查工具。
# 返回只读上下文对象供后续探针/构建复用；不加载或持久修改开发者 shell 环境。
function Resolve-WindowsNativeToolchain {
    $cmake = Require-BuildExecutable -Names @('cmake.exe', 'cmake') -Purpose 'CMake 3.28 or newer'
    $cmakeText = Get-BuildToolOutput -Executable $cmake -Arguments @('--version')
    $cmakeVersion = Confirm-MinimumVersion -Name 'CMake' -Text $cmakeText -Minimum ([version]'3.28.0')
    $visualStudio = Get-VisualStudioInstallation

    $generatorName = ''
    $generatorSlug = ''
    $generatorArguments = @()
    $makeProgram = $null

    if ($Generator -eq 'ninja') {
        $makeProgram = Find-NinjaExecutable -VisualStudio $visualStudio
        if ($null -eq $makeProgram) {
            throw 'Ninja was selected explicitly, but ninja.exe was not found in PATH or the Visual Studio installation.'
        }
        $cl = Find-BuildExecutable -Names @('cl.exe', 'cl')
        if ($null -eq $cl) {
            throw 'Ninja with MSVC requires an initialized developer environment. Use Developer PowerShell or -Generator visual-studio.'
        }
        $generatorName = 'Ninja'
        $generatorSlug = 'ninja'
        $generatorArguments = @(
            "-DCMAKE_MAKE_PROGRAM=$($makeProgram -replace '\\', '/')",
            '-DCMAKE_C_COMPILER=cl',
            '-DCMAKE_CXX_COMPILER=cl'
        )
    } else {
        # Visual Studio 生成器不要求调用者预先运行 VsDevCmd，并且是 Windows 上
        # 工具依赖最少、对含空格路径最稳健的默认值。
        $help = Get-BuildToolOutput -Executable $cmake -Arguments @('--help')
        $matches = [regex]::Matches($help, 'Visual Studio\s+(\d+)\s+(\d{4})')
        if ($matches.Count -eq 0) {
            throw 'The installed CMake does not list a Visual Studio generator.'
        }
        # CMake 可能列出多个 Visual Studio 生成器。必须选择与 vswhere 实际返回的
        # 最新安装主版本一致的生成器，不能简单选择 CMake 认识的最高版本，否则在
        # 机器只安装较旧 Visual Studio 时会得到误导性的“找不到实例”配置错误。
        $installedMajorMatch = [regex]::Match($visualStudio.InstallationVersion, '^(\d+)')
        if (-not $installedMajorMatch.Success) {
            throw "Could not determine the Visual Studio major version from: $($visualStudio.InstallationVersion)"
        }
        $installedMajor = [int]$installedMajorMatch.Groups[1].Value
        $selected = $matches |
            Where-Object { [int]$_.Groups[1].Value -eq $installedMajor } |
            Select-Object -First 1
        if ($null -eq $selected) {
            $message = "CMake $cmakeVersion does not provide a generator for the installed Visual Studio " +
                "major version $installedMajor. Upgrade CMake or select a compatible Visual Studio installation."
            throw $message
        }
        $generatorName = "Visual Studio $($selected.Groups[1].Value) $($selected.Groups[2].Value)"
        $generatorSlug = "vs$($selected.Groups[1].Value)"
        $generatorArguments = @('-A', 'x64')
    }

    $clangFormatNames = @('clang-format.exe', 'clang-format')
    foreach ($major in 22..18) {
        $clangFormatNames += "clang-format-$major.exe", "clang-format-$major"
    }
    $clangFormat = Find-BuildExecutable -Names $clangFormatNames
    if ($null -eq $clangFormat) {
        $clangCandidate = Join-Path $visualStudio.Path 'VC\Tools\Llvm\x64\bin\clang-format.exe'
        if (Test-Path -LiteralPath $clangCandidate -PathType Leaf) {
            $clangFormat = [System.IO.Path]::GetFullPath($clangCandidate)
        }
    }

    return [pscustomobject]@{
        CMake = $cmake
        CMakeVersion = $cmakeVersion.ToString()
        CompilerLabel = $visualStudio.Label
        CompilerVersion = $visualStudio.ToolsetVersion
        GeneratorName = $generatorName
        GeneratorSlug = $generatorSlug
        GeneratorArguments = [string[]]$generatorArguments
        MultiConfig = $generatorName.StartsWith('Visual Studio', [StringComparison]::Ordinal)
        VisualStudio = $visualStudio
        Ninja = $makeProgram
        ClangFormat = $clangFormat
        RunClangTidy = $null
    }
}

# 依据显式 VcpkgRoot、环境变量、PATH、常见精确目录和 Visual Studio 内置路径依次
# 寻找现有 vcpkg。VisualStudio 用于最后回退；可用候选还必须通过只读 version 查询，
# 返回根/程序/toolchain/版本或 null，严禁递归扫描磁盘与自动 bootstrap。
function Resolve-VcpkgInstallation {
    param([Parameter(Mandatory = $true)]$VisualStudio)

    $candidates = [System.Collections.Generic.List[string]]::new()
    $explicitRoot = $null
    if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        # 显式路径代表调用者的确定选择，拼写错误或不完整安装必须立即报告，不能
        # 静默回退到另一份 vcpkg 后构建出来源不同的依赖。
        $explicitCandidate = $VcpkgRoot
        if (Test-Path -LiteralPath $explicitCandidate -PathType Leaf) {
            $explicitCandidate = Split-Path -Parent $explicitCandidate
        }
        if (-not (Test-Path -LiteralPath $explicitCandidate -PathType Container)) {
            throw "The explicit vcpkg root does not exist or is not a directory: $VcpkgRoot"
        }
        $explicitRoot = [System.IO.Path]::GetFullPath($explicitCandidate)
        $candidates.Add($explicitRoot)
    }
    if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        $candidates.Add($env:VCPKG_ROOT)
    }

    # CMAKE_TOOLCHAIN_FILE 可能已经直接指向某个 vcpkg.cmake。向上回溯固定的
    # scripts/buildsystems 两级即可得到根目录，不做模糊搜索。
    if (-not [string]::IsNullOrWhiteSpace($env:CMAKE_TOOLCHAIN_FILE) -and
        [System.IO.Path]::GetFileName($env:CMAKE_TOOLCHAIN_FILE) -ieq 'vcpkg.cmake') {
        $buildsystems = Split-Path -Parent $env:CMAKE_TOOLCHAIN_FILE
        $scripts = Split-Path -Parent $buildsystems
        $candidates.Add((Split-Path -Parent $scripts))
    }

    $fromPath = Find-BuildExecutable -Names @('vcpkg.exe', 'vcpkg')
    if ($null -ne $fromPath) {
        $candidates.Add((Split-Path -Parent $fromPath))
    }

    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $candidates.Add((Join-Path $env:USERPROFILE 'vcpkg'))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $candidates.Add((Join-Path $env:LOCALAPPDATA 'vcpkg'))
    }
    # 用户的 vcpkg 可能位于非系统盘。这里只检查每个文件系统盘的两个常见精确
    # 位置，例如 D:\vcpkg 和 D:\Program Files\vcpkg，绝不递归枚举目录。
    foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
        if (-not [string]::IsNullOrWhiteSpace($drive.Root)) {
            $candidates.Add((Join-Path $drive.Root 'vcpkg'))
            $candidates.Add((Join-Path $drive.Root 'Program Files\vcpkg'))
        }
    }
    # Visual Studio 自带的 vcpkg 放在独立安装之后作为最后回退，避免遮蔽用户专门
    # 维护、缓存更完整的 standalone vcpkg。
    $candidates.Add((Join-Path $VisualStudio.Path 'VC\vcpkg'))

    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($rawCandidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($rawCandidate)) {
            continue
        }
        $candidate = $rawCandidate
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $candidate = Split-Path -Parent $candidate
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
            continue
        }
        $full = [System.IO.Path]::GetFullPath($candidate)
        if (-not $seen.Add($full)) {
            continue
        }
        $executable = Join-Path $full 'vcpkg.exe'
        $toolchain = Join-Path $full 'scripts\buildsystems\vcpkg.cmake'
        if ((Test-Path -LiteralPath $executable -PathType Leaf) -and
            (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
            # 文件存在并不足以证明安装可用；执行只读版本查询可以提前发现损坏的
            # executable、缺失运行库或错误架构。自动发现的坏候选继续搜索，显式
            # 候选则保持 fail-fast 语义。
            try {
                $versionText = Get-BuildToolOutput -Executable $executable -Arguments @('version')
            } catch {
                if ($null -ne $explicitRoot -and $full -eq $explicitRoot) {
                    throw "The explicit vcpkg installation could not run its version command: $executable`n$($_.Exception.Message)"
                }
                continue
            }
            return [pscustomobject]@{
                Root = $full
                Executable = [System.IO.Path]::GetFullPath($executable)
                Toolchain = [System.IO.Path]::GetFullPath($toolchain)
                Version = Get-FirstOutputLine $versionText
            }
        }
        if ($null -ne $explicitRoot -and $full -eq $explicitRoot) {
            throw "The explicit vcpkg root is incomplete. Expected vcpkg.exe and scripts/buildsystems/vcpkg.cmake below: $explicitRoot"
        }
    }
    if ($null -ne $explicitRoot) {
        throw "The explicit vcpkg root is incomplete. Expected vcpkg.exe and scripts/buildsystems/vcpkg.cmake below: $explicitRoot"
    }
    return $null
}

# 为一个隔离的最小 CMake 探针生成参数数组。BuildDirectory 决定日志/缓存位置，
# WithOpenSsl 决定是否额外验证 OpenSSL 3.0 头文件及 Crypto/SSL 链接目标。
function Get-CMakeProbeArguments {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDirectory,
        [Parameter(Mandatory = $true)][bool]$WithOpenSsl
    )

    $arguments = @(
        '--fresh',
        '-S', $script:ProbeRoot,
        '-B', $BuildDirectory,
        '-G', $script:Native.GeneratorName
    )
    $arguments += $script:Native.GeneratorArguments
    $arguments += "-DVERDANDI_PROBE_OPENSSL=$(if ($WithOpenSsl) { 'ON' } else { 'OFF' })"

    # 系统探针必须排除 CMAKE_TOOLCHAIN_FILE 环境变量的隐式 vcpkg 注入；显式的
    # OPENSSL_ROOT_DIR 和 CMAKE_PREFIX_PATH 仍由 CMake 正常使用。
    $arguments += '-DCMAKE_TOOLCHAIN_FILE='
    return [string[]]$arguments
}

# 配置并编译一个最小探针，将完整原始输出写入独立 probe.log。Name 形成安全的已知
# 路径，WithOpenSsl 选择能力；成功返回 true，失败返回 false，由上层决定回退或报错。
function Invoke-CMakeProbe {
    param(
        [Parameter(Mandatory = $true)][bool]$WithOpenSsl,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $probeDirectory = Join-Path $script:BuildRoot "probes\windows\x64\$($script:Native.CompilerLabel)\$($script:Native.GeneratorSlug)\$Name"
    $logPath = Join-Path $probeDirectory 'probe.log'
    $configureArguments = Get-CMakeProbeArguments -BuildDirectory $probeDirectory -WithOpenSsl $WithOpenSsl
    $buildArguments = @('--build', $probeDirectory, '--parallel', [string]$script:ResolvedJobs)
    if ($script:Native.MultiConfig) {
        $buildArguments += @('--config', 'Release')
        # Visual Studio 可能只安装本地化的 linker 资源。探针详细输出已写入日志，
        # quiet 可避免成功路径把机器相关语言混进标准英文控制台协议。
        $buildArguments += @('--', '/nologo', '/verbosity:quiet')
    }

    if ($script:DryRunEnabled) {
        Write-BuildNote "Skipping the $Name compilation probe in dry-run mode."
        return $true
    }

    New-Item -ItemType Directory -Path $probeDirectory -Force | Out-Null
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("configure: $($script:Native.CMake) $($configureArguments -join ' ')")

    # Windows PowerShell 5.1 会把重定向后的原生 stderr 包装成 ErrorRecord；当脚本
    # 使用 Stop 策略时，正常的“系统 OpenSSL 探针失败后回退 vcpkg”会被提前终止。
    # 探针局部按退出码裁决并保留完整输出，离开临界段立即恢复全局严格策略。
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $configureOutput = & $script:Native.CMake @configureArguments 2>&1 | Out-String
        $configureExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    $lines.Add($configureOutput)
    if ($configureExit -eq 0) {
        $lines.Add("build: $($script:Native.CMake) $($buildArguments -join ' ')")
        try {
            $ErrorActionPreference = 'Continue'
            $buildOutput = & $script:Native.CMake @buildArguments 2>&1 | Out-String
            $buildExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
        }
        $lines.Add($buildOutput)
    } else {
        $buildExit = $configureExit
    }
    [System.IO.File]::WriteAllLines($logPath, $lines, [System.Text.UTF8Encoding]::new($false))

    $script:LastProbeLog = $logPath
    return $configureExit -eq 0 -and $buildExit -eq 0
}

# 当探针失败时输出最近日志路径及末尾 30 行，帮助定位实际编译器/CMake 原因。
# 没有可读日志时直接返回；该函数只读构建产物，不改变失败分类。
function Show-LastProbeLog {
    if ($null -eq $script:LastProbeLog -or -not (Test-Path -LiteralPath $script:LastProbeLog -PathType Leaf)) {
        return
    }
    Write-BuildWarning "Probe log: $script:LastProbeLog"
    Get-Content -LiteralPath $script:LastProbeLog -Tail 30 | ForEach-Object { Write-Host $_ }
}

# 先证明 C++23 工具链，再按 Dependencies 选择 system 或 vcpkg OpenSSL。返回稳定的
# provider 字符串；system 使用真实编译链接探针，vcpkg 必须已存在，二者都不可用
# 时抛错且绝不自行编译 OpenSSL/安装 vcpkg。
function Resolve-OpenSslProvider {
    $script:Vcpkg = Resolve-VcpkgInstallation -VisualStudio $script:Native.VisualStudio

    if (-not (Invoke-CMakeProbe -WithOpenSsl $false -Name 'cpp23')) {
        Show-LastProbeLog
        throw 'The C++23 compile/link probe failed. Check MSVC, the Windows SDK, and the selected CMake generator.'
    }
    if (-not $script:DryRunEnabled) {
        Write-BuildNote 'The C++23 compile/link probe passed.'
    }

    if ($Dependencies -eq 'managed') {
        if ($null -eq $script:Vcpkg) {
            throw 'Managed dependency mode requires an existing vcpkg installation. Install vcpkg or specify -VcpkgRoot.'
        }
        return 'vcpkg'
    }

    if ($script:DryRunEnabled) {
        if (-not [string]::IsNullOrWhiteSpace($env:OPENSSL_ROOT_DIR)) {
            Write-BuildNote 'Dry-run selected system OpenSSL from OPENSSL_ROOT_DIR; doctor performs the actual compile/link probe.'
            return 'system'
        }
        if ($null -ne $script:Vcpkg -and $Dependencies -eq 'auto') {
            Write-BuildNote 'Dry-run skipped the system OpenSSL probe and conservatively selected the available vcpkg fallback.'
            return 'vcpkg'
        }
        Write-BuildNote 'Dry-run plans to use system OpenSSL; run doctor without -DryRun to perform the required compile/link probe.'
        return 'system'
    }

    if (Invoke-CMakeProbe -WithOpenSsl $true -Name 'openssl-system') {
        Write-BuildNote 'The system OpenSSL 3.0+ compile/link probe passed.'
        return 'system'
    }
    if ($Dependencies -eq 'system') {
        Show-LastProbeLog
        throw 'System dependency mode could not compile and link OpenSSL 3.0+. Set OPENSSL_ROOT_DIR/CMAKE_PREFIX_PATH or use auto mode.'
    }
    if ($null -ne $script:Vcpkg) {
        Write-BuildNote "System OpenSSL is unavailable; using the existing vcpkg installation: $($script:Vcpkg.Root)"
        return 'vcpkg'
    }

    throw @'
No usable OpenSSL 3.0+ installation or vcpkg installation was found.
Verdandi never builds OpenSSL or installs vcpkg automatically. Install vcpkg and set
VCPKG_ROOT/-VcpkgRoot, or provide OPENSSL_ROOT_DIR and retry.
'@
}

# 把多行版本输出压缩成环境清单中的首行。空/null 文本返回空字符串；该函数不用于
# 版本比较，避免丢失错误上下文影响验证逻辑。
function Get-FirstOutputLine {
    param([AllowEmptyString()][string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return ''
    }
    return ($Text -split "`r?`n" | Select-Object -First 1).Trim()
}

# 完成一次调用所需的全部只读诊断，计算隔离路径并设置仅对子进程有效的环境变量。
# 成功后后续阶段可直接使用脚本级上下文；任一必需能力缺失即在下载依赖前失败。
function Initialize-BuildContext {
    Write-BuildSection 'Toolchain diagnostics'

    # 本入口只拥有 C++23 核心、C ABI 和 Legacy 原生边界，因此一次性完成原生
    # 工具链与 OpenSSL 编译/链接探针；使用方语言的编译器不属于此处的依赖。
    $script:Tools = [ordered]@{}
    $script:Native = Resolve-WindowsNativeToolchain
    $script:OpenSslProvider = Resolve-OpenSslProvider
    $script:Tools.cmake = [ordered]@{ path = $script:Native.CMake; version = $script:Native.CMakeVersion }
    $script:Tools.cpp = [ordered]@{
        path = $script:Native.VisualStudio.CompilerExecutable
        version = $script:Native.CompilerVersion
    }
    $script:Tools.visual_studio = [ordered]@{
        path = $script:Native.VisualStudio.Path
        version = $script:Native.VisualStudio.InstallationVersion
    }
    if ($null -ne $script:Native.Ninja) {
        $script:Tools.ninja = [ordered]@{ path = $script:Native.Ninja; version = '' }
    }
    if ($null -ne $script:Native.ClangFormat) {
        $formatVersion = Get-FirstOutputLine (Get-BuildToolOutput -Executable $script:Native.ClangFormat -Arguments @('--version'))
        $script:Tools.clang_format = [ordered]@{ path = $script:Native.ClangFormat; version = $formatVersion }
    } elseif ($Profile -eq 'check') {
        throw 'The check profile requires clang-format, but it was not found in PATH or Visual Studio. Install LLVM and retry.'
    }

    # 路径包含所有可能改变 ABI、依赖来源或对象内容的维度，避免不同编译器、
    # 生成器、OpenSSL 来源、依赖策略、Profile 和链接类型共用 CMake cache。
    $providerSlug = $script:OpenSslProvider
    $nativePath = "windows\x64\$($script:Native.CompilerLabel)\$($script:Native.GeneratorSlug)\" +
        "$providerSlug\$Dependencies\$Profile-$($script:ResolvedLinkage)"
    $script:CppBuildDirectory = Join-Path $script:BuildRoot "cpp\$nativePath"
    $script:CppDependencyDirectory = Join-Path $script:BuildRoot "deps\$nativePath"
    if ($script:ResolvedLinkage -eq 'shared') {
        # Visual Studio 是多配置生成器，实际 DLL 位于 Debug/Release 子目录；Ninja
        # 单配置生成器直接写在构建根。该路径仅用于明确告诉调用者应复制哪个文件。
        $script:NativeRuntimePath = if ($script:Native.MultiConfig) {
            Join-Path $script:CppBuildDirectory "$script:NativeConfiguration\verdandi_cpp.dll"
        } else {
            Join-Path $script:CppBuildDirectory 'verdandi_cpp.dll'
        }
    } else {
        $script:NativeRuntimePath = $null
    }

    Write-BuildNote "Platform: windows/x64; command: $Command; profile: $Profile; jobs: $script:ResolvedJobs"
    Write-BuildNote "Dependency policy: $Dependencies; offline mode: $($script:OfflineEnabled.ToString().ToLowerInvariant())"
    Write-BuildNote "CMake: $($script:Native.CMake) (version $($script:Native.CMakeVersion))"
    Write-BuildNote "C/C++ compiler: $($script:Native.VisualStudio.CompilerExecutable) (MSVC toolset $($script:Native.CompilerVersion))"
    Write-BuildNote "CMake generator: $($script:Native.GeneratorName); linkage: $script:ResolvedLinkage"
    Write-BuildNote "OpenSSL provider: $script:OpenSslProvider (minimum supported version 3.0.0)"
    if ($null -ne $script:Vcpkg) {
        Write-BuildNote "vcpkg: $($script:Vcpkg.Executable) ($($script:Vcpkg.Version))"
    }
    Write-BuildNote "C++ build directory: $script:CppBuildDirectory"
    Write-BuildNote "C++ dependency directory: $script:CppDependencyDirectory"
    if ($null -ne $script:NativeRuntimePath) {
        Write-BuildNote "Shared runtime output: $script:NativeRuntimePath"
        Write-BuildNote 'Copy this DLL beside the consuming executable or into its runtimes/win-x64/native directory.'
    }
}

# 把已解析平台、工具版本、依赖策略和输出路径写为 UTF-8 JSON。DryRun 不写文件；
# 清单不含凭据且只描述本次调用，它是复现辅助信息而不是发布签名或资格证明。
function Write-EnvironmentManifest {
    if ($script:DryRunEnabled) {
        return
    }

    $manifestPath = Join-Path $script:BuildRoot 'environment.json'
    New-Item -ItemType Directory -Path $script:BuildRoot -Force | Out-Null
    $paths = [ordered]@{
        cpp = $script:CppBuildDirectory
        cpp_dependencies = $script:CppDependencyDirectory
        native_runtime = $script:NativeRuntimePath
    }

    $manifest = [ordered]@{
        schema = 'v1'
        generated_at_utc = [DateTime]::UtcNow.ToString('o')
        platform = 'windows'
        architecture = 'x64'
        command = $Command
        profile = $Profile
        linkage = $script:ResolvedLinkage
        dependencies = $Dependencies
        offline = $script:OfflineEnabled
        jobs = $script:ResolvedJobs
        openssl = [ordered]@{
            minimum = '3.0.0'
            provider = $script:OpenSslProvider
        }
        vcpkg = if ($null -ne $script:Vcpkg) {
            [ordered]@{
                root = $script:Vcpkg.Root
                executable = $script:Vcpkg.Executable
                toolchain = $script:Vcpkg.Toolchain
                version = $script:Vcpkg.Version
            }
        } else {
            $null
        }
        paths = $paths
        tools = $script:Tools
    }
    $json = $manifest | ConvertTo-Json -Depth 8
    # 先写同目录临时文件再替换，Windows/Linux 构建并发更新“最近一次调用”清单时，
    # 读者至多看到较旧的完整 JSON，不会看到被截断或交织的半份文档。
    $temporaryPath = "$manifestPath.$PID.tmp"
    [System.IO.File]::WriteAllText($temporaryPath, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporaryPath -Destination $manifestPath -Force
    Write-BuildNote "Environment manifest written to: $manifestPath"
}

# 根据已冻结的调用选项生成完整 CMake configure 参数数组，同时设置必要的进程级
# vcpkg 缓存变量。返回值保持逐参数边界；offline 会同时约束 FetchContent/vcpkg。
function Get-CppConfigureArguments {
    $arguments = @(
        '-S', $script:CppRoot,
        '-B', $script:CppBuildDirectory,
        '-G', $script:Native.GeneratorName
    )
    $arguments += $script:Native.GeneratorArguments
    if (-not $script:Native.MultiConfig) {
        $arguments += "-DCMAKE_BUILD_TYPE=$script:NativeConfiguration"
    }
    $arguments += @(
        "-DBUILD_SHARED_LIBS=$(if ($script:ResolvedLinkage -eq 'shared') { 'ON' } else { 'OFF' })",
        # 统一入口始终配置原生测试目标，使 configure/build/test 阶段共享完全相同
        # 的 CMake 形状，不因后续命令顺序改写 cache 或重新生成工程。
        '-DVERDANDI_BUILD_TESTS=ON',
        "-DVERDANDI_FETCH_DEPENDENCIES=$(if ($Dependencies -eq 'system') { 'OFF' } else { 'ON' })",
        "-DVERDANDI_USE_MANAGED_DEPENDENCIES=$(if ($Dependencies -eq 'managed') { 'ON' } else { 'OFF' })",
        "-DVERDANDI_OFFLINE_DEPENDENCIES=$(if ($script:OfflineEnabled) { 'ON' } else { 'OFF' })",
        '-DVERDANDI_ENABLE_SANITIZERS=OFF',
        # FULLY_DISCONNECTED 也会禁止从本地压缩包首次解压，因此固定为 OFF。真正的
        # 离线边界由 VERDANDI_OFFLINE_DEPENDENCIES 的校验本地 URL 和 vcpkg 的
        # --no-downloads 共同执行；UPDATES_DISCONNECTED 额外禁止 VCS 更新。
        '-DFETCHCONTENT_FULLY_DISCONNECTED=OFF',
        "-DFETCHCONTENT_UPDATES_DISCONNECTED=$(if ($script:OfflineEnabled) { 'ON' } else { 'OFF' })",
        "-DFETCHCONTENT_BASE_DIR=$(($script:CppDependencyDirectory -replace '\\', '/'))/fetchcontent",
        "-DVERDANDI_DOWNLOAD_CACHE=$(($script:BuildRoot -replace '\\', '/'))/deps/common/downloads/fetchcontent",
        "-DVERDANDI_CLANG_FORMAT=$(if ($null -eq $script:Native.ClangFormat) { '' } else { $script:Native.ClangFormat -replace '\\', '/' })",
        "-DVERDANDI_RUN_CLANG_TIDY=$(if ($null -eq $script:Native.RunClangTidy) { '' } else { $script:Native.RunClangTidy -replace '\\', '/' })"
    )

    if ($script:OpenSslProvider -eq 'vcpkg') {
        $installed = Join-Path $script:CppDependencyDirectory 'vcpkg_installed'
        $downloads = Join-Path $script:BuildRoot 'deps\common\downloads\vcpkg'
        # vcpkg 二进制缓存按平台/编译器 ABI 共享；Debug/Release 和静态/共享的实际
        # 区别已进入 vcpkg 自己的 ABI 哈希，因此不需要人为复制同一压缩包。
        $binaryCache = Join-Path $script:BuildRoot "deps\common\vcpkg-binary-cache\windows\x64\$($script:Native.CompilerLabel)"
        $env:VCPKG_DOWNLOADS = $downloads
        $env:VCPKG_DEFAULT_BINARY_CACHE = $binaryCache
        $arguments += @(
            "-DCMAKE_TOOLCHAIN_FILE=$($script:Vcpkg.Toolchain -replace '\\', '/')",
            '-DVCPKG_TARGET_TRIPLET=x64-windows',
            '-DVCPKG_HOST_TRIPLET=x64-windows',
            "-DVCPKG_MANIFEST_DIR=$($script:CppRoot -replace '\\', '/')",
            "-DVCPKG_INSTALLED_DIR=$($installed -replace '\\', '/')"
        )
        # vcpkg 的稳定接口允许严格禁止新下载。临时 buildtrees/packages 保持在
        # vcpkg 默认位置；实验性重定向会进入包 ABI，反而破坏跨 Profile 缓存复用。
        if ($script:OfflineEnabled) {
            $arguments += '-DVCPKG_INSTALL_OPTIONS=--no-downloads'
        } else {
            # 显式清空可避免同一构建树从先前 offline/实验配置继承旧选项。
            $arguments += '-DVCPKG_INSTALL_OPTIONS='
        }
    } else {
        # 明确传空可以阻止环境变量 CMAKE_TOOLCHAIN_FILE 把 system 构建悄悄切换成
        # vcpkg；显式 OPENSSL_ROOT_DIR/CMAKE_PREFIX_PATH 仍然有效。
        $arguments += '-DCMAKE_TOOLCHAIN_FILE='
    }

    return [string[]]$arguments
}

# 创建当前 C++ 依赖/公共下载缓存目录并执行 CMake 配置。DryRun 仅打印；失败由统一
# 命令包装抛出。共享的是摘要校验下载，解压源码和对象仍按构建维度隔离。
function Invoke-CppConfigure {
    Write-BuildSection 'Configure the C++23/C ABI runtime'
    if (-not $script:DryRunEnabled) {
        New-Item -ItemType Directory -Path $script:CppDependencyDirectory -Force | Out-Null
        # 带摘要校验的只读下载包可以跨 Profile/链接方式复用；解压与编译目录继续
        # 保持在当前 CppDependencyDirectory 内，兼顾空间与并行构建隔离。
        New-Item -ItemType Directory -Path (Join-Path $script:BuildRoot 'deps\common\downloads\fetchcontent') -Force | Out-Null
        if ($script:OpenSslProvider -eq 'vcpkg') {
            # vcpkg 接受显式下载缓存和二进制缓存目录。预先创建它们可以让路径或
            # 权限错误在依赖解析前暴露，而不是在较长构建后才失败。
            New-Item -ItemType Directory -Path (Join-Path $script:BuildRoot 'deps\common\downloads\vcpkg') -Force | Out-Null
            $binaryCacheDirectory = Join-Path $script:BuildRoot (
                "deps\common\vcpkg-binary-cache\windows\x64\$($script:Native.CompilerLabel)"
            )
            New-Item -ItemType Directory -Path $binaryCacheDirectory -Force | Out-Null
        }
    }
    Invoke-BuildCommand `
        -Label 'CMake configure' `
        -Executable $script:Native.CMake `
        -Arguments (Get-CppConfigureArguments) `
        -WorkingDirectory $script:RepositoryRoot
}

# 编译已配置的完整原生构建树。Visual Studio 成功输出保持安静以避开本地化 linker
# 文本；脚本自身仍输出完整英文命令、配置、阶段结果和耗时。
function Invoke-CppBuild {
    Write-BuildSection 'Build the C++23/C ABI runtime'
    $arguments = @('--build', $script:CppBuildDirectory, '--parallel', [string]$script:ResolvedJobs)
    if ($script:Native.MultiConfig) {
        $arguments += @('--config', $script:NativeConfiguration)
    }
    if ($script:Native.MultiConfig) {
        # quiet 只抑制 Visual Studio 工具链可能忽略 VSLANG 的本地化成功消息；脚本
        # 仍打印完整命令、配置、目标和耗时。错误仍由 MSBuild 输出并传播非零状态。
        $arguments += @('--', '/nologo', '/verbosity:quiet')
    }
    Invoke-BuildCommand -Label 'CMake build' -Executable $script:Native.CMake -Arguments $arguments -WorkingDirectory $script:RepositoryRoot

    # shared 构建必须实际产生前面报告的 DLL；立即校验可防止目标名、生成器布局或
    # CMake 选项漂移后仍向下游报告一个不存在的复制路径。
    if (-not $script:DryRunEnabled -and $script:ResolvedLinkage -eq 'shared') {
        if (-not (Test-Path -LiteralPath $script:NativeRuntimePath -PathType Leaf)) {
            throw "The shared runtime was not produced at the expected path: $script:NativeRuntimePath"
        }
        $runtimeBytes = (Get-Item -LiteralPath $script:NativeRuntimePath).Length
        Write-BuildNote "Shared runtime verified: $script:NativeRuntimePath ($runtimeBytes bytes)"
    }
}

# 执行 CTest，并在 check Profile 下执行强制格式检查。Redis 端点未配置时集成测试
# 按 CTest skip 契约退出；任何真实失败均传播为非零脚本结果。
function Invoke-CppTests {
    Write-BuildSection 'Test C++23, C ABI, and Legacy consumers'
    $ctest = Require-BuildExecutable -Names @('ctest.exe', 'ctest') -Purpose 'CTest'
    $arguments = @('--test-dir', $script:CppBuildDirectory, '--output-on-failure', '--parallel', [string]$script:ResolvedJobs)
    if ($script:Native.MultiConfig) {
        $arguments += @('-C', $script:NativeConfiguration)
    }
    Invoke-BuildCommand -Label 'CTest' -Executable $ctest -Arguments $arguments -WorkingDirectory $script:RepositoryRoot

    # check 是发布前原生源码门：除运行行为测试外，还必须验证格式目标。
    if ($Profile -eq 'check') {
        Write-BuildSection 'Check the C++ source'
        $formatArguments = @('--build', $script:CppBuildDirectory, '--target', 'verdandi_cpp_format_check')
        if ($script:Native.MultiConfig) {
            $formatArguments += @('--config', $script:NativeConfiguration, '--', '/nologo', '/verbosity:quiet')
        }
        Invoke-BuildCommand -Label 'C++ format check' -Executable $script:Native.CMake -Arguments $formatArguments -WorkingDirectory $script:RepositoryRoot
        Write-BuildNote 'Windows does not require run-clang-tidy; the Linux check profile owns that qualification step.'
    }
}

# 执行唯一的原生配置阶段。使用方语言独立编译并加载这里生成的共享运行库，不属于
# 此状态机；失败时立即停止，不尝试在不完整构建树上继续。
function Invoke-ConfigureStage {
    Invoke-CppConfigure
}

# 构建 C++23 核心、C ABI 和各标准消费测试；不会探测或触碰使用方语言工程。
function Invoke-BuildStage {
    Invoke-CppBuild
}

# 运行已经构建的原生测试；check 额外执行静态门。函数不隐式配置或编译。
function Invoke-TestStage {
    Invoke-CppTests
}

try {
    Initialize-BuildContext
    Write-EnvironmentManifest

    switch ($Command) {
        'doctor' {
            Write-BuildSection 'Diagnostics complete'
            if ($script:DryRunEnabled) {
                Write-BuildNote 'Surface-level tool discovery passed. Compilation probes were intentionally skipped in dry-run mode.'
            } else {
                Write-BuildNote 'All required tools passed validation. No dependencies were downloaded and no source files were modified.'
            }
        }
        'configure' { Invoke-ConfigureStage }
        'build' { Invoke-BuildStage }
        'test' { Invoke-TestStage }
        'all' {
            Invoke-ConfigureStage
            Invoke-BuildStage
            Invoke-TestStage
        }
    }

    if ($Command -ne 'doctor') {
        if ($script:DryRunEnabled) {
            Write-BuildSection 'Dry-run complete'
            Write-BuildNote "The Verdandi $Command plan completed successfully. No build commands were executed and no files were written."
        } else {
            Write-BuildSection 'Completed'
            Write-BuildNote "Verdandi $Command completed successfully. Environment manifest: $(Join-Path $script:BuildRoot 'environment.json')"
        }
    }
} catch {
    [Console]::Error.WriteLine("[verdandi] error: $($_.Exception.Message)")
    exit 1
} finally {
    foreach ($name in $script:EnvironmentNames) {
        [Environment]::SetEnvironmentVariable($name, $script:OriginalEnvironment[$name], 'Process')
    }
}
