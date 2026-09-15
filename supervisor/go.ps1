# 使用项目内缓存运行 Supervisor Go, 所有设置仅作用于子进程.
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$astraRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
$astraTemporary = Join-Path $astraRoot 'build/tmp/supervisor'
[IO.Directory]::CreateDirectory($astraTemporary) | Out-Null
& "$astraRoot/sdk/run-tool.ps1" -Executable go -WorkingDirectory $PSScriptRoot -Environment @{
    GOMODCACHE = Join-Path $astraRoot 'build/deps/go/pkg/mod'
    GOCACHE = Join-Path $astraRoot 'build/cache/go'
    TEMP = $astraTemporary; TMP = $astraTemporary; GOTMPDIR = $astraTemporary
    GOMAXPROCS = '2'
    GOTOOLCHAIN = 'local'
    GOPROXY = 'off'
    GOSUMDB = 'off'
    GOFLAGS = '-mod=readonly'
    GOWORK = 'off'
} -ToolArguments $args
exit $LASTEXITCODE
