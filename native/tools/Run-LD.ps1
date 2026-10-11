# LD (2026-10-08): only part A - its LOADSHOT job takes the loading screen (desktop load_N.png, headset xr_load_N);
# the game is closed as soon as gameplay is reached.
& "$PSScriptRoot\Run-M5c-A.ps1"
Start-Sleep 3
Get-Job | Wait-Job -Timeout 90 | Out-Null; Get-Job | Remove-Job -Force
$p = Get-Process BatmanAK -ErrorAction SilentlyContinue
if ($p) { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
"{0:HH:mm:ss} game closed" -f (Get-Date)
