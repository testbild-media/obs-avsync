# Starts the portable OBS on a scene, collects /api/status results and stops OBS again.
param(
    [Parameter(Mandatory)] [string]$Target,
    [string]$Scene = "Testsignal",
    [int]$Seconds = 40,
    [int]$Port = 8765,
    [string]$Snapshot = "",
    [string]$StatusOut = ""
)
$ErrorActionPreference = "Stop"

$exeDir = "$Target\bin\64bit"
$env:AVSYNC_SHOW_DOCK = "1"
if ($Snapshot) { $env:AVSYNC_DOCK_SNAPSHOT = $Snapshot } else { Remove-Item env:AVSYNC_DOCK_SNAPSHOT -ErrorAction SilentlyContinue }

$obsArgs = @("--portable", "--multi", "--disable-updater", "--disable-missing-files-check", "--collection", "AVSyncTest", "--profile", "AVSyncTest", "--scene", $Scene)
$proc = Start-Process -FilePath "$exeDir\obs64.exe" -ArgumentList $obsArgs -WorkingDirectory $exeDir -PassThru
Write-Host "OBS gestartet (PID $($proc.Id)), Szene '$Scene', $Seconds s ..."

$status = $null
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if ($proc.HasExited) { Write-Host "OBS wurde beendet (Exit $($proc.ExitCode))"; break }
    try {
        $status = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/status" -TimeoutSec 3
        $line = ($status.probes | ForEach-Object {
            "{0}/{1}: n={2} med={3} sig={4}" -f $_.kind, $_.name, $_.total.count, $_.live.median, $_.signal.text
        }) -join " | "
        Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $line)
    } catch {
        Write-Host ("[{0:HH:mm:ss}] noch keine Antwort: {1}" -f (Get-Date), $_.Exception.Message)
    }
}

if ($status -and $StatusOut) { $status | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 $StatusOut }

if (-not $proc.HasExited) {
    $null = $proc.CloseMainWindow()
    if (-not $proc.WaitForExit(15000)) { Stop-Process -Id $proc.Id -Force; Write-Host "OBS hart beendet" }
}
$log = Get-ChildItem "$Target\config\obs-studio\logs" -Filter *.txt | Sort-Object LastWriteTime | Select-Object -Last 1
Write-Host "--- Log: $($log.FullName)"
Select-String -Path $log.FullName -Pattern "avsync|obs-avsync|error|crash|Failed to load" | Select-Object -Last 40 | ForEach-Object { $_.Line }

