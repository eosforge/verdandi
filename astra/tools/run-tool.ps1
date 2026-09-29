# Go/Rust/C++ 项目入口共用的子进程启动器；环境只写入子进程启动参数。
# 不修改调用终端的环境或工作目录，也不写用户/系统配置。
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$WorkingDirectory,
    [Parameter(Mandatory = $true)][hashtable]$Environment,
    [string[]]$ToolArguments = @(),
    [string]$CancellationFile
)

$ErrorActionPreference = 'Stop'
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = (Get-Command $Executable -CommandType Application -ErrorAction Stop).Source
$startInfo.WorkingDirectory = $WorkingDirectory
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
foreach ($entry in $Environment.GetEnumerator()) {
    $startInfo.EnvironmentVariables[$entry.Key] = $entry.Value
}

# PowerShell 7 使用原生参数列表；Windows PowerShell 5.1 按 Windows 规则转义，
# 保留空参数、空格、嵌入引号和末尾反斜杠；不经过 cmd 或拼接 shell 命令。
if ($null -ne $startInfo.PSObject.Properties['ArgumentList']) {
    foreach ($argument in $ToolArguments) { $startInfo.ArgumentList.Add($argument) }
} else {
    $startInfo.Arguments = ($ToolArguments | ForEach-Object {
        '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
    }) -join ' '
}

# 逐行传递标准输出以支持 PowerShell 管道；标准错误和输入直接继承父进程。
# 不缓存整份构建日志；退出时保留工具的原始退出码。
$originalEncoding = [Console]::OutputEncoding
$process = $null
try {
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $process = [System.Diagnostics.Process]::Start($startInfo)
    while ($true) {
        $verdandiRead = $process.StandardOutput.ReadLineAsync()
        while (-not $verdandiRead.IsCompleted) { Start-Sleep -Milliseconds 100 }
        $line = $verdandiRead.GetAwaiter().GetResult()
        if ($null -eq $line) { break }
        Write-Output $line
    }
    $process.WaitForExit()
    exit $process.ExitCode
} finally {
    if ($null -ne $process) {
        if ($CancellationFile -and -not $process.HasExited) {
            [IO.File]::WriteAllText($CancellationFile, 'cancel')
            if (-not $process.WaitForExit(60000)) { $process.Kill() }
        }
        $process.Dispose()
    }
    [Console]::OutputEncoding = $originalEncoding
}
