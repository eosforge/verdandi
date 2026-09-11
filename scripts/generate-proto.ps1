# 显式生成协议源码. -Check 只比较, 两种模式都不下载工具或依赖.
param([switch]$Check)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
$arguments = @('run', '--manifest-path', "$verdandiRoot/proto/generator/Cargo.toml", '--locked', '--offline', '--jobs', '2', '--')
if ($Check) { $arguments += '--check' }
& "$verdandiRoot/sdk/run-tool.ps1" -Executable cargo -WorkingDirectory $verdandiRoot -Environment @{
    CARGO_HOME = "$verdandiRoot/build/deps/cargo"
    CARGO_TARGET_DIR = "$verdandiRoot/build/proto/target"
    CARGO_NET_OFFLINE = 'true'
} -ToolArguments $arguments
exit $LASTEXITCODE
