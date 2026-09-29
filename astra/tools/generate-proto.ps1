# 显式生成协议源码. -Check 只比较, 两种模式都不下载工具或依赖.
param([switch]$Check)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$astraRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
$astraCache = if ($env:ASTRA_CACHE_ROOT) { $env:ASTRA_CACHE_ROOT } else { Join-Path $astraRoot 'build' }
$arguments = @('run', '--manifest-path', "$astraRoot/tools/protocol/Cargo.toml", '--locked', '--offline', '--jobs', '2', '--')
if ($Check) { $arguments += '--check' }
& "$astraRoot/tools/run-tool.ps1" -Executable cargo -WorkingDirectory $astraRoot -Environment @{
    CARGO_HOME = "$astraCache/deps/cargo"
    CARGO_TARGET_DIR = "$astraRoot/build/proto/target"
    CARGO_NET_OFFLINE = 'true'
} -ToolArguments $arguments
exit $LASTEXITCODE
