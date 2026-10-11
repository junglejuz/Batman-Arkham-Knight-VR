# RF4: AllowImageReflectionShadowing off for one launch (BmSystemSettings.ini, every section), the per-eye gloss test,
# then the player's file restored byte for byte (RF3's method). JJ 2026-10-08: no extra pixels for the gloss.
param([string]$Key = "AllowImageReflectionShadowing")
$ini = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\BmGame\Config\BmSystemSettings.ini"
$bak = "$ini.rf4-backup"
Copy-Item $ini $bak -Force
$t = [IO.File]::ReadAllText($ini)
$n = ([regex]::Matches($t, "(?m)^$Key=True")).Count
$t2 = [regex]::Replace($t, "(?m)^$Key=True", "$Key=False")
[IO.File]::WriteAllText($ini, $t2, (New-Object System.Text.UTF8Encoding($false)))
"{0:HH:mm:ss} RF4: $Key=False in $n line(s)" -f (Get-Date)
try {
    & "$PSScriptRoot\Run-M5c-A.ps1"
    if ($LASTEXITCODE -ne 2 -and (Get-Process BatmanAK -ErrorAction SilentlyContinue)) { & "$PSScriptRoot\Run-RF4-B.ps1" } else { "part A did not reach gameplay"; Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force }
}
finally {
    Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    Copy-Item $bak $ini -Force; Remove-Item $bak -Force
    "{0:HH:mm:ss} RF4: settings file restored ({1})" -f (Get-Date), ((Get-FileHash $ini).Hash.Substring(0, 12))
}
