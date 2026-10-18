# Laedt die MSVC-x64-Umgebung (vcvars64) in die aktuelle PowerShell-Sitzung.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = $null
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (-not $vsPath) {
        $vsPath = @(
            "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
            "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community"
        ) | Where-Object { Test-Path "$_\VC\Auxiliary\Build\vcvars64.bat" } | Select-Object -First 1
    }
    if (-not $vsPath) { throw "Keine MSVC-Installation (VS 2019/2022 Build Tools) gefunden." }
    $bat = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    # vcvars sucht vswhere im PATH; den Installer-Ordner ergaenzen, sonst gibt es eine harmlose Fehlermeldung.
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    cmd /c "`"$bat`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}
