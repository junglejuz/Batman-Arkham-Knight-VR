# AKVR changes to third-party files (geo-11 and the Arkham Knight fix)

This is the record the install scripts are built from. Users download geo-11 and the Arkham Knight
geo-11 fix themselves, so every difference AKVR needs must be recreated by a script on their copy.
**Every AKVR change to a file we did not write is listed here, with the exact edit.** Update this file
in the same step as any new change (rule from JJ, 2026-09-28).

- **Baseline (the fix as shipped) - CURRENT: release of 2026-09-25** ("fixed ambient occlusion. Fixed cubemaps
  inside building windows. Fixed non 16:9 resolutions", geo-11 build v0.7.11), same file name
  `Batman_Arkham_Knight_geo11_fix.7z`, from https://masterotaku.s3.amazonaws.com/Batman+Arkham+Knight/Batman_Arkham_Knight_geo11_fix.7z
  (linked from the HelixMod page), SHA1 `5ff54551890a29f8e8017ffb4686e1817805aa15`, 4.6 MB. It already contains the
  `[TextureOverrideAllNonSquareRT]` block (section 3) and the new ambient occlusion / UE3_BatmanAK.ini files (section 5).
- Older baseline: `E:\Games\# MODS\Geo-11 Fixes\Batman_Arkham_Knight_geo11_fix.7z` (SHA1
  `3ba80a45dfa890022d12ef79fe8e7cd3bac09ee7`, 2025-02-16, geo-11 0.6.182). It contains `FixFiles.7z`, which
  unpacks to `FixFiles\` (the contents of `Binaries\Win64`). The patch script gives identical results on both.
- **Target folder:** `<game>\Binaries\Win64`.
- **Last full comparison of the installed game against the baseline:** 2026-09-28. Shader caches
  (`ShaderCache`, `ShaderCacheDM`) are skipped because geo-11 regenerates them itself.
- **Reference script:** `akvr/tools/AKVR-fix-patches.ps1` performs sections 1-1e and 2 on an installed
  fix (idempotent, backups in `akvr_fix_backup\`, `-Mode undo`, `-GameDir <Binaries\Win64>`; exit code 2 if any
  edit could not be applied).
- **Installer (2026-09-29):** `akvr/install/Install-AKVR.ps1` does sections 3 (load_library_redirect,
  NonSquareRT block) and 4 (geo-11 0.7.11 as geo11.dll, dxgi.dll parked, bundled d3dxdm.ini = section 2), then
  runs the patch script (step 6b) and stops if it reports a failure. Package: `akvr/dist/AKVR-ArkhamKnight/`
  (+ zip), not in git. **Verified 2026-09-29** on practice folders from the baseline archive: all 13 HUD shader
  texts byte-identical to the live game; d3dx.ini differs only by geo-11's help include and the NonSquareRT
  block's position; d3dxdm.ini same settings (comments/order); a second run changes nothing. Not reproduced:
  section 5 files.

Status key: **AKVR** = required by the mod, origin documented. **UNKNOWN** = differs from the baseline, but
no AKVR note records who changed it. Ask JJ before scripting these.

---

## 1. ShaderFixesDM: 13 HUD vertex shaders (AKVR, build HUDDEPTH, 2026-09-28)

**Files** (each `ShaderFixesDM\<hash>-vs.txt`, and the matching `-vs.bin` is deleted):

```
9938094af96353c0  3b819a7e86e9d631  05154232f7872d0d  599bd060016570bb  fd60f2d764b91756
c9b47e60935f3f03  4b432a87d072f988  54cd897e5ce3b8b3  e12863b9484b4660  7d8fcdc225ce3fc7
ef1c1604346331e2  9689d605c06946bf  91e2b2225bf3ea04
```

These are the shaders the fix's `d3dx.ini` names in `[ShaderOverride_HUD5]`, `[ShaderOverride_HUD_Icons1..11]`
and `[ShaderOverride_HUD_letters1]`.

**Why:** flat HUD pieces (clip `w == 1`) got no left/right shift in these shaders, so they always sat at
infinity. The per-eye HUD value geo-11 uploads (StereoParams, `t125` element 1, `.y`), which AKVR's
"HUD distance" slider sets live, was never read. See `PLAYBOOK_REVIEW.md` "HUDDEPTH".

**Edit (identical rule in all 13):**

1. `dcl_temps N` becomes `dcl_temps N+1`. The new register `rN` is used below.
2. Each file has exactly one stereo tail of this shape (register names differ per file):
   ```
   ne A.y, l(1.000000), B.w
   movc B.x, A.y, A.x, B.x
   ```
   It is replaced by:
   ```
   // AKVR HUDDEPTH 2026-09-28: flat HUD pieces (w == 1) take geo-11's live HUD shift, StereoParams[1].y
   // (per eye; AKVR's HUD distance slider). Not in menus (x11), not the icons that follow scene depth (filter 2 / 42).
   ld_indexable(buffer)(float,float,float,float) rN.xyzw, l(1, 0, 0, 0), t125.xyzw
   ld_indexable(texture1d)(float,float,float,float) rN.x, l(11, 0, 0, 0), t120.xyzw
   ld_indexable(texture1d)(float,float,float,float) rN.z, l(2, 0, 0, 0), t120.xyxw
   eq rN.x, rN.x, l(0.000000)
   eq rN.zw, rN.zzzz, l(0.000000, 0.000000, 2.000000, 42.000000)
   or rN.z, rN.z, rN.w
   not rN.z, rN.z
   and rN.x, rN.x, rN.z
   and rN.y, rN.y, rN.x
   add rN.y, rN.y, B.x
   ne A.y, l(1.000000), B.w
   movc B.x, A.y, A.x, rN.y
   ```
   Keep the file's line endings (the originals are LF) and write **no BOM**.
3. Delete `<hash>-vs.bin` so geo-11 rebuilds it from the text on the next launch. (Confirmed: geo-11
   rebuilt all 13 on the 2026-09-28 00:48 launch.)

**Detect already applied:** the text contains `// AKVR HUDDEPTH`.
**Safety:** if the tail pattern is not found exactly once, leave that file untouched and report it.

---

## 1b. ShaderFixesDM: the same 13 HUD vertex shaders, HUDSPLIT switch (AKVR, build HUDLAYER6, 2026-09-28)

**Files:** the same 13 `ShaderFixesDM\<hash>-vs.txt` as section 1 (apply section 1 first); the matching
`-vs.bin` is deleted again.

**Why:** AKVR can draw the HUD into its own headset layer. Pieces the fix places at scene depth (the grapple
reticle and other icons tagged `filter_index = 2 / 42` in `d3dx.ini`) must stay in the 3D picture, so AKVR
draws each HUD piece twice with a switch in constant buffer slot 13: `1` keeps flat pieces only (the layer),
`2` keeps scene-depth pieces only (the picture). With nothing bound in slot 13 (the value reads 0, which is the
case without AKVR's layer) every piece is drawn exactly as before. See `PLAYBOOK_REVIEW.md` "HUDSPLIT".

**Edit (per file; `rN` = the NEW temp register after section 1, i.e. `dcl_temps` goes up by one more):**

1. `dcl_temps N` becomes `dcl_temps N+1`.
2. After the first `dcl_constantbuffer ...` line, add `dcl_constantbuffer CB13[1], immediateIndexed`.
3. Only in the two files that contain the fix's scene-depth test (`9938094af96353c0`, `05154232f7872d0d`):
   find the line `eq rA.xyz, rA.xyxx, l(42.000000, 42.000000, 2.000000, 0.000000)`; the first `if_nz rB.c`
   after it holds the decision (`r0.z` in both). Directly before that `if_nz`, add:
   ```
   // AKVR HUDSPLIT: remember the fix's scene-depth decision
   mov rN.x, r0.z
   ```
4. Directly before the file's single `ret` (code section, before `// HLSL Code`), add (the `mov rN.x, l(0)`
   line only in the 11 files WITHOUT the scene-depth test):
   ```
   // AKVR HUDSPLIT 2026-09-28: cb13[0].x = 1 keeps flat pieces only (AKVR's HUD layer), 2 keeps the
   // scene-depth pieces only (filter 2 / 42, the 3D picture), 0 / unbound keeps everything. Dropped = off screen.
   mov rN.x, l(0)
   eq rN.yz, cb13[0].xxxx, l(0.000000, 1.000000, 2.000000, 0.000000)
   and rN.y, rN.y, rN.x
   not rN.w, rN.x
   and rN.z, rN.z, rN.w
   or rN.y, rN.y, rN.z
   if_nz rN.y
     mov oP.xyzw, l(-10.000000, -10.000000, 0.000000, 1.000000)
   endif
   ```
   `oP` = the shader's OWN position output, from its `dcl_output_siv oP.xyzw, position` line (o2 in
   3b819a7e, 599bd060, 4b432a87, e12863b9, 7d8fcdc2, 91e2b222; o3 in fd60f2d7, c9b47e60, ef1c1604; o4 in the
   rest). **Writing o4 in all 13 crashed the NVIDIA driver at game start (2026-09-28 02:41) - an undeclared
   output is invalid bytecode.**
5. Delete `<hash>-vs.bin`. LF line endings, no BOM.

**Detect already applied:** the text contains `// AKVR HUDSPLIT`. AKVR itself reads that marker in
`05154232f7872d0d` and `9938094af96353c0` and only draws twice when both have it.
**Safety:** skip (and report) a file with no `dcl_temps`, no `dcl_constantbuffer`, more than one `ret`, an
existing `cb13`, or an `eq ... l(42...` line without a following `if_nz`.
**Reference implementation:** `Patch-HudSplit` in `akvr/tools/AKVR-fix-patches.ps1` (tested on a copy: 13
patched, second run no-op; applied to the installed fix 2026-09-28). Every edited shader was assembled with geo-11's
`cmd_Decompiler -a` and loaded into the NVIDIA driver by `akvr/tools/shader_driver_test` (all 13 OK; the
crashing first version fails there). Check that the 13 `.bin` files reappear after the next launch.

---

## 1c. ShaderFixesDM: flat reticle, one scene depth per piece (AKVR, RETFLAT, 2026-09-28)

**Files:** `05154232f7872d0d-vs.txt`, `9938094af96353c0-vs.txt` (the two with the fix's scene-depth search;
apply 1 and 1b first); their `-vs.bin` deleted.

**Why:** JJ: the reticle "tilts" (rotates in depth or squishes) towards the edges of the view. The fix's
depth search starts from each VERTEX's screen position, so on a slanted surface the corners land at
different depths. Now every corner searches from the piece's own origin (its Scaleform matrix
translation), so the piece gets one depth. If only the squish remains, that is the flat-sticker
foreshortening of a wide view, a different change.

**Edit** (`rT` = new temp, `dcl_temps` +1; exact text, each needle must occur exactly once, else skip):

| file | insert after | origin x / y | row line becomes | column line becomes |
|---|---|---|---|---|
| 05154232f7872d0d | `dp4 r3.y, v1.xyzw, cb0[r1.w + 0].xyzw` | `cb0[r1.z + 0].w` / `cb0[r1.w + 0].w` (corner `r3.x` / `r3.y`) | `mad r2.z, rT.z, l(-0.500000), l(0.500000)` | `add r5.y, r2.z, rT.y` |
| 9938094af96353c0 | `dp4 r1.y, v1.xyzw, cb0[7].xyzw` | `cb0[6].w` / `cb0[7].w` (corner `r1.x` / `r1.y`) | `mad r3.x, rT.z, l(-0.500000), l(0.500000)` | `add r4.w, rT.y, r4.x` |

Inserted block:
```
// AKVR RETFLAT 2026-09-28: search scene depth from the piece's origin (matrix translation) instead of
// each corner, so the whole piece gets one depth. Falls back to the corner when the vertex w is not 1.
eq rT.x, v1.w, l(1.000000)
movc rT.y, rT.x, <origin x>, <corner x>
movc rT.z, rT.x, <origin y>, <corner y>
```
**Compass band (same step; JJ: the compass flickered with the reticle split on).** Also in both files:
- the fix's "not the top strip" test uses the piece origin: `lt r1.x, r3.y, l(0.650000)` -> `lt r1.x, rT.z, l(0.650000)`
  (05154232f7872d0d) and `lt r0.w, r1.y, l(0.650000)` -> `lt r0.w, rT.z, l(0.650000)` (9938094af96353c0);
- directly before the HUDSPLIT line `// AKVR HUDSPLIT: remember the fix's scene-depth decision` (so section 1b first):
  ```
  // AKVR RETFLAT band: with AKVR's HUD layer (cb13 bound), a piece whose origin is above clip y = cb13[0].y stays flat
  eq rT.x, cb13[0].y, l(0.000000)
  lt rT.w, rT.z, cb13[0].y
  or rT.x, rT.x, rT.w
  and r0.z, r0.z, rT.x
  ```
  (cb13 unbound -> .y = 0 -> no change.)

**Detect:** `// AKVR RETFLAT`. **Reference:** `Patch-RetFlat` in `AKVR-fix-patches.ps1`. **Driver test:** 13/13 OK
(assembled with cmd_Decompiler, loaded by `tools/shader_driver_test`). **Applied** 2026-09-28 (installed texts
byte-identical to the tested ones).

---

## 1d. ShaderFixesDM: reticle keeps its size near the view edges (AKVR, RETSQUASH, 2026-09-28)

**Files:** `05154232f7872d0d-vs.txt`, `9938094af96353c0-vs.txt` (after 1, 1b, 1c); `-vs.bin` deleted.
**Why:** JJ: the reticle squashes near the window edge. A fixed-size screen sprite on a flat (rectilinear) picture
covers a smaller angle off-centre: narrower by sqrt(1+v^2)/(1+u^2+v^2), shorter by sqrt(1+u^2)/(1+u^2+v^2), with
(u, v) = (clip x * tanH, clip y * tanV) at the piece origin. The inverse is applied to each vertex's offset from the
piece origin, for scene-depth pieces only, only when AKVR binds cb13 with tanH / tanV in `.z / .w`.
**Edit:** `dcl_temps` +1 (`rS`); directly after the HUDSPLIT lines (`// AKVR HUDSPLIT: remember ...` + `mov rN.x, r0.z`)
insert, with `rT` = the RETFLAT origin register (`rT.y` x, `rT.z` y) and position registers `r3.x/r3.y`
(05154232f7872d0d) or `r1.x/r1.y` (9938094af96353c0):
```
// AKVR RETSQUASH 2026-09-28: scene-depth pieces keep their angular size near the view edges (cb13[0].zw = tan half-angles)
ne rS.w, cb13[0].z, l(0.000000)
and rS.w, rS.w, r0.z
if_nz rS.w
  mul rS.xy, rT.yzyy, cb13[0].zwzz
  mul rS.xy, rS.xyxx, rS.xyxx
  add rS.z, rS.x, rS.y
  add rS.z, rS.z, l(1.000000)
  add rS.xy, rS.yxyy, l(1.000000, 1.000000, 0.000000, 0.000000)
  sqrt rS.xy, rS.xyxx
  div rS.xy, rS.zzzz, rS.xyxx
  add PX, PX, -rT.y
  mad PX, PX, rS.x, rT.y
  add PY, PY, -rT.z
  mad PY, PY, rS.y, rT.z
endif
```
**Detect:** `// AKVR RETSQUASH`. **Reference:** `Patch-RetSquash`. **Driver test:** 13/13 OK. **Applied** 2026-09-28
(installed texts identical to the tested ones).
**Status (PANELTIDY, 2026-09-28):** JJ: never worked; AKVR no longer sends the tan half-angles (cb13[0].zw = 0), so
this edit is inert. It can stay in or be dropped by an installer; it changes nothing either way.

---

## 1e. ShaderFixesDM: the other 11 HUD shaders keep their scene-depth pieces in the picture (AKVR, DEPTHALL, 2026-09-28)

**Files:** the 11 HUD `-vs.txt` WITHOUT the filter-42 test: `3b819a7e86e9d631`, `599bd060016570bb`,
`fd60f2d764b91756`, `c9b47e60935f3f03`, `4b432a87d072f988`, `54cd897e5ce3b8b3`, `e12863b9484b4660`,
`7d8fcdc225ce3fc7`, `ef1c1604346331e2`, `9689d605c06946bf`, `91e2b2225bf3ea04` (after 1 and 1b); `-vs.bin` deleted.

**Why:** JJ: the objective marker (its distance readout) sat at the HUD distance while the grapple reticle followed
the object. 1b copied the fix's scene-depth decision only in the two filter-42 shaders and wrote `mov rN.x, l(0)`
("every piece flat") in the other 11. But each of those 11 has its own decision too (texture filter 2 / 32 / 42 / 22
in gameplay, sometimes a screen region; e.g. the letters shader puts filter-22 text between clip y -0.5 and 0.75 at
scene depth): the first top-level `if_nz rX.c` after the first IniParams (`t120`) load, whose block starts with the
StereoParams test and holds the depth-search `loop`. Without AKVR's layer those pieces follow scene depth; with the
layer they were drawn only into the flat layer.

**Edit (per file; `rN` = the HUDSPLIT register from 1b):**
1. Delete the 1b line `mov rN.x, l(0)` (directly before `eq rN.yz, cb13[0].xxxx, ...`).
2. Directly before that first top-level `if_nz rX.c`, insert:
   ```
   // AKVR DEPTHALL 2026-09-28: remember the fix's own scene-depth decision (split: picture vs layer)
   ine rN.x, rX.c, l(0)
   ```
   Decision registers found: r1.x (3b819a7e, fd60f2d7, c9b47e60, e12863b9, 9689d605, 91e2b222), r0.x (599bd060,
   4b432a87, 7d8fcdc2), r0.z (54cd897e, ef1c1604).
No `dcl_temps` change (rN already exists). Without cb13 bound nothing changes (the tail only acts on 1 or 2).

**Detect:** `// AKVR DEPTHALL`. **Safety:** skip (and report) a file whose block after the `if_nz` has no t125 load and
`loop`, or whose decision register is `rN`. **Reference:** `Patch-DepthAll` in `akvr/tools/AKVR-fix-patches.ps1`
(run on a copy: 11 patched, the two filter-42 shaders left alone). **Driver test:** 13/13 assembled
(cmd_Decompiler 0.6.90) and loaded OK by `tools/shader_driver_test`. **Applied** 2026-09-28 (installed texts
byte-identical to the tested ones). Rollback: `akvr/diagnostics/before-DEPTHALL-20260928/` (11 texts + bins).
Note: AKVR's compass band (1c) exists only in the two filter-42 shaders; if compass pieces from these 11 flicker
with the split on, the band needs adding here too.

---

## 1f. ShaderFixesDM: top and bottom strips stay on the HUD layer, all 13 shaders (AKVR, EDGEBAND, 2026-09-29)

**RETIRED the same night, UNDONE on JJ's game** (the 13 texts + bins restored from
`akvr/diagnostics/before-EDGEBAND-20260929/`, byte-identical; `Patch-EdgeBand` removed from the script). JJ: almost every
HUD element showed twice in the headset (the desktop, geo-11's picture, once). One element is built from pieces the fix
tags differently, and many straddled the strip lines, so part went to the room-fixed layer and part stayed in the
picture; with the layer hung in the room the parts drift apart. Kept below for the record.

**Files:** all 13 HUD `-vs.txt` (after 1, 1b, 1c, 1d, 1e); `-vs.bin` deleted.

**Why:** JJ, build HUDWORLD (the HUD layer hangs in the room): the compass (top) and the gameplay tips (bottom) still
moved with the head. The fix's scene-depth decision (1b / 1e) keeps them in the 3D picture, and the RETFLAT compass band
(1c) exists only in the two filter-42 shaders. The compass, the tips and the target-distance icon are one HUD container
(JJ hid "C1.0" and all three went).

**Edit (per file; `rM` = new temp, `dcl_temps` +1; `rD` = the split register holding the decision):**
1. `dcl_constantbuffer CB13[1], immediateIndexed` becomes `CB13[2]`.
2. After the LAST `dp4 rA.y, vK.xyzw, cb0[ROW].xyzw` into a temp register before the first IniParams (`t120`) load
   (the position transform), insert:
   ```
   // AKVR EDGEBAND: the piece origin's y (matrix translation; the vertex when its w is not 1)
   eq rM.x, vK.w, l(1.000000)
   movc rM.y, rM.x, cb0[ROW].w, rA.y
   ```
3. Directly before the split tail's `eq rD.yz, cb13[0].xxxx, ...`, insert the strip test (top: origin above clip y =
   cb13[0].y; bottom: origin below clip y = -cb13[1].x; each only when its value is not 0), ending
   `and rD.x, rD.x, rM.z` (the decision is turned off inside the strips).
Rows found: cb0[7].w (9938094a, 3b819a7e, fd60f2d7, e12863b9, 9689d605, 91e2b222), cb0[9].w (c9b47e60), cb0[r1.w + 0].w
(05154232), cb0[r0.y + 0].w (599bd060), cb0[r0.w + 0].w (4b432a87, 54cd897e, 7d8fcdc2, ef1c1604).
cb13 unbound (no AKVR layer) = all zero = no change. AKVR sends the top band (28%) and the bottom strip (panel slider
"bottom strip that hangs in the room", default 28%, setting `hudbottom`) in a 32-byte buffer.

**Detect:** `// AKVR EDGEBAND`. **Reference:** `Patch-EdgeBand` in `tools/AKVR-fix-patches.ps1` (copy: 13 patched, second
run 0). **Driver test:** 13/13 assembled (cmd_Decompiler 0.6.90 `-a`) and loaded OK by `tools/shader_driver_test`.
**Applied** 2026-09-29 to JJ's game with the script (all texts byte-identical to the tested copy). Rollback:
`akvr/diagnostics/before-EDGEBAND-20260929/` (13 texts + bins, the DLL and settings).

---

## 1g. ShaderFixesDM: HUD parts marked "hang in the room" never follow scene depth (AKVR, PARTTAG, 2026-09-30)

**PARTTAG5 2026-09-30 (current, applied to JJ's game):** CBDUMP (two F2s, JJ's four stuck compass pieces shown / hidden)
showed (a) stacked marks: compass AND child ticked, add = -0.003906, outside -0.003; (b) pieces coloured by their own add
(multiply 0, add ~0.75..0.86) that swallow a small rgb mark. The mark now also goes into add ALPHA (a child's own alpha add
is 0) and any of .x/.y/.z/.w in -0.02..-0.0002 counts. Test lines: `lt rT.xyzw, ADD.xyzw, l(-0.0002 x4)`,
`lt rU.xyzw, l(-0.02 x4), ADD.xyzw`, `and`, `or rT.xy, rT.xyxx, rT.zwzz`, `or`, `not`. 9/9 driver-tested, temps checked.
Previous state: `diagnostics/before-PARTTAG5-20260930/`.

**PARTTAG4 (superseded):** the mark is written to add red, green and blue, and a piece
counts as marked when ANY of add .x/.y/.z is in -0.003..-0.0002 (a tinted child scales the parent's add by its own
multiply, so a green icon lost a blue-only mark; JJ: pieces over the compass stayed head-locked). Two new temps
(`dcl_temps` +2): `lt rT.xyz, ADD.xyzx, l(-0.0002 x3, 0)`, `lt rU.xyz, l(-0.003 x3, 0), ADD.xyzx`, `and`, two `or`, `not`.
Applied from each file's pre-1g state (`before-PARTTAG-20260930` for 7, `before-PARTTAG3-20260930` for 4b432a87 /
91e2b222); 9/9 driver-tested, declared temps checked. Previous state: `diagnostics/before-PARTTAG4-20260930/`.

**CORRECTED 2026-09-30 (PARTTAG3, applied to JJ's game):** the first version read the SECOND row of each colour pair and
never saw the mark (JJ: compass still doubled). MARKREC (F2 `hudmarks.csv`) showed the mark arriving exactly (-0.001953)
in the FIRST row - the game uploads add before multiply. The step now reads the first row of a `mov/mov` pair (cb0[12],
cb0[6], cb0[r0.x + 0]) and also handles `mad oN, vK, cb0[MUL], cb0[ADD]` (4b432a87: cb0[r0.y + 0]; 91e2b222: cb0[8]),
where it reads right after the mad into the new temp. 9 shaders patched, 4 have no colour transform. 9/9 driver-tested.
The 7 first-version texts were restored from `diagnostics/before-PARTTAG-20260930/` before re-applying; the first-version
state is in `diagnostics/before-PARTTAG3-20260930/`. The text below describes the first version.

**Files:** the 7 HUD `-vs.txt` that pass a colour transform (`9938094a`, `05154232`, `fd60f2d7`, `c9b47e60`, `54cd897e`,
`ef1c1604`, `9689d605`; after 1-1e); `-vs.bin` deleted. The other 6 have no colour transform and are left alone.

**Why:** JJ: every HUD element except those attached to world objects must hang, whole, on one room-fixed plane. The
fix's scene-depth decision is per piece (texture tags incl. shared atlases), so one element came apart between AKVR's
room-fixed layer and the 3D picture (doubled). AKVR now marks whole Scaleform parts (panel box "hang in the room") by
writing add blue = -1/512 into the part's colour transform (TreeNode data +0x68, through GetWritableData(node, 2)
0x1411c96f0); every piece of the part inherits it.

**Edit (per file; `rT` = new temp, `dcl_temps` +1):** the colour transform is the pair `mov oA.xyzw, cb0[M].xyzw` /
`mov oB.xyzw, cb0[ADD].xyzw` (multiply, add). Directly before the 1b / 1e line that remembers the fix's decision
(`// AKVR DEPTHALL` + `ine rN.x, rX.c, l(0)`, or `// AKVR HUDSPLIT: remember` + `mov rN.x, r0.z`), insert:
```
// AKVR PARTTAG 2026-09-30: ...
lt rT.x, cb0[ADD].z, l(-0.001000)
lt rT.y, l(-0.003000), cb0[ADD].z
and rT.x, rT.x, rT.y
not rT.x, rT.x
and DEC, DEC, rT.x
```
(DEC = the decision register: r0.z in 9938094a / 05154232 / 54cd897e / ef1c1604, r1.x in fd60f2d7 / c9b47e60 / 9689d605;
ADD = cb0[13], cb0[7] or the looked-up cb0[r0.y + 0], whose index register is checked unchanged in between.) A marked
piece skips the depth search (flat, takes the HUD distance) and the split sends it only to the layer. No mark = no change.

**Detect:** `// AKVR PARTTAG`. **Reference:** `Patch-PartTag` in `tools/AKVR-fix-patches.ps1` (copy: 7 patched, 6 not
needed, second run 0). **Driver test:** 7/7 assembled (cmd_Decompiler 0.6.90 `-a`) and loaded OK. **Applied** 2026-09-30
to JJ's game (texts byte-identical to the tested copy). Rollback: `akvr/diagnostics/before-PARTTAG-20260930/`.

---

## 1h. ShaderFixesDM: the rain block kept around the camera hangs in the room (AKVR, NEARRAIN, 2026-10-01)

**File:** `ShaderFixesDM\f50d1365e929b3a0-vs.txt` (the fix's rain-streak vertex shader; the `-vs.bin` is deleted).

**Why:** JJ: one layer of rain "attached to the face", in 3D, moving with the head even in the pause. AKVR's draw
probe (build DRAWPROBE4 -> RAINPARTS2) showed it is part of the world rain draw itself (PS 5d787946eda54077 + this VS,
6 x 20480 instances, streaks read from a structured buffer by SV_InstanceID): the FIRST 2048 streaks are a block the
game places around its camera, and in VR the camera turns with the head. JJ wanted them hung in space, not removed.

**Edit** (register names as in the fix's file today; the script reads them from the file):
1. `dcl_constantbuffer CB12[3], immediateIndexed` after the first `dcl_constantbuffer` line; `dcl_temps 9` -> `16`.
2. After `ld_structured_indexable(...) r2.xyzw, v1.x, l(48), t0.xyzw` (the streak position): 47 lines behind
   `// AKVR NEARRAIN`: flags (instance < cb12[1].w AND cb12[0].w == 1 -> re-place; == 2 -> hide); the head-turned
   camera from this draw's view-projection (cb0[6..9]: the w column = forward, the x / y columns minus their forward
   part = right / up, the camera position solved from clip x = y = w = 0); the streak in that camera's axes; put back
   with the axes AKVR sends in cb12[0..2].xyz (the game camera's own forward / right / up, UE3 world); `movc r2.xyz`.
3. `mov o2.xyzw, r6.xyzw` (the final position) -> `movc o2.xyzw, r9.zzzz, l(-10, -10, 0, 1), r6.xyzw` (mode 2 hides).

AKVR binds cb12 only around this draw (hudsplit.cpp `rain_cb_pre`). **Unbound, cb12 reads 0 = the fix's shader,
unchanged** (so the edit is harmless without AKVR). Modes in the panel (RAIN LAYERS "rain close to you", setting
`nearrain`): 1 hang in the room (default), 0 as the game draws it, 2 hidden.

**NEARRAIN2 (same day, replaces v1):** JJ: head translation fixed, but "when rotating your head around, the rain as a
complete block seems to rotate". `CB12[6]`; after the axes are taken from the view-projection, 4 lines (behind
`// NEARRAIN2`): `eq rT.y, cb12[3].w, l(1)` and three `movc` that take the drawn camera's forward / right / up from
cb12[3..5].xyz instead (AKVR sends the pair from N Presents back: panel "camera frames back", setting `nearrainlag`,
0 = the draw's own view-projection as in v1). The script restores a v1 file from `akvr_fix_backup` before patching.
Tested on a practice copy both ways (v1 upgrade and a fresh original -> identical text), assembled and driver-loaded OK;
applied to JJ's game with the real script (text identical to the tested copy, sha256 0C3B2F75...).

**NEARRAIN3 (same day, replaces v2):** JJ: "previously the rain was attached to head position translation, and now
it's attached somehow to head rotation while staying mostly hanging in space" - the block follows the camera's
POSITION, never its rotation. Mode 3 (new default): 2 lines after the flags (`eq r15.w, cb12[0].w, l(3)` / `and`) and
3 after the mode-1 `movc` (`add r14.xyz, r2.xyzx, -cb12[0].xyzx` / `movc r2.xyz, r15.wwww, ...`): the streak position
minus the position offset AKVR adds to the camera for the head (cb12[0].xyz, from the stub's dPos slots, N Presents
back). Tested on copies both ways (v2 upgrade and a fresh original -> identical, sha256 1CD8B360...), assembled and
driver-loaded OK; applied to JJ's game with the real script (identical text).

**Detect:** `// AKVR NEARRAIN3` (older: `NEARRAIN2` / `NEARRAIN`). **Reference:** `Patch-NearRain` in `tools/AKVR-fix-patches.ps1`.
**Driver test:** assembled (cmd_Decompiler 0.6.90 `-a`) and loaded OK in vstest.exe. **Applied** 2026-10-01 to JJ's
game with the real script (text byte-identical to the tested copy, `.bin` removed, original in `akvr_fix_backup\`).
Untested in the headset.

---

## 1i. ShaderFixesDM: the rain simulation keeps its regions ahead of the GAME camera (AKVR, FARRAIN, 2026-10-01)

**File:** `ShaderFixesDM\a96594b16ceb399b-cs.txt` (the fix's "Rain haloing CS" - the compute shader that moves the rain
and writes the streak buffer the rain draw reads; the `-cs.bin` is deleted).

**Why:** the 1h edits (re-placing streaks in the draw) never held. AKVR's RAINWRITER capture (F2 2026-10-01 17:03) named
this shader as the writer and recorded its cb0: [10] = camera position, [11] = camera forward x 512 WITH the head turn
(matched the drawn camera's forward). The shader centres the main rain box at [10] + [11], places drops relative to
the camera with [11], and pushes a distant layer (streaks 1024..2047, thread groups 4..7) a further 2 x [11] along the
view - in VR that layer swings round the player with every head turn ("as if there's another camera orbiting it"),
which is what JJ saw (the cut test lost it between 2048 and 1024).

**Edit:** `dcl_constantbuffer CB13[1], immediateIndexed` after the first `dcl_constantbuffer`; `dcl_temps 32` -> `33`;
in front of `add r10.xyz, cb0[10].xyzx, cb0[11].xyzx`, 7 lines behind `// AKVR FARRAIN`: with cb13[0].w = 1,
r32.xyz = cb13[0].xyz (the game camera's own forward, from AKVR) x |cb0[11]|, else cb0[11]; that line, `add r4.yzw,
r0.xxyz, cb0[11].xxyz` and `mad r10.yzw, cb0[11].xxyz, l(0, 2, 2, 2), r4.yyzw` then use r32 instead of cb0[11]. AKVR
binds CS cb13 only around this dispatch (hudsplit.cpp `far_cb_pre/post`, setting `farrain`, panel "rain stays in the
world when you turn your head"). **Unbound, cb13 reads 0 = the fix's shader, unchanged.**

**Detect:** `// AKVR FARRAIN`. **Reference:** `Patch-FarRain` (step 1i) in `tools/AKVR-fix-patches.ps1`.
**Driver test:** the original and the edited text assembled (cmd_Decompiler 0.6.90 `-a`) and loaded OK as compute
shaders (vstest.exe now loads `-cs` files with CreateComputeShader). **Applied** 2026-10-01 to JJ's game with the
real script (text identical to the tested copy, sha256 296884F0...). Untested in the headset. The 1h draw-side modes are
off by default now (`nearrain=0`); the 1h edit stays (inert at mode 0).

---

## 1j. ShaderFixesDM: a HUD piece drawn far from its origin follows the depth under itself (AKVR, RETNEAR, 2026-10-01)

**Files:** `9938094af96353c0-vs.txt`, `05154232f7872d0d-vs.txt` (the two RETFLAT shaders, after 1c / 1d); `-vs.bin` deleted.

**Why:** JJ: the target-distance number "floats around a general area ... when you move your head or move the game
camera, it doesn't stay locked onto anything". Its HUD part (`K2/0.0.0.0.1.0.0.1`) sits at the stage centre with an
identity matrix and the game draws the number away from that origin, so RETFLAT (1c) made it search scene depth at the
middle of the view. (Assumes the number is drawn by one of these two shaders: the per-draw recorder came back empty,
so this is inferred from the part tree, not measured.)

**Edit (per file; `rT` = the RETFLAT temp, `rC` = the corner register: r1 in 9938094a, r3 in 05154232; `rU` = new
temp, `dcl_temps` +1):** directly after RETFLAT's `movc rT.z, ...` line:
```
// AKVR RETNEAR 2026-10-01: a piece drawn far from its origin (a number the game places away from it) searches
// scene depth from its own corner instead (blend from 0.15 to 0.25 clip units), so it follows what it marks.
add rU.xy, rC.xyxx, -rT.yzyy
dp2 rU.z, rU.xyxx, rU.xyxx
sqrt rU.z, rU.z
mad_sat rU.z, rU.z, l(10.000000), l(-1.500000)
mad rT.yz, rU.zzzz, rU.xxyx, rT.yyzy
```
Pieces whose corners are within 0.15 clip units of their origin (the reticle rings) are unchanged.

**Detect:** `// AKVR RETNEAR`. **Reference:** `Patch-RetNear` (step 1j) in `tools/AKVR-fix-patches.ps1` (practice copy: 2
patched, second run 0). **Driver test:** both assembled (cmd_Decompiler 0.6.90 `-a`) and loaded OK in vstest.exe.
**Applied** 2026-10-01 to JJ's game with the script (texts identical to the tested copies). Untested in the headset.
Rollback: `akvr/diagnostics/before-RETNEAR-20261001/` (2 texts + bins, DLL, settings).

---

## 1k. ShaderFixesDM: the "stays on its target" parts follow scene depth as one piece, all 13 shaders (AKVR, TARGETDEPTH, 2026-10-02)

**Files:** all 13 HUD `-vs.txt` (after 1, 1b, 1c, 1d, 1e, 1g, 1j); `-vs.bin` deleted.

**Why:** JJ after RETNEAR: the target-distance widget is "a combination of both sticking to its target and sticking to
my face"; it "appears wherever the world object is that it's pointing to, but it moves around when you move your head";
it "should be at the depth of what's behind it, what it's pointing to". The widget (`K2/0.0.0.0.1.0.0.1` -> its child
`.0`) is ~20 pieces (digits, bracket halves, lines), each searching scene depth at its own spot (RETNEAR's premise, a
number far from its origin, was wrong). AKVR now sends one point per on-target part (earlyres `akvr_hud_target_points`,
live tree, checked against JJ's F2 to ~0.02 of the eye).

**Edit (per file; `rW`, `rU`, `rV` = three new temps, `dcl_temps` +3; `CB13[1]` -> `CB13[3]`):**
1. After the last `dp4 rP.y, vK.xyzw, cb0[ROWY].xyzw` before the first IniParams load (EDGEBAND's position transform)
   and its `dp4 rP.x, vK.xyzw, cb0[ROWX]`: `eq rW.x, vK.w, l(1)`, `movc rW.y, rW.x, cb0[ROWX].w, rP.x`, same for `.z` / y.
2. Before AKVR's "remember" line (DEPTHALL `ine rN.x, rD.c, l(0)` / HUDSPLIT `mov r10.x, rD.c`): near1 / near2 = origin
   within cb13[1].z of cb13[1].xy (and .w != 0), same for row 2; `rV.xy` = the nearer point, else the depth search's
   own start (march x, row y); near = near1 | near2, AND IniParams x11 == 0 (gameplay), AND not room-marked (the
   PARTTAG `and rD, rD, rM.x` register, where the shader has one); `or rD, rD, near` (follows depth).
3. In the depth block: the row `mad ., Y, l(-0.5), l(0.5)` reads `rV.y`; the march `add o, start, step` becomes
   `add o, step, rV.x`. The result is added to every vertex as before, so the piece moves rigidly.
cb13 rows 1/2 zero or unbound = unchanged. AKVR sends the points only with `hudroomall=1` (radius 0.12 clip).

**Detect:** `// AKVR TARGETDEPTH`. **Reference:** `Patch-TargetDepth` (step 1k) in `tools/AKVR-fix-patches.ps1`
(practice copy: 13 patched, second run 0; per-file registers in the script's green lines). **Driver test:** 13/13
assembled (cmd_Decompiler 0.6.90 `-a`) and loaded OK in vstest.exe. **Applied** 2026-10-02 to JJ's game with the
script (13 texts identical to the tested copies). Untested in the headset.
Rollback: `akvr/diagnostics/before-TARGETDEPTH-20261002/` (13 texts + bins, DLL, settings).

---

## 1l. ShaderFixesDM: optional - world rain streaks without the per-frame camera term (AKVR, RAINSTRETCH, 2026-10-02)

**File:** `f50d1365e929b3a0-vs.txt` (after 1h); `-vs.bin` deleted.

**Why:** JJ: isolated rain in the pause "sort of jitters a bit" when he turns his head, "it kind of follows a little
bit". The rain VS projects each streak from its tail (position minus its own velocity) to its head (position +
`cb0[12] * 0.5` when the streak's flag is set). `cb0[12]` looks like the camera's movement since the last frame; in
third person a head turn swings the camera round Batman. Hypothesis, not measured: a test switch.

**Edit:** directly after `mul r4.xyz, r4.xxxx, cb0[12].xyzx`:
```
// AKVR RAINSTRETCH 2026-10-02: cb12[2].w = 1 (AKVR) drops the per-frame camera term from the streak's head
ne r15.x, cb12[2].w, l(0.000000)
movc r4.xyz, r15.xxxx, l(0, 0, 0, 0), r4.xyzx
```
(r15 is free there: 1h uses it only before.) AKVR binds cb12 around the rain draw when `nearrain` != 0 OR the panel
switch "rain streaks ignore camera movement" (`rainnostretch=1`) is on; default off = unchanged.

**Detect:** `// AKVR RAINSTRETCH`. **Reference:** `Patch-RainStretch` (step 1l) in `tools/AKVR-fix-patches.ps1`
(practice copy: patched, second run 0). **Driver test:** assembled (cmd_Decompiler 0.6.90 `-a`) and loaded OK in
vstest.exe. **Applied** 2026-10-02 to JJ's game (identical to the tested copy). Untested in the headset.
Rollback: `akvr/diagnostics/before-RAINSTRETCH-20261002/`.
JJ 2026-10-02: the switch "doesn't seem to do anything" - hypothesis not supported; leave off (default).

---

## 1o. ShaderFixesDM: the world rain drawn for the world's camera, one frame older (AKVR, RAINLAG / FRAMEPAIR, 2026-10-02)

**File:** `f50d1365e929b3a0-vs.txt` (after 1h, 1l); `-vs.bin` deleted.

**Why:** JJ found it: "Head pose delay of 2 makes the rain go rock solid and stable, and it makes the [reticle] and the
distance HUD marker go rock solid. The problem with that is that the world becomes jittery." UE3's one-frame thread lag
(OneFrameThreadLag=True, kept: 44 -> 84 fps under geo-11) - the world is drawn with the camera one frame older than the
one the game used for the rain draw and the HUD markers. AKVR (hudsplit `frame_pair`) takes the drawn camera of
`frameworld` Presents back (the world, o) and one newer (n) from the camera ring (`akvr_camera_pose_ago`, axes +
position) and sends X' = M X + t, M = An Ao^T, t = Pn - M Po: the point the newer camera sees where the older one sees X.

**Edit:** `dcl_constantbuffer CB12[6]` -> `CB12[9]`; after the streak tail `mad r1.xyz, -r0.yyyy, r1.yzwy, r2.xyzx` and
after the streak head `mad r4.xyz, r4.xyzx, l(0.5, 0.5, 0.5, 0), r2.xyzx`, for R = r1 / r4:
```
// AKVR RAINLAG 2026-10-02: the streak's tail/head as the world's (one frame older) camera sees it (cb12[4].w = 1)
dp3 r14.x, cb12[6].xyzx, R.xyzx      (and .y with cb12[7], .z with cb12[8])
mov r15.x, cb12[6].w                 (and .y / .z)
add r14.xyz, r14.xyzx, r15.xyzx
eq r15.w, cb12[4].w, l(1.000000)
movc R.xyz, r15.wwww, r14.xyzx, R.xyzx
```
(r14 / r15 are free at both points.) AKVR binds cb12 around the rain draw while `framefix=1` (default; panel HUD "rain and
markers match the world's frame", "timing" = `frameworld`, default 1). The HUD markers get the same one-frame shift
(turn only, at the reticle's point) in cb13 row 4 .zw - DLL only.

**Detect:** `// AKVR RAINLAG`. **Reference:** `Patch-RainLag` (step 1o) in `tools/AKVR-fix-patches.ps1` (practice copy:
patched, second run 0). **Driver test:** assembled and loaded OK in vstest.exe. **Applied** 2026-10-02 to JJ's game
(identical to the tested copy); `posedelay` set back to 3 in JJ's settings. Untested in the headset.
Rollback: `akvr/diagnostics/before-FRAMEPAIR-20261002/`.

---

## 1m. ShaderFixesDM: the on-target parts move with the head turn the game's HUD does not know about (AKVR, TARGETMOVE, 2026-10-02)

**Files:** all 13 HUD `-vs.txt` (after 1k); `-vs.bin` deleted.

**Why:** JJ: "the reticle for different grappling hook points and the target distance HUD element are still following
head movement". The game places its markers with its own camera; AKVR adds the head turn to the camera only for the
picture, so the markers keep their screen spot while the world turns. AKVR (hudsplit `layer_begin`) takes each on-target
point (1k), casts the ray through it from the game's camera and projects it with the drawn camera (the pair recorded
k Presents back, `akvr_camera_axes_ago`; default k = pose delay in use - 1; tan half-angles from `akvr_xr_game_tan`),
and sends the screen offset in cb13 row 3 (point 1 .xy, point 2 .zw). Hypothesis (HUD = camera without the head)
not yet measured: TARGETTRACE records points, offsets and both cameras when test tools are on.

**Edit (per file; `rW` / `rU` / `rV` = 1k's temps, `rP` = 1k's position register; `CB13[3]` -> `CB13[4]`):**
1. After 1k's `movc rV.xy, rU.zzzz, cb13[1].xyxx, cb13[2].xyxx`: `movc rW.xw, rU.zzzz, cb13[3].xxxy, cb13[3].zzzw` and
   `add rV.xy, rV.xyxx, rW.xwxx` (depth is searched at the moved point).
2. After 1k's `or rD, rD, rU.z`: `and rW.xw, rW.xxxw, rU.zzzz` and `add rP.xy, rP.xyxx, rW.xwxx` (the piece moves; only
   pieces that follow a point - gameplay, not room-marked).
Row 3 zero / unbound = no change. Panel (HUD): "reticle and distance marker stay on their target when you turn your
head" (`markerhead`, default on) and "marker timing" (`markerlag`, -1 = automatic).

**Detect:** `// AKVR TARGETMOVE`. **Reference:** `Patch-TargetMove` (step 1m) in `tools/AKVR-fix-patches.ps1` (practice
copy: 13 patched, second run 0). **Driver test:** 13/13 assembled and loaded OK in vstest.exe. **Applied** 2026-10-02 to
JJ's game (13 texts identical to the tested copies). Untested in the headset.
Rollback: `akvr/diagnostics/before-TARGETMOVE-20261002/` (13 texts + bins, DLL, settings).
**2026-10-02 later (DLL only, no shader change):** the camera-based offset was wrong (JJ: "the opposite and more
exaggerated", then the lag version "appears to do nothing"). Row 3 now carries TARGETSTOCK: the HUD shrink undone for the
on-target points (movie's own matrix vs AKVR's shrunk root). The shader edit is unchanged.
**Superseded for the pieces by 1n** (JJ: the TARGETSTOCK markers "are jittering when you move your head"): 1m's two
`and` / `add rP` lines are replaced; 1m's first edit (the depth sample point `rV += row 3`) stays.

---

## 1n. ShaderFixesDM: on-target pieces drawn through the movie's own layout, per vertex (AKVR, TARGETSCALE, 2026-10-02)

**Files:** all 13 HUD `-vs.txt` (after 1m); `-vs.bin` deleted.

**Why:** the per-frame offset of 1m (TARGETSTOCK) was read from the live HUD tree on the render thread, while the game
may lay out the next frame -> jitter on head movement. AKVR's HUD shrink is one fixed affine map (AKVR's root matrix vs
the movie's own matrix view+0x110), so the shader maps each vertex back: x' = x + x * cb13[4].x + cb13[4].z, y' = y +
y * cb13[4].y + cb13[4].w (row 4 = kx - 1, ky - 1, bx, by in clip units; earlyres `akvr_hud_target_points` xf). The
markers come back at the game's own size (~1.67x the shrunk HUD).

**Edit (per file):** 1m's
```
// AKVR TARGETMOVE: the piece moves by its point's offset (only pieces that follow the point)
and rW.xw, rW.xxxw, rU.zzzz
add rP.xy, rP.xyxx, rW.xwxx
```
becomes
```
// AKVR TARGETSCALE 2026-10-02: the piece is drawn through the movie's own layout (cb13[4] = kx-1, ky-1, bx, by)
mad rW.xw, rP.xxxy, cb13[4].xxxy, cb13[4].zzzw
and rW.xw, rW.xxxw, rU.zzzz
add rP.xy, rP.xyxx, rW.xwxx
```
and `CB13[4]` -> `CB13[5]`. Row 4 zero / unbound = no change.

**Detect:** `// AKVR TARGETSCALE`. **Reference:** `Patch-TargetScale` (step 1n) in `tools/AKVR-fix-patches.ps1` (practice
copy: 13 patched, second run 0). **Driver test:** 13/13 assembled and loaded OK in vstest.exe. **Applied** 2026-10-02 to
JJ's game (13 texts identical to the tested copies). Untested in the headset.
Rollback: `akvr/diagnostics/before-TARGETSCALE-20261002/` (13 texts + bins, DLL, settings).

---

## 1p. ShaderFixesDM: up to 8 on-target points, for world-marker lists (AKVR, TARGETMULTI, 2026-10-02)

**Files:** all 13 HUD `-vs.txt` (after 1n); `-vs.bin` deleted.

**Why:** JJ: a HUD element (the Batmobile marker - one of the 74-marker list `K4/0.0.0.0.1.0.0.0`) "was moving around with
head movement, but not attached to the face, more like moving in the opposite direction": a world marker placed by the
game per frame and then hung in the room. 1k compared pieces with 2 points; a list needs a point per visible marker. AKVR
(earlyres `akvr_hud_target_points`) now gives each VISIBLE child of a part with 6+ children its own point (up to 8).

**Edit (per file):** 1k's two-point test plus 1m's offset pick (from `add rU.xy, rW.yzyy, -cb13[1].xyxx` through
`or rU.z, rU.z, rU.x`) is replaced by `mov rU.z, l(0)`, `mov rV.xy, l(0,0,0,0)`, then for each row R in 1, 2, 5-10 the
same distance test (`add/dp2/mul/lt/ne/and`), `movc rV.xy, rU.xxxx, cb13[R].xyxx, rV.xyxx`, `or rU.z, rU.z, rU.x` (last
match wins), then `mad rU.xy, rV.xyxx, cb13[4].xyxx, cb13[4].zwzz` / `add rV.xy, rV.xyxx, rU.xyxx` (the depth point
through the movie-layout map; row 3 is no longer read). `CB13[5]` -> `CB13[11]`. Rows zero / unbound = no change.

**Detect:** `// AKVR TARGETMULTI`. **Reference:** `Patch-TargetMulti` (step 1p) in `tools/AKVR-fix-patches.ps1` (practice
copy: 13 patched, second run 0). **Driver test:** 13/13 assembled and loaded OK in vstest.exe. **Applied** 2026-10-02 to
JJ's game (identical to the tested copies). JJ's `hudlayers` got `K4/0.0.0.0.1.0.0.0` ... `:1` (stays on its target).
Untested in the headset. Rollback: `akvr/diagnostics/before-TARGETMULTI-20261002/`.

---

## 1q. ShaderFixesDM: the on-target pieces face the eye, so turning the head no longer turns them (AKVR, TARGETFACE, 2026-10-02)

**Files:** all 13 HUD `-vs.txt` (after 1p); `-vs.bin` deleted.

**Why:** JJ: "the HUD elements that are attached to world space objects, like the reticle, and the distance marker turn on
their y-axis as you turn your head". The game draws them as stickers flat on its picture; the picture turns with the head,
so a marker off to the side is seen on a slant. On a flat (rectilinear) picture a fixed-size sticker at angle t from the
centre covers cos^2 t of its straight-ahead width and cos t of its height (45 deg: 50% / 71%, checked numerically). Not
RETSQUASH (1d, never worked, inert): that scaled each piece about its own corner in 2 shaders, before the on-target points
existed; this lays the whole piece on a plane facing the eye, about its target point, in all 13.

**Edit (per file; `rP` = position, `rV` = the matched point, `rU.z` = 1p's on-target mask, all read from the 1n/1p lines;
`rA`, `rB` = two new temps, `dcl_temps` +2):** directly after 1n's `add rP.xy, rP.xyxx, rW.xwxx`:
```
// AKVR TARGETFACE 2026-10-02: an on-target piece faces the eye: its offset from its point is laid on the plane square
// to the line of sight to that point (cb13[3] = tan half-angles h, v, on), so turning the head no longer turns it
ne rA.w, cb13[3].z, l(0.000000)
and rA.w, rA.w, rU.z
if_nz rA.w
  (u, v = rV * tan; dx, dy = (rP - rV) * tan; L = sqrt(1+u^2+v^2), M = sqrt(1+u^2); a = dx L/M, b = dy/M;
   P = (u + a - b u v, v + b (1+u^2), max(1 - a u - b v, 0.05)); rP.xy = (P.xy / P.z) / tan)  - 22 instructions
endif
```
CB13 stays 11 rows (row 3 was unused since 1p). AKVR sends row 3 = (tan half-angle x, y of the game frame
[`akvr_xr_game_tan`], 1, 0) only with on-target points and the panel's "... and face you" box on (`markerface`, default
1); RETSQUASH's row 0 .zw stays 0. Row 3 zero / unbound = no change. Unchanged at the centre of the view.

**Detect:** `// AKVR TARGETFACE`. **Reference:** `Patch-TargetFace` (step 1q) in `tools/AKVR-fix-patches.ps1` (practice
copy of JJ's ShaderFixesDM: 13 patched, second run 0, exit 0; the instruction sequence was simulated in Python against
the closed form and a direct 3D construction). **Driver test:** 13/13 assembled (cmd_Decompiler 0.6.90 `-a`, round-trip
disassembly shows every instruction) and loaded OK in vstest.exe. **Applied** 2026-10-02 to JJ's game with the script
(13 texts identical to the tested copies). Untested in the headset.
Also fixed in the script: on an already-patched set, 1m reported "1k lines not found" (1n replaces 1m's marker) - a
repair run exited 2; 1m now counts 1n as already patched.
Rollback: `akvr/diagnostics/before-TARGETFACE-20261002/` (13 texts, DLL, settings), or untick "... and face you".

---

## 1r. ShaderFixesDM: the on-target pieces keep the world's up, so rolling the head no longer rolls them (AKVR, TARGETUP, 2026-10-02)

**Files:** all 13 HUD `-vs.txt` (after 1q); `-vs.bin` deleted.

**Why:** JJ after 1q + TARGETANCHOR: the distance marker "is still rotating on the z-axis when rolling your head". 1q laid
each piece on the plane facing the eye but took that plane's up from the picture, which rolls with the head.

**Edit (per file):** 1q's block (from its `// AKVR TARGETFACE` comment to its `endif`) is REPLACED by a 3D version, one more
temp `rC`, `CB13[11]` -> `CB13[12]`: n = normalize(u, v, 1) (the point's tangents, cb13[3].xy); W = cb13[11].xyz (the
world's up in the drawn camera's frame, x right / y up / z forward), (0, 1, 0) if |W|^2 < 0.01; R = normalize(W x n);
U = n x R; P = n + dx R + dy U (dx, dy = the vertex's offset from the point in tangent units); x' = P.x / max(P.z, 0.05)
/ tanH, likewise y. W = (0, 1, 0) gives exactly 1q (checked numerically, and a 20 deg roll keeps an up-offset along W).
AKVR sends row 11 = the drawn camera's axes' world-z components (right.z, up.z, fwd.z, from the camera recorded one
Present back) with the face rows. Row 11 zero / unbound = 1q's behaviour. 1q's patch now counts 1r as already patched.

**Detect:** `// AKVR TARGETUP`. **Reference:** `Patch-TargetUp` (step 1r). Practice copy: 13 patched, rerun 0, no
TARGETFACE block left. **Driver test:** 13/13 assembled and loaded OK. **Applied** 2026-10-02 to JJ's game (identical to
the tested copies). Untested in the headset. Rollback: `akvr/diagnostics/before-TARGETUP-20261002/`.

---

## 2. d3dxdm.ini

| Key / section | Baseline | AKVR value | Status and reason |
|---|---|---|---|
| `[Direct Mode] direct_mode` | `sbs` | `katanga_vr` | **AKVR.** AKVR reads geo-11's two-eye picture through the Katanga shared surface. |
| `shader_regex_patch_mode` | `5` | `4` | **AKVR.** Mode 5 stalled on AK's shaders (memory `geo11-july-failure-rediagnosed`, `geo11-update-reverts-hook-mode`). |
| `[Stereo] dm_hud_detection` | `0` | `1` | **AKVR** (HUDFIX, 2026-09-27). |
| `[Stereo] dm_static_hud_depth` | absent | `1.0` | **AKVR** (HUDFIX). Add if missing. |
| `[Stereo] dm_auto_hud_depth` | absent | `0` | **AKVR** (HUDFIX). Add if missing. |
| `[Stereo] dm_auto_hud_offset_min` | absent | `0.0` | **AKVR** (HUDFIX). Add if missing. |
| `[Stereo] dm_auto_hud_offset_max` | absent | `1.0` | **AKVR** (HUDFIX). Add if missing. |
| `[Stereo] dm_convergence` | `168.0` | `2500.0` | **AKVR, written by the mod itself** (world-scale slider saves it). The installer only needs a sane start value; 2500 = world scale 1.00 at separation 1. |
| `[Stereo] dm_separation` | `100` | `1.00` | **AKVR, written by the mod itself** (VRSEP holds separation at 1). |
| `[Stereo] dm_auto_convergence` | `1` | `0` | **UNKNOWN** origin; AKVR's live convergence assumes it is off. Likely needed. |
| `upscaling` | `0` | `1` | **UNKNOWN** origin. |
| `fps_show_hide` | commented out | `ctrl F` | **UNKNOWN** origin (a key binding; harmless). |
| new sections `[TextureOverrideAKVRTinyRT]` and `[TextureOverrideAKVRTinyUAV]` | absent | see below | **AKVR** (EXPOSURE, 2026-09-27): both eyes share one auto-exposure. |

EXPOSURE sections (insert once, anywhere among the TextureOverride sections):

```
[TextureOverrideAKVRTinyRT]
match_type = Texture2D
match_bind_flags = +render_target
match_width = <5
match_height = <5
StereoMode = 2

[TextureOverrideAKVRTinyUAV]
match_type = Texture2D
match_bind_flags = +unordered_access
match_width = <5
match_height = <5
StereoMode = 2
```

Not AKVR: the installed file also has an `[Anaglyph]` section, the key-preset block (Ctrl+F3..F7) and extra
comment lines. Those came with the newer geo-11 release's template (see section 4). AKVR's live world-scale
slider works without the Ctrl+F keys, but the Ctrl+F7 save line in `d3dx_user.ini` can override
`dm_convergence` (the mod warns about it).

---

## 3. d3dx.ini

| Change | Baseline | AKVR | Status and reason |
|---|---|---|---|
| `load_library_redirect` | `2` | `0` | **AKVR, HOOK mode.** geo-11 is loaded by AKVR as `geo11.dll`; done by `AKVR-geo11-mode.ps1 -Mode hook`. |
| `[TextureOverrideAllNonSquareRT]` (uncommented from the commented `;[TextureOverrideAllRT]` example) | commented example | `match_type = Texture2D`, `match_bind_flags = +render_target`, `match_width = !height`, `StereoMode = 1` | **AKVR.** Keeps stereo on AKVR's non-16:9 render size (memory `geo11-contact-non-16x9`). |
| `include = ShaderFixes\help_text\help.ini` | absent | present | Not AKVR: came with the newer geo-11 release (its help overlay). |
| `hunting` | `0` | `0` | Unchanged (it was 2 during a test on 2026-09-27 and set back). |

---

## 3b. d3dx.ini TEMPORARY diagnostic "AKVR DIAG RB" (2026-09-28) - REMOVED 2026-09-28 (diag_rb.py off), NOT for the installer

To find the RB icon's texture hash (JJ: the RB icon next to the grapple reticle stays at the HUD distance; the fix
only gives scene depth to textures tagged `filter_index = 2 / 42`). `akvr/tools/diag_rb.py on` sets `hunting=1`,
adds `analyse_frame = no_modifiers VK_F13` + `analyse_options = log` under `[Hunting]`, and
`analyse_options = dump_tex mono` to every `[ShaderOverride_HUD*]` section that has none (so only HUD draws dump
their textures). Every added line follows a `; AKVR DIAG RB` comment. Backup: `d3dx.ini.before-akvr-diag-rb`;
`diag_rb.py off` restores it. The AKVR panel button "record the HUD for geo-11" holds F13 for 250 ms.

---

## 3c. d3dx.ini TEMPORARY diagnostic + test "AKVR HUNTRAIN" (2026-10-01) - ON JJ's game, NOT for the installer

JJ (2026-10-01): with PAUSELOOK, (a) the game's own "screen-sized black vignette-type overlay" shows in the pause
(wanted: gone, only AKVR's whole-world darkening), and (b) "one particular layer" of rain is attached to the head.
Both need shader hashes. Applied by a byte-preserving script (Latin-1, CRLF kept); backup of the fix's file:
`akvr/diagnostics/before-HUNTRAIN-20261001/d3dx.ini` (copy it back to undo everything below). Each edit follows a
`;AKVR HUNTRAIN` comment:
- `[Hunting]`: `hunting=0` -> `hunting=2` (numpad 0 turns hunting on/off; numpad 1/2 step through visible pixel
  shaders, `marking_mode=skip` hides the selected one; numpad 3 marks it).
- `[Hunting]`: `marking_actions = clipboard regex hlsl asm stereo_snapshot snapshot_if_pink` ->
  `marking_actions = clipboard asm` (a mark writes only the exact disassembly, never a decompiled HLSL that would
  then load as a "fix"). **Marked shaders' `.txt` dumps land in the fix folder: delete them after reading.**
  2026-10-01 03:0x: JJ's one mark (5d787946eda54077-ps .txt/.bin in ShaderFixes AND ShaderFixesDM) deleted.
- `[Hunting]`: added `analyse_frame = no_modifiers VK_SCROLL` (Scroll Lock; F8 is the AKVR panel) and
  `analyse_options = mono deferred_ctx_accurate` (log only, no images): a pause frame vs a gameplay frame -> the
  pause-only pixel shaders.
- ~~TEST: `[TextureOverrideRain1]` commented out~~ — RESTORED the same day, before any run: JJ clarified the rain
  layer "moves around with your head, even when the game is paused", i.e. it is drawn in screen space; a stereo
  setting cannot move it into the world. Plan: find its pixel shader in the pause (the only moving thing there) and
  skip it.
- **RESTORED 2026-10-01 (build CLEANUP):** the rain is fixed (steps 1h/1i), so `d3dx.ini` was copied back from
  `diagnostics/before-HUNTRAIN-20261001/d3dx.ini` (hash-checked; the diff had been the HUNTRAIN lines only:
  `hunting=0` again, the original `marking_actions`, no `analyse_frame` / `analyse_options` lines). The HUNTRAIN copy
  is kept as `diagnostics/before-CLEANUP-20261001/d3dx.ini`. The shader folders were NOT touched (steps 1h/1i stay).
  Nothing of 3c remains on JJ's game; the installer never carried it.

---

## 4. geo-11 itself (HOOK mode and version)

| File | Baseline (fix pack) | Installed | Status |
|---|---|---|---|
| `d3d11.dll` | geo-11 0.6.182 | absent (renamed) | **AKVR HOOK mode:** geo-11 lives as `geo11.dll`. |
| `geo11.dll` | absent | geo-11 **0.7.11**, SHA1 `600ea47f1d75805aed9402e90c0969182ca629b0` | **AKVR.** The mod's live geo-11 links (world scale, HUD distance, eye view) are verified against this exact build and switch off on any other. |
| `dxgi.dll` | the fix's dxgi.dll | renamed `dxgi.dll.wrapmode` | **AKVR HOOK mode** (the rename script does it). |
| `nvapi64.dll` | fix version | SHA1 `be0f4f6572a5fa16d7ad86d9007d8397c862b452` | Came with the geo-11 0.7.11 update. |
| `uninstall.bat` | fix version | newer | Came with the geo-11 update. |
| `ShaderFixesDM\help.hlsl-{cs,gs,ps}.{txt,bin}` | absent | present | Came with the geo-11 update (help overlay). |

Rename/undo logic: `akvr/tools/AKVR-geo11-mode.ps1` (+ `AKVR-geo11-HOOK.bat`, `AKVR-geo11-WRAP.bat`).
A new geo-11 release puts `d3d11.dll` back and breaks HOOK mode (memory `geo11-update-reverts-hook-mode`).

---

## 6. The game's own config: BmGame\Config (AKVR installer + mod, 2026-09-29)

Not geo-11 or the fix, but still a file we did not write.

**Installer, first install only** (no `akvr_settings.ini` yet), `[SystemSettings]` section only, format `Key=Value`:
`MaxFPS=90.000000`, `TextureResolution=2`, `ShadowQuality=2`, `LevelOfDetail=2`, `MaxDrawDistanceScale=1.200000`,
`SkeletalMeshDisplayFactorScale=0.800000`, `TextureFiltering=1` = JJ's confirmed graphics menu (Max FPS 90, texture
resolution / shadows / level of detail High, texture filtering 2x anisotropic, GameWorks off), read back from the file the
game saved. Stock after a fresh Steam install differs only in `MaxFPS=60` and `TextureFiltering=0`; JJ confirmed that
with this menu the pose delay is 3 again and the blur is gone, so the 60 fps cap was the cause. (JJ's older install had
ShadowQuality / LevelOfDetail 1 and both scales 1.0 - lower detail, not needed.) Target: `BmSystemSettings.ini` if it exists, else the template `DefaultSystemSettings.ini`
(never the template once the generated file exists: UE3 then regenerates and drops the player's config).
**Why:** JJ's fresh-install test: with the stock cap of 60 and higher detail, head movement blurred and the pose delay
had to change. Reference: `Set-SystemSettings` in `install/Install-AKVR.ps1` (unit-tested on both files: 6 changed,
second run 0).

**GameWorks + NVIDIA store, added 2026-09-29 (installer, first install only):** `bEnableInteractiveSmoke`,
`bEnableInteractivePaperDebris`, `bEnableRainFX`, `bEnableVolumetricLighting` = `0` in the same `[SystemSettings]` list,
and the same menu written into NVIDIA's settings store (below) with `Set-GfxStore`: `Texture_Resolution` /
`Shadow_Quality` / `Level_Of_Detail` 2, `TextureFiltering` 1, `Interactive_Smoke` / `Interactive_Paper_Debris` /
`Rain_FX` / `Volumetric_Lighting` false (only options already in the file; encoding kept, UTF-16 without BOM).
**Why:** JJ's clean fresh-install test: he had set GameWorks on for flat play, and they stayed on in VR - the first list
only covered what differs from a STOCK install (GameWorks off there), and the store (read every start, 16x filtering,
GameWorks true) was never touched. Tested on copies of JJ's files: ini 4 changed, store 5 changed, second run 0 / 0.
The uninstaller already puts both files back from `vrmod_graphics_backup`.

**The mod itself, every start** (earlyres.cpp `force_comfort_settings`, older than this record): MotionBlur,
ChromaticAberration, FilmGrain, UseVsync, UseAdaptiveVsync = False; Fullscreen=False, WindowDisplayMode=0 (with the
forced render size); each also as its `Default*` key and in every section (buckets re-enable blur); MaxFPS / DefaultMaxFPS
raised to 120 only when below 60; OneFrameThreadLag left alone under geo-11. Original kept as `*.akvr-original`.
The game also keeps graphics options in NVIDIA's settings store (NvGsa.x64.dll) and saves them back to the file, which
undid the MotionBlur fix between two starts of JJ's fresh install (log: "5 lines flipped" again). Build GSACOMFORT
answers the store's `MotionBlur`, `Vsync` and `AdaptiveVsync` with 0 (off) in the mod's `GFSDK_GSA_GetOptionValue`
hook (earlyres.cpp; names are the wide strings next to the `GFSDK_GSA_RegisterOption` calls in BatmanAK.exe). No file
edit - status line "blur/vsync off N (store held blur X)". Untested in the game.

**That store is a file:** `<Documents>\WB Games\Batman Arkham Knight\GFXSettings.BatmanArkhamKnight.xml` (UTF-16 GSA
SDK XML; path from the NVIDIA App's `ApplicationOntology\...\batman_arkham_knight\current_game.lua`). The game reads it at
every start and saves the menu to it, so the mod's GSAHOLD answers (Display_Mode 0, ResolutionX/Y = the VR size) end up
stored there: after the uninstall JJ's game opened as a 2888x2860 window (2026-09-29). The uninstaller deletes it (the
game recreates defaults); JJ's copy is in `akvr/diagnostics/gfxsettings-store-20260929/`. Outside the game folder, so a
Steam reinstall or "verify files" does not reset it.

**THREADSYNC KEPT (2026-10-02):** JJ played without a crash once the test tools and the rain options were off. The mod
now forces `OneFrameThreadLag=False` under geo-11 as well (earlyres.cpp comfort list; it used to stand down), and the
default `posedelay` is 2. The installer's settings file needs `posedelay=2` and no `testtools`.

**SCALE110 (2026-10-02), d3dxdm.ini:** `dm_convergence = 2381.0` -> `2272.7` on JJ's game (his world scale 1.1, never
saved because a controller step does not end an edit; the mod's scale is rebased so 1.1 reads 1.00 - geo11conv.cpp
kProductAt1 = 2500/1.1). The installer's bundled d3dxdm.ini convergence should match. Backup:
`akvr/diagnostics/before-SCALE110-20261002/`.

**VRLAUNCH (2026-10-02): VR from a shortcut, 2D from Steam, separate graphics settings per mode.** JJ: "the installer
also needs to create a way for the game to be launched in VR, leaving launching it from Steam to just play normally in
2D mode". The mod (dinput8.dll, src/vrmode.cpp) decides in DllMain: VR when `akvr_launch_vr.flag` in the game folder is
under 3 minutes old (the launcher `AKVR-Launch-VR.bat` writes it, then `steam://rungameid/208650`; the note is deleted
either way) or `-akvr` is on the command line; otherwise 2D and nothing of AKVR starts (geo-11 is `geo11.dll`, loaded
only by the mod, so 2D is flat). On a mode change it saves the live `BmGame\Config\BmSystemSettings.ini` and
`<Documents>\WB Games\Batman Arkham Knight\GFXSettings.BatmanArkhamKnight.xml` to `akvr_profiles\<old mode>\` and puts
`akvr_profiles\<new mode>\` back; `akvr_profiles\last.txt` = the mode that ran last; log `akvr_mode_log.txt`. A 2D start
with no 2D copy of a file deletes the live one (the player never had it; the game remakes its defaults - the ini from
DefaultSystemSettings.ini) once VR's copy is saved. An install from before VRLAUNCH (no last.txt): the live files are
taken as VR's and the 2D copies are seeded from `vrmod_graphics_backup`.
- **Installer** (first install only): no longer edits the live ini/store. It copies both into `akvr_profiles\vr\` and
  applies GameGraphics/GameStore to those copies, writes `last.txt` = 2d, copies the launcher into the game folder and
  makes "Batman Arkham Knight (VR).lnk" on the Desktop and in Start menu Programs (cmd /c launcher, minimised, game icon).
- **Uninstaller**: removes the launcher and both shortcuts; graphics: last = 2d keeps the live files; last = vr puts the
  2D copies back (no 2D store copy: deletes the live store); otherwise the old vrmod_graphics_backup route.
- Practice-tested 2026-10-02 (scratchpad): mode switches 2D->VR, VR->2D incl. the delete branch; packaged installer + uninstaller round
  trip on a fake game folder (13/13 + rain shaders patched, shortcuts made and removed). JJ's real store hash unchanged.
- **JJ's game, 2026-10-02:** new dinput8.dll, launcher, Uninstall-AKVR.ps1 and both shortcuts deployed; `testtools=0`.
  His first start of either kind migrates (last = vr, 2D copies from vrmod_graphics_backup: fullscreen 2560x1440, 30 fps).
  Backup of the replaced files: `akvr/diagnostics/before-VRLAUNCH-20261002/`.
- Open: the fix's `nvapi64.dll` (geo-11's NVAPI wrapper) stays in the game folder and loads in 2D too - untested.

**TEST on JJ's game only, 2026-10-02 (THREADSYNC): `OneFrameThreadLag=True` -> `False`** in
`BmGame\Config\BmSystemSettings.ini` (line 28; one word changed, file otherwise byte-identical), and `posedelay=2` in
`akvr_settings.ini`. **Why:** JJ: "the rain and the HUD elements need to somehow be on the same frame timing as the 3D
elements" - with the lag on, the world is drawn one frame behind the camera the game used for the rain and the HUD
markers (pose delay 3 vs 2). Off = one frame for all. The risk is speed (2026-09-26: True measured 44 -> 84 fps under
geo-11, "much smoother"); the game is now held at 45 in the headset, so the cost may be smaller. NOT in the installer.
Rollback: `akvr/diagnostics/before-THREADSYNC-20261002/` (BmSystemSettings.ini + akvr_settings.ini), or set True and
posedelay=3.

---

## 5. RESOLVED 2026-09-29: these came from the fix's 2026-09-25 release (not AKVR)

JJ: "the fix was recently updated so that ambient occlusion shaders worked correctly". HelixMod update note of
2026-09-25 confirms it. A practice install of the CURRENT release matches the live game in both files below, so
nothing to script. Left over in JJ's live game from the old release: `ShaderFixes\0c8fb661657fcc9a-ps.txt/.bin` and
its `ShaderFixesDM` copies ("MANUALLY DUMPED [ShaderRegex\...UE3_BatmanAK.ini\_Regex2]", 2020) - absent from the new
release, and as a ShaderFixes file it overrides the new pattern file's correction for that shader. Not an AKVR file.

### (original notes, before the cause was known)

| File | What differs |
|---|---|
| `ShaderFixes\536b2b31d22e6bd7-cs.txt` / `.bin` and `ShaderFixesDM\536b2b31d22e6bd7-cs.txt` / `.bin` | A compute shader (tiled lighting) rebuilt "using 3Dmigoto v0.6.181 on Fri Sep 25 22:52:57 2026": `dcl_temps 13` becomes `33`, `CB12` moved, and stereo corrections `x += cb12[0].x * (cb12[0].y - z) / cb0[6].x` added at several light positions. Files dated 2026-09-26 06:26. Possibly from a newer fix release or an earlier session; no AKVR note mentions it. |
| `ShaderFixes\UE3_BatmanAK.ini` | Regex corrections in geo-11's Unreal 3 shader patterns: `.xyz` escaped as `\.xyz` (two lines), `temps = stereo tmp1` becomes `stereo tmp0 tmp1`, and an extra pattern section (`dp3 / rsq / mul` normalisation) with its replacement. Looks like the fix author's or geo-11's own update. |

2026-09-29 closer look: `ShaderFixes\536b2b31d22e6bd7-cs.txt` is the fix's "Ambient occlusion CS 1" (header: 3Dmigoto
1.3.16, 2020, same as the baseline); the live copy adds ~370 lines of per-light stereo corrections by hand (irregular
tab indentation). The `ShaderFixesDM` copy is geo-11's own "AUTOMATICALLY CONVERTED FROM SHADER FIXES" output, so it
regenerates from the ShaderFixes one. `UE3_BatmanAK.ini`: fixes unescaped `.xyz` in three patterns, `tmp0` added, and a
new hand-written specular-lighting stereo correction (`[ShaderRegex_SpecularPS3...]`, with a "//Useless comment." line).
Both look hand-made, not tool output. Only 2 source files to account for.
2026-09-29 search: these files are dated 2026-09-26 05:58 / 06:26, the morning of JJ's geo-11 0.6.182 -> 0.7.11
update (PLAYBOOK_REVIEW 2026-09-26; `Win64\_geo11_backup_before_update_swap_20260926\d3dx.ini.as-shipped-by-update`
= the baseline d3dx.ini + geo-11's help include + the NonSquareRT block). Not in `_disabled_mods_backup`, not in any
geo-11 folder under `E:\Games\# MODS\Geo-11`, no newer Arkham fix archive on the PC. Most likely from that update
download (a newer fix or geo-11 package). The installer does not reproduce them (the practice install keeps the
baseline versions). Needs JJ: where the 2026-09-26 update came from, or an in-game check of lights without them.

---

## Change log

| Date | Build | Files | Section |
|---|---|---|---|
| 2026-08-06 → 09-26 | GEO11 plan, HOOK mode, non-16:9 | d3dxdm.ini `direct_mode`, `shader_regex_patch_mode`; d3dx.ini `load_library_redirect`, `TextureOverrideAllNonSquareRT`; geo-11 renamed and updated to 0.7.11 | 2, 3, 4 |
| 2026-09-27 | HUDFIX (MENUMODE) | d3dxdm.ini `[Stereo]` HUD keys | 2 |
| 2026-09-27 | GSAHOLD (EXPOSURE) | d3dxdm.ini TinyRT / TinyUAV sections | 2 |
| 2026-09-28 | HUDDEPTH | 13 HUD vertex shaders in ShaderFixesDM | 1 |
| 2026-09-28 | HUDLAYER6 (HUDSPLIT) | 13 HUD vertex shaders: cb13 flat / scene-depth switch | 1b |
| 2026-09-28 | HUDLAYER6 fix | 1b corrected: position output per shader (first version crashed the driver; reverted, fixed, re-applied) | 1b |
| 2026-09-28 | RETFLAT | 2 HUD vertex shaders: depth search + top-strip test from the piece origin, compass band via cb13 | 1c |
| 2026-09-28 | RETSQUASH | 2 HUD vertex shaders: edge-squash correction for scene-depth pieces | 1d |
| 2026-09-28 | DIAG RB (temporary) | d3dx.ini hunting=1, F13 record, HUD dump_tex - remove with diag_rb.py off | 3b |
| 2026-09-28 | DEPTHALL | 11 HUD vertex shaders: copy the fix's own scene-depth decision into the split | 1e |
| 2026-09-29 | PANELTIDY / BAND28 (DLL only) | none - RETSQUASH (1d) now inert; compass band fixed at 28% by the mod | 1c, 1d |
| 2026-09-29 | installer | Install-AKVR.ps1 runs the patch script; NonSquareRT block on for Arkham; verified on practice folders | all |
| 2026-09-29 | installer (GAMEWORKS) | BmSystemSettings.ini GameWorks keys = 0; NVIDIA store: detail / 2x filtering / GameWorks off (first install) | 6 |
| 2026-09-29 | EDGEBAND | 13 HUD vertex shaders: top + bottom strips stay on the layer (CB13[2]) | 1f |
| 2026-09-29 | EDGEBAND undone | 13 HUD vertex shaders restored to the 1e state (doubled elements) | 1f |
| 2026-09-30 | PARTTAG | 7 HUD vertex shaders: marked parts never at scene depth | 1g |
| 2026-09-30 | PARTTAG3 | 1g corrected: first colour row (add), + mad layout; 9 shaders | 1g |
| 2026-09-30 | PARTTAG4 | 1g: mark in r/g/b, any channel -0.003..-0.0002 (tinted children) | 1g |
| 2026-09-30 | PARTTAG5 | 1g: mark also in alpha, any channel -0.02..-0.0002 (stacked / own-add pieces) | 1g |
| 2026-10-01 | NEARRAIN3 | world rain VS f50d1365: cb12 re-place / hide / head-offset modes (inert at nearrain=0) | 1h |
| 2026-10-01 | FARRAIN | rain CS a96594b1: regions kept ahead of the game camera (cb13) | 1i |
| 2026-10-01 | HUNTRAIN (temporary) | d3dx.ini hunting=2 + marking/analyse keys | 3c |
| 2026-10-01 | CLEANUP | d3dx.ini restored from before HUNTRAIN (hunting=0) | 3c |
| 2026-10-01 | BANDOFF (DLL only) | none - the 1c compass band is sent as 0 while hudroomall=1 | 1c |
| 2026-10-01 | RETNEAR | 2 HUD vertex shaders: a piece drawn far from its origin searches depth from its corner | 1j |
| 2026-10-02 | TARGETDEPTH | 13 HUD vertex shaders: pieces near an on-target point take its depth, as one piece (CB13[3]) | 1k |
| 2026-10-02 | RAINSTRETCH | world rain VS f50d1365: optional drop of the streak's per-frame camera term (cb12[2].w) | 1l |
| 2026-10-02 | TARGETMOVE | 13 HUD vertex shaders: on-target pieces moved by the head-turn offset (CB13[4]) | 1m |
| 2026-10-02 | RAINLAG (FRAMEPAIR) | world rain VS f50d1365: streak ends re-placed for the world's (one frame older) camera (CB12[9]) | 1o |
| 2026-10-02 | TARGETMULTI | 13 HUD vertex shaders: 8 on-target points (rows 1-2, 5-10), depth point through the layout map (CB13[11]) | 1p |
| 2026-10-02 | SCALE110 | d3dxdm.ini dm_convergence 2381.0 -> 2272.7 (JJ's world scale 1.1 = the new 1.00) | 2 |
| 2026-10-02 | THREADSYNC (mod) | BmSystemSettings OneFrameThreadLag forced False under geo-11 too (earlyres comfort list) | 6 |
| 2026-10-02 | TARGETSCALE | 13 HUD vertex shaders: on-target pieces mapped per vertex back to the movie's own layout (CB13[5]); replaces 1m's move | 1n |
| 2026-10-02 | VRLAUNCH (mod + installer) | VR via shortcut/flag/-akvr, Steam = 2D; BmSystemSettings.ini + GFX store swapped per mode (akvr_profiles); installer edits the VR copies only | 6 |
| 2026-10-02 | TARGETFACE | 13 HUD vertex shaders: on-target pieces laid on the plane facing the eye about their point (CB13[3] = tan half-angles, on) | 1q |
| 2026-10-02 | TARGETUP | 13 HUD vertex shaders: 1q block replaced, on-target pieces keep the world's up (CB13[11]) | 1r |
