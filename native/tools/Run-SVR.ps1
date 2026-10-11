# SVR 2026-10-09: the NATIVE mod (engine stereo, -akn) through SteamVR's OpenXR runtime - JJ: "get it working in
# SteamVR". One launch answers: (1) SVRAUTO turned the DXGI 1.1 factory on by itself, (2) SteamVR accepts every eye
# picture (xrclient log: no SharedTexturesNotSupported), (3) keyed-mutex share probe passes on the game device,
# (4) both eyes come out of the engine (pairshot), (5) frame rate under SteamVR's pacing, (6) orderly exit.
# No registry change: SteamVR is chosen for the game process only (XR_RUNTIME_JSON). JJ's akvr_settings.ini is backed
# up and put back (engineshape is saved from the session's lens - memory akn-test-runs-touch-jj-settings).
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$root = "D:\Documents\Antigravity\VR_Modding"
$scr = $args[0]; if (-not $scr) { "usage: Run-SVR.ps1 <scratch dir>"; exit 1 }
$svlog = "C:\Program Files (x86)\Steam\logs"
if (Get-Process | Where-Object { $_.Path -like "*steamapps*" }) { "another game is running - stop"; exit 1 }
$steamvrBefore = [bool](Get-Process vrserver -ErrorAction SilentlyContinue)
New-Item -ItemType Directory -Force -Path $scr | Out-Null
Copy-Item "$g\akvr_settings.ini" "$scr\akvr_settings.ini.jj" -Force
Copy-Item "$root\akvr\build-dinput8\Release\dinput8.dll" "$g\dinput8.dll" -Force
Copy-Item "$root\akvr_native\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force
"deployed: dinput8 {0}  xinput {1}" -f (Get-FileHash "$g\dinput8.dll").Hash.Substring(0, 12), (Get-FileHash "$g\XINPUT9_1_0.dll").Hash.Substring(0, 12)
$rt = (Get-ItemProperty 'HKCU:\SOFTWARE\Khronos\OpenXR\1').ActiveRuntime
if ($rt -notmatch 'steamxr') { "HKCU runtime is not SteamVR: $rt"; exit 1 }
$env:XR_RUNTIME_JSON = $rt
$env:SteamAppId = "208650"
Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akvr", "-akn", "-aknstereo" -WorkingDirectory $g
"{0:HH:mm:ss} launched through SteamVR ($rt)" -f (Get-Date)
try {
for ($i = 0; $i -lt 30; $i++) { Start-Sleep 3; if (-not (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { "game exited during start-up"; exit 1 }; if ((Get-Process BatmanAK).MainWindowHandle -ne 0) { break } }
Start-Sleep 10
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

"{0:HH:mm:ss} in gameplay - 40 s of steady play for the rate + the 60 s startup snapshot" -f (Get-Date)
Start-Sleep 40
Set-Content "$akn\command.txt" "pairshot svr_game"; Start-Sleep 25
Copy-Item "$g\akvr_startup_log.txt", "$akn\akn_log.txt", "$svlog\xrclient_BatmanAK.txt", "$svlog\vrcompositor.txt", "$svlog\vrserver.txt" $scr -Force -ErrorAction SilentlyContinue
Get-ChildItem "$akn\*svr_game*" | Copy-Item -Destination $scr -Force
"--- xrclient: share errors {0}, failed submits {1}" -f (Select-String -Path "$scr\xrclient_BatmanAK.txt" -Pattern "SharedTexturesNotSupported").Count, (Select-String -Path "$scr\xrclient_BatmanAK.txt" -Pattern "failed to submit").Count
# orderly exit test: close the window like a player would
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { $null = $p.CloseMainWindow(); "{0:HH:mm:ss} asked the game to close" -f (Get-Date) }
for ($i = 0; $i -lt 20 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue); $i++) { Start-Sleep 2 }
if (Get-Process BatmanAK -ErrorAction SilentlyContinue) { "game did NOT close in 40 s (menu quit prompt?) - force-closing"; Get-Process BatmanAK | Stop-Process -Force } else { "game closed by itself" }
if (Get-ChildItem "$logs\UE3AutoReportDump*.txt" | Where-Object { $_.LastWriteTime -gt $t0 }) { "CRASH REPORT written during the run" }
}
finally {
  Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 3
  Copy-Item "$scr\akvr_settings.ini.jj" "$g\akvr_settings.ini" -Force; "JJ's akvr_settings.ini restored"
  if (-not $steamvrBefore) { Get-Process vrmonitor, vrdashboard, vrcompositor, vrserver, vrwebhelper, vrstartup -ErrorAction SilentlyContinue | Stop-Process -Force; "SteamVR closed (it was not running before)" }
}
