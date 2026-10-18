# Konfiguriert und baut das Projekt (Ninja + MSVC).
param(
    [ValidateSet("Release", "RelWithDebInfo", "Debug")]
    [string]$Config = "RelWithDebInfo",
    [string]$BuildDir = "$PSScriptRoot\..\_build",
    [switch]$NoPlugin,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\msvc-env.ps1"

$root = (Resolve-Path "$PSScriptRoot\..").Path
if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }

$plugin = if ($NoPlugin) { "OFF" } else { "ON" }
cmake -S $root -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DAVSYNC_BUILD_PLUGIN=$plugin" "-DCMAKE_PREFIX_PATH=$root\_deps\qt6"
if ($LASTEXITCODE -ne 0) { throw "CMake-Konfiguration fehlgeschlagen" }
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Build fehlgeschlagen" }
