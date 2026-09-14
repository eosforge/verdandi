# 仅供维护者明确授权后以管理员运行. 两条临时规则限定程序和两端 IP, 到期或 stop 文件出现后自动删除.
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[a-f0-9]{8}$')][string]$RunId,
    [ValidateRange(60, 1800)][int]$LifetimeSeconds = 600
)
$ErrorActionPreference = 'Stop'
$verdandiRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
$verdandiIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$verdandiPrincipal = [Security.Principal.WindowsPrincipal]::new($verdandiIdentity)
if (-not $verdandiPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator rights and explicit maintainer approval are required.'
}
$verdandiReport = Join-Path $verdandiRoot "build/testkit/firewall-$RunId.json"
$verdandiStop = Join-Path $verdandiRoot "build/testkit/firewall-$RunId.stop"
if ((Test-Path -LiteralPath $verdandiReport) -or (Test-Path -LiteralPath $verdandiStop)) { throw 'Run ID already exists.' }
$verdandiPrograms = @{
    star = Join-Path $verdandiRoot 'build/cluster/target/release/star.exe'
    supervisor = Join-Path $verdandiRoot 'build/supervisor/supervisor.exe'
}
$verdandiCreated = [Collections.Generic.List[string]]::new()
$verdandiResult = @{ status = 'starting'; rules = @(); remaining_rules = @(); local_address = '192.168.0.25'; remote_address = '192.168.0.119'; error = $null }
function Save-Result {
    [IO.File]::WriteAllText($verdandiReport, ($verdandiResult | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
}
try {
    foreach ($verdandiEntry in $verdandiPrograms.GetEnumerator()) {
        $verdandiProgram = (Resolve-Path -LiteralPath $verdandiEntry.Value).ProviderPath
        $verdandiName = "VerdandiServiceTest-$RunId-$($verdandiEntry.Key)"
        # 唯一名称先检查, 不覆盖既有规则. 每个程序仅放行这两个 IP 之间的 TCP 入站.
        if (Get-NetFirewallRule -Name $verdandiName -ErrorAction SilentlyContinue) { throw 'Firewall rule name already exists.' }
        New-NetFirewallRule -Name $verdandiName -DisplayName $verdandiName -Group "Verdandi service test $RunId" `
            -Direction Inbound -Action Allow -Protocol TCP -Program $verdandiProgram `
            -LocalAddress 192.168.0.25 -RemoteAddress 192.168.0.119 -Profile Any | Out-Null
        $verdandiCreated.Add($verdandiName)
    }
    $verdandiResult.rules = $verdandiCreated.ToArray()
    $verdandiResult.status = 'enabled'
    Save-Result
    $verdandiDeadline = [DateTime]::UtcNow.AddSeconds($LifetimeSeconds)
    while ([DateTime]::UtcNow -lt $verdandiDeadline -and -not (Test-Path -LiteralPath $verdandiStop)) { Start-Sleep -Milliseconds 250 }
} catch {
    $verdandiResult.error = $_.Exception.Message
    throw
} finally {
    # 只删除此管理员进程成功创建的确切规则, 不按应用名或宽泛通配符清理.
    $verdandiCleanupErrors = [Collections.Generic.List[string]]::new()
    foreach ($verdandiName in $verdandiCreated) {
        try { Remove-NetFirewallRule -Name $verdandiName -ErrorAction Stop }
        catch { $verdandiCleanupErrors.Add($_.Exception.Message) }
    }
    # 删除后独立读取活动规则, 只核对本次成功创建的名称. 查询失败也不能报告清理成功.
    try {
        $verdandiResult.remaining_rules = @(Get-NetFirewallRule -PolicyStore ActiveStore -ErrorAction Stop |
            Where-Object { $verdandiCreated.Contains($_.Name) } | Select-Object -ExpandProperty Name)
        if ($verdandiResult.remaining_rules.Count) { $verdandiCleanupErrors.Add('Owned firewall rules remain active.') }
    } catch { $verdandiCleanupErrors.Add($_.Exception.Message) }
    $verdandiResult.status = if ($verdandiCleanupErrors.Count) { 'cleanup_failed' } else { 'removed' }
    if ($verdandiCleanupErrors.Count) { $verdandiResult.error = $verdandiCleanupErrors -join '; ' }
    Save-Result
}
