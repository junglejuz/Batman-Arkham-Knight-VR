# PROMPT (2026-10-11, JJ: "some game hints are not being shown on screen (for example, A to rescue)"). JJ stands where the
# game should show a button prompt (rescue, open, interrogate ...), head still; this saves a capture per switch (both eyes,
# HUD sheet, both eye marker layers - PROMPTCAP - and the marker routing log), then puts every switch back. ~40 s.
# Where the prompt turns up tells where it was lost:
#   p0 control: in mk_left / mk_right = routed as a world marker (placed wrong?); on hud_rgb = on the sheet (AKVR side);
#      nowhere = drawn outside the HUD capture
#   p1 hudall 1: appears now = the game draws it outside the HUD phase (lands on the window only)
#   p2 mkeye 0 / p3 marksheet 0: appears now = the marker routing took it
#   p4 mkgs 0: appears now = the per-vertex marker placement puts it out of view
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
$steps = @(
    @('', ''),
    @('hudall 1', 'hudall 0'),
    @('mkeye 0', 'mkeye 1'),
    @('marksheet 0', 'marksheet 1'),
    @('mkgs 0', 'mkgs 1'),
    @('', '')
)
Cmd "mark PROMPT begin" 1
for ($i = 0; $i -lt $steps.Count; $i++) {
    $on, $off = $steps[$i]
    if ($on) { Cmd $on 2 }
    Cmd "capmk p$i" 4
    if ($off) { Cmd $off 1 }
}
Cmd "mark PROMPT end" 1
"done - akn\pair_p*.bmp, hud_rgb_p*.bmp, mk_left_p*.bmp / mk_right_p*.bmp"
