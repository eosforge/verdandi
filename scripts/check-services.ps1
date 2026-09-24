# 当前 C++26 服务仅支持 Linux, 不以无效选项或不可到达的逻辑报告检查通过.
param(
    [ValidateSet('all', 'star')][string]$Service = 'all',
    [ValidateSet('cpp')][string]$Implementation = 'cpp',
    [ValidateRange(0, 256)][int]$Jobs = 0
)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
throw 'C++26 service checks require Linux. Run bash scripts/check-services.sh there; no checks were executed.'
