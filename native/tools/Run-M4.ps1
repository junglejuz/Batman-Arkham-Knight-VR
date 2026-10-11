# AKN test plan M4 (NOTES.md "TEST PLAN M4"): per-eye lens + per-eye view state, by command.txt.
# One launch; only a crash in the view-state test relaunches, straight into the next prepared variant (5, then 3).
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$log = "$g\akn\akn_log.txt"
function Cmd($c, $wait) {
    if (-not (Alive)) { "{0:HH:mm:ss} SKIP (game gone): {1}" -f (Get-Date), $c; return }
    Set-Content "$g\akn\command.txt" $c
    for ($i = 0; $i -lt 30 -and (Test-Path "$g\akn\command.txt"); $i++) { Start-Sleep -Milliseconds 100 }
    "{0:HH:mm:ss} sent: {1}" -f (Get-Date), $c
    Start-Sleep $wait
}
function Alive { [bool](Get-Process BatmanAK -ErrorAction SilentlyContinue) }
function Launch($vs, $tag) {
    if (Test-Path $log) { Copy-Item $log "$g\akn\akn_log_M4_$tag.txt" -Force }
    $env:SteamAppId = "208650"
    Start-Process -FilePath "$g\BatmanAK.exe" -ArgumentList "-akn", "-aknstereo", "aknvs=$vs" -WorkingDirectory $g
    for ($i = 0; $i -lt 70; $i++) {
        Start-Sleep 3
        $t = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($t -cmatch "eye renders \(render thread\) \d{4}" -or -not (Alive)) { break }
    }
    "{0:HH:mm:ss} launch vs=$vs : 3D scene up = $(Alive)" -f (Get-Date)
    Start-Sleep 4
}
function CloseGame {
    $p = Get-Process BatmanAK -ErrorAction SilentlyContinue
    if ($p) { $null = $p.CloseMainWindow(); $p.WaitForExit(20000) | Out-Null; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
}
function HistoryTests($tag) {
    Cmd "capture ${tag}1" 6              # own history, normal spacing
    Cmd "ipd 40" 2; Cmd "capture ${tag}2" 6; Cmd "ipd 6.4" 2   # wide spacing: sharp = per-eye history works
    Start-Sleep 30                       # soak
    Cmd "capture ${tag}3" 6
    Cmd "stereo 0" 8; Cmd "stereo 1" 10  # stereo off/on with the own history
    "{0:HH:mm:ss} history variant $tag alive at end: $(Alive)" -f (Get-Date)
}

if (Get-Process | Where-Object { $_.Path -like "*steamapps*" }) { "another game is running - stop"; exit 1 }
Copy-Item "$PSScriptRoot\..\build\Release\XINPUT9_1_0.dll" "$g\XINPUT9_1_0.dll" -Force

Launch 0 "prev"
Cmd "capture a" 6                        # T1 baseline: shared history, game's centred lens
Cmd "asym 1" 2; Cmd "capture b" 6        # T2 per-eye off-centre lens (Quest 3-like test angles)
Cmd "vs 4" 10                            # T3 own history in zeroed memory (may crash)
"alive after vs 4: $(Alive)"
if (Alive) { HistoryTests "c" }
else {
    "vs 4 crashed -> relaunch with vs 5"
    Launch 5 "vs4crash"; Cmd "asym 1" 2
    if (Alive) { HistoryTests "e" }
    if (-not (Alive)) {
        "vs 5 crashed -> relaunch with vs 3"
        Launch 3 "vs5crash"; Cmd "asym 1" 2
        if (Alive) { HistoryTests "f" }
    }
}
CloseGame
"{0:HH:mm:ss} done, game closed" -f (Get-Date)
