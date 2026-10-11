# RF3: one reflection setting off for one launch (BmSystemSettings.ini, every section), the RF2 per-eye test, then the
# player's file restored byte for byte. Usage: Run-RF3.ps1 -Key AllowImageReflections   (or Reflections)
param([string]$Key = "AllowImageReflections")
$ini = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\BmGame\Config\BmSystemSettings.ini"
$bak = "$ini.rf3-backup"
Copy-Item $ini $bak -Force
$t = [IO.File]::ReadAllText($ini)
$n = ([regex]::Matches($t, "(?m)^$Key=True")).Count
$t2 = [regex]::Replace($t, "(?m)^$Key=True", "$Key=False")
[IO.File]::WriteAllText($ini, $t2, (New-Object System.Text.UTF8Encoding($false)))
"{0:HH:mm:ss} RF3: $Key=False in $n line(s)" -f (Get-Date)
try { & "$PSScriptRoot\Run-RF2.ps1" }
finally {
    Copy-Item $bak $ini -Force; Remove-Item $bak -Force
    "{0:HH:mm:ss} RF3: settings file restored ({1})" -f (Get-Date), ((Get-FileHash $ini).Hash.Substring(0, 12))
}
