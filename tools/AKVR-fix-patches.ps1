# AKVR - apply AKVR's changes to the Arkham Knight geo-11 fix, in place.
#
# The fix is downloaded by the user from its original source, so AKVR never ships edited fix
# files. This script edits the installed fix instead. It is safe to run again (it skips what is
# already done) and it keeps the originals in akvr_fix_backup\ so -Mode undo puts them back.
#
# What it changes:
#   1. HUDDEPTH (2026-09-28): the fix's 13 HUD vertex shaders in ShaderFixesDM. Flat HUD pieces
#      (w == 1) never received any left/right shift, so they sat at infinity and geo-11's HUD
#      value (StereoParams[1].y, which AKVR's "HUD distance" slider drives live) never reached
#      them. Each shader now adds that value to flat pieces, except in menus (IniParams x11) and
#      for the icons that already follow scene depth (texture filter 2 / 42 in gameplay). The
#      compiled .bin beside each edited shader is removed so geo-11 rebuilds it from the text.
#   1b. HUDSPLIT (2026-09-28): the same 13 shaders read a switch in constant buffer 13 so AKVR can draw
#      the flat HUD into its own headset layer and the scene-depth pieces (reticle) into the picture.
#      Unbound (no AKVR layer) = everything drawn as before.
#   2. d3dxdm.ini: AKVR HUDFIX keys in [Stereo] and the EXPOSURE sections (tiny render targets
#      drawn once for both eyes, so the two eyes get the same brightness).
#
# Usage: AKVR-fix-patches.bat (apply), AKVR-fix-patches-undo.bat, or
#        powershell -ExecutionPolicy Bypass -File AKVR-fix-patches.ps1 -Mode apply|undo|status
# Run it from the game's Binaries\Win64 folder with the game closed.

param([ValidateSet('apply', 'undo', 'status')][string]$Mode = 'apply',
      [string]$GameDir)   # Binaries\Win64; default = the folder this script is in (Install-AKVR.ps1 passes it)

$dir    = if ($GameDir) { $GameDir } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$script:failed = 0      # files an edit could not be applied to (exit code 2 when > 0)
$dm     = Join-Path $dir 'ShaderFixesDM'
$bak    = Join-Path $dir 'akvr_fix_backup'
$bakDm  = Join-Path $bak 'ShaderFixesDM'
$dmIni  = Join-Path $dir 'd3dxdm.ini'
$marker = '// AKVR HUDDEPTH'
$utf8   = New-Object System.Text.UTF8Encoding($false)   # no BOM, ever
$hudVs  = @('9938094af96353c0', '3b819a7e86e9d631', '05154232f7872d0d', '599bd060016570bb',
            'fd60f2d764b91756', 'c9b47e60935f3f03', '4b432a87d072f988', '54cd897e5ce3b8b3',
            'e12863b9484b4660', '7d8fcdc225ce3fc7', 'ef1c1604346331e2', '9689d605c06946bf',
            '91e2b2225bf3ea04')

function Say([string]$text, [string]$color = 'Gray') {
    if ($color -eq 'Red') { $script:failed++ }   # every "NOT patched" report is red
    Write-Host $text -ForegroundColor $color
}

function Backup-Once([string]$src, [string]$destDir) {
    if (-not (Test-Path $src)) { return }
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Force $destDir | Out-Null }
    $dst = Join-Path $destDir (Split-Path -Leaf $src)
    if (-not (Test-Path $dst)) { Copy-Item $src $dst }
}

# ---- 1. HUD shaders -----------------------------------------------------------------------------
function Patch-HudShader([string]$hash) {
    $txt = Join-Path $dm "$hash-vs.txt"
    $bin = Join-Path $dm "$hash-vs.bin"
    if (-not (Test-Path $txt)) { Say "  $hash : not in this fix - skipped" 'Yellow'; return }
    $s = [System.IO.File]::ReadAllText($txt)
    if ($s.Contains($marker)) { Say "  $hash : already patched"; return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }

    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    if (-not $temps.Success) { Say "  $hash : no dcl_temps - NOT patched" 'Red'; return }
    $n = [int]$temps.Groups[1].Value
    $t = "r$n"

    # The fix's stereo tail: w != 1 gets x += S*(w - C); flat pieces (w == 1) were left alone.
    $tail = [regex]::Matches($s, '(?m)^(?<i>[ \t]*)ne (?<a>r\d+)\.y, l\(1\.000000\), (?<b>r\d+)\.w[ \t]*\r?\n[ \t]*movc \k<b>\.x, \k<a>\.y, \k<a>\.x, \k<b>\.x[ \t]*$')
    if ($tail.Count -ne 1) { Say "  $hash : stereo tail found $($tail.Count) times - NOT patched" 'Red'; return }
    $m = $tail[0]; $a = $m.Groups['a'].Value; $b = $m.Groups['b'].Value; $i = $m.Groups['i'].Value

    $block = @(
        "$marker 2026-09-28: flat HUD pieces (w == 1) take geo-11's live HUD shift, StereoParams[1].y",
        "// (per eye; AKVR's HUD distance slider). Not in menus (x11), not the icons that follow scene depth (filter 2 / 42).",
        "ld_indexable(buffer)(float,float,float,float) $t.xyzw, l(1, 0, 0, 0), t125.xyzw",
        "ld_indexable(texture1d)(float,float,float,float) $t.x, l(11, 0, 0, 0), t120.xyzw",
        "ld_indexable(texture1d)(float,float,float,float) $t.z, l(2, 0, 0, 0), t120.xyxw",
        "eq $t.x, $t.x, l(0.000000)",
        "eq $t.zw, $t.zzzz, l(0.000000, 0.000000, 2.000000, 42.000000)",
        "or $t.z, $t.z, $t.w",
        "not $t.z, $t.z",
        "and $t.x, $t.x, $t.z",
        "and $t.y, $t.y, $t.x",
        "add $t.y, $t.y, $b.x",
        "ne $a.y, l(1.000000), $b.w",
        "movc $b.x, $a.y, $a.x, $t.y"
    ) | ForEach-Object { $i + $_ }
    $s = $s.Substring(0, $m.Index) + ($block -join $nl) + $s.Substring($m.Index + $m.Length)
    $s = $s.Substring(0, $temps.Index) + "dcl_temps $($n + 1)" + $s.Substring($temps.Index + $temps.Length)

    Backup-Once $txt $bakDm
    Backup-Once $bin $bakDm
    [System.IO.File]::WriteAllText($txt, $s, $utf8)
    if (Test-Path $bin) { Remove-Item $bin }
    Say "  $hash : patched" 'Green'
}

# ---- 1b. HUDSPLIT: one switch for "flat pieces only" / "scene-depth pieces only" -------------------
# AKVR draws the HUD into its own headset layer. Pieces that follow scene depth (the grapple reticle,
# fix texture filter 2 / 42) must stay in the 3D picture, so AKVR draws each HUD piece twice with a
# switch in constant buffer 13: 1 = keep flat pieces only (the layer), 2 = keep scene-depth pieces only
# (the picture), 0 = everything (unbound = 0, so without AKVR's layer nothing changes). Dropped pieces
# are moved off screen. This step reads the decision only in 9938094af96353c0 and 05154232f7872d0d and marks
# the others all flat; 1e (DEPTHALL) then gives the other 11 their own decision back.
$splitMarker = '// AKVR HUDSPLIT'
function Patch-HudSplit([string]$hash) {
    $txt = Join-Path $dm "$hash-vs.txt"
    $bin = Join-Path $dm "$hash-vs.bin"
    if (-not (Test-Path $txt)) { Say "  $hash : not in this fix - skipped" 'Yellow'; return }
    $s = [System.IO.File]::ReadAllText($txt)
    if ($s.Contains($splitMarker)) { Say "  $hash : split already patched"; return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    $code = $s.IndexOf('// HLSL Code')
    if ($code -lt 0) { $code = $s.Length }

    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    if (-not $temps.Success) { Say "  $hash : no dcl_temps - split NOT patched" 'Red'; return }
    $n = [int]$temps.Groups[1].Value
    $t = "r$n"
    if ($s -match '(?i)\bcb13\b') { Say "  $hash : already uses cb13 - split NOT patched" 'Red'; return }
    $cbDecl = [regex]::Match($s, '(?m)^dcl_constantbuffer [^\r\n]*$')
    if (-not $cbDecl.Success) { Say "  $hash : no constant buffer declaration - split NOT patched" 'Red'; return }
    $rets = [regex]::Matches($s.Substring(0, $code), '(?m)^[ \t]*ret[ \t]*$')
    if ($rets.Count -ne 1) { Say "  $hash : $($rets.Count) returns - split NOT patched" 'Red'; return }
    $ret = $rets[0]
    # The position output differs per shader (o2 / o3 / o4). Writing an undeclared output crashed the
    # NVIDIA driver at game start (HUDLAYER6, 2026-09-28): always use the shader's own.
    $posDecl = [regex]::Match($s, '(?m)^dcl_output_siv (?<o>o\d+)\.xyzw, position\s*$')
    if (-not $posDecl.Success) { Say "  $hash : position output not found - split NOT patched" 'Red'; return }
    $pos = $posDecl.Groups['o'].Value

    # The fix's own scene-depth decision: the if_nz right after the "filter 42 / 42 / 2" compare.
    $depthSrc = 'l(0)'
    $eq42 = [regex]::Match($s, '(?m)^[ \t]*eq r\d+\.\w+, r\d+\.\w+, l\(42\.000000, 42\.000000, 2\.000000')
    $ifnz = $null
    if ($eq42.Success) {
        $ifnz = [regex]::Match($s.Substring($eq42.Index), '(?m)^(?<i>[ \t]*)if_nz (?<c>r\d+\.[xyzw])[ \t]*$')
        if (-not $ifnz.Success) { Say "  $hash : scene-depth test not found - split NOT patched" 'Red'; return }
        $depthSrc = $ifnz.Groups['c'].Value
    }

    $tail = @(
        "$splitMarker 2026-09-28: cb13[0].x = 1 keeps flat pieces only (AKVR's HUD layer), 2 keeps the",
        "// scene-depth pieces only (filter 2 / 42, the 3D picture), 0 / unbound keeps everything. Dropped = off screen.",
        "eq $t.yz, cb13[0].xxxx, l(0.000000, 1.000000, 2.000000, 0.000000)",
        "and $t.y, $t.y, $t.x",
        "not $t.w, $t.x",
        "and $t.z, $t.z, $t.w",
        "or $t.y, $t.y, $t.z",
        "if_nz $t.y",
        "  mov $pos.xyzw, l(-10.000000, -10.000000, 0.000000, 1.000000)",
        "endif"
    )
    if (-not $eq42.Success) { $tail = @($tail[0], $tail[1], "mov $t.x, l(0)") + $tail[2..($tail.Count - 1)] }

    # Edit from the end backwards so earlier offsets stay valid.
    $s = $s.Substring(0, $ret.Index) + (($tail -join $nl) + $nl) + $s.Substring($ret.Index)
    if ($eq42.Success) {
        $at = $eq42.Index + $ifnz.Index
        $pad = $ifnz.Groups['i'].Value
        $s = $s.Substring(0, $at) + $pad + "${splitMarker}: remember the fix's scene-depth decision" + $nl +
             $pad + "mov $t.x, $depthSrc" + $nl + $s.Substring($at)
    }
    $s = $s.Substring(0, $cbDecl.Index + $cbDecl.Length) + $nl + 'dcl_constantbuffer CB13[1], immediateIndexed' + $s.Substring($cbDecl.Index + $cbDecl.Length)
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    $s = $s.Substring(0, $temps.Index) + "dcl_temps $($n + 1)" + $s.Substring($temps.Index + $temps.Length)

    Backup-Once $txt $bakDm
    Backup-Once $bin $bakDm
    [System.IO.File]::WriteAllText($txt, $s, $utf8)
    if (Test-Path $bin) { Remove-Item $bin }
    $kind = if ($eq42.Success) { "scene-depth test from $depthSrc" } else { 'all flat' }
    Say "  $hash : split patched ($kind)" 'Green'
}

# ---- 1c. RETFLAT: one scene depth per reticle piece -------------------------------------------------
# JJ 2026-09-28: the reticle "tilts" towards the edges of the view. The fix's depth search runs per
# VERTEX, from each corner's own screen position, so on a slanted surface the four corners land at four
# depths and the icon lies along the surface. Now every corner searches from the piece's own origin
# (the translation of its Scaleform matrix: the matrix rows' .w, valid when the vertex w is 1; otherwise
# the corner's own position, as before), so the whole piece gets one depth and stays flat.
# Only the two shaders with the fix's depth search. Exact-text edits, each must match once.
$flatMarker = '// AKVR RETFLAT'
$flatEdits = @{
    '05154232f7872d0d' = @{
        After  = 'dp4 r3.y, v1.xyzw, cb0[r1.w + 0].xyzw'
        OrigX  = 'cb0[r1.z + 0].w'; OrigY = 'cb0[r1.w + 0].w'; CornerX = 'r3.x'; CornerY = 'r3.y'
        Row    = @('mad r2.z, r3.y, l(-0.500000), l(0.500000)', 'mad r2.z, {Y}, l(-0.500000), l(0.500000)')
        Col    = @('add r5.y, r2.z, r3.x', 'add r5.y, r2.z, {X}')
        Band   = @('lt r1.x, r3.y, l(0.650000)', 'lt r1.x, {Y}, l(0.650000)')
    }
    '9938094af96353c0' = @{
        After  = 'dp4 r1.y, v1.xyzw, cb0[7].xyzw'
        OrigX  = 'cb0[6].w'; OrigY = 'cb0[7].w'; CornerX = 'r1.x'; CornerY = 'r1.y'
        Row    = @('mad r3.x, r1.y, l(-0.500000), l(0.500000)', 'mad r3.x, {Y}, l(-0.500000), l(0.500000)')
        Col    = @('add r4.w, r1.x, r4.x', 'add r4.w, {X}, r4.x')
        Band   = @('lt r0.w, r1.y, l(0.650000)', 'lt r0.w, {Y}, l(0.650000)')
    }
}
function Patch-RetFlat([string]$hash) {
    $e = $flatEdits[$hash]
    if (-not $e) { return }
    $txt = Join-Path $dm "$hash-vs.txt"
    $bin = Join-Path $dm "$hash-vs.bin"
    if (-not (Test-Path $txt)) { Say "  $hash : not in this fix - skipped" 'Yellow'; return }
    $s = [System.IO.File]::ReadAllText($txt)
    if ($s.Contains($flatMarker)) { Say "  $hash : flat reticle already patched"; return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    if (-not $temps.Success) { Say "  $hash : no dcl_temps - flat reticle NOT patched" 'Red'; return }
    $n = [int]$temps.Groups[1].Value
    $t = "r$n"
    $splitLine = '// AKVR HUDSPLIT: remember the fix''s scene-depth decision'
    foreach ($needle in @($e.After, $e.Row[0], $e.Col[0], $e.Band[0], $splitLine)) {
        $c = ([regex]::Matches($s, [regex]::Escape($needle))).Count
        if ($c -ne 1) { Say "  $hash : '$needle' found $c times - flat reticle NOT patched (apply HUDSPLIT first)" 'Red'; return }
    }
    $s = $s.Replace($e.Row[0], $e.Row[1].Replace('{Y}', "$t.z"))
    $s = $s.Replace($e.Col[0], $e.Col[1].Replace('{X}', "$t.y"))
    # JJ 2026-09-28: the compass flickered with the reticle split on. The fix's "not the top strip" test
    # (clip y < 0.65) ran per corner, so a compass piece near that line was cut in two; and AKVR's HUD
    # size / position moves the compass below it. Test the piece's origin instead, and with AKVR's layer
    # (cb13 bound) keep every piece whose origin is above clip y = cb13[0].y flat (AKVR's compass band).
    $s = $s.Replace($e.Band[0], $e.Band[1].Replace('{Y}', "$t.z"))
    $pad = [regex]::Match($s, '(?m)^(?<i>[ \t]*)' + [regex]::Escape($splitLine)).Groups['i'].Value
    $band = @(
        "$flatMarker band: with AKVR's HUD layer (cb13 bound), a piece whose origin is above clip y = cb13[0].y stays flat",
        "eq $t.x, cb13[0].y, l(0.000000)",
        "lt $t.w, $t.z, cb13[0].y",
        "or $t.x, $t.x, $t.w",
        "and r0.z, r0.z, $t.x"
    ) | ForEach-Object { $pad + $_ }
    $at2 = $s.IndexOf($pad + $splitLine)
    $s = $s.Substring(0, $at2) + ($band -join $nl) + $nl + $s.Substring($at2)
    $at = $s.IndexOf($e.After) + $e.After.Length
    $pad = ''
    $block = @(
        "$flatMarker 2026-09-28: search scene depth from the piece's origin (matrix translation) instead of",
        "// each corner, so the whole piece gets one depth. Falls back to the corner when the vertex w is not 1.",
        "eq $t.x, v1.w, l(1.000000)",
        "movc $t.y, $t.x, $($e.OrigX), $($e.CornerX)",
        "movc $t.z, $t.x, $($e.OrigY), $($e.CornerY)"
    )
    $s = $s.Substring(0, $at) + $nl + ($block -join $nl) + $s.Substring($at)
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    $s = $s.Substring(0, $temps.Index) + "dcl_temps $($n + 1)" + $s.Substring($temps.Index + $temps.Length)
    Backup-Once $txt $bakDm
    Backup-Once $bin $bakDm
    [System.IO.File]::WriteAllText($txt, $s, $utf8)
    if (Test-Path $bin) { Remove-Item $bin }
    Say "  $hash : flat reticle patched" 'Green'
}

# ---- 1d. RETSQUASH: undo the flat-sticker squash of scene-depth pieces near the view edges -----------
# JJ 2026-09-28: the reticle squashes towards the edges. A screen-space sprite of fixed size on a flat
# (rectilinear) picture covers a smaller angle off-centre. For a piece at tan-space (u, v) = (x tanH, y tanV)
# it looks narrower by sqrt(1+v^2)/(1+u^2+v^2) and shorter by sqrt(1+u^2)/(1+u^2+v^2). Each vertex's offset
# from the piece origin is scaled by the inverse, only for scene-depth pieces, only when AKVR binds cb13 with
# the game's tan half-angles in .z / .w (unbound or 0 = unchanged).
$squashMarker = '// AKVR RETSQUASH'
$squashPos = @{ '05154232f7872d0d' = @('r3.x', 'r3.y'); '9938094af96353c0' = @('r1.x', 'r1.y') }
function Patch-RetSquash([string]$hash) {
    $pos = $squashPos[$hash]
    if (-not $pos) { return }
    $txt = Join-Path $dm "$hash-vs.txt"
    $bin = Join-Path $dm "$hash-vs.bin"
    if (-not (Test-Path $txt)) { return }
    $s = [System.IO.File]::ReadAllText($txt)
    if ($s.Contains($squashMarker)) { Say "  $hash : squash fix already patched"; return }
    if (-not $s.Contains($flatMarker)) { Say "  $hash : flat reticle (1c) missing - squash fix NOT patched" 'Red'; return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    $o = [regex]::Match($s, '(?m)^movc (?<t>r\d+)\.y, \k<t>\.x, ')      # RETFLAT origin register (.y x, .z y)
    $m = [regex]::Match($s, '(?m)^(?<i>[ \t]*)// AKVR HUDSPLIT: remember[^\r\n]*\r?\n[ \t]*mov r\d+\.x, r0\.z[ \t]*\r?\n')
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    if (-not $o.Success -or -not $m.Success -or -not $temps.Success) { Say "  $hash : anchors not found - squash fix NOT patched" 'Red'; return }
    $n = [int]$temps.Groups[1].Value; $t = "r$n"; $og = $o.Groups['t'].Value; $px = $pos[0]; $py = $pos[1]; $i = $m.Groups['i'].Value
    $block = @(
        "$squashMarker 2026-09-28: scene-depth pieces keep their angular size near the view edges (cb13[0].zw = tan half-angles)",
        "ne $t.w, cb13[0].z, l(0.000000)",
        "and $t.w, $t.w, r0.z",
        "if_nz $t.w",
        "  mul $t.xy, $og.yzyy, cb13[0].zwzz",
        "  mul $t.xy, $t.xyxx, $t.xyxx",
        "  add $t.z, $t.x, $t.y",
        "  add $t.z, $t.z, l(1.000000)",
        "  add $t.xy, $t.yxyy, l(1.000000, 1.000000, 0.000000, 0.000000)",
        "  sqrt $t.xy, $t.xyxx",
        "  div $t.xy, $t.zzzz, $t.xyxx",
        "  add $px, $px, -$og.y",
        "  mad $px, $px, $t.x, $og.y",
        "  add $py, $py, -$og.z",
        "  mad $py, $py, $t.y, $og.z",
        "endif"
    ) | ForEach-Object { $i + $_ }
    $at = $m.Index + $m.Length
    $s = $s.Substring(0, $at) + ($block -join $nl) + $nl + $s.Substring($at)
    $s = $s.Substring(0, $temps.Index) + "dcl_temps $($n + 1)" + $s.Substring($temps.Index + $temps.Length)
    Backup-Once $txt $bakDm
    Backup-Once $bin $bakDm
    [System.IO.File]::WriteAllText($txt, $s, $utf8)
    if (Test-Path $bin) { Remove-Item $bin }
    Say "  $hash : squash fix patched" 'Green'
}

# ---- 1e. DEPTHALL: the other 11 HUD shaders have a scene-depth decision too ------------------------------
# JJ 2026-09-28: the objective marker (with its distance) sat at the HUD distance while the grapple reticle
# followed the object. 1b read the fix's scene-depth decision only in 9938094a / 05154232 and marked every
# piece of the other 11 "flat" (mov rN.x, l(0)). But each of those 11 has its own decision (texture filter
# 2 / 32 / 42 / 22 in gameplay, sometimes a screen region), a top-level if_nz right before its depth search
# (nested if_nz on StereoParams, then a loop). Without AKVR's layer those pieces follow scene depth; with it
# they were pulled into the flat layer. Now the split copies that decision, like 1b does in the two others.
$depthAllMarker = '// AKVR DEPTHALL'
function Patch-DepthAll([string]$hash) {
    $txt = Join-Path $dm "$hash-vs.txt"
    $bin = Join-Path $dm "$hash-vs.bin"
    if (-not (Test-Path $txt)) { return }
    $s = [System.IO.File]::ReadAllText($txt)
    if ($s.Contains($depthAllMarker)) { Say "  $hash : scene-depth split already patched"; return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    # Only the "all flat" split tail (1b without the fix's filter-42 test).
    $flat = [regex]::Matches($s, '(?m)^(?<i>[ \t]*)mov (?<t>r\d+)\.x, l\(0\)[ \t]*\r?\n(?=[ \t]*eq \k<t>\.yz, cb13\[0\]\.xxxx)')
    if ($flat.Count -eq 0) { return }   # 9938094a / 05154232: 1b already reads their decision
    if ($flat.Count -ne 1) { Say "  $hash : split tail found $($flat.Count) times - scene-depth split NOT patched" 'Red'; return }
    $t = $flat[0].Groups['t'].Value
    # The fix's decision: the first top-level if_nz after its first IniParams (t120) load, whose block
    # opens with the StereoParams test and holds the depth-search loop.
    $ld = [regex]::Match($s, '(?m)^ld_indexable\(texture1d\)[^\r\n]*t120\.')
    if (-not $ld.Success) { Say "  $hash : no IniParams load - scene-depth split NOT patched" 'Red'; return }
    $ifnz = [regex]::Match($s.Substring($ld.Index), '(?m)^if_nz (?<c>r\d+\.[xyzw])[ \t]*$')
    if (-not $ifnz.Success) { Say "  $hash : scene-depth test not found - scene-depth split NOT patched" 'Red'; return }
    $at = $ld.Index + $ifnz.Index
    $after = $s.Substring($at, [Math]::Min(1600, $s.Length - $at))
    if ($after -notmatch '(?m)^  ld_indexable\(buffer\)[^\r\n]*t125\.' -or $after -notmatch '(?m)^[ \t]+loop[ \t]*$') {
        Say "  $hash : block after $($ifnz.Groups['c'].Value) is not a depth search - scene-depth split NOT patched" 'Red'; return
    }
    $c = $ifnz.Groups['c'].Value
    if ($c.StartsWith("$t.")) { Say "  $hash : decision register clashes with the split register - NOT patched" 'Red'; return }
    # Edit from the end backwards: drop the "all flat" line, then insert the copy before the if_nz.
    $s = $s.Substring(0, $flat[0].Index) + $s.Substring($flat[0].Index + $flat[0].Length)
    $ins = "${depthAllMarker} 2026-09-28: remember the fix's own scene-depth decision (split: picture vs layer)" + $nl +
           "ine $t.x, $c, l(0)" + $nl
    $s = $s.Substring(0, $at) + $ins + $s.Substring($at)
    Backup-Once $txt $bakDm
    Backup-Once $bin $bakDm
    [System.IO.File]::WriteAllText($txt, $s, $utf8)
    if (Test-Path $bin) { Remove-Item $bin }
    Say "  $hash : scene-depth split patched (decision $c)" 'Green'
}

# ---- 1f. EDGEBAND: RETIRED 2026-09-29 (same night) ------------------------------------------------------
# Top/bottom strips moved pieces to AKVR's room-fixed HUD layer by position. HUD elements straddle the strip
# lines and are built from pieces the fix tags differently, so with the layer hung in the room one element
# came apart into two copies (JJ). Not applied any more; FIX_CHANGES.md 1f has the text and the rollback.

# ---- 1g. PARTTAG: HUD parts AKVR marks "hang in the room" never follow scene depth ---------------------
# JJ 2026-09-30: every HUD element except the ones attached to world objects must hang, whole, on one plane in
# the room. The fix's scene-depth decision is per PIECE (texture tags, shared atlases), so with AKVR's room-fixed
# layer one element came apart (part layer, part picture = doubled). AKVR marks whole Scaleform parts (the panel's
# "hang in the room") by writing a tiny negative blue ADD (-1/512, below one 8-bit step) into the part's colour
# transform; every piece of the part inherits it. Measured (MARKREC, F2 hudmarks.csv): the mark arrives in cb0
# exactly (-0.001953) in the FIRST row of each colour pair - the game uploads add before multiply. Two layouts:
#   A. "mov oA.xyzw, cb0[ADD].xyzw" + "mov oB.xyzw, cb0[MUL].xyzw" at the end (7 shaders): read cb0[ADD] at the
#      decision point (a looked-up row's index register is checked unchanged in between);
#   B. "mad oN.xyzw, vK.xyzw, cb0[MUL], cb0[ADD]" early (4b432a87): read right there into the new temp.
# A marked piece has the fix's decision turned off before it is used: no depth search, flat, and AKVR's split sends
# it only to the layer. No mark = unchanged.
# PARTTAG4 2026-09-30 (JJ: pieces over the compass stayed head-locked): the game combines a child's colour with its
# parent's, so a tinted child scales the parent's add by its own multiply - a green icon (blue x 0) loses a blue-only
# mark. AKVR now marks red, green and blue (-1/512 each) and a piece counts as marked when ANY of add .x/.y/.z lies in
# -0.003..-0.0002 (the mark scaled by a tint down to ~0.1). Two new temps.
# PARTTAG5 2026-09-30 (CBDUMP, JJ's compass pieces shown / hidden): the four stuck pieces were (a) marked TWICE - the
# compass and the child were both ticked, and marks add up (add = -0.003906) - and (b) pieces coloured by their OWN
# add (multiply 0, add ~0.75..0.86), where a small mark disappears inside the colour. The mark now goes into alpha
# too (a child's own alpha add is 0), and any of add .x/.y/.z/.w in -0.02..-0.0002 counts (up to ~10 stacked marks).
$tagMarker = '// AKVR PARTTAG'
function Patch-PartTag([string]$hash) {
    $txt = Join-Path $dm "$hash-vs.txt"
    $bin = Join-Path $dm "$hash-vs.bin"
    if (-not (Test-Path $txt)) { return }
    $s = [System.IO.File]::ReadAllText($txt)
    if ($s.Contains($tagMarker)) { Say "  $hash : part mark already patched"; return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    $code = $s.IndexOf('// HLSL Code')
    if ($code -lt 0) { $code = $s.Length }
    $body = $s.Substring(0, $code)
    $pairA = [regex]::Matches($body, '(?m)^mov o\d+\.xyzw, (?<add>cb0\[[^\]]+\])\.xyzw[ \t]*\r?\nmov o\d+\.xyzw, cb0\[[^\]]+\]\.xyzw[ \t]*$')
    $madB = [regex]::Matches($body, '(?m)^mad o\d+\.xyzw, v\d+\.xyzw, cb0\[[^\]]+\]\.xyzw, (?<add>cb0\[[^\]]+\])\.xyzw[ \t]*$')
    if ($pairA.Count + $madB.Count -eq 0) { Say "  $hash : no colour transform - part mark not needed"; return }
    if ($pairA.Count + $madB.Count -ne 1) { Say "  $hash : colour transform found $($pairA.Count + $madB.Count) times - part mark NOT patched" 'Red'; return }
    $early = $madB.Count -eq 1
    $cx = if ($early) { $madB[0] } else { $pairA[0] }
    $add = $cx.Groups['add'].Value
    # Where the fix's decision is remembered (1b / 1e); the decision register comes from the next line.
    $m = [regex]::Match($s, '(?m)^(?<i>[ \t]*)// AKVR (DEPTHALL[^\r\n]*|HUDSPLIT: remember[^\r\n]*)\r?\n[ \t]*(ine r\d+\.x, (?<d1>r\d+\.[xyzw]), l\(0\)|mov r\d+\.x, (?<d2>r\d+\.[xyzw]))[ \t]*$')
    if (-not $m.Success) { Say "  $hash : decision (1b / 1e) not found - part mark NOT patched" 'Red'; return }
    $dec = if ($m.Groups['d1'].Success) { $m.Groups['d1'].Value } else { $m.Groups['d2'].Value }
    if ($early -and $cx.Index -gt $m.Index) { Say "  $hash : colour read after the decision - part mark NOT patched" 'Red'; return }
    if (-not $early) {
        # A looked-up row (cb0[rK.c + 0]) must hold the same index at the decision point as at the colour lines.
        $ix = [regex]::Match($add, 'cb0\[(?<r>r\d+)\.(?<c>[xyzw]) \+ 0\]')
        if ($ix.Success) {
            $r = $ix.Groups['r'].Value; $c = $ix.Groups['c'].Value
            $mid = $s.Substring($m.Index, $cx.Index - $m.Index)
            $pre = $s.Substring(0, $m.Index)
            $w = '(?m)^[ \t]*[a-z_0-9]+(\([^)]*\))* ' + $r + '\.[xyzw]*' + $c + '[xyzw]*,'
            if ([regex]::IsMatch($mid, $w) -or -not [regex]::IsMatch($pre.Substring([Math]::Max(0, $pre.IndexOf('vs_5_0'))), $w)) {
                Say "  $hash : colour row index $r.$c not stable - part mark NOT patched" 'Red'; return
            }
        }
    }
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    if (-not $temps.Success) { Say "  $hash : no dcl_temps - part mark NOT patched" 'Red'; return }
    $n = [int]$temps.Groups[1].Value; $t = "r$n"; $u = "r$($n + 1)"; $i = $m.Groups['i'].Value
    $test = @(
        "lt $t.xyzw, $add.xyzw, l(-0.000200, -0.000200, -0.000200, -0.000200)",
        "lt $u.xyzw, l(-0.020000, -0.020000, -0.020000, -0.020000), $add.xyzw",
        "and $t.xyzw, $t.xyzw, $u.xyzw",
        "or $t.xy, $t.xyxx, $t.zwzz",
        "or $t.x, $t.y, $t.x",
        "not $t.x, $t.x"
    )
    $apply = @(
        "$tagMarker 2026-09-30: a piece of a HUD part AKVR marked 'hang in the room' (colour add .x/.y/.z/.w in -0.02..-0.0002)",
        "// never follows scene depth: flat, and AKVR's split sends it whole to the layer."
    )
    if ($early) { $apply += @("and $dec, $dec, $t.x") } else { $apply += $test + @("and $dec, $dec, $t.x") }
    # Edit from the end backwards: the decision point first (later in the file for B), then the early read.
    $s = $s.Substring(0, $m.Index) + (($apply | ForEach-Object { $i + $_ }) -join $nl) + $nl + $s.Substring($m.Index)
    if ($early) {
        $at = $cx.Index + $cx.Length
        $s = $s.Substring(0, $at) + $nl + "$tagMarker read: the part mark in this piece's colour add row" + $nl + ($test -join $nl) + $s.Substring($at)
    }
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    $s = $s.Substring(0, $temps.Index) + "dcl_temps $($n + 2)" + $s.Substring($temps.Index + $temps.Length)
    Backup-Once $txt $bakDm
    Backup-Once $bin $bakDm
    [System.IO.File]::WriteAllText($txt, $s, $utf8)
    if (Test-Path $bin) { Remove-Item $bin }
    $kind = if ($early) { 'read early' } else { 'read at the decision' }
    Say "  $hash : part mark patched (colour add $add, $kind, decision $dec)" 'Green'
}

# ---- 1h. NEARRAIN: the rain block the game keeps around the camera hangs in the room ------------------------
# JJ 2026-10-01: one layer of rain "attached to the face", in 3D, moving with the head even in the pause. AKVR's draw
# probe (DRAWPROBE4 / RAINPARTS2) found it inside the world rain draw itself: pixel shader 5d787946eda54077 with the
# fix's vertex shader f50d1365e929b3a0, 6 vertices x 20480 instances, each streak read from a structured buffer by
# SV_InstanceID. Cutting the instance count: the stuck streaks are the FIRST 2048 (with a few world drops among them;
# everything past 2048 is world-fixed rain). The game places that block around its camera - in VR the head-turned one.
# JJ: "Can't we hang them in space like the rest of them?" Edit, driven by AKVR through cb12 (bound only around the
# rain draw; unbound it reads 0 and the shader is exactly the fix's):
#   cb12[0] = the game camera's own forward + mode (w: 0 unchanged, 1 hang in the room, 2 hidden)
#   cb12[1] = its right + how many streaks from the start (w), cb12[2] = its up.
# Mode 1, right after a streak's position is read: the head-turned camera's position and axes come from this draw's
# own view-projection (cb0[6..9]: clip.w row = forward; x / y rows minus their forward part = right / up; the camera
# is where clip x, y and w are all 0), the streak is expressed in that camera's axes and put back with the game
# camera's axes, so turning the head no longer turns the block. Mode 2: the final position write sends it off screen.
$rainVs = 'f50d1365e929b3a0'
$rainMarker = '// AKVR NEARRAIN'
function Patch-NearRain {
    $txt = Join-Path $dm "$rainVs-vs.txt"
    $bin = Join-Path $dm "$rainVs-vs.bin"
    if (-not (Test-Path $txt)) { Say "  $rainVs : rain shader not in the fix - near rain NOT patched" 'Red'; return }
    $s = [System.IO.File]::ReadAllText($txt)
    if ($s.Contains($rainMarker)) { Say "  $rainVs : near rain already patched"; return }
    if ($s -match '(?i)\bcb12\b') { Say "  $rainVs : already uses cb12 - near rain NOT patched" 'Red'; return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    $iidM = [regex]::Match($s, '(?m)^dcl_input_sgv (?<v>v\d+)\.x, instance_id\s*$')
    if (-not $iidM.Success) { Say "  $rainVs : no instance id input - near rain NOT patched" 'Red'; return }
    $iid = $iidM.Groups['v'].Value
    $ldM = [regex]::Matches($s, '(?m)^ld_structured_indexable\(structured_buffer, stride=80\)\(mixed,mixed,mixed,mixed\) (?<p>r\d+)\.xyzw, ' + $iid + '\.x, l\(48\), t0\.xyzw[ \t]*$')
    if ($ldM.Count -ne 1) { Say "  $rainVs : streak position read found $($ldM.Count) times - near rain NOT patched" 'Red'; return }
    $P = $ldM[0].Groups['p'].Value
    $posDcl = [regex]::Match($s, '(?m)^dcl_output_siv (?<o>o\d+)\.xyzw, position\s*$')
    if (-not $posDcl.Success) { Say "  $rainVs : no position output - near rain NOT patched" 'Red'; return }
    $hits = @([regex]::Matches($s, '(?m)^mov (?<o>o\d+)\.xyzw, (?<r>r\d+)\.xyzw[ \t]*$') | Where-Object { $_.Groups['o'].Value -eq $posDcl.Groups['o'].Value })
    if ($hits.Count -ne 1) { Say "  $rainVs : position write found $($hits.Count) times - near rain NOT patched" 'Red'; return }
    if ($hits[0].Index -lt $ldM[0].Index) { Say "  $rainVs : position written before the streak is read - near rain NOT patched" 'Red'; return }
    $cbDecl = [regex]::Match($s, '(?m)^dcl_constantbuffer [^\r\n]*$')
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    if (-not $cbDecl.Success -or -not $temps.Success) { Say "  $rainVs : declarations not found - near rain NOT patched" 'Red'; return }
    $n = [int]$temps.Groups[1].Value
    $T = 0..6 | ForEach-Object { "r$($n + $_)" }
    $t0, $t1, $t2, $t3, $t4, $t5, $t6 = $T
    $o = $hits[0].Groups['o'].Value; $r = $hits[0].Groups['r'].Value
    $place = @(
        "$rainMarker 2026-10-01: AKVR (cb12) re-places the first cb12[1].w streaks from the head-turned camera to the",
        "// game's own camera (mode 1), or hides them (mode 2). cb12 unbound = 0 = the fix's shader unchanged.",
        "utof $t0.y, $iid.x",
        "lt $t0.y, $t0.y, cb12[1].w",
        "eq $t0.w, cb12[0].w, l(1.000000)",
        "and $t0.w, $t0.w, $t0.y",
        "eq $t0.z, cb12[0].w, l(2.000000)",
        "and $t0.z, $t0.z, $t0.y",
        "mov $t1.x, cb0[6].x",
        "mov $t1.y, cb0[7].x",
        "mov $t1.z, cb0[8].x",
        "mov $t2.x, cb0[6].y",
        "mov $t2.y, cb0[7].y",
        "mov $t2.z, cb0[8].y",
        "mov $t3.x, cb0[6].w",
        "mov $t3.y, cb0[7].w",
        "mov $t3.z, cb0[8].w",
        "mul $t4.xyz, $t2.yzxy, $t3.zxyz",
        "mad $t4.xyz, $t2.zxyz, -$t3.yzxy, $t4.xyzx",
        "dp3 $t0.x, $t1.xyzx, $t4.xyzx",
        "mul $t4.xyz, $t4.xyzx, cb0[9].xxxx",
        "mul $t5.xyz, $t3.yzxy, $t1.zxyz",
        "mad $t5.xyz, $t3.zxyz, -$t1.yzxy, $t5.xyzx",
        "mad $t4.xyz, $t5.xyzx, cb0[9].yyyy, $t4.xyzx",
        "mul $t5.xyz, $t1.yzxy, $t2.zxyz",
        "mad $t5.xyz, $t1.zxyz, -$t2.yzxy, $t5.xyzx",
        "mad $t4.xyz, $t5.xyzx, cb0[9].wwww, $t4.xyzx",
        "div $t4.xyz, -$t4.xyzx, $t0.xxxx",
        "dp3 $t0.y, $t3.xyzx, $t3.xyzx",
        "rsq $t0.y, $t0.y",
        "mul $t3.xyz, $t3.xyzx, $t0.yyyy",
        "dp3 $t0.y, $t1.xyzx, $t3.xyzx",
        "mad $t1.xyz, -$t3.xyzx, $t0.yyyy, $t1.xyzx",
        "dp3 $t0.y, $t1.xyzx, $t1.xyzx",
        "rsq $t0.y, $t0.y",
        "mul $t1.xyz, $t1.xyzx, $t0.yyyy",
        "dp3 $t0.y, $t2.xyzx, $t3.xyzx",
        "mad $t2.xyz, -$t3.xyzx, $t0.yyyy, $t2.xyzx",
        "dp3 $t0.y, $t2.xyzx, $t2.xyzx",
        "rsq $t0.y, $t0.y",
        "mul $t2.xyz, $t2.xyzx, $t0.yyyy",
        "add $t5.xyz, $P.xyzx, -$t4.xyzx",
        "dp3 $t6.x, $t1.xyzx, $t5.xyzx",
        "dp3 $t6.y, $t2.xyzx, $t5.xyzx",
        "dp3 $t6.z, $t3.xyzx, $t5.xyzx",
        "mad $t5.xyz, cb12[0].xyzx, $t6.zzzz, $t4.xyzx",
        "mad $t5.xyz, cb12[1].xyzx, $t6.xxxx, $t5.xyzx",
        "mad $t5.xyz, cb12[2].xyzx, $t6.yyyy, $t5.xyzx",
        "movc $P.xyz, $t0.wwww, $t5.xyzx, $P.xyzx"
    ) -join $nl
    $hide = @(
        "$rainMarker hide (mode 2): off screen. Original line: mov $o.xyzw, $r.xyzw",
        "movc $o.xyzw, $t0.zzzz, l(-10.000000, -10.000000, 0.000000, 1.000000), $r.xyzw"
    ) -join $nl
    # From the end backwards: the position write, the streak read, dcl_temps, the constant buffer declaration.
    $h = $hits[0]
    $s = $s.Substring(0, $h.Index) + $hide + $s.Substring($h.Index + $h.Length)
    $ld = $ldM[0]
    $s = $s.Substring(0, $ld.Index + $ld.Length) + $nl + $place + $s.Substring($ld.Index + $ld.Length)
    $temps = [regex]::Match($s, '(?m)^dcl_temps (\d+)\s*$')
    $s = $s.Substring(0, $temps.Index) + "dcl_temps $($n + 7)" + $s.Substring($temps.Index + $temps.Length)
    $cbDecl = [regex]::Match($s, '(?m)^dcl_constantbuffer [^\r\n]*$')
    $s = $s.Substring(0, $cbDecl.Index + $cbDecl.Length) + $nl + 'dcl_constantbuffer CB12[3], immediateIndexed' + $s.Substring($cbDecl.Index + $cbDecl.Length)
    Backup-Once $txt $bakDm
    Backup-Once $bin $bakDm
    [System.IO.File]::WriteAllText($txt, $s, $utf8)
    if (Test-Path $bin) { Remove-Item $bin }
    Say "  $rainVs : near rain patched (first streaks re-placed / hidden by AKVR, position $P, output $o)" 'Green'
}

# ---- 2. d3dxdm.ini --------------------------------------------------------------------------------
$stereoKeys = [ordered]@{
    'dm_hud_detection'       = '1'
    'dm_static_hud_depth'    = '1.0'
    'dm_auto_hud_depth'      = '0'
    'dm_auto_hud_offset_min' = '0.0'
    'dm_auto_hud_offset_max' = '1.0'
}
$exposure = @(
    '; AKVR EXPOSURE 2026-09-27: auto-exposure ran per eye (one eye darker in the each-eye view). Tiny targets',
    '; (<= 4x4, the luminance chain) are drawn once and shared by both eyes (playbook STR-009).',
    '[TextureOverrideAKVRTinyRT]',
    'match_type = Texture2D',
    'match_bind_flags = +render_target',
    'match_width = <5',
    'match_height = <5',
    'StereoMode = 2',
    '',
    '[TextureOverrideAKVRTinyUAV]',
    'match_type = Texture2D',
    'match_bind_flags = +unordered_access',
    'match_width = <5',
    'match_height = <5',
    'StereoMode = 2',
    ''
)

function Patch-DmIni {
    if (-not (Test-Path $dmIni)) { Say '  d3dxdm.ini not found - skipped' 'Yellow'; return }
    Backup-Once $dmIni $bak
    $lines = New-Object System.Collections.Generic.List[string]
    [System.IO.File]::ReadAllLines($dmIni) | ForEach-Object { $lines.Add($_) }
    $changed = $false

    $stereo = -1
    for ($k = 0; $k -lt $lines.Count; $k++) { if ($lines[$k] -match '^\s*\[Stereo\]\s*$') { $stereo = $k; break } }
    if ($stereo -lt 0) { Say '  d3dxdm.ini has no [Stereo] section - keys skipped' 'Red' }
    else {
        $end = $lines.Count
        for ($k = $stereo + 1; $k -lt $lines.Count; $k++) { if ($lines[$k] -match '^\s*\[') { $end = $k; break } }
        foreach ($key in $stereoKeys.Keys) {
            $want = "$key = $($stereoKeys[$key])"
            $hit = -1
            for ($k = $stereo + 1; $k -lt $end; $k++) { if ($lines[$k] -match "^\s*$key\s*=") { $hit = $k; break } }
            if ($hit -ge 0) {
                if (($lines[$hit] -replace '\s', '') -ne ($want -replace '\s', '')) { $lines[$hit] = $want; $changed = $true }
            } else {
                $lines.Insert($stereo + 1, $want); $end++; $changed = $true
            }
        }
    }

    if (-not ($lines -match '^\s*\[TextureOverrideAKVRTinyRT\]')) {
        $at = $lines.Count
        for ($k = 0; $k -lt $lines.Count; $k++) { if ($lines[$k] -match '^\s*\[Stereo\]\s*$') { $at = $k; break } }
        $lines.InsertRange($at, [string[]]$exposure)
        $changed = $true
    }

    if ($changed) {
        $nl = if ([System.IO.File]::ReadAllText($dmIni).Contains("`r`n")) { "`r`n" } else { "`n" }
        [System.IO.File]::WriteAllText($dmIni, (($lines -join $nl) + $nl), $utf8)
        Say '  d3dxdm.ini : updated' 'Green'
    } else { Say '  d3dxdm.ini : already up to date' }
}

# ---- run ----------------------------------------------------------------------------------------------
if (Get-Process -Name 'BatmanAK' -ErrorAction SilentlyContinue) {
    Say 'Close Batman: Arkham Knight first.' 'Red'; exit 1
}
if (-not (Test-Path $dm)) { Say 'ShaderFixesDM not found: run this from Binaries\Win64 with the geo-11 fix installed.' 'Red'; exit 1 }

# geo-11 is also the user's own download. AKVR's live geo-11 links (world scale, HUD distance) were
# built against geo-11 0.7.11; on another build they switch themselves off, so say so here.
$geoKnown = '600EA47F1D75805AED9402E90C0969182CA629B0'
$geoDll = @('geo11.dll', 'd3d11.dll') | ForEach-Object { Join-Path $dir $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($geoDll) {
    $sha = (Get-FileHash $geoDll -Algorithm SHA1).Hash
    if ($sha -eq $geoKnown) { Say "geo-11: $(Split-Path -Leaf $geoDll) is the version AKVR was built against (0.7.11)." }
    else { Say "geo-11: $(Split-Path -Leaf $geoDll) is a DIFFERENT build ($sha). AKVR's live world scale / HUD distance may stay off." 'Yellow' }
} else { Say 'geo-11: no geo11.dll or d3d11.dll here.' 'Yellow' }

switch ($Mode) {
    'status' {
        foreach ($h in $hudVs) {
            $p = Join-Path $dm "$h-vs.txt"
            $st = if (-not (Test-Path $p)) { 'missing' } elseif ((Get-Content $p -Raw).Contains($marker)) { 'patched' } else { 'original' }
            $sp = if ((Test-Path $p) -and (Get-Content $p -Raw).Contains($splitMarker)) { ', split patched' } else { '' }
            Say ("  {0} : {1}{2}" -f $h, $st, $sp)
        }
        $rp = Join-Path $dm "$rainVs-vs.txt"
        $rs = if (-not (Test-Path $rp)) { 'missing' } elseif ((Get-Content $rp -Raw).Contains($rainMarker)) { 'near rain patched' } else { 'original' }
        Say ("  {0} : {1}" -f $rainVs, $rs)
    }
    'apply' {
        Say 'AKVR fix patches' 'Cyan'
        foreach ($h in $hudVs) { Patch-HudShader $h }
        foreach ($h in $hudVs) { Patch-HudSplit $h }
        foreach ($h in $hudVs) { Patch-RetFlat $h }
        foreach ($h in $hudVs) { Patch-RetSquash $h }
        foreach ($h in $hudVs) { Patch-DepthAll $h }
        foreach ($h in $hudVs) { Patch-PartTag $h }
        Patch-NearRain
        Patch-DmIni
        if ($script:failed -gt 0) { Say "$($script:failed) edit(s) could not be applied - see the red lines above." 'Yellow'; exit 2 }
        Say 'Done. Originals are in akvr_fix_backup\.' 'Cyan'
    }
    'undo' {
        if (-not (Test-Path $bak)) { Say 'Nothing to undo (no akvr_fix_backup folder).' 'Yellow'; exit 0 }
        if (Test-Path $bakDm) { Get-ChildItem $bakDm | ForEach-Object { Copy-Item $_.FullName (Join-Path $dm $_.Name) -Force } }
        $iniBak = Join-Path $bak 'd3dxdm.ini'
        if (Test-Path $iniBak) { Copy-Item $iniBak $dmIni -Force }
        Say 'Restored the original fix files from akvr_fix_backup\.' 'Green'
    }
}
