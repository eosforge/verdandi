# 使用项目内缓存运行 Peer Cargo, 只修改所启动子进程的环境.
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/.." -ErrorAction Stop).ProviderPath
& "$verdandiRoot/sdk/run-tool.ps1" -Executable cargo -WorkingDirectory $PSScriptRoot -Environment @{
    CARGO_HOME = Join-Path $verdandiRoot 'build/deps/cargo'
    CARGO_TARGET_DIR = Join-Path $verdandiRoot 'build/peer/target'
    CARGO_NET_OFFLINE = 'true'
} -ToolArguments $args
exit $LASTEXITCODE
