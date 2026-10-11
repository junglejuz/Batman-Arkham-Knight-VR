# HF2 (2026-10-09): HUDDSV (the HUD sheet's own depth/stencil) on/off with the sheet trace, BACKEXT on the pause,
# LOADFIT via part A's LOADSHOT headset shots. One launch.
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
function Stat($tag) { Cmd "mark HF2 $tag" 1; Cmd "hudstat 60" 3 }
Cmd "mark HF2 start" 1
python "$PSScriptRoot\akmenu.py" hold 0x53 2.0; Start-Sleep 3
Head "head rot 0 0 0"; Start-Sleep 2
# 1. HUD flicker: the sheet's own depth/stencil (default on) vs the game's (off)
Stat "dsv1"
Cmd "huddsv 0" 2; Stat "dsv0"
Cmd "huddsv 1" 2; Stat "dsv1b"
Cmd "capmk hf2game" 4; XrShot "xr_hf2game"
# 2. pause: backdrop carried over the whole view (on / off), sheet trace
Cmd "mark HF2 pause" 1
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 4
Stat "pause"
XrShot "xr_hf2pause1"; Cmd "backext 0" 2; XrShot "xr_hf2pause0"; Cmd "backext 1" 2
Head "head rot 25 0 0"; Start-Sleep 2; XrShot "xr_hf2pause1t"; Head "head rot 0 0 0"
Cmd "capmk hf2pause" 4
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 3
Cmd "mark HF2 end" 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
