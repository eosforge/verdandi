# 使用项目内缓存运行 Supervisor Go, 所有设置仅作用于子进程.
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
$verdandiTemporary = Join-Path $verdandiRoot 'build/tmp/supervisor'
[IO.Directory]::CreateDirectory($verdandiTemporary) | Out-Null
& "$verdandiRoot/sdk/run-tool.ps1" -Executable go -WorkingDirectory $PSScriptRoot -Environment @{
    GOMODCACHE = Join-Path $verdandiRoot 'build/deps/go/pkg/mod'
    GOCACHE = Join-Path $verdandiRoot 'build/cache/go'
    TEMP = $verdandiTemporary; TMP = $verdandiTemporary; GOTMPDIR = $verdandiTemporary
    GOMAXPROCS = '2'
    GOTOOLCHAIN = 'local'
    GOPROXY = 'off'
    GOSUMDB = 'off'
    GOFLAGS = '-mod=readonly'
    GOWORK = 'off'
} -ToolArguments $args
exit $LASTEXITCODE
