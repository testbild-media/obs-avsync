# Lädt alles, was zum Bauen nötig ist, nach _deps:
#   - OBS-Quelltext (nur Header) passend zur installierten OBS-Version
#   - offizielles OBS-Qt6-SDK (obs-deps), Hash wird geprüft
#   - Import-Libraries aus der installierten obs.dll / obs-frontend-api.dll
param(
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [string]$QtDepsVersion = "2025-08-23",
    [string]$QtDepsHash = "c62e82483bc7c0bf199e8ac3220c66a85a6e8a0cd69a05b6d44f873b830e415f"
)
$ErrorActionPreference = "Stop"
$deps = (Join-Path $PSScriptRoot "..\_deps")
New-Item -ItemType Directory -Force $deps | Out-Null

$obsVersion = (Get-Item "$ObsDir\bin\64bit\obs.dll").VersionInfo.ProductVersion
Write-Host "Installierte OBS-Version: $obsVersion"

# 1) OBS headers (sparse checkout)
$src = Join-Path $deps "obs-studio"
# git writes progress to stderr: no global "Stop" for the git calls, check exit codes instead
$ErrorActionPreference = "Continue"
if (-not (Test-Path "$src\.git")) {
    git clone -q --depth 1 --branch $obsVersion --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git $src
    if ($LASTEXITCODE -ne 0) { throw "git clone fehlgeschlagen" }
} else {
    git -C $src fetch -q --depth 1 origin tag $obsVersion
    if ($LASTEXITCODE -ne 0) { throw "git fetch fehlgeschlagen" }
    git -C $src checkout -q $obsVersion
    if ($LASTEXITCODE -ne 0) { throw "git checkout fehlgeschlagen" }
}
git -C $src sparse-checkout set libobs frontend/api
if ($LASTEXITCODE -ne 0) { throw "git sparse-checkout fehlgeschlagen" }
$ErrorActionPreference = "Stop"

# 2) Qt6 SDK (official obs-deps build, Qt 6.8 - also builds with VS 2019)
$qt = Join-Path $deps "qt6"
if (-not (Test-Path "$qt\lib\cmake\Qt6")) {
    $zipDir = Join-Path $deps "qt6-dl"
    New-Item -ItemType Directory -Force $zipDir | Out-Null
    $zip = Join-Path $zipDir "qt6.zip"
    $url = "https://github.com/obsproject/obs-deps/releases/download/$QtDepsVersion/windows-deps-qt6-$QtDepsVersion-x64.zip"
    Write-Host "Lade $url ..."
    Invoke-WebRequest -Uri $url -OutFile $zip
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $QtDepsHash) { throw "SHA256 stimmt nicht: $hash" }
    New-Item -ItemType Directory -Force $qt | Out-Null
    tar -xf $zip -C $qt
    if ($LASTEXITCODE -ne 0) { throw "Entpacken fehlgeschlagen" }
}

# 3) import libraries
& "$PSScriptRoot\make-implibs.ps1" -ObsDir $ObsDir
Write-Host "Fertig. Bauen mit: tools\build.ps1"
