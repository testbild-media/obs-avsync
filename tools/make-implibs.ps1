# Erzeugt MSVC-Import-Libraries (obs.lib, obs-frontend-api.lib) aus einer
# installierten OBS-Version. Damit muss libobs nicht selbst gebaut werden.
param(
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [string]$OutDir = "$PSScriptRoot\..\_deps\implib"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\msvc-env.ps1"

New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($name in @("obs", "obs-frontend-api")) {
    $dll = Join-Path $ObsDir "bin\64bit\$name.dll"
    if (-not (Test-Path $dll)) { throw "Nicht gefunden: $dll" }
    $exports = & dumpbin /nologo /exports $dll
    $names = foreach ($line in $exports) {
        # Format: "    ordinal hint RVA      name"
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $Matches[1] }
    }
    $def = Join-Path $OutDir "$name.def"
    @("LIBRARY `"$name.dll`"", "EXPORTS") + ($names | ForEach-Object { "    $_" }) |
        Set-Content -Encoding ascii $def
    $lib = Join-Path $OutDir "$name.lib"
    & lib /nologo "/def:$def" /machine:x64 "/out:$lib" | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $lib)) { throw "lib.exe fehlgeschlagen fuer $name" }
    Write-Host "$name.lib: $($names.Count) Exporte"
}
