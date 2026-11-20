param(
    [ValidateSet('Start', 'Stop', 'Status', 'Install', 'Uninstall')]
    [string]$Action = 'Status',
    [string]$InterfaceAlias = 'WLAN',
    [string]$Hostname = 'lmstudio',
    [ValidateRange(1, 65535)][int]$Port = 1234
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$python = Join-Path $project '.tools\lmstudio-mdns\Scripts\pythonw.exe'
$publisher = Join-Path $PSScriptRoot 'lmstudio_mdns.py'
$state = Join-Path $project '.tools\lmstudio-mdns-state'
$startup = Join-Path ([Environment]::GetFolderPath('Startup')) 'CClaw-LMStudio-mDNS.lnk'
$pidFile = Join-Path $state 'process.json'
$stopFile = Join-Path $state 'stop'
New-Item -ItemType Directory -Path $state -Force | Out-Null

function Get-PublisherProcess {
    if (Test-Path -LiteralPath $pidFile) {
        $saved = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
        $process = Get-Process -Id $saved.pid -ErrorAction SilentlyContinue
        if ($process -and $process.Path -eq $python -and $process.StartTime.ToUniversalTime().Ticks.ToString() -eq $saved.startTicks) {
            return $process
        }
    }
    return $null
}

if ($Action -eq 'Status') {
    $process = Get-PublisherProcess
    Write-Output "Running=$([bool]$process)"
    $statusFile = Join-Path $state 'status.json'
    if (Test-Path -LiteralPath $statusFile) { Get-Content -LiteralPath $statusFile }
    Write-Output "LoginAutostart=$(Test-Path -LiteralPath $startup)"
    exit
}
if ($Action -in @('Stop', 'Uninstall')) {
    if ($Action -eq 'Uninstall' -and (Test-Path -LiteralPath $startup)) {
        Remove-Item -LiteralPath $startup
    }
    $process = Get-PublisherProcess
    if ($process) {
        New-Item -ItemType File -Path $stopFile -Force | Out-Null
        if (-not $process.WaitForExit(15000)) { throw 'Publisher did not stop within 15 seconds; inspect publisher.log.' }
    }
    Write-Output 'Publisher stopped.'
    exit
}
if (-not (Test-Path -LiteralPath $python)) { throw 'Missing runtime: see README-lmstudio-mdns.md.' }
if ($Hostname -notmatch '^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$') { throw 'Use a single hostname label without .local.' }
$interfaces = @([System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | Where-Object { $_.Name -eq $InterfaceAlias })
if ($interfaces.Count -ne 1) { throw "Adapter '$InterfaceAlias' was not found uniquely." }
$description = $interfaces[0].Description
if ($Action -eq 'Install') {
    if ($InterfaceAlias.Contains('"')) { throw 'Invalid adapter alias.' }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($startup)
    $shortcut.TargetPath = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $shortcut.Arguments = '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}" -Action Start -InterfaceAlias "{1}" -Hostname "{2}" -Port {3}' -f $PSCommandPath, $InterfaceAlias, $Hostname, $Port
    $shortcut.WorkingDirectory = $project
    $shortcut.WindowStyle = 7
    $shortcut.Save()
    Write-Output 'Login autostart installed for the current Windows user.'
}
if (Get-PublisherProcess) { Write-Output 'Publisher already running; stop it first to change settings.'; exit }
if (Test-Path -LiteralPath $stopFile) { Remove-Item -LiteralPath $stopFile }
$arguments = '"{0}" --interface "{1}" --hostname "{2}" --port {3} --state-dir "{4}"' -f $publisher, $description, $Hostname, $Port, $state
$process = Start-Process -FilePath $python -ArgumentList $arguments -WorkingDirectory $project -WindowStyle Hidden -PassThru
@{ pid = $process.Id; startTicks = $process.StartTime.ToUniversalTime().Ticks.ToString() } | ConvertTo-Json | Set-Content -LiteralPath $pidFile -Encoding UTF8
Start-Sleep -Seconds 3
if ($process.HasExited) { throw 'Publisher exited; inspect .tools\lmstudio-mdns-state\publisher.log.' }
Write-Output "Publisher started. Base URL: http://${Hostname}.local:${Port}"
