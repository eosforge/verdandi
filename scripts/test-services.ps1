# 一键服务回归或长时测试. 工具和依赖必须预先准备, 全部缓存与结果位于项目 build/.
param(
    [ValidateSet('regression', 'soak')][string]$Mode = 'regression',
    [ValidateSet('cpp')][string]$Implementation = 'cpp',
    [ValidateRange(1, 604800)][int]$Duration = 60,
    [switch]$SkipChecks,
    [string]$Address = '127.0.0.1',
    [string]$RemoteConfig = ''
)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$astraRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
if ($Implementation -eq 'cpp') { throw 'C++26 services require Linux; the Rust service is retired.' }
if (-not $SkipChecks) {
    & "$astraRoot/scripts/check-services.ps1" -Implementation $Implementation
    if ($LASTEXITCODE -ne 0) { throw 'Service checks failed.' }
}
$astraPython = Join-Path $astraRoot 'build/tools/python-build/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $astraPython -PathType Leaf)) { throw 'Prepare the approved project-local Python environment first.' }
if (-not $SkipChecks) {
    & "$astraRoot/sdk/run-tool.ps1" -Executable $astraPython -WorkingDirectory $astraRoot -Environment @{
        PYTHONDONTWRITEBYTECODE = '1'; PYTHONIOENCODING = 'utf-8'
    } -ToolArguments @('-B', '-m', 'unittest', 'discover', '-s', 'testkit/tests', '-p', 'test_*.py')
    if ($LASTEXITCODE -ne 0) { throw 'Test harness checks failed.' }
}
$astraArguments = @('-B', '-m', 'testkit.services', '--implementation', $Implementation, '--mode', $Mode, '--duration', "$Duration", '--address', $Address)
if ($RemoteConfig) { $astraArguments += @('--remote-config', (Resolve-Path -LiteralPath $RemoteConfig).ProviderPath) }
& "$astraRoot/sdk/run-tool.ps1" -Executable $astraPython -WorkingDirectory $astraRoot -Environment @{
    PYTHONDONTWRITEBYTECODE = '1'; PYTHONIOENCODING = 'utf-8'
} -ToolArguments $astraArguments
exit $LASTEXITCODE
