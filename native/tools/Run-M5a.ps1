# AKN stage 5a (NOTES.md "5a test - measured pass/fail"): one launch, desktop only, judged by judge_m5a.py.
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$log = "$akn\akn_log.txt"
function Alive { [bool](Get-Process BatmanAK -ErrorAction SilentlyContinue) }
function Cmd($c, $wait) {
    if (-not (Alive)) { "{0:HH:mm:ss} SKIP (game gone): {1}" -f (Get-Date), $c; return }
    Set-Content "$akn\command.txt" $c
    for ($i = 0; $i -lt 30 -and (Test-Path "$akn\command.txt"); $i++) { Start-Sleep -Milliseconds 100 }
    "{0:HH:mm:ss} sent: {1}" -f (Get-Date), $c
    Start-Sleep $wait
}
function Shot($name) { if (Alive) { python "$PSScriptRoot\shot.py" "$akn\$name" | Out-Null; "{0:HH:mm:ss} screenshot {1}" -f (Get-Date), $name } }

# another session's game (Steam runs one at a time): wait for it to finish, then start (JJ 2026-10-07)
$waited = 0
while (Get-Process | Where-Object { $_.Path -like "*steamapps*" }) {
    if ($waited % 300 -eq 0) { "{0:HH:mm:ss} another game is running ({1}) - waiting for it to close" -f (Get-Date), ((Get-Process | Where-Object { $_.Path -like "*steamapps*" }).Name -join ", ") }
    Start-Sleep 15; $waited += 15
    if ($waited -ge 4 * 3600) { "gave up waiting after 4 h"; exit 1 }
}
if ($waited) { "{0:HH:mm:ss} the other game closed - starting in 20 s" -f (Get-Date); Start-Sleep 20 }
foreach ($f in "pair_h0.bmp", "pair_h1.bmp", "hud_rgb_h1.bmp", "hud_alpha_h1.bmp", "shot_h0.bmp", "shot_h1.bmp", "M5a_result.txt") {
    if (Test-Path "$akn\$f") { Remove-Item "$akn\$f" -Force -Confirm:$false }
}
Copy-Item "$PSScriptRoot\..\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force
$env:SteamAppId = "208650"
Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akn", "-aknstereo" -WorkingDirectory $g
for ($i = 0; $i -lt 70; $i++) {
    Start-Sleep 3
    $t = Get-Content $log -Raw -ErrorAction SilentlyContinue
    if ($t -cmatch "eye renders \(render thread\) \d{4}" -or -not (Alive)) { break }
}
"{0:HH:mm:ss} 3D scene up = $(Alive)" -f (Get-Date)
Start-Sleep 4
Cmd "mark T1" 1; Cmd "capture h0" 5; Shot "shot_h0.bmp"
Cmd "hud 1" 3; Cmd "mark T2" 1; Cmd "capture h1" 5; Shot "shot_h1.bmp"; Start-Sleep 2
Cmd "mark T3 start" 60; Cmd "mark T3 end" 3
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
"{0:HH:mm:ss} game closed; judging" -f (Get-Date)
python "$PSScriptRoot\judge_m5a.py" $akn
