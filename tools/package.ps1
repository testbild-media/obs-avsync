# Erstellt die Release-Pakete in _build\release:
#   obs-avsync-<version>-windows-x64.zip  (Plugin + install.cmd + README)
#   obs-avsync-web-<version>.zip          (Handy-Seite, statisches HTML/JS/CSS)
param(
    [string]$Version = "1.0.0",
    [string]$BuildDir = "$PSScriptRoot\..\_build"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$dist = Join-Path $BuildDir "dist\obs-avsync"
$out = Join-Path $BuildDir "release"
$stage = Join-Path $out "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\plugin", "$stage\web" | Out-Null

Copy-Item $dist "$stage\plugin\obs-avsync" -Recurse
Copy-Item "$root\README.md", "$root\LICENSE" "$stage\plugin\"
@"
@echo off
rem Installiert AV-Sync fuer OBS Studio (Windows, 64 Bit). OBS vorher schliessen.
xcopy /E /I /Y "%~dp0obs-avsync" "%ProgramData%\obs-studio\plugins\obs-avsync" >nul
if errorlevel 1 (
  echo Installation fehlgeschlagen - bitte als Administrator ausfuehren.
) else (
  echo AV-Sync installiert. OBS starten, Menue Docks -^> AV-Sync.
)
pause
"@ | Set-Content -Encoding ascii "$stage\plugin\install.cmd"

Copy-Item "$root\web\*" "$stage\web\" -Recurse

$pluginZip = Join-Path $out "obs-avsync-$Version-windows-x64.zip"
$webZip = Join-Path $out "obs-avsync-web-$Version.zip"
Remove-Item $pluginZip, $webZip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\plugin\*" -DestinationPath $pluginZip
Compress-Archive -Path "$stage\web\*" -DestinationPath $webZip
Remove-Item -Recurse -Force $stage
Get-ChildItem $out | Select-Object Name, Length
