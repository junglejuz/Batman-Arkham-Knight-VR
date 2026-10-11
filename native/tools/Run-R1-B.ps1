# R1 (JJ 2026-10-08: "some of the reflections on Batman appear to be mismatched per eye ... not converging"):
# same spot + same frame, each eye its own turned lens (eyesame 2) -> the right eye warped onto the left must match.
# Still head vs a head sweep, DLSS motion modes 1 / 2 / 0; control = same camera (eyesame 1). Judged by judge_r1.py.
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
function Head($c) { & "$tools\xrsim-cmd.ps1" -Dir $xs -Quiet $c | Out-Null }
function SweepShot($tag) {
    # a slow head turn (about 25 deg/s) with the pair picture taken in the middle of it
    if (-not (Alive)) { "{0:HH:mm:ss} SKIP sweep {1}" -f (Get-Date), $tag; return }
    $y = -12
    for ($k = 0; $k -lt 6; $k++) { Head "head rot $y 0 0"; $y += 2 }
    Set-Content "$akn\command.txt" "pairshot $tag"
    for ($k = 0; $k -lt 6; $k++) { Head "head rot $y 0 0"; $y += 2 }
    "{0:HH:mm:ss} sweep shot {1} (head yaw -12 -> {2})" -f (Get-Date), $tag, $y
    Head "head rot 0 0 0"; Start-Sleep 3
}
foreach ($f in Get-ChildItem "$akn\pair_r*.bmp" -ErrorAction SilentlyContinue) { Remove-Item $f.FullName -Force -Confirm:$false }
Head "head rot 0 0 0"
Cmd "mark R0" 3
Cmd "pairshot rnorm" 4                                               # normal stereo, for looking at Batman
Cmd "eyesame 1" 2; Cmd "framesync 1" 3; Cmd "pairshot rctl" 4      # control: same camera and lens
Cmd "eyesame 2" 4
foreach ($m in 1, 2, 0) {
    Cmd "dlssmv $m" 4                                                # new DLSS features: history rebuilt
    Cmd "mark R mv$m still" 1; Cmd "pairshot rs$m" 4
    Cmd "mark R mv$m sweep" 1; SweepShot "rm$m"
}
Cmd "dlssmv 1" 2; Cmd "eyesame 0" 2; Cmd "mark R end" 1
$crash = Crashed
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { if ($crash) { Stop-Process -Id $p.Id -Force } else { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } }
"{0:HH:mm:ss} game closed (crashed: {1}); judging" -f (Get-Date), $crash
python "$PSScriptRoot\judge_r1.py" $akn
