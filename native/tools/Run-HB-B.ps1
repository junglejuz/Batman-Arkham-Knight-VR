# HB (2026-10-09): HUDVP (window-size HUD layouts stretched to the sheet), wheel key discovery + shape + backdrop skip,
# pause backdrop candidates, LOADFIT2 (loading shots in part A). One launch.
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
function Stat($tag, $n) { Cmd "mark HB $tag" 1; Cmd "hudstat $n" 2 }
function LastCells { $l = Select-String -Path "$akn\akn_log.txt" -Pattern "HUDSTAT .* sheet cells (\d+)" | Select-Object -Last 1; if ($l) { [int]$l.Matches[0].Groups[1].Value } else { -1 } }
function HoldBg($vk, $secs) { Start-Job -ScriptBlock { param($here, $vk, $secs) python "$here\akmenu.py" hold $vk $secs } -ArgumentList $PSScriptRoot, $vk, $secs }
Cmd "mark HB start" 1
Head "head rot 0 0 0"; Start-Sleep 2
# 2. gameplay HUD with HUDVP on / off (nothing else may move)
Stat "game-vp1" 20; Cmd "capmk hbgame" 4; XrShot "xr_hbgame"
Cmd "hudvp 0" 2; Stat "game-vp0" 20; XrShot "xr_hbgamevp0"; Cmd "hudvp 1" 2
# 3. which key opens a wheel (pad buttons are keyboard aliases: D-pad = arrows, RB = Equals)
$wheelKey = $null
foreach ($k in 0x27, 0x25, 0x26, 0x28, 0xBB) {
    $base = LastCells
    $j = HoldBg $k 4.5; Start-Sleep 2.5
    Stat ("key" + $k) 3; Start-Sleep 0.5; $c = LastCells; XrShot ("xr_hbkey" + $k)
    "{0:HH:mm:ss} key {1:X}: sheet cells {2} (before {3})" -f (Get-Date), $k, $c, $base
    Wait-Job $j -Timeout 10 | Out-Null; Remove-Job $j -Force; Start-Sleep 2
    if (-not $wheelKey -and $c -gt 60) { $wheelKey = $k }
}
"{0:HH:mm:ss} wheel key: {1}" -f (Get-Date), $wheelKey
# 4. the wheel: shape (capture), backdrop dropped (gradient vignette draw)
if ($wheelKey) {
    $j = HoldBg $wheelKey 9; Start-Sleep 2.5
    Cmd "capmk hbwheel" 4; XrShot "xr_hbwheel"; Stat "wheel" 3
    Cmd "hudskip add 9689d605c06946bf 256 16" 1; Stat "wheel-skip" 3; XrShot "xr_hbwheelskip"; Cmd "hudskip clear" 1
    Wait-Job $j -Timeout 15 | Out-Null; Remove-Job $j -Force; Start-Sleep 2
    Cmd "menufitstat" 1
}
# 5. pause backdrop candidates
Cmd "mark HB pause" 1
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 4
Cmd "capmk hbpause" 4; XrShot "xr_hbpause"; Stat "pause" 3
$cands = @("4b432a87d072f988 1024 1024", "3b819a7e86e9d631 256 1", "54cd897e5ce3b8b3 256 1", "9689d605c06946bf 256 16", "94f88871e378a57c 0 0")
$i = 0
foreach ($cd in $cands) { $i++; Cmd "hudskip add $cd" 1; Stat ("pause-skip" + $i) 3; XrShot ("xr_hbpauseskip" + $i); Cmd "hudskip clear" 1 }
python "$PSScriptRoot\akmenu.py" key 0x1B; Start-Sleep 3
Cmd "menufitstat" 1
Cmd "mark HB end" 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1})" -f (Get-Date), $crash
