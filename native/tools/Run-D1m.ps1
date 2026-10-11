# DLSS D1m: one launch with dlss=5 (Performance) - motion mode comparison; dlss stays 5 afterwards (JJ's test build).
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$ini = "$g\akvr_settings.ini"
$t = [IO.File]::ReadAllText($ini)
if ($t -match "(?m)^dlss=\d+") { $t = [regex]::Replace($t, "(?m)^dlss=\d+", "dlss=5") } else { $t = $t.TrimEnd() + "`ndlss=5`n" }
$t = [regex]::Replace($t, "(?m)^engineres=\d+", "engineres=2860"); $t = [regex]::Replace($t, "(?m)^engineshape=\d+", "engineshape=925")
[IO.File]::WriteAllText($ini, $t)
& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -ne 2 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { & "$PSScriptRoot\Run-D1m-B.ps1" } else { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force }
