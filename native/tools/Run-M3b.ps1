# AKN test plan M3b (NOTES.md "TEST PLAN M3b"): one launch, every test by command.txt, game closed at the end.
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$log = "$g\akn\akn_log.txt"
function Cmd($c, $wait) {
    Set-Content "$g\akn\command.txt" $c
    for ($i = 0; $i -lt 30 -and (Test-Path "$g\akn\command.txt"); $i++) { Start-Sleep -Milliseconds 100 }
    "{0:HH:mm:ss} sent: {1}" -f (Get-Date), $c
    Start-Sleep $wait
}
function Alive { [bool](Get-Process BatmanAK -ErrorAction SilentlyContinue) }

if (Get-Process | Where-Object { $_.Path -like "*steamapps*" }) { "another game is running - stop"; exit 1 }
Copy-Item "$PSScriptRoot\..\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force
$env:SteamAppId = "208650"
Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akn", "-aknstereo", "aknvs=0" -WorkingDirectory $g
for ($i = 0; $i -lt 70; $i++) {
    Start-Sleep 3
    $t = Get-Content $log -Raw -ErrorAction SilentlyContinue
    if ($t -cmatch "eye renders \(render thread\) \d{4}" -or -not (Alive)) { break }
}
if (-not (Alive)) { "game not running before the tests"; exit 1 }
"{0:HH:mm:ss} 3D scene up" -f (Get-Date)
Start-Sleep 5
Cmd "trace" 3                       # T1
Cmd "capture a" 6                   # T2
Cmd "ipd 40" 2; Cmd "capture b" 6; Cmd "ipd 6.4" 2   # T3
Cmd "stereo 0" 15; Cmd "stereo 1" 15                 # T5
Cmd "vs 2" 5; Cmd "capture c" 6; Cmd "vs 0" 3        # T6
"alive before T7: $(Alive)"
Cmd "vs 3" 10                       # T7 (may crash)
"alive after T7: $(Alive)"
if (Alive) { Cmd "capture d" 6 }
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
"{0:HH:mm:ss} done, game closed" -f (Get-Date)
