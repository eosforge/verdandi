# 当前 C++26 服务仅支持 Linux. Windows 明确失败, 不保留永远无法到达的旧测试分支.
param(
    [ValidateSet('regression', 'soak')][string]$Mode = 'regression',
    [ValidateSet('cpp')][string]$Implementation = 'cpp',
    [ValidateRange(0, 256)][int]$Jobs = 0,
    [ValidateRange(0, 256)][int]$TestJobs = 0,
    [switch]$SkipChecks
)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
throw 'C++26 service tests require Linux. Run bash scripts/test-services.sh --mode=regression there; no tests were executed.'
