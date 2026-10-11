# AKN stage 5d part B (JJ 6th Quest test: VR panel will not open; rain differs per eye): one launch after Run-M5c-A.
# A crash at any step: the game is force-closed (JJ is away) and the judge still runs.
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
Remove-Item "$akn\pair_r*.bmp", "$akn\xr_p*.*" -Force -ErrorAction SilentlyContinue
Cmd "mark D0" 4                                   # D3: rain steps per eye (telemetry, rainonce 0)
# D1 panel opened by the test switch (same path as the double stick click), D2 by F8
Cmd "mark D1" 1; Cmd "panel 1" 3; XrShot "xr_panel"; Cmd "panel 0" 3; XrShot "xr_pclosed"
Cmd "mark D2" 1; python "$PSScriptRoot\akmenu.py" key 0x77; Start-Sleep 3; XrShot "xr_pkey"; python "$PSScriptRoot\akmenu.py" key 0x77; Start-Sleep 2
Cmd "mark D2 end" 1
# D4 rain, same camera both eyes + shared frame number: rain steps in both eyes (control) vs second eye skipped
Cmd "eyesame 1" 2; Cmd "framesync 1" 3
Cmd "rainonce 0" 3; Cmd "capture rs0" 5
Cmd "rainonce 1" 3; Cmd "capture rs1" 5
Cmd "rainonce 3" 3; Cmd "capture rs3" 5
Cmd "eyesame 0" 3
# D5 normal stereo: rain present in both eyes with the second eye's steps skipped (pictures for a look), 20 s run
Cmd "capture rn3" 5; Cmd "mark D5 run" 20; Cmd "rainonce 0" 3; Cmd "capture rn0" 5
if (Test-Path "$xs\state.json") { Copy-Item "$xs\state.json" "$akn\xr_state_end.json" -Force }
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1}); judging" -f (Get-Date), $crash
python "$PSScriptRoot\judge_m5d.py" $akn
