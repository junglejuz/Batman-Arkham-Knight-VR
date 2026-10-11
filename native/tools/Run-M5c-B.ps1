# AKN stage 5c part B: the tests, in gameplay, on the fake headset (after Run-M5c-A + menus). Judged by judge_m5c.py.
# A crash at any step: the game is force-closed (JJ is away; he will not click OK) and the judge still runs.
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$xs = "$env:LOCALAPPDATA\AKN\xrsim"
$tools = "D:\Documents\Antigravity\VR_Modding\ac6vr\tools\xrsim-tools"
$logs = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\BmGame\Logs"
$t0 = (Get-Process BatmanAK -ErrorAction SilentlyContinue).StartTime
function Crashed { [bool](Get-ChildItem "$logs\UE3AutoReportDump*.txt" | Where-Object { $_.LastWriteTime -gt $t0 }) }
function Alive { [bool](Get-Process BatmanAK -ErrorAction SilentlyContinue) -and -not (Crashed) }
function Cmd($c, $wait) {
    if (-not (Alive)) { "{0:HH:mm:ss} SKIP (game gone/crashed): {1}" -f (Get-Date), $c; return }
    Set-Content "$akn\command.txt" $c
    for ($i = 0; $i -lt 30 -and (Test-Path "$akn\command.txt"); $i++) { Start-Sleep -Milliseconds 100 }
    "{0:HH:mm:ss} sent: {1}" -f (Get-Date), $c
    Start-Sleep $wait
}
function XrShot($name) {
    if (-not (Alive)) { "{0:HH:mm:ss} SKIP headset shot {1}" -f (Get-Date), $name; return }
    try {
        try { $s = & "$tools\xrsim-shot.ps1" -Dir $xs -Out "$akn\$name" -Quiet }
        catch { "{0:HH:mm:ss} headset shot {1}: first try failed ({2}) - once more" -f (Get-Date), $name, $_; Start-Sleep 2; $s = & "$tools\xrsim-shot.ps1" -Dir $xs -Out "$akn\$name" -Quiet -TimeoutSec 30 }
        $s | ConvertTo-Json -Depth 4 | Set-Content "$akn\$name.shot.json"
        "{0:HH:mm:ss} headset shot {1}: layers {2}, quads {3}, luma L {4} R {5}" -f (Get-Date), $name, $s.Layers, $s.QuadLayers, $s.MeanLumaL, $s.MeanLumaR
    } catch { "{0:HH:mm:ss} headset shot {1} FAILED: {2}" -f (Get-Date), $name, $_ }
}
foreach ($f in Get-ChildItem "$akn\*_g.bmp", "$akn\*_s.bmp", "$akn\*_s0.bmp", "$akn\pair_lk*.bmp", "$akn\pair_lens.bmp", "$akn\xr_t*.*" -ErrorAction SilentlyContinue) { Remove-Item $f.FullName -Force -Confirm:$false }
if (Test-Path "$xs\state.json") { Copy-Item "$xs\state.json" "$akn\xr_state_start.json" -Force }
Cmd "mark B0" 5
Cmd "mark T1" 1; Cmd "capture g" 6           # T1 pair + T2 HUD (gameplay)
Cmd "eyesame 1" 2; Cmd "framesync 0" 4; Cmd "capture s0" 6    # T1b control: same camera, each render its own frame number
Cmd "framesync 1" 4; Cmd "capture s" 6; Cmd "eyesame 0" 2        # T1b: same frame number -> the two pictures must match (rain too)
# T17 (JJ's 5th test: lighting/shadows/reflections differ per eye; the leak test found no leak): same spot, same frame,
# each eye through its OWN mirrored off-centre lens -> must be a pure sideways shift of each other if the game's
# lighting, shadow and reflection shaders handle off-centre lenses
Cmd "eyesame 2" 3; Cmd "pairshot lens" 4; Cmd "eyesame 0" 2
XrShot "xr_t3"                               # T3 headset stereo, T4 lens, T5 HUD layer
Cmd "eyemark 1" 2; XrShot "xr_mark"; Cmd "eyemark 0" 1   # T12: left eye's picture red band, right eye's blue band
# T13 head turn (JJ's 3rd Quest test: shearing on head turns): the world must move by exactly the head's turn
function Head($c) { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet $c | Out-Null; "{0:HH:mm:ss} fake head: {1}" -f (Get-Date), $c }
Head "head rot 0 0 0"; Start-Sleep 3; XrShot "xr_h0"
Head "head rot 20 0 0"; Start-Sleep 3; XrShot "xr_hy"
Head "head rot 0 15 0"; Start-Sleep 3; XrShot "xr_hp"
Head "head rot 0 0 0"; Start-Sleep 2
Cmd "stereo 0" 4; XrShot "xr_t6"; Cmd "stereo 1" 4   # T6 control
# T16 eye leak (JJ's 5th Quest test: lighting, shadows and reflections differ per eye): move ONE eye's camera 3 m,
# the OTHER eye's picture must not change beyond the frame-to-frame noise; own history per eye off (control), then on
foreach ($sw in 0, 1) {
    Cmd "histswap $sw" 4
    Cmd "pairshot lk${sw}a" 3; Cmd "pairshot lk${sw}b" 3                                # noise: nothing moved
    Cmd "eyeshift 1 300" 3; Cmd "pairshot lk${sw}r" 3; Cmd "eyeshift 1 0" 2         # right eye moved
    Cmd "eyeshift 0 300" 3; Cmd "pairshot lk${sw}l" 3; Cmd "eyeshift 0 0" 2         # left eye moved
}
Cmd "mark T7 start" 60; Cmd "mark T7 end" 2  # T7 soak (own history per eye stays on)
if (Test-Path "$xs\state.json") { Copy-Item "$xs\state.json" "$akn\xr_state_end.json" -Force }
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1}); judging" -f (Get-Date), $crash
python "$PSScriptRoot\judge_m5c.py" $akn
