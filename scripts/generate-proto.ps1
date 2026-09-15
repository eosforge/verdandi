# 显式生成协议源码. -Check 只比较, 两种模式都不下载工具或依赖.
param([switch]$Check)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$astraRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
$arguments = @('run', '--manifest-path', "$astraRoot/proto/generator/Cargo.toml", '--locked', '--offline', '--jobs', '2', '--')
if ($Check) { $arguments += '--check' }
& "$astraRoot/sdk/run-tool.ps1" -Executable cargo -WorkingDirectory $astraRoot -Environment @{
    CARGO_HOME = "$astraRoot/build/deps/cargo"
    CARGO_TARGET_DIR = "$astraRoot/build/proto/target"
    CARGO_NET_OFFLINE = 'true'
} -ToolArguments $arguments
exit $LASTEXITCODE
