$env:AKN_LOADSHOT = "1"; $env:AKN_MENUSHOTS = "1"; $env:AKN_MENUVP = "1"
& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -ne 2 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { & "$PSScriptRoot\Run-HB-B.ps1" } else { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force }
Get-Job | Remove-Job -Force
