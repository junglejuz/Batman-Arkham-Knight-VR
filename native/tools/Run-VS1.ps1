# VS1 2026-10-09 (copy of SVR2 start): right-eye view-state fields vs the wet-ground reflection / sky glow gap.
# SVR2 2026-10-09: fake headset, into GAMEPLAY quickly (JJ: 5-10 s max between menu steps), turned eyes (lens mode 2):
# (1) SVRSTRAIGHT off/on composited headset shots (straightened eyes must show the same world as the turned ones),
# (2) a both-eyes pair for the wet-ground reflection difference. JJ's akvr_settings.ini is backed up and put back.
param([string]$scr = "$env:TEMP\akn_svr2")
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$root = "D:\Documents\Antigravity\VR_Modding"
$xs = "$env:LOCALAPPDATA\AKN\xrsim"; $tools = "$root\ac6vr\tools\xrsim-tools"; $cap = "$xs\capture"
$menu = "$PSScriptRoot\akmenu.py"
if (Get-Process | Where-Object { $_.Path -like "*steamapps*" }) { "another game is running - stop"; exit 1 }
New-Item -ItemType Directory -Force -Path $scr | Out-Null
Copy-Item "$g\akvr_settings.ini" "$scr\akvr_settings.ini.jj" -Force
$t = [IO.File]::ReadAllText("$g\akvr_settings.ini")
# lens left as JJ has it
[IO.File]::WriteAllText("$g\akvr_settings.ini", $t)
Copy-Item "$root\akvr\build-dinput8\Release\dinput8.dll" "$g\dinput8.dll" -Force
Copy-Item "$root\akvr_native\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force
function Shot($tag) {
    & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet "shot $tag" | Out-Null
    for ($i = 0; $i -lt 30; $i++) { Start-Sleep -Milliseconds 500; if (Test-Path "$cap\${tag}_sbs.png") { Start-Sleep 1; Copy-Item "$cap\${tag}_*.png" $scr -Force; return "$tag ok" } }
    return "$tag MISSING"
}
function Focus { python $menu click 0.98 0.97 | Out-Null; Start-Sleep -Milliseconds 400 }   # empty corner: activates the window
function InMenu { $l = Select-String -Path "$akn\akn_log.txt" -Pattern "  main menu (-?\d)" | Select-Object -Last 1; return ($l -and $l.Matches[0].Groups[1].Value -eq "1") }
try {
    $env:XR_RUNTIME_JSON = "$xs\akn_xrsim64.json"; $env:BVR_XRSIM_DIR = $xs; $env:SteamAppId = "208650"
    Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akvr", "-akn", "-aknstereo" -WorkingDirectory $g
    $t0 = Get-Date
    for ($i = 0; $i -lt 60; $i++) { Start-Sleep 1; $p = Get-Process BatmanAK -ErrorAction SilentlyContinue; if ($p -and $p.MainWindowHandle -ne 0) { break } }
    try { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet "fov eye l -49.48 35.50 39.38 -50.52" "fov eye r -35.50 49.48 39.38 -50.52" | Out-Null } catch {}
    Start-Sleep 8; python $menu click 0.5 0.4 | Out-Null                      # skip the logos
    Start-Sleep 6; python $menu click 0.352 0.523 | Out-Null                  # title: CLICK HERE TO START
    Start-Sleep 8; Focus; python $menu key 0x0D | Out-Null                    # the save
    for ($i = 0; $i -lt 10 -and -not (InMenu); $i++) { Start-Sleep 1 }       # main menu up (polled, max 10 s)
    Start-Sleep 2; Focus; python $menu key 0x0D | Out-Null                    # Continue Story
    "{0:HH:mm:ss} Continue pressed ({1:N0} s after launch)" -f (Get-Date), ((Get-Date) - $t0).TotalSeconds
    for ($try = 0; $try -lt 2; $try++) {                                      # a load must start within 10 s
        Start-Sleep 8
        if (-not (InMenu)) { break }
        "still in the main menu - Enter again"; Focus; python $menu key 0x0D | Out-Null
    }
    $ws = python "$PSScriptRoot\wait_steady.py" 8 20 60 --after-hitch; $ws    # the game's own loading time
    if (InMenu) { "did not leave the main menu - stop"; python $menu shot "$scr\menu_fail.png" | Out-Null; exit 2 }
    "{0:HH:mm:ss} in gameplay" -f (Get-Date)
    Start-Sleep 3
    function Cmd($c, $w) { Set-Content "$akn\command.txt" $c; Start-Sleep -Milliseconds $w }
    Cmd "eyesame 1" 2500
    Cmd "pairshot vA" 2500
    Cmd "vscopy 8834 8" 300; Cmd "vscopy 8844 c" 1500
    Cmd "pairshot vB" 2500
    Cmd "vscopy 52c 4" 1500
    Cmd "pairshot vC" 2500
    foreach ($r in "6c 10", "94 4", "108 8", "1d0 40", "3e8 8", "44c 10", "474 4", "484 64", "4ec 1c") { Cmd "vscopy $r" 300 }
    Start-Sleep 1
    Cmd "pairshot vD" 2500
    Cmd "vscopy clear" 300
    Cmd "vs 0" 2500
    Cmd "pairshot vE" 2500
    Cmd "vs 4" 2500
    Cmd "pairshot vF" 2500
    Cmd "eyesame 0" 500
    "alive at the end: " + [bool](Get-Process BatmanAK -ErrorAction SilentlyContinue)
    Get-ChildItem "$akn\pair_v[A-F].bmp" | Copy-Item -Destination $scr -Force
}
finally {
    Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    Copy-Item "$scr\akvr_settings.ini.jj" "$g\akvr_settings.ini" -Force; "JJ's akvr_settings.ini restored, game closed"
}
