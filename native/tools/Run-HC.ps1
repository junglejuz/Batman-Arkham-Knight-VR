$env:AKN_LOADSHOT = "1"
& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -ne 2 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { & "$PSScriptRoot\Run-HC-B.ps1" } else { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force }
Get-Job | Wait-Job -Timeout 60 | Out-Null; Get-Job | Remove-Job -Force
