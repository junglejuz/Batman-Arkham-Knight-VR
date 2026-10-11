# MB (MIPBIAS, DLSS texture sharpness): same spot, still head; per mode two pair pictures 1 s apart.
# Detail (sharpness) and still-head change (shimmer) per mode 0 off / 1 full / 2 mild. Judged by judge_mb.py.
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
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
foreach ($f in Get-ChildItem "$akn\pair_mb*.bmp" -ErrorAction SilentlyContinue) { Remove-Item $f.FullName -Force -Confirm:$false }
Cmd "mark MB0" 3
Cmd "rainview 1" 2                       # rain hidden: its streaks would swamp the shimmer measure
foreach ($m in 0, 3, 1, 2) {
    Cmd "mipbias $m" 4
    Cmd "mark MB mode $m" 1; Cmd "pairshot mb${m}a" 2; Cmd "pairshot mb${m}b" 3
}
Cmd "rainview 0" 1; Cmd "mark MB end" 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1}); judging" -f (Get-Date), $crash
python "$PSScriptRoot\judge_mb.py" $akn
