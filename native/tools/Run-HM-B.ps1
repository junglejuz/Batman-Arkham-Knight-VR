# HM (2026-10-08, JJ report 22:3x): markers head-free (MARKERBASE/MARKERMATCH), HUD size (HUDSIZE), lens mode 3 (SAMEDIR),
# pause HUD in the eyes (WHEELEYE), map the flat game's way (MAPFLAT). One launch. A crash at any step: force-closed.
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
function Head($c) { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet $c | Out-Null; "{0:HH:mm:ss} fake head: {1}" -f (Get-Date), $c }
function Mk($tag) { Cmd "projlog 4" 1; Cmd "capmk $tag" 4 }
if (Test-Path "$xs\state.json") { Copy-Item "$xs\state.json" "$akn\xr_state_start.json" -Force }
Cmd "mark HM start" 1
python "$PSScriptRoot\akmenu.py" hold 0x53 2.0; Start-Sleep 4
python "$PSScriptRoot\akmenu.py" shot "$akn\hm_walked.png"
# 1. markers: the game's marker layout must not follow the head (MARKERBASE), cards on their objects
Head "head rot 0 0 0"; Start-Sleep 2; Mk "hm0"; XrShot "xr_hm0"
Head "head rot 15 0 0"; Start-Sleep 2; Mk "hm15"; XrShot "xr_hm15"
Head "head rot -15 0 0"; Start-Sleep 2; Mk "hmn15"; XrShot "xr_hmn15"
Cmd "markerbase 0" 2; Mk "hmn15b0"; XrShot "xr_hmn15b0"; Cmd "markerbase 1" 2
Head "head rot 0 0 0"; Start-Sleep 1
# 2. Batman's suit: lens mode 2 (turned eyes) vs 3 (one direction), facing him
python "$PSScriptRoot\akmenu.py" mouse 950 0 | Out-Null; Start-Sleep 3
Cmd "mark HM lens" 1
foreach ($m in 2, 3) { Cmd "lensmode $m" 3; Cmd "pairshot rl${m}a" 2; Cmd "pairshot rl${m}b" 2; XrShot "xr_rl$m" }
Cmd "lensmode 2" 2
python "$PSScriptRoot\akmenu.py" mouse -950 0 | Out-Null; Start-Sleep 2
# 3. pause: HUD drawn into the eyes (no HUD quad), darkening under it (WHEELEYE)
Cmd "mark HM pause" 1
python "$PSScriptRoot\akmenu.py" key 0x1B | Out-Null; Start-Sleep 4
XrShot "xr_hmpause"; Cmd "capmk hmpause" 4
python "$PSScriptRoot\akmenu.py" key 0x1B | Out-Null; Start-Sleep 4
# 4. map: the flat game's lens + HUD on the screen (mapflat 1) vs the old way (0)
Cmd "mark HM map" 1
python "$PSScriptRoot\akmenu.py" key 0x09 | Out-Null; Start-Sleep 6
python "$PSScriptRoot\akmenu.py" shot "$akn\hm_map.png"
Cmd "capmk hmmap1" 4; XrShot "xr_hmmap1"
Head "head rot 25 0 0"; Start-Sleep 2; XrShot "xr_hmmap1t"; Head "head rot 0 0 0"
Cmd "mapflat 0" 3; Cmd "capmk hmmap0" 4; XrShot "xr_hmmap0"; Cmd "mapflat 1" 2
python "$PSScriptRoot\akmenu.py" key 0x1B | Out-Null; Start-Sleep 4
python "$PSScriptRoot\akmenu.py" shot "$akn\hm_after.png"
Cmd "mark HM end" 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
