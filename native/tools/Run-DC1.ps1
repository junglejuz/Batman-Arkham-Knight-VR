# DC1 2026-10-12 (DETCALLOUT: detective mode's scan box + label on its object; JJ AFK, fake headset allowed).
# Into gameplay (SMOKE's quick menu path), detective mode by pad injection, find a scan box with mouse sweeps, then captures
# (both eyes, HUD sheet, both eye marker layers) and composited headset shots with head turns; "scan 0" control; quit.
# JJ's akvr_settings.ini is backed up and put back; JJ's installed DLLs are backed up before the new ones go in.
param([string]$scr = "$env:TEMP\akn_dc1")
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$root = "D:\Documents\Antigravity\VR_Modding"
$xs = "$env:LOCALAPPDATA\AKN\xrsim"; $tools = "$root\ac6vr\tools\xrsim-tools"; $cap = "$xs\capture"
$menu = "$PSScriptRoot\akmenu.py"
if (Get-Process | Where-Object { $_.Path -like "*steamapps*" -and $_.Path -notlike "*SteamVR*" }) { "another game is running - stop"; exit 1 }
New-Item -ItemType Directory -Force -Path $scr | Out-Null
Copy-Item "$g\akvr_settings.ini" "$scr\akvr_settings.ini.jj" -Force
$bk = "$root\akvr\diagnostics\before-DETCALLOUT-20261012"
if (-not (Test-Path $bk)) { New-Item -ItemType Directory -Force -Path $bk | Out-Null; Copy-Item "$g\dinput8.dll", "$g\XINPUT9_1_0.dll", "$g\akvr_settings.ini" $bk -Force }
$t = [IO.File]::ReadAllText("$g\akvr_settings.ini")
$t = $t -replace "(?m)^lensmode=\d+", "lensmode=2" -replace "(?m)^engineshape=\d+", "engineshape=918"
[IO.File]::WriteAllText("$g\akvr_settings.ini", $t)
Copy-Item "$root\akvr\build-dinput8\Release\dinput8.dll" "$g\dinput8.dll" -Force
Copy-Item "$root\akvr_native\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force
function Say($m) { $l = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; $l; Add-Content "$scr\dc1_steps.txt" $l }
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
function Head($y, $p) { try { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet "head rot $y $p 0" | Out-Null; Say "head yaw $y pitch $p" } catch { Say "head command failed: $_" } }
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
    Start-Sleep 8
    Cmd "mark DC1 begin" 1
    Cmd "capmk dc0" 5
    Say (Shot "dc0")
    # 1. detective mode: the X key (BmInput.ini kToggleDetectiveMode=Tap [X]; injected pad buttons are ignored)
    Focus; python $menu hold 0x58 0.15 | Out-Null; Say "X tapped (detective mode)"; Start-Sleep 4
    Say ("detective: " + (LastLine "detective: composite"))
    # 2. find a scan box: camera sweeps (mouse), checking AKN's per-second line after each
    $found = (ScanSeen) -gt 0
    $moves = @(@(0, 0)); foreach ($pl in 1..3) { $moves += ,@(0, 60); $moves += (1..24 | ForEach-Object { ,@(200, 0) }) }   # 3 shallow downward angles, a full turn each in small steps
    $sight = 0
    for ($k = 0; $k -lt $moves.Count -and $sight -lt 3; $k++) {
        if (-not (Alive)) { break }
        Focus; python $menu mouse $moves[$k][0] $moves[$k][1] | Out-Null; Start-Sleep 2.5
        $found = (ScanSeen) -gt 0
        if ($found) {   # capture at once (the box comes and goes), head turns on the first sighting
            $sight++; Say ("sweep {0}: BOX SEEN (sighting {1}) - {2}" -f $k, $sight, (LastLine "DETCALLOUT:"))
            if ($sight -eq 1) { Cmd "projlog 40" 2 }   # raw projections next to the box line
            Cmd "capmk dcf$sight" 4; Say (Shot "dcf$sight")
            # HEADRAMP: the head turned in 1 deg steps (a jump is taken as a tracking glitch and re-centred); the box's layout
            # point and the nearest projected point logged at each pose, with a headset shot
            $yawNow = 0
            foreach ($y in @(0, 20, 0, -20, 0)) {
                while ($yawNow -ne $y) { $yawNow += $(if ($y -gt $yawNow) { 1 } else { -1 }); try { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet "head rot $yawNow 0 0" | Out-Null } catch {}; Start-Sleep -Milliseconds 60 }
                Start-Sleep 1.3
                $l = LastLine "DETCALLOUT: scan box"; $i = $l.IndexOf("layout"); $j = $l.IndexOf("left eye")
                Say ("   ramp yaw {0}: {1} | {2}" -f $y, $(if ($i -ge 0) { $l.Substring($i, [Math]::Min(40, $l.Length - $i)) } else { "no layout" }), $(if ($j -ge 0) { $l.Substring($j, [Math]::Min(60, $l.Length - $j)) } else { "" }))
                Say (Shot ("dcf{0}r{1}" -f $sight, $y))
            }
            Say (Shot "dcf$sight")
            Say ("after: " + (LastLine "DETCALLOUT:"))
        }
        elseif ($k % 8 -eq 7) { Cmd "pairshot dcs$k" 2 }
    }
    Say ("DETCALLOUT: " + (LastLine "DETCALLOUT:"))
    # 3. captures with the box up (or the last view if none was found)
    Start-Sleep 3
    Say ("DETCALLOUT: " + (LastLine "DETCALLOUT:"))
    Cmd "capmk dc1" 5
    Say (Shot "dc1")
    # 4. head turns: the box must stay on its object
    foreach ($h in @(@(15, 0), @(-15, 0), @(0, -10), @(0, 0))) { Head $h[0] $h[1]; Start-Sleep 1.5; Say (Shot ("dch{0}_{1}" -f $h[0], $h[1])) }
    Cmd "capmk dc2" 5
    Say ("DETCALLOUT: " + (LastLine "DETCALLOUT:"))
    # 5. control: callout back on the HUD sheet
    Cmd "scan 0" 3
    Cmd "capmk dcoff" 5
    Say (Shot "dcoff")
    Cmd "scan 1" 2
    # 6. detective off
    Focus; python $menu hold 0x58 0.15 | Out-Null; Say "X tapped (detective off)"; Start-Sleep 4
    Cmd "mark DC1 end" 1
    Say ("alive at the end: " + (Alive))
}
finally {
    Copy-Item "$akn\akn_log.txt" $scr -Force -ErrorAction SilentlyContinue
    Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    Copy-Item "$scr\akvr_settings.ini.jj" "$g\akvr_settings.ini" -Force; "JJ's akvr_settings.ini restored, game closed"
    Copy-Item "$g\akvr_startup_log.txt" $scr -Force -ErrorAction SilentlyContinue
    foreach ($t in "dc0", "dc1", "dc2", "dcoff", "dcf1", "dcf2", "dcf3") { foreach ($f in "pair", "hud_rgb", "mk_left", "mk_right") { Copy-Item "$akn\${f}_$t.bmp" $scr -Force -ErrorAction SilentlyContinue } }
}
