# DLSS stage D0 part B (DLSS_PLAN.md): SMAA swap points + motion vectors per eye, and where the frame time goes
# (eye render processor / graphics-card ms, frames/s with the fake headset unpaced at 240 Hz). One launch per eye size.
param([string]$label = "native")
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
& "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet "refresh 240" | Out-Null; "fake headset refresh 240 Hz (unpaced)"
Cmd "mark D0 $label start" 2
Cmd "d0probe 1" 4
Cmd "mark D0 $label measure" 25
Cmd "mark D0 $label end" 2
Cmd "d0probe 0" 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
Copy-Item "$akn\akn_log.txt" "$akn\akn_log_D0_$label.txt" -Force
