<# .SYNOPSIS
Runs the shared Python C++ build entry. Use -Python to select an existing interpreter.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Command = 'all',
    [string]$Profile = 'dev',
    [string]$Linkage = 'auto',
    [Alias('Deps')][string]$Dependencies = 'auto',
    [string]$Generator = 'auto',
    [string]$Compiler = 'auto',
    [string]$VcpkgRoot = '',
    [int]$Jobs = 0,
    [switch]$Offline,
    [switch]$DryRun,
    [string]$Python = ''
)

if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Use build.sh or build.py on Linux.' }
if (-not $Python) {
    $candidates = @(Get-Command python, python3 -CommandType Application -All -ErrorAction SilentlyContinue | ForEach-Object Source)
    if ($env:USERPROFILE) { $candidates += Join-Path $env:USERPROFILE '.local\bin\python.exe' }
    # 忽略 Windows Store 的空占位入口；探测不会启动安装器或下载 Python。
    $Python = $candidates | Where-Object {
        (Test-Path -LiteralPath $_ -PathType Leaf) -and (Get-Item -LiteralPath $_).Length -gt 0
    } | Select-Object -First 1
}
if (-not $Python) { throw 'Python 3.10+ was not found. Select an existing interpreter with -Python PATH.' }
& $Python -I -S -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
if ($LASTEXITCODE -ne 0) { throw 'An existing Python 3.10+ interpreter is required.' }

# 此处只适配 PowerShell 参数名，选项校验和全部构建策略归 build.py 所有。
$arguments = @(
    $Command, '--profile', $Profile, '--linkage', $Linkage, '--deps', $Dependencies,
    '--generator', $Generator, '--compiler', $Compiler, '--jobs', [string]$Jobs
)
if ($VcpkgRoot) { $arguments += '--vcpkg-root', $VcpkgRoot }
if ($Offline) { $arguments += '--offline' }
if ($DryRun) { $arguments += '--dry-run' }
$runnerArguments = @{
    Executable = $Python
    WorkingDirectory = (Get-Location).ProviderPath
    Environment = @{ PYTHONIOENCODING = 'utf-8' }
    ToolArguments = @('-B', (Join-Path $PSScriptRoot 'build.py')) + $arguments
}
& "$PSScriptRoot/../run-tool.ps1" @runnerArguments
exit $LASTEXITCODE
