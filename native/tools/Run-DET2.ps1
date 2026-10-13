# DET2 (2026-10-11, JJ: "Investigate mode seems to be missing some lights in the left eye"; capture c190600: the green /
# warm lights on the pipes, tanks and the building are in the right eye only). JJ stands still in detective mode, looking
# at a lit area; this sends one switch at a time and saves the pair picture for each, then puts every switch back.
# Judge offline: Measure-DET2.py (left vs right brightness of the lit area, per step). JJ only holds still (~70 s).
$g = "E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
$akn = "$g\akn"
function Alive { [bool](Get-Process BatmanAK -ErrorAction SilentlyContinue) }
function Cmd($c, $wait) {
    if (-not (Alive)) { "{0:HH:mm:ss} SKIP (game gone): {1}" -f (Get-Date), $c; return }
    Set-Content "$akn\command.txt" $c
    for ($i = 0; $i -lt 30 -and (Test-Path "$akn\command.txt"); $i++) { Start-Sleep -Milliseconds 100 }
    "{0:HH:mm:ss} sent: {1}" -f (Get-Date), $c
    Start-Sleep $wait
}
# step = (switch to set, value to put back) - the control first and last, so a drift (JJ moved) shows
$steps = @(
    @('', ''),                                   # d0 control
    @('eyesame 1', 'eyesame 0'),                 # d1 both eyes the same camera + lens: equal = view-dependent (culling / lens)
    @('eyesame 3', 'eyesame 0'),                 # d2 own eye spots, the LEFT lens for both
    @('eyesame 2', 'eyesame 0'),                 # d3 the same spot, own lenses
    @('gloss 0', 'gloss 1'),                     # d4 suit-gloss camera rows off (a light shader caught by the flag?)
    @('vssync 0', 'vssync 1'),                   # d5 ambient light copy game -> right eye off
    @('vssplit 0', 'vssplit 1'),                 # d6 right eye's view-state split off
    @('detvig 0', 'detvig 1'),                   # d7 the game's own detective composite back
    @('mipbias 0', 'mipbias 1'),                 # d8 sampler swaps off
    @('framesync 0', 'framesync 1'),             # d9 frame number sync off
    @('rainonce 0', 'rainonce 1'),               # d10 once-per-pair skips off
    @('', '')                                    # d11 control again
)
Cmd "mark DET2 begin" 1
for ($i = 0; $i -lt $steps.Count; $i++) {
    $on, $off = $steps[$i]
    if ($on) { Cmd $on 2 }
    Cmd "pairshot det$i" 3
    if ($off) { Cmd $off 1 }
}
Cmd "mark DET2 end" 1
"done - pictures akn\pair_det0..det$($steps.Count - 1).bmp"
