# AKN stage 5p part B: NEARRAIN2 recheck (rain camera rows centred / leaned / turned) + MARKERPROBE (who projects
# world points, head aimed at grapple points). One launch.
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
function Head($c) { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet $c | Out-Null; "{0:HH:mm:ss} fake head: {1}" -f (Get-Date), $c }
Cmd "mark N0" 2
Head "head rot 0 0 0"; Start-Sleep 2; Cmd "mark N centre" 1; Cmd "raincb 1" 3
Head "head move 0.3 0 0"; Start-Sleep 2; Cmd "mark N lean" 1; Cmd "raincb 1" 3
Head "head move -0.3 0 0"; Head "head rot 20 0 0"; Start-Sleep 2; Cmd "mark N turn" 1; Cmd "raincb 1" 3
Head "head rot 0 0 0"; Start-Sleep 1; Cmd "mark N end" 2
Remove-Item "$akn\hud_*_mk*.bmp", "$akn\pair_mk*.bmp" -Force -ErrorAction SilentlyContinue
Cmd "mark O0" 6
$poses = @("0 0 0", "0 25 0", "-30 20 0", "30 20 0", "0 40 0", "60 15 0", "-60 15 0")
$i = 0
foreach ($ps in $poses) {
    Head "head rot $ps"; Start-Sleep 3
    Cmd "mark O pose $i ($ps)" 1; Cmd "projlog 60" 2; Cmd "capture mk$i" 5
    $i++
}
Head "head rot 0 0 0"; Start-Sleep 2; Cmd "mark O end" 6
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
Select-String -Path "$akn\akn_log.txt" -Pattern "MARK N|RAINCB: this render|RAINCB patched|  cb0\[1[01]\]" | ForEach-Object { $_.Line.Substring([Math]::Min(13, $_.Line.Length)) }
python "$PSScriptRoot\judge_m5o.py" $akn
