# DLSS D1 follow-up (JJ 2026-10-08): Performance mode at the full headset size, then Quality at a 2448-tall output.
& "$PSScriptRoot\Run-D1.ps1" 5 2860
Copy-Item "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn\akn_log_D1.txt" "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn\akn_log_D1_perf.txt" -Force
Copy-Item "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn\desktop_mirror.png" "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn\desktop_mirror_perf.png" -Force -ErrorAction SilentlyContinue
& "$PSScriptRoot\Run-D1.ps1" 3 2448
