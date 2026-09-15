# Windows 入口明确报告当前平台限制, 不隐式进入 WSL、下载 GCC 或连接虚拟机.
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$astraRoot = Split-Path -Parent $PSScriptRoot
$astraPython = Join-Path $astraRoot 'build/tools/python-build/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $astraPython)) { $astraPython = 'python' }
& $astraPython -B (Join-Path $PSScriptRoot 'build.py') @args
exit $LASTEXITCODE
