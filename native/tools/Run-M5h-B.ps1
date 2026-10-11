# AKN stage 5h part B (rain per eye, SWBF2 lesson "particles spawned per view"): does the rain differ / move while the
# game is PAUSED (per-render placement) - and is the 103530-vertex draw the rain (hide it)? One launch.
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
Remove-Item "$akn\pair_h*.bmp" -Force -ErrorAction SilentlyContinue
Cmd "mark H0" 3
Cmd "eyesame 1" 2; Cmd "framesync 1" 3
Cmd "capture hp1" 5                                            # playing: same camera, both eyes
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 5          # pause menu
Cmd "mark H pause" 1; Cmd "capture hq1" 4; Cmd "capture hq2" 4    # paused: still different per eye? still moving?
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 5          # back to the game
Cmd "eyesame 0" 3
Cmd "capture hs0" 5; Cmd "skipvs 62c3cb6af9d6796c" 3; Cmd "capture hs1" 5; Cmd "skipvs 0" 2   # is that draw the rain?
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
python "$PSScriptRoot\judge_m5h.py" $akn
