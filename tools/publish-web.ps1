# Veröffentlicht die Handy-Seite (web/) im github.io-Repository der Organisation,
# Unterordner obs-avsync/ -> https://testbild-media.github.io/obs-avsync/
#   powershell -ExecutionPolicy Bypass -File tools\publish-web.ps1
param(
    [string]$SiteRepo = "https://github.com/testbild-media/testbild-media.github.io.git",
    [string]$SubDir = "obs-avsync",
    [string]$Clone = "$PSScriptRoot\..\_deps\github-io",
    [switch]$NoPush
)
# no global "Stop": git writes progress to stderr, failures are checked via $LASTEXITCODE
$ErrorActionPreference = "Continue"
$root = (Resolve-Path "$PSScriptRoot\..").Path

if (-not (Test-Path "$Clone\.git")) {
    git clone -q $SiteRepo $Clone
    if ($LASTEXITCODE -ne 0) { throw "git clone fehlgeschlagen" }
} else {
    git -C $Clone pull -q --ff-only
    if ($LASTEXITCODE -ne 0) { throw "git pull fehlgeschlagen" }
}

$target = Join-Path $Clone $SubDir
if (Test-Path $target) { Remove-Item -Recurse -Force $target -ErrorAction Stop }
New-Item -ItemType Directory -Force $target -ErrorAction Stop | Out-Null
Copy-Item "$root\web\*" $target -Recurse -ErrorAction Stop

git -C $Clone add -A $SubDir
git -C $Clone diff --cached --quiet
if ($LASTEXITCODE -eq 0) { Write-Host "Keine Änderungen an der Handy-Seite."; exit 0 }

$rev = git -C $root rev-parse --short HEAD
git -C $Clone commit -q -m "AV-Sync Handy-Seite aktualisiert (obs-avsync $rev)"
if ($NoPush) { Write-Host "Commit erstellt, nicht gepusht ($Clone)."; exit 0 }
git -C $Clone push -q
if ($LASTEXITCODE -ne 0) { throw "git push fehlgeschlagen" }
Write-Host "Veröffentlicht: https://testbild-media.github.io/$SubDir/ (GitHub braucht ca. 1 Minute)"
