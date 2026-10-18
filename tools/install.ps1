# Installiert (oder entfernt) das Plugin für die normale OBS-Installation.
#   powershell -ExecutionPolicy Bypass -File tools\install.ps1
#   powershell -ExecutionPolicy Bypass -File tools\install.ps1 -Uninstall
param(
    [switch]$Uninstall,
    [string]$Dist = "$PSScriptRoot\..\_build\dist\obs-avsync",
    [string]$Target = "$env:ProgramData\obs-studio\plugins\obs-avsync"
)
$ErrorActionPreference = "Stop"

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    Write-Warning "OBS läuft noch. Bitte OBS schließen und das Skript erneut starten."
    exit 1
}

try {
    if ($Uninstall) {
        if (Test-Path $Target) { Remove-Item -Recurse -Force $Target }
        Write-Host "AV-Sync wurde entfernt ($Target)."
        exit 0
    }
    if (-not (Test-Path "$Dist\bin\64bit\obs-avsync.dll")) {
        throw "Build nicht gefunden: $Dist (zuerst tools\build.ps1 ausführen)"
    }
    New-Item -ItemType Directory -Force "$Target\bin\64bit", "$Target\data" | Out-Null
    Copy-Item "$Dist\bin\64bit\*" "$Target\bin\64bit\" -Force
    Copy-Item "$Dist\data\*" "$Target\data\" -Recurse -Force
    Write-Host "AV-Sync installiert nach $Target"
    Write-Host "OBS starten -> Menü Docks -> 'AV-Sync'."
} catch [System.UnauthorizedAccessException] {
    Write-Warning "Keine Schreibrechte für $Target. PowerShell als Administrator starten und erneut ausführen."
    exit 1
}
