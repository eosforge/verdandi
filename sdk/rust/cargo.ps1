# 使用项目内缓存运行一次 Cargo；保留调用终端环境、PATH 和已有 Rust 工具链。
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiBuildRoot = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../.." -ErrorAction Stop).ProviderPath 'build'
& "$PSScriptRoot/../run-tool.ps1" -Executable cargo -WorkingDirectory $PSScriptRoot -Environment @{
    CARGO_HOME = Join-Path $verdandiBuildRoot 'deps/cargo'
    CARGO_TARGET_DIR = Join-Path $verdandiBuildRoot 'rust/target'
} -ToolArguments $args
exit $LASTEXITCODE
