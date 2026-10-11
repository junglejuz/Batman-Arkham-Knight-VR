# DLSS stage D0: two launches - today's eye size, then 67 % per side (the DLSS Quality render size), no DLSS.
# akvr_settings.ini engineres is put back afterwards.
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$ini = "$g\akvr_settings.ini"
$orig = 2860   # JJ's full eye size, fixed: a stopped run once left a test size in the ini and the next run took it as the base
"engine eye size now $orig"
$runs = @(@{l = "67pct"; h = [int][math]::Round([int]$orig * 0.67)}, @{l = "58pct"; h = [int][math]::Round([int]$orig * 0.58)}, @{l = "50pct"; h = [int][math]::Round([int]$orig * 0.50)})
if ($args -contains "native") { $runs = @(@{l = "native"; h = $orig}) + $runs }
foreach ($run in $runs) {
    $t = [IO.File]::ReadAllText($ini); [IO.File]::WriteAllText($ini, ([regex]::Replace($t, "(?m)^engineres=\d+", "engineres=$($run.h)")))   # keeps the file's own line endings
    "=== D0 run $($run.l): engineres=$($run.h)"
    & "$PSScriptRoot\Run-M5c-A.ps1"
    if ($LASTEXITCODE -eq 2 -or -not (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force; continue }
    & "$PSScriptRoot\Run-D0-B.ps1" -label $run.l
}
$t = [IO.File]::ReadAllText($ini); [IO.File]::WriteAllText($ini, ([regex]::Replace($t, "(?m)^engineres=\d+", "engineres=$orig")))
$t = [IO.File]::ReadAllText($ini); [IO.File]::WriteAllText($ini, ([regex]::Replace($t, "(?m)^engineshape=\d+", "engineshape=925")))   # JJ's shape (D0: a fought window once saved 813)
"engineres restored to $orig, engineshape 925"
python "$PSScriptRoot\judge_d0.py"
