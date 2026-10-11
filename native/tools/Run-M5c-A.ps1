# AKN stage 5c part A: deploy AKVR (engine stereo mode) + AKN, launch on the FAKE headset (xrsim), wait for the title
# screen. Menus are then clicked by hand-checked steps (title -> current save -> Continue Story), then Run-M5c-B.ps1.
# Never touches the registry: the fake headset is chosen for the game process only (XR_RUNTIME_JSON).
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$xs = "$env:LOCALAPPDATA\AKN\xrsim"
$root = "D:\Documents\Antigravity\VR_Modding"

# another session's game: wait for it to close, then start (JJ 2026-10-07)
$waited = 0
while (Get-Process | Where-Object { $_.Path -like "*steamapps*" }) {
    if ($waited % 300 -eq 0) { "{0:HH:mm:ss} another game is running ({1}) - waiting" -f (Get-Date), ((Get-Process | Where-Object { $_.Path -like "*steamapps*" }).Name -join ", ") }
    Start-Sleep 15; $waited += 15
    if ($waited -ge 4 * 3600) { "gave up waiting after 4 h"; exit 1 }
}
if ($waited) { "{0:HH:mm:ss} the other game closed - starting in 20 s" -f (Get-Date); Start-Sleep 20 }

# deploy (game closed): back up AKVR's dll + settings first (handoff rule), then copy both mod DLLs
$bk = "$root\akvr\diagnostics\before-AKN5C-20261007"
if (-not (Test-Path $bk)) {
    New-Item -ItemType Directory -Force -Path $bk | Out-Null
    Copy-Item "$g\dinput8.dll", "$g\akvr_settings.ini" $bk -Force
}
Copy-Item "$root\akvr\build-dinput8\Release\dinput8.dll" "$g\dinput8.dll" -Force
Copy-Item "$root\akvr_native\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force

if (-not (Test-Path "$g\akn\nvngx_dlss.dll")) { Copy-Item "$root\akvr_native\third_party\ngx\bin\nvngx_dlss.dll" "$g\akn\nvngx_dlss.dll" -Force }   # DLSS D1
"deployed: dinput8 {0}  xinput {1}" -f (Get-FileHash "$g\dinput8.dll").Hash.Substring(0, 12), (Get-FileHash "$g\XINPUT9_1_0.dll").Hash.Substring(0, 12)

# fake headset manifest (AC6VR's xrsim build), own folder so other sessions' sims are not touched
$dll = "$root\ac6vr\build\Release\ac6vr_xrsim64.dll"
if (-not (Test-Path $dll)) { "fake headset dll missing: $dll"; exit 1 }
New-Item -ItemType Directory -Force -Path "$xs\capture" | Out-Null
$manifest = "$xs\akn_xrsim64.json"
[System.IO.File]::WriteAllText($manifest, '{"file_format_version":"1.0.0","runtime":{"name":"akn-xrsim","library_path":"' + $dll.Replace('\', '\\') + '"}}', [System.Text.UTF8Encoding]::new($false))
foreach ($leaf in "command.txt", "state.json", "ack.txt") { if (Test-Path "$xs\$leaf") { Remove-Item "$xs\$leaf" -Force -Confirm:$false } }

$env:XR_RUNTIME_JSON = $manifest
$env:BVR_XRSIM_DIR = $xs
$env:SteamAppId = "208650"
Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akvr", "-akn", "-aknstereo" -WorkingDirectory $g
"{0:HH:mm:ss} launched (fake headset at $xs)" -f (Get-Date)
# JJ's Quest run 2026-10-07: Virtual Desktop's own in-process device polluted our hooks; the fake headset has none.
# A ROGUE device (second D3D11 device drawing every 3 ms on its own thread) stands in for it for the whole session.
Start-Sleep 4; Set-Content "$akn\command.txt" "rogue 2"   # 2 = single-threaded = its own context table, like VD's; before any 3D
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep 3
    if (-not (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { "game exited during start-up"; exit 1 }
    $st = Get-Content "$xs\state.json" -Raw -ErrorAction SilentlyContinue
    if ($st -match '"sessionRunning":\s*true') { break }
}
"{0:HH:mm:ss} fake headset session running: {1}" -f (Get-Date), ([bool]($st -match '"sessionRunning":\s*true'))
# JJ's Quest via Virtual Desktop (his session log): L tan -1.1699 0.7132 up 0.8208 down -1.2139 = 49.48 / 35.50 / 39.38 /
# 50.52 deg. The xrsim default is vertically symmetric, which hid AKVR's geo-11 vertical corrections (3rd Quest test).
# (Run 11: sent before the session existed, the command threw and ended the script - now after it, and never fatal.)
try { & "$root\ac6vr\tools\xrsim-tools\xrsim-cmd.ps1" -Dir $xs -Quiet "fov eye l -49.48 35.50 39.38 -50.52" "fov eye r -35.50 49.48 39.38 -50.52" | Out-Null; "fake headset lens set to JJ's Quest numbers" }
catch { "fake headset lens NOT set: $_" }
$logs = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\BmGame\Logs"
$t0 = (Get-Process BatmanAK -ErrorAction SilentlyContinue).StartTime
for ($i = 0; $i -lt 4; $i++) {   # MENUTIMING 2026-10-08: the proven gaps of the earlier runs are back (MENUFAST's shorter
    Start-Sleep 5                 # ones lost the title click while the game still drew nothing); only the menu captures
    if (Get-ChildItem "$logs\UE3AutoReportDump*.txt" | Where-Object { $_.LastWriteTime -gt $t0 }) { "CRASHED during start-up - force-closing"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force; exit 2 }
}                                 # and the main-menu headset shot stay out
python "$PSScriptRoot\akmenu.py" click 0.5 0.4     # JJ: one click skips every logo straight to the first menu
Start-Sleep 6
# JJ's path to gameplay (verified 2026-10-07 05:13 from HUD captures): title "CLICK HERE TO START" at (0.352, 0.523)
# of the client area, then Enter on the save (the only one, "Bleake Island"), then Enter on "Continue Story".
python "$PSScriptRoot\akmenu.py" shot "$akn\menu_1_title.png"
python "$PSScriptRoot\akmenu.py" click 0.352 0.523; Start-Sleep 8
python "$PSScriptRoot\akmenu.py" shot "$akn\menu_2_save.png"
python "$PSScriptRoot\akmenu.py" key 0x0D; Start-Sleep 8
python "$PSScriptRoot\wait_steady.py" 5 20 60
python "$PSScriptRoot\akmenu.py" shot "$akn\menu_3_main.png"
# MENUSHOTS / LOADSHOT are opt-in ($env:AKN_MENUSHOTS / $env:AKN_LOADSHOT = 1): JJ wants straight through the menus,
# and the extra work here made the Continue Enter miss (LD runs 2026-10-08)
if ($env:AKN_MENUSHOTS) {
    Set-Content "$akn\command.txt" "capmk mmain"; Start-Sleep 4   # HM: the main menu's HUD sheet (JJ: button prompts out of position)
    try { $s = & "$root\ac6vr\tools\xrsim-tools\xrsim-shot.ps1" -Dir $xs -Out "$akn\xr_mmain" -Quiet -TimeoutSec 20; "main menu headset shot: layers {0} quads {1}" -f $s.Layers, $s.QuadLayers } catch { "main menu headset shot failed: $_" }
    if ($env:AKN_MENUVP) {   # HB: the same with HUDVP off (JJ: the prompts sit in a smaller layout)
        Set-Content "$akn\command.txt" "hudvp 0"; Start-Sleep 2; Set-Content "$akn\command.txt" "capmk mmainvp0"; Start-Sleep 4
        try { $s = & "$root\ac6vr\tools\xrsim-tools\xrsim-shot.ps1" -Dir $xs -Out "$akn\xr_mmainvp0" -Quiet -TimeoutSec 20 } catch {}
        Set-Content "$akn\command.txt" "hudvp 1"; Start-Sleep 2
    }
}
python "$PSScriptRoot\akmenu.py" key 0x0D
# LOADSHOT 2026-10-08 (JJ: "the loading screen is still too big to fit on the window, most of it is being cropped out"):
# desktop + headset pictures of the loading screen, in the background so the load wait below is not delayed
if ($env:AKN_LOADSHOT) { $null = Start-Job -ScriptBlock { param($tools, $xs, $akn, $here)
    Start-Sleep 3
    for ($i = 0; $i -lt 30; $i++) {   # headset shots only (a desktop shot here seemed to disturb the menu keys, LD2)
        try { & "$tools\xrsim-shot.ps1" -Dir $xs -Out "$akn\xr_load_$i" -Quiet -TimeoutSec 8 | Out-Null } catch {}
        Start-Sleep 2
    }
} -ArgumentList "$root\ac6vr\tools\xrsim-tools", $xs, $akn, $PSScriptRoot }
# in gameplay = a loading hitch, then steady frames (5c run 2: the steady MENU looked like gameplay). If no load comes,
# the Enter did not land: capture what is on screen and press it again (twice at most).
$inGame = $false
for ($try = 0; $try -lt 4 -and -not $inGame; $try++) {   # NOFOCUSCLICK: fewer blind Enter retries
    $ws = python "$PSScriptRoot\wait_steady.py" 8 20 12 --after-hitch; $ws   # MENUFAST: JJ "ridiculous amount of time" - retry Enter after 12 s
    if ($LASTEXITCODE -eq 0) { $inGame = $true; break }
    if (-not ($ws -match "no loading hitch")) {   # LOADWAIT 2026-10-08: a load HAS started - wait, never press Enter into it
        $ws = python "$PSScriptRoot\wait_steady.py" 8 20 40 --after-hitch-seen; $ws
        if ($LASTEXITCODE -eq 0) { $inGame = $true; break }
    }
    "{0:HH:mm:ss} no load yet - Enter again (try {1})" -f (Get-Date), ($try + 1)
    python "$PSScriptRoot\akmenu.py" shot "$akn\menu_4_retry$try.png"
    python "$PSScriptRoot\akmenu.py" key 0x0D
}
if (-not $inGame) { "did not reach gameplay - stop"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force; exit 2 }
# MENUCHECK 2026-10-08 (MK2 run: the Continue Enter did not land and the menu's own short hitch passed as the load):
# AKN's status line carries AKVR's main-menu state - still 1 = press Enter on "Continue Story" again and wait for the load
function InMenu { $l = Select-String -Path "$akn\akn_log.txt" -Pattern "  main menu (-?\d)" | Select-Object -Last 1; return ($l -and $l.Matches[0].Groups[1].Value -eq "1") }
for ($try = 0; $try -lt 2; $try++) {
    Start-Sleep 2
    if (-not (InMenu)) { break }
    "{0:HH:mm:ss} still in the main menu - Enter on Continue again (try {1})" -f (Get-Date), ($try + 1)
    python "$PSScriptRoot\akmenu.py" shot "$akn\menu_5_stillmenu$try.png"
    python "$PSScriptRoot\akmenu.py" key 0x0D
    python "$PSScriptRoot\wait_steady.py" 8 20 60 --after-hitch
}
if (InMenu) { "still in the main menu - stop"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force; exit 2 }
if (Get-ChildItem "$logs\UE3AutoReportDump*.txt" | Where-Object { $_.LastWriteTime -gt $t0 }) { "CRASHED - force-closing"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force; exit 2 }
Set-Content "$akn\command.txt" "pairshot menu_game"; Start-Sleep 2
"{0:HH:mm:ss} part A done - in gameplay (check hud_rgb_menu_game.bmp / pair_menu_game.bmp)" -f (Get-Date)
