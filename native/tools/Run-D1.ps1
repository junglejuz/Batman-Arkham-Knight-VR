# DLSS stage D1: one launch with dlss=3 (Quality) in akvr_settings.ini; put back to 0 afterwards.
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$ini = "$g\akvr_settings.ini"
$mode = if ($args.Count -gt 0) { $args[0] } else { 3 }
$eres = if ($args.Count -gt 1) { $args[1] } else { 2860 }   # headset eye height (the DLSS output size)
$keep = $args -contains "keep"                              # leave this DLSS setting in place for JJ's headset test
$t = [IO.File]::ReadAllText($ini)
if ($t -match "(?m)^dlss=\d+") { $t = [regex]::Replace($t, "(?m)^dlss=\d+", "dlss=$mode") } else { $t = $t.TrimEnd() + "`ndlss=$mode`n" }
$t = [regex]::Replace($t, "(?m)^engineres=\d+", "engineres=$eres"); $t = [regex]::Replace($t, "(?m)^engineshape=\d+", "engineshape=925")
[IO.File]::WriteAllText($ini, $t)
"dlss=$mode set"
& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -ne 2 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { & "$PSScriptRoot\Run-D1-B.ps1" } else { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force }
$t = [IO.File]::ReadAllText($ini); if (-not $keep) { $t = [regex]::Replace($t, "(?m)^dlss=\d+", "dlss=0") }; $t = [regex]::Replace($t, "(?m)^engineres=\d+", "engineres=2860"); $t = [regex]::Replace($t, "(?m)^engineshape=\d+", "engineshape=925"); [IO.File]::WriteAllText($ini, $t)
"dlss=0, engineres 2860, engineshape 925 restored"
$log = "$g\akn\akn_log_D1.txt"
if (Test-Path $log) {
    python "$PSScriptRoot\judge_d1.py"
}
