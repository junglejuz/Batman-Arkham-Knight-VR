# HF (2026-10-09): HUD flicker trace + candidate fixes (huddef / hudall / hudfull 0), pause pose stamp (PAUSESTAMP),
# VIEWSCAN (suit gloss theory), pause HUD draw dump (dark window). One launch.
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
function Stat($tag) { Cmd "mark HF $tag" 1; Cmd "hudstat 60" 3 }
Cmd "mark HF start" 1
python "$PSScriptRoot\akmenu.py" hold 0x53 2.0; Start-Sleep 3
Head "head rot 0 0 0"; Start-Sleep 2
# 1. HUD flicker: 60 frames of the sheet per setup (static scene: a stable HUD = the same cells every frame)
Stat "base"
Cmd "huddef 1" 2; Stat "huddef"
Cmd "hudall 1" 2; Stat "hudall+huddef"
Cmd "huddef 0" 2; Stat "hudall"
Cmd "hudall 0" 1; Cmd "hudfull 0" 3; Stat "hudfull0"
Cmd "hudfull 1" 3; Stat "base2"
Cmd "capmk hfgame" 4
# 3. reflections theory: the eye views scanned for the head's own camera axes
Cmd "viewscan 4" 2
# 2 + 4. pause: pose stamp (status lines), HUD sheet trace, HUD draw dump of the backdrop
Cmd "mark HF pause" 1
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 4
Stat "pause"
Head "head rot 20 0 0"; Start-Sleep 1; Head "head rot -20 0 0"; Start-Sleep 1; Head "head rot 0 0 0"; Start-Sleep 2
Cmd "capmk hfpause" 4; XrShot "xr_hfpause"
Cmd "hudall 1" 2; Stat "pause-hudall"; Cmd "hudall 0" 1
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 3
Cmd "mark HF end" 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
