$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$reportDir = Join-Path $repoRoot 'artifacts/environment'
New-Item -ItemType Directory -Force $reportDir | Out-Null
$commands = @(@('--version'), @('--status'), @('--list', '--verbose'), @('--list', '--online'))
$results = @()
foreach ($arguments in $commands) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = 'wsl.exe'
    $start.Arguments = $arguments -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::Unicode
    $start.StandardErrorEncoding = [Text.Encoding]::Unicode
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw 'Failed to start wsl.exe' }
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) {
        $process.Kill()
        throw ('WSL diagnostic timed out: ' + $start.Arguments)
    }
    $results += [ordered]@{ argv = @('wsl.exe') + $arguments; exit_code = $process.ExitCode; stdout = $outTask.Result; stderr = $errTask.Result }
}
$audit = [ordered]@{
    collected_at = (Get-Date).ToString('o')
    os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,TotalVisibleMemorySize,FreePhysicalMemory
    cpu = Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors
    disks = Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' | Select-Object DeviceID,Size,FreeSpace
    wslconfig = if (Test-Path -LiteralPath "$env:USERPROFILE\.wslconfig") { Get-Content -Raw -LiteralPath "$env:USERPROFILE\.wslconfig" } else { $null }
    commands = $results
}
$audit | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $reportDir 'windows.json')
$audit | ConvertTo-Json -Depth 8
