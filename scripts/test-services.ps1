# 一键服务回归或长时测试. 工具和依赖必须预先准备, 全部缓存与结果位于项目 build/.
param(
    [ValidateSet('regression', 'soak')][string]$Mode = 'regression',
    [ValidateRange(1, 604800)][int]$Duration = 60,
    [switch]$SkipChecks,
    [string]$Address = '127.0.0.1',
    [string]$RemoteConfig = ''
)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
if (-not $SkipChecks) {
    & "$verdandiRoot/scripts/check-services.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'Service checks failed.' }
}
$verdandiPython = Join-Path $verdandiRoot 'build/tools/python-build/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $verdandiPython -PathType Leaf)) { throw 'Prepare the approved project-local Python environment first.' }
if (-not $SkipChecks) {
    & "$verdandiRoot/sdk/run-tool.ps1" -Executable $verdandiPython -WorkingDirectory $verdandiRoot -Environment @{
        PYTHONDONTWRITEBYTECODE = '1'; PYTHONIOENCODING = 'utf-8'
    } -ToolArguments @('-B', '-m', 'unittest', 'discover', '-s', 'testkit/tests', '-p', 'test_*.py')
    if ($LASTEXITCODE -ne 0) { throw 'Test harness checks failed.' }
}
$verdandiArguments = @('-B', '-m', 'testkit.services', '--mode', $Mode, '--duration', "$Duration", '--address', $Address)
if ($RemoteConfig) { $verdandiArguments += @('--remote-config', (Resolve-Path -LiteralPath $RemoteConfig).ProviderPath) }
& "$verdandiRoot/sdk/run-tool.ps1" -Executable $verdandiPython -WorkingDirectory $verdandiRoot -Environment @{
    PYTHONDONTWRITEBYTECODE = '1'; PYTHONIOENCODING = 'utf-8'
} -ToolArguments $verdandiArguments
exit $LASTEXITCODE
