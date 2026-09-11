# Thin PowerShell adapter; all test policy belongs to run.py.
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
$verdandiPython = Join-Path $verdandiRoot 'build/tools/python-build/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $verdandiPython)) {
    $verdandiCandidates = @(Get-Command python, python3 -CommandType Application -All -ErrorAction SilentlyContinue | ForEach-Object Source)
    if ($env:USERPROFILE) { $verdandiCandidates += Join-Path $env:USERPROFILE '.local/bin/python.exe' }
    $verdandiPython = $verdandiCandidates | Where-Object { (Test-Path -LiteralPath $_ -PathType Leaf) -and (Get-Item -LiteralPath $_).Length -gt 0 } | Select-Object -First 1
}
if (-not $verdandiPython) { throw 'An existing Python 3.10+ interpreter is required.' }
$verdandiSettings = @{ PYTHONIOENCODING = 'utf-8' }
if ('--plan' -notin $args -and '-h' -notin $args -and '--help' -notin $args) {
    if (-not $env:VERDANDI_TEST_SSH_PASSWORD) {
        $verdandiSecret = Read-Host 'Ubuntu SSH/sudo password (not saved)' -AsSecureString
        $verdandiSecretPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($verdandiSecret)
        try { $verdandiSettings.VERDANDI_TEST_SSH_PASSWORD = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($verdandiSecretPointer) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($verdandiSecretPointer) }
    }
}
$verdandiCancelDirectory = Join-Path $verdandiRoot 'build/testkit/cancel'
[IO.Directory]::CreateDirectory($verdandiCancelDirectory) | Out-Null
$verdandiCancelFile = Join-Path $verdandiCancelDirectory ([Guid]::NewGuid().ToString('N') + '.request')
$verdandiSettings.VERDANDI_TEST_CANCEL_FILE = $verdandiCancelFile
$verdandiSettings.VERDANDI_TEST_PARENT_PID = [string]$PID
& "$verdandiRoot/sdk/run-tool.ps1" -Executable $verdandiPython -WorkingDirectory $verdandiRoot -Environment $verdandiSettings -CancellationFile $verdandiCancelFile -ToolArguments (@('-B', "$PSScriptRoot/run.py") + $args)
exit $LASTEXITCODE
