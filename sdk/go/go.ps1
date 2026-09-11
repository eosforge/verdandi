# 使用项目内缓存运行一次 Go；调用终端的环境变量和工作目录保持原样。
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiBuildRoot = Join-Path (Resolve-Path -LiteralPath "$PSScriptRoot/../.." -ErrorAction Stop).ProviderPath 'build'
& "$PSScriptRoot/../run-tool.ps1" -Executable go -WorkingDirectory $PSScriptRoot -Environment @{
    GOMODCACHE = Join-Path $verdandiBuildRoot 'deps/go/pkg/mod'
    GOCACHE = Join-Path $verdandiBuildRoot 'cache/go'
} -ToolArguments $args
exit $LASTEXITCODE
