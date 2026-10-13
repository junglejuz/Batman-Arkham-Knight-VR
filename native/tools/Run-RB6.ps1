# RB2 2026-10-13 (JJ AFK, fake headset allowed: "Stepping away now"). Two checks in one launch:
#  1. PARTID: every room HUD part gets its own mark value; ~45 s of normal play with the distance marker in view -> FLIPREC rows
#     name the part whose pieces land on the HUD sheet next to the marker (the flickering duplicate).
#  2. SCANBOX: detective mode, find a scan box; captures + headset shots: four plus signs drawn by AKVR, one line (ours), the
#     marker sheet not shown.
# JJ's akvr_settings.ini is backed up and put back; JJ's installed DLLs are backed up before the new ones go in.
param([string]$scr = "$env:TEMP\akn_rb6")
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$root = "D:\Documents\Antigravity\VR_Modding"
$xs = "$env:LOCALAPPDATA\AKN\xrsim"; $tools = "$root\ac6vr\tools\xrsim-tools"; $cap = "$xs\capture"
$menu = "$PSScriptRoot\akmenu.py"
if (Get-Process | Where-Object { $_.Path -like "*steamapps*" -and $_.Path -notlike "*SteamVR*" }) { "another game is running - stop"; exit 1 }
New-Item -ItemType Directory -Force -Path $scr | Out-Null
Copy-Item "$g\akvr_settings.ini" "$scr\akvr_settings.ini.jj" -Force
$bk = "$root\akvr\diagnostics\before-RB2-20261013"
if (-not (Test-Path $bk)) { New-Item -ItemType Directory -Force -Path $bk | Out-Null; Copy-Item "$g\dinput8.dll", "$g\XINPUT9_1_0.dll", "$g\akvr_settings.ini" $bk -Force }
$t = [IO.File]::ReadAllText("$g\akvr_settings.ini")
$t = $t -replace "(?m)^lensmode=\d+", "lensmode=2" -replace "(?m)^engineshape=\d+", "engineshape=918"
[IO.File]::WriteAllText("$g\akvr_settings.ini", $t)
Copy-Item "$root\akvr\build-dinput8\Release\dinput8.dll" "$g\dinput8.dll" -Force
Copy-Item "$root\akvr_native\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force
function Say($m) { $l = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; $l; Add-Content "$scr\rb6_steps.txt" $l }
function Alive { [bool](Get-Process BatmanAK -ErrorAction SilentlyContinue) }
function Cmd($c, $w) {
    if (-not (Alive)) { Say "SKIP (game gone): $c"; return }
    Set-Content "$akn\command.txt" $c
    for ($i = 0; $i -lt 30 -and (Test-Path "$akn\command.txt"); $i++) { Start-Sleep -Milliseconds 100 }
    Say "sent: $c"; Start-Sleep $w
}
function Shot($tag) {
    if (-not (Alive)) { return "$tag SKIP" }
    try { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet "shot $tag" | Out-Null } catch { return "$tag FAILED $_" }
    for ($i = 0; $i -lt 30; $i++) { Start-Sleep -Milliseconds 500; if (Test-Path "$cap\${tag}_sbs.png") { Start-Sleep 1; Copy-Item "$cap\${tag}_*.png" $scr -Force; return "$tag ok" } }
    return "$tag MISSING"
}
function Focus { python $menu click 0.98 0.97 | Out-Null; Start-Sleep -Milliseconds 400 }
function InMenu { $l = Select-String -Path "$akn\akn_log.txt" -Pattern "  main menu (-?\d)" | Select-Object -Last 1; return ($l -and $l.Matches[0].Groups[1].Value -eq "1") }
function LastLine($pat) { $l = Select-String -Path "$akn\akn_log.txt" -Pattern $pat | Select-Object -Last 1; if ($l) { $l.Line } else { "" } }
function ScanSeen { $l = Select-String -Path "$akn\akn_log.txt" -Pattern "DETCALLOUT: scan box seen/s (\d+)" | Select-Object -Last 1
    if (-not $l) { return 0 }; $ts = [datetime]::ParseExact($l.Line.Substring(0, 8), "HH:mm:ss", $null); if (((Get-Date) - $ts).TotalSeconds -gt 3) { return 0 }; return [int]$l.Matches[0].Groups[1].Value }
try {
    $env:XR_RUNTIME_JSON = "$xs\akn_xrsim64.json"; $env:BVR_XRSIM_DIR = $xs; $env:SteamAppId = "208650"
    Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akvr", "-akn", "-aknstereo" -WorkingDirectory $g
    $t0 = Get-Date
    for ($i = 0; $i -lt 60; $i++) { Start-Sleep 1; $p = Get-Process BatmanAK -ErrorAction SilentlyContinue; if ($p -and $p.MainWindowHandle -ne 0) { break } }
    try { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet "fov eye l -49.48 35.50 39.38 -50.52" "fov eye r -35.50 49.48 39.38 -50.52" | Out-Null } catch {}
    Start-Sleep 8; python $menu click 0.5 0.4 | Out-Null
    Start-Sleep 6; python $menu click 0.352 0.523 | Out-Null
    Start-Sleep 8; Focus; python $menu key 0x0D | Out-Null
    for ($i = 0; $i -lt 10 -and -not (InMenu); $i++) { Start-Sleep 1 }
    Start-Sleep 2; Focus; python $menu key 0x0D | Out-Null
    Say ("Continue pressed ({0:N0} s after launch)" -f ((Get-Date) - $t0).TotalSeconds)
    for ($try = 0; $try -lt 2; $try++) { Start-Sleep 8; if (-not (InMenu)) { break }; Say "still in the main menu - Enter again"; Focus; python $menu key 0x0D | Out-Null }
    $ws = python "$PSScriptRoot\wait_steady.py" 8 20 60 --after-hitch; Say "$ws"
    if (InMenu) { Say "did not leave the main menu - stop"; python $menu shot "$scr\menu_fail.png" | Out-Null; exit 2 }
    Say "in gameplay"
    Start-Sleep 6
    Cmd "recorders 1" 2
    # RB5: MKJUMP - ~60 s of normal play with the distance marker in view, small camera turns
    foreach ($k in 1..20) { Focus; python $menu mouse $(if ($k % 4 -lt 2) { 60 } else { -60 }) 0 | Out-Null; Start-Sleep 3 }
    Cmd "mark RB2 end" 1
    Say ("alive at the end: " + (Alive))
}
finally {
    Copy-Item "$akn\akn_log.txt" $scr -Force -ErrorAction SilentlyContinue
    Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    Copy-Item "$scr\akvr_settings.ini.jj" "$g\akvr_settings.ini" -Force; "JJ's akvr_settings.ini restored, game closed"
    Copy-Item "$g\akvr_startup_log.txt" $scr -Force -ErrorAction SilentlyContinue
    Copy-Item "$g\akvr_captures\partid.txt" $scr -Force -ErrorAction SilentlyContinue
    foreach ($t in "rb0", "rbf1", "rbf2") { foreach ($f in "pair", "hud_rgb", "mk_left", "mk_right") { Copy-Item "$akn\${f}_$t.bmp" $scr -Force -ErrorAction SilentlyContinue } }
}
