# DLSS stage D0b: one launch with UE3 ScreenPercentage=67 (BmSystemSettings.ini [SystemSettings], first occurrence);
# the file is backed up and put back afterwards (FIX_CHANGES.md).
$cfg = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\BmGame\Config\BmSystemSettings.ini"
$bak = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn\BmSystemSettings_before_D0b.ini"
Copy-Item $cfg $bak -Force
$t = [IO.File]::ReadAllText($cfg)
$re = [regex]"(?m)^ScreenPercentage=[\d.]+"
[IO.File]::WriteAllText($cfg, $re.Replace($t, "ScreenPercentage=67.000000", 1))
"ScreenPercentage set to 67 (first occurrence)"
& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -ne 2 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { & "$PSScriptRoot\Run-D0b-B.ps1" } else { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force }
Copy-Item $bak $cfg -Force
"BmSystemSettings.ini restored: " + ((Get-FileHash $cfg).Hash -eq (Get-FileHash $bak).Hash)
python "$PSScriptRoot\judge_d0.py"
$log = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn\akn_log_D0_sp67.txt"
if (Test-Path $log) { Select-String -Path $log -Pattern "D0b R16G16|D0 SMAA|D0 motion" | Select-Object -First 30 | ForEach-Object { $_.Line.Substring([Math]::Min(13, $_.Line.Length)) } }
