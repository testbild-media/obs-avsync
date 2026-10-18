# Creates an isolated portable OBS copy with the plugin and a test scene collection.
# The user's own OBS configuration is not touched.
param(
    [Parameter(Mandatory)] [string]$Target,
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [string]$Dist = "$PSScriptRoot\..\..\_build\dist\obs-avsync",
    [int]$Port = 8765
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path "$Target\bin\64bit\obs64.exe")) {
    Write-Host "Kopiere OBS nach $Target ..."
    robocopy $ObsDir $Target /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy fehlgeschlagen ($LASTEXITCODE)" }
}
Set-Content -Path "$Target\portable_mode.txt" -Value "" -Encoding ascii

# plugin (legacy layout inside the OBS folder)
Copy-Item "$Dist\bin\64bit\*" "$Target\obs-plugins\64bit\" -Force
New-Item -ItemType Directory -Force "$Target\data\obs-plugins\obs-avsync" | Out-Null
Copy-Item "$Dist\data\*" "$Target\data\obs-plugins\obs-avsync\" -Recurse -Force

# config
$cfg = "$Target\config\obs-studio"
New-Item -ItemType Directory -Force "$cfg\basic\profiles\AVSyncTest", "$cfg\basic\scenes", "$cfg\plugin_config\obs-avsync" | Out-Null
@"
[General]
LastVersion=536870916
Pre31Migrated=true
EnableAutoUpdates=false
InfoIncrement=999
MaxLogs=5

[Video]
Renderer=Direct3D 11
"@ | Set-Content -Encoding utf8 "$cfg\global.ini"
@"
[General]
FirstRun=true
ConfirmOnExit=false
Language=de-DE
WarnedAboutClosingDocks=true

[Basic]
Profile=AVSyncTest
ProfileDir=AVSyncTest
SceneCollection=AVSyncTest
SceneCollectionFile=AVSyncTest

[BasicWindow]
PreviewEnabled=true
SysTrayEnabled=false
"@ | Set-Content -Encoding utf8 "$cfg\user.ini"
@"
[General]
Name=AVSyncTest

[Video]
BaseCX=1280
BaseCY=720
OutputCX=1280
OutputCY=720
FPSType=0
FPSCommon=60

[Audio]
SampleRate=48000
ChannelSetup=Stereo
"@ | Set-Content -Encoding utf8 "$cfg\basic\profiles\AVSyncTest\basic.ini"

# plugin config: fixed port, CSV next to the portable copy
$csvDir = Join-Path $Target "csv"
@{ port = $Port; csv_folder = $csvDir; session_duration_s = 15 } | ConvertTo-Json | Set-Content -Encoding utf8 "$cfg\plugin_config\obs-avsync\config.json"

function New-Source($id, $name, $settings, $filters) {
    [ordered]@{
        id = $id; versioned_id = $id; name = $name; uuid = [guid]::NewGuid().ToString()
        settings = $settings; filters = @($filters); enabled = $true; muted = $false
        volume = 1.0; mixers = 255; monitoring_type = 0; sync = 0; flags = 0; balance = 0.5
        private_settings = @{}
    }
}
function New-Filter($id, $name, $settings) {
    [ordered]@{ id = $id; versioned_id = $id; name = $name; uuid = [guid]::NewGuid().ToString(); settings = $settings; enabled = $true }
}
function New-Scene($name, $source, $itemId) {
    $item = [ordered]@{
        name = $source.name; source_uuid = $source.uuid; visible = $true; locked = $false; id = $itemId
        pos = @{ x = 0.0; y = 0.0 }; scale = @{ x = 1.0; y = 1.0 }; rot = 0.0; align = 5
        bounds_type = 0; bounds_align = 0; bounds = @{ x = 0.0; y = 0.0 }
        crop_left = 0; crop_top = 0; crop_right = 0; crop_bottom = 0; scale_filter = "disable"
        blend_method = "default"; blend_type = "normal"; private_settings = @{}
    }
    New-Source "scene" $name ([ordered]@{ items = @($item); id_counter = $itemId; custom_size = $false }) @()
}

$sensorFilters = { @(
    (New-Filter "avsync_video_sensor" "AV-Sync Video-Sensor" @{ overlay = $true }),
    (New-Filter "avsync_audio_sensor" "AV-Sync Audio-Sensor" @{})
) }
$test = New-Source "avsync_test_signal" "Testsignal-Quelle" @{ width = 1280; height = 720; volume_db = -12.0 } (& $sensorFilters)
$browser = New-Source "browser_source" "Handy-Seite (Browser)" ([ordered]@{
    url = "http://127.0.0.1:$Port/?auto=1&name=OBS-Browser"; width = 1280; height = 720
    fps_custom = $true; fps = 60; reroute_audio = $true; shutdown = $false; restart_when_active = $false
}) (& $sensorFilters)
$sceneTest = New-Scene "Testsignal" $test 1
$sceneBrowser = New-Scene "Browser" $browser 1

$collection = [ordered]@{
    name = "AVSyncTest"
    current_scene = "Testsignal"; current_program_scene = "Testsignal"
    scene_order = @(@{ name = "Testsignal" }, @{ name = "Browser" })
    sources = @($test, $browser, $sceneTest, $sceneBrowser)
    groups = @(); transition_duration = 300
}
$collection | ConvertTo-Json -Depth 12 | Set-Content -Encoding utf8 "$cfg\basic\scenes\AVSyncTest.json"
Write-Host "Portable OBS bereit: $Target"
