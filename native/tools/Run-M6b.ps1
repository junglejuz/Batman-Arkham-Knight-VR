# Stage 6b regression: AKVR WITHOUT -akn (its normal geo-11 VR path) with the new dinput8, on the fake headset.
# AKN stays dormant (no -akn). Menus by Enter (title -> save -> Continue Story), then a headset shot. One launch.
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
$xs = "$env:LOCALAPPDATA\AKN\xrsim"
$root = "D:\Documents\Antigravity\VR_Modding"
$tools = "$root\ac6vr\tools\xrsim-tools"
$logs = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\BmGame\Logs"
$waited = 0
while (Get-Process | Where-Object { $_.Path -like "*steamapps*" }) {
    if ($waited % 300 -eq 0) { "{0:HH:mm:ss} another game is running - waiting" -f (Get-Date) }
    Start-Sleep 15; $waited += 15; if ($waited -ge 4 * 3600) { exit 1 }
}
if ($waited) { Start-Sleep 20 }
Copy-Item "$root\akvr\build-dinput8\Release\dinput8.dll" "$g\dinput8.dll" -Force
Copy-Item "$root\akvr_native\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force
$env:XR_RUNTIME_JSON = "$xs\akn_xrsim64.json"; $env:BVR_XRSIM_DIR = $xs; $env:SteamAppId = "208650"
foreach ($leaf in "command.txt", "state.json", "ack.txt") { if (Test-Path "$xs\$leaf") { Remove-Item "$xs\$leaf" -Force -Confirm:$false } }
Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akvr" -WorkingDirectory $g
$t0 = Get-Date
"{0:HH:mm:ss} launched -akvr (geo-11 path)" -f $t0
function Crashed { [bool](Get-ChildItem "$logs\UE3AutoReportDump*.txt" | Where-Object { $_.LastWriteTime -gt $t0 }) }
for ($i = 0; $i -lt 40; $i++) { Start-Sleep 3; if (Crashed -or -not (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { break }; if ((Get-Content "$xs\state.json" -Raw -ErrorAction SilentlyContinue) -match '"sessionRunning":\s*true') { break } }
"fake headset session: {0}" -f ((Get-Content "$xs\state.json" -Raw -ErrorAction SilentlyContinue) -match '"sessionRunning":\s*true')
Start-Sleep 20
python "$PSScriptRoot\akmenu.py" click 0.5 0.4; Start-Sleep 6           # skip the logos
foreach ($k in 1..4) { python "$PSScriptRoot\akmenu.py" key 0x0D; Start-Sleep 9 }   # title, save, Continue Story (+1 spare)
Start-Sleep 50                                                          # loading
$ok = (Get-Process BatmanAK -ErrorAction SilentlyContinue) -and -not (Crashed)
if ($ok) {
    try { $s = & "$tools\xrsim-shot.ps1" -Dir $xs -Out "$akn\reg_geo11" -Quiet -TimeoutSec 30; "headset shot: layers {0}, luma {1}/{2}, non-black {3}/{4}" -f $s.Layers, $s.MeanLumaL, $s.MeanLumaR, $s.NonBlackPctL, $s.NonBlackPctR }
    catch { "headset shot FAILED: $_" }
    python "$PSScriptRoot\akmenu.py" shot "$akn\reg_geo11_window.png" | Out-Null
}
$crash = Crashed
Get-Process BatmanAK -ErrorAction SilentlyContinue | ForEach-Object { if ($crash) { Stop-Process -Id $_.Id -Force } else { $null = $_.CloseMainWindow(); $_.WaitForExit(20000) | Out-Null; if (-not $_.HasExited) { Stop-Process -Id $_.Id -Force } } }
"{0:HH:mm:ss} closed (crashed: {1})" -f (Get-Date), $crash
$st = Get-Content "$g\akvr_startup_log.txt" -ErrorAction SilentlyContinue | Select-String "geo-11|engine stereo|build:" | Select-Object -First 6
$st | ForEach-Object { "startup: " + $_.Line }
