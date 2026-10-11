& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -ne 2 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { & "$PSScriptRoot\Run-MC-B.ps1" } else { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force }
