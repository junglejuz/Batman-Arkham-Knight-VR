# XEYE: textures one eye reads but only the other eye draws (Batman reflections per eye). One launch.
# A crash at any step: the game is force-closed.
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
# MP2: away from the training terminal (its popup takes input and hides markers), then markers, then the map
Cmd "mark MP2 start" 1
python "$PSScriptRoot\akmenu.py" hold 0x53 2.0; Start-Sleep 4
python "$PSScriptRoot\akmenu.py" shot "$akn\mp2_walked.png"
Cmd "projlog 3" 2; Cmd "capmk mp2a" 4
Head "head rot 0 0 0"; Cmd "markercut 0" 2; XrShot "xr_mp2_m0"; Cmd "markercut 1" 2; XrShot "xr_mp2_m1"
Head "head rot 15 0 0"; Start-Sleep 2; Cmd "markercut 0" 2; XrShot "xr_mp2_t0"; Cmd "markercut 1" 2; XrShot "xr_mp2_t1"; Head "head rot 0 0 0"
Cmd "mark MP2 game" 1; Cmd "famdump 2" 2
python "$PSScriptRoot\akmenu.py" key 0x09 | Out-Null; Start-Sleep 5
python "$PSScriptRoot\akmenu.py" shot "$akn\mp2_map.png"
Cmd "mark MP2 map" 1; Cmd "famdump 2" 2; Cmd "capmk mp2map" 5; XrShot "xr_mp2_map"
python "$PSScriptRoot\akmenu.py" key 0x1B | Out-Null; Start-Sleep 4
python "$PSScriptRoot\akmenu.py" shot "$akn\mp2_after.png"
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
