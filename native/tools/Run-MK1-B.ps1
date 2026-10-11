# MK1 marker probe (see below). One launch.
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
# MK1 probe (VR_WORLD_MARKERS_GUIDE Â§2-4): HUD picture + both eyes, HUD draw constants and marker projections,
# head straight, then turned 15 deg (the head-free reference must not move; the rebuilt full camera must)
Head "head rot 0 0 0"; Start-Sleep 2
# leave the training terminal (its popup hides the markers): walk back 1.5 s, check with a window shot
python "$PSScriptRoot\akmenu.py" hold 0x53 1.5; Start-Sleep 3
python "$PSScriptRoot\akmenu.py" shot "$akn\mk_walked.png"
# turn the game camera until the objective marker (Canvas.Project) is near the screen centre
function LastProjX {
    $l = Select-String -Path "$akn\akn_log.txt" -Pattern "PROJ ra 86DC0D" | Select-Object -Last 1
    if ($l -and $l.Line -match '-> ([-\d.]+) ([-\d.]+) depth [-\d.]+ w ([-\d.]+)') { return @([double]$Matches[1], [double]$Matches[3]) }
    return $null
}
for ($k = 0; $k -lt 14; $k++) {
    Cmd "projlog 2" 1
    $px = LastProjX
    if (-not $px) { "{0:HH:mm:ss} turn: no projection seen" -f (Get-Date); continue }
    $x = $px[0]; $w = $px[1]
    "{0:HH:mm:ss} turn step {1}: marker x {2:F2} (w {3:F0})" -f (Get-Date), $k, $x, $w
    if ($w -gt 0 -and [math]::Abs($x) -lt 0.25) { break }
    $dx = if ($w -le 0) { 600 } elseif ([math]::Abs($x) -gt 2) { 400 * [math]::Sign($x) } elseif ([math]::Abs($x) -gt 0.8) { 150 * [math]::Sign($x) } else { 60 * [math]::Sign($x) }
    python "$PSScriptRoot\akmenu.py" mouse $dx 0 | Out-Null
    Start-Sleep 1
}
python "$PSScriptRoot\akmenu.py" shot "$akn\mk_turned.png"
Cmd "mark MK1 straight" 1; Cmd "capture mk0" 6; Cmd "mkdump 60" 4
Head "head rot 15 0 0"; Start-Sleep 3
Cmd "mark MK1 turned" 1; Cmd "capture mk1" 6; Cmd "mkdump 60" 4
Head "head rot 0 0 0"; Start-Sleep 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
