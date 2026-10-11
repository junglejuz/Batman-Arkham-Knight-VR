# AKN stage 5m in ONE launch: part A (wait for other games, deploy, fake headset, menus to gameplay), then part B
# (VR panel + rain tests, close, judge). A crash in either part force-closes the game (JJ is away).
& "$PSScriptRoot\Run-M5c-A.ps1"
if ($LASTEXITCODE -eq 2 -or -not (Get-Process BatmanAK -ErrorAction SilentlyContinue)) {
    "part A did not reach gameplay - judging what there is"
    Get-Process BatmanAK -ErrorAction SilentlyContinue | Stop-Process -Force
    python "$PSScriptRoot\judge_m5m.py"
    exit 1
}
& "$PSScriptRoot\Run-M5m-B.ps1"
