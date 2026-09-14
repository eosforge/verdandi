# Windows 入口明确报告当前平台限制, 不隐式进入 WSL、下载 GCC 或连接虚拟机.
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiRoot = Split-Path -Parent $PSScriptRoot
$verdandiPython = Join-Path $verdandiRoot 'build/tools/python-build/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $verdandiPython)) { $verdandiPython = 'python' }
& $verdandiPython -B (Join-Path $PSScriptRoot 'build.py') @args
exit $LASTEXITCODE
