# AKN stage 5c, the whole run in ONE launch: part A (wait for other games, deploy, fake headset, menus to gameplay)
# then part B (tests, close, judge). A crash in either part force-closes the game (JJ is away).
& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -eq 2 -or -not (Get-Process BatmanAK -ErrorAction SilentlyContinue)) {
    "part A did not reach gameplay - judging what there is"
    Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force
    python "$PSScriptRoot\judge_m5c.py"
    exit 1
}
& "$PSScriptRoot\Run-M5c-B.ps1"
