# AKVR handoff — 2026-09-30 (end of session)

Previous handoff: `diagnostics/CLAUDE_HANDOFF_2026-09-29_archived.md` (installer / uninstaller / GSACOMFORT work, and a
build-by-build log of this session's HUD work). Every change to the 3D fix's files: [FIX_CHANGES.md](FIX_CHANGES.md).

## Rules (all still apply)

- **Talking to JJ:** plain language, short replies, no code or jargon. Lead with what JJ will see. JJ is a graphics
  professional, not a coder. Never propose SteamVR or changing the OpenXR runtime.
- **JJ prefers that you set HUD part ticks for him** in `akvr_settings.ini` (game CLOSED) rather than make him dig
  through the panel. **Keep every existing `hudlayers` entry** when editing: parse and rewrite, never regenerate. JJ's
  own moves at the time of writing: `K1/0` y -0.135 (size 0.8), `K9/0` y -0.135, `K3/0` and `K5/0` y -0.1.
  Memory: `preserve-hud-part-settings`.
- **Every change to a file we did not write** (geo-11, the fix, the game's config) goes into FIX_CHANGES.md in the
  same step. Fix-shader edits: patch a COPY with `tools/AKVR-fix-patches.ps1` (copy the script with the BatmanAK
  process check removed to test while the game runs), assemble with geo-11 0.6.90 `cmd_Decompiler -a`, load each with
  `tools/shader_driver_test/build/Release/vstest.exe`, and check `dcl_temps` covers the registers used. Only then apply
  to the game with the real script, and compare the texts byte for byte with the tested copy.
- **Read scripted edits back.** This session a Python edit left a `//` comment mid-line and silently commented out a
  buffer's bind flags. That doubled the whole HUD for hours (see "SWITCHFIX" below). Heredocs in the Bash tool eat
  backslashes: write edit scripts with the Write tool, and use `chr(92)` for `\n` inside C string literals.
- **Build and deploy:** `cmake --build akvr/build-dinput8 --config Release`. Game CLOSED: back up `dinput8.dll` +
  `akvr_settings.ini` to `diagnostics/before-<BUILD>-<date>/`, run `deploy.bat` (through PowerShell), compare hashes.
  Bump the build tag in hooks.cpp (`"   build: <TAG> "`). Check `tasklist | grep -i batman` first: JJ often has
  the game running.
- **Packaging:** read `../PACKAGING_GUIDE.md`; package only with `python akvr/tools/make_package.py`; upload with
  `gh release upload v0.1.0 <zip> -R junglejuz/akvr --clobber`, then download it back and compare hashes.
- Commit messages: write to a file and `git commit -F` (PowerShell mangles quotes). JJ's Documents is `D:\Documents`.

## Where things stand

**JJ's game** (`E:\Games\Steam\steamapps\common\Batman Arkham Knight`) runs build **LOADSIZE** (commit cdd674f) with
fix step 1g (PARTTAG5) applied. Everything since HUDWORLD is **deployed to JJ's game only**. The GitHub release
v0.1.0 still holds the 2026-09-29 package (GameWorks + game-finder + picture-sharpness installer, mod build
GSACOMFORT). Do not re-package until the HUD work below has been confirmed in the headset.

**Confirmed by JJ in the headset this session:** menu head-pose delay 2 fixes the menu jiggle (MENUDELAY). The zoom
vignette works. The compass hangs in the room as one piece after SWITCHFIX + PARTTAG.

**Installed but NOT yet confirmed** (the last few builds, in order): PARTTAG5, SCREENTAN, SPLASHSIZE, ZOOMHIDE,
ROOMALL, ROOMALL2, WHOLEFRAME, LOADSIZE.

## How the HUD works now (read before touching it)

1. **HUD layer** (`hudsplit.cpp`, `xr.cpp`): each Scaleform draw in the game's HUD call is issued twice with a switch
   in cb13. Pass 1 (`cb13.x = 1`) goes into our HUD image and keeps flat pieces. Pass 2 (`cb13.x = 2`) goes into the game
   picture and keeps scene-depth pieces. The split tail in the 13 fix HUD shaders drops the other kind. The HUD image
   is a quad **fixed in the room** (g_hudSpace 2, anchored at the last recenter). "attach UI to head movement"
   (`hudattach`) puts it back in view space.
2. **Which pieces are "scene depth":** the fix's own per-piece decision (texture tags 2/42/22/32, regions). It is far
   broader than real world markers, so with a room-fixed layer it tore elements apart.
3. **Part marks (PARTTAG, fix step 1g):** the mod writes add = -1/512 into R, G, B **and A** of a Scaleform part's
   colour transform (TreeNode data +0x60..+0x6C via GetWritableData(node, 2) at 0x1411c96f0). A guard hook on the
   game's SetCxform (0x1411eedf0) puts the mark back after the game rewrites it. In 9 of the 13 HUD shaders, a piece
   whose colour **add row** has any channel in -0.02..-0.0002 skips the fix's depth decision (flat, layer only).
   The add row is the FIRST of each colour pair (measured). The other 4 shaders (3b819a7e, 599bd060, e12863b9,
   7d8fcdc2) never see the colour transform; PSMARK checks their PS cb0 but has never found a mark there.
4. **Whole HUD in the room (ROOMALL, `hudroomall=1`, JJ's choice):** every HUD movie on screen (except
   FrontendBGFader) is read into a part tree. Every maximal branch without a **"stays on its target"** part (8th
   field in `hudlayers`) is marked. Saved world keys protect their branch even before the part exists. JJ's world
   parts: `K2/0.0.0.0.1.0.0.0` (reticle) and `K2/0.0.0.0.1.0.0.1` (target distance). The tree is read at 0.5 s and 3 s
   into every gameplay stretch.
5. **Container ids** (`K1`..`K16`, `hudcontainers=` fingerprints) key every saved part. ModularHud containers claim
   their ids first; other movies take free slots; fingerprints refresh only from >= 80% complete trees.
   **Any change to discovery or cont_assign risks moving JJ's saved settings.**
6. **Diagnostics on F2:** `<capture>hudlayer.bmp` (the layer image), `hudmarks.csv` (per HUD draw: shader, split,
   mark hits in VS/PS cb0), `hudcb.txt` (every HUD draw's target, counts and full constants). The panel parts list
   shows "marks set / kept after the game's own colour writes / found in place / missing / guard", and
   "[in the room]" on auto-marked branches.

## Screens (flat 2D) now

- SCREENTAN: sizes keep the screen's width and scale tangents uniformly (they used to scale angles, so 16:9 showed
  1.58:1 at 70%).
- The screen kind is latched when the screen appears. Start-up logos: `splashscreen` 0.55. The loading screen into
  the game (menu phase 1, not live): `loadscreen` 0.6. Both show the WHOLE frame (WHOLEFRAME; the game draws them much
  taller than 16:9, and the 16:9 slice cut their bottom). Pause/map/in-game loading: `pausescreen` 0.7 with the 16:9
  slice (PAUSE169). Main menu: `screen` 0.75 (JJ). Gameplay is always full size.
- Insert/Delete menu-size keys were REMOVED: GetAsyncKeyState is system-wide, so JJ's typing elsewhere drifted the
  size.
- Main menu: head-pose delay `menuposedelay` 2 (JJ confirmed), the tipped roll-free camera (MENUTIPPED, so the
  top-down title shot sits ahead), and the whole UI on the layer with no split (MENUONE / MENUONE2), sized by `screen`.

## Zoom (right stick click)

Our FOV lock cancels the game's zoom magnification. ZOOMVIG draws our own vignette (a view-space quad) while the
game's own FOV (camera.cpp `g_gameFov`) is below `vigbelow` 45. JJ's settings: strength 1.0, clear centre 28. The
game's 2D zoom overlay is the same part as the gameplay tips (`K1/0.0.0.0.2`): it is set to "hide while zoomed"
(ZOOMHIDE, 7th field), and it is also "hang in the room".

## WORLDKIDS + LOADALL + LOADUP (2026-09-30, deployed, untested) — supersedes the MARKCARRY diagnosis below

JJ on MARKCARRY: reticle + distance still hang in space at first; on the first grapple the reticle "split in half"
(half on target, half in space), then fixed itself. **Real cause (code-read, fits the split):** the ROOMALL rule
marks every branch with no world part whose parent holds one, so every CHILD of a world part was auto-marked. The
reticle part `K2/0.0.0.0.1.0.0.0` has 4 children (two rotated halves). WORLDKIDS: nothing inside a world part is
marked. MARKCARRY (kept) makes the old marks come off after the next tree read. The startup log stops recording long
before gameplay (log_add caps at 120 hits), so the per-part check moved to F2 `hudlayers.txt`: each line now ends with
ON-TARGET / ROOM / MARK / MARK(not ours) as read from the node.

LOADALL: an in-game screen (menu phase 2) that runs FLAT is latched as a loading screen: `loadscreen` size and the
whole frame, not the pause size and 16:9 slice. Pause/map stay 3D in the mode log. If the map turns out flat, it will
take the loading size too. LOADUP: `loadup=` degrees (+ up), panel "loading screens up / down", an off-axis shift of
the frustum in tangent space. Panel labels: "pause / map size %", "loading screens size %".

## MARKCARRY (2026-09-30, deployed, untested)

JJ on LOADSIZE: the reticle and target distance sit at HUD depth "until you actually hit R1 (grapple)", then sit on
their targets. Found: a re-read of the HUD tree dropped `taggedByUs`, so a mark set by an earlier read (likely the
0.5 s pass on a half-built HUD) stayed on its node for good, and the colour guard kept re-applying it. Now the mark
state is carried by node, the guard list is pruned to the new tree, and every discovery logs, per "stays on its
target" part, any marked part above it (`hud-layers: world part ...: <key>(ours|NOT ours)`). If JJ still sees it:
read those lines in the startup log first. A "(NOT ours)" mark means the game itself holds a value in the mark range.

JJ also said the loading screens "aren't any smaller and the sliders don't work". Code path looks right for the
start-up logos (`splashscreen`) and the loading screen after Continue (`loadscreen`); in-game loads (fast travel,
death) still use `pausescreen`. Asked JJ which screens. Lead: the mode log shows loading screens run "flat" and
pause/map run "3D", so `g_anamorphic` may separate in-game loads from pause (open issue 4).

## NEXT: what JJ should check (ask for F2 on anything wrong)

1. Reticle and target distance sit on their targets from the first second of gameplay (ROOMALL2).
2. The launch objective text and the small arrow by Local Surveillance hang in the room (all screen movies are in
   the tree now).
3. No doubled or face-locked HUD pieces anywhere. Any world marker that now floats in the room (enemy indicators,
   grapple points, in-world objective markers?): find its part from an F2 (`hudlayers.txt` + `hudmarks.csv` /
   `hudcb.txt`; hide-diff two F2s if needed) and set its 8th field to 1.
4. JJ's moved HUD elements are still where he put them (container ids unchanged).
5. Start-up logos: the whole logo and legal text visible, not stretched, a comfortable size. Loading screen into the
   game at 60%. Gameplay entry at full size.
6. Game zoom overlay hidden while zoomed; tips visible otherwise.
7. Compass pieces `K2/…2.0.4`, `.0.5`, `.0.8` are still HIDDEN (JJ hid them as duplicates before the fixes). Offer to
   unhide them and re-check.

## Open issues, in priority order

1. The checks above.
2. **Release readiness of the HUD work:** world-marker ticks and JJ's moves are keyed to container ids fingerprinted
   on JJ's PC. A fresh install has no "stays on its target" parts, so the reticle and distance would float in the
   room. Before packaging: ship the fingerprints and world keys (a `hudcontainers=` + `hudlayers=` default in
   `install/files/akvr_settings.ini`) and test that fingerprints match on a fresh run. Also add the new keys
   (`hudroomall`, `menuposedelay`, `splashscreen`, `loadscreen`, `vig*`) to that tested settings file.
3. The brief face-locked HUD when entering gameplay (much shorter now): probably frames already in flight, drawn
   before the layer gate opened.
4. In-game loading screens (fast travel, death) are indistinguishable from pause: 16:9 slice, pause size.
5. The first-menu Batman flicker ("flickers for quite a while before it settles"): not investigated; needs an F2
   during it.
6. The mod's built-in defaults are still not the tested setup (~25 keys). A missing key falls back to a wrong default.
7. Earlier items: VR session refused after other VR games until reboot (parked); Sekiro/DS3 installers lack HUD
   edits and an uninstaller; geo-11 is bundled (3Dmigoto GPL; credits in the release notes); OpenVR not needed.

## Key facts learned this session

- **Scaleform colour transform in the render tree:** node data +0x50 = multiply RGBA, +0x60 = add RGBA. Change code
  2 in GetWritableData. The final add = parent multiply x child add + parent add, so marks on a parent reach every
  descendant and stack when a child is marked too. The game uploads each piece's colour pair **add first**, then
  multiply (cb0[12]/[13], batched cb0[14+6i], ...).
- The game's SetAlpha (0x1411efe60) keeps the node's add row. SetCxform (0x1411eedf0) overwrites all 8 floats.
- The HUD is one RenderUI call per frame (26-32 draws), plus two small render-to-texture draws (32x32, 96x128).
- F2 layer + picture captures showing the SAME piece in both places = the split is dead (check the cb13 buffer).
- The 4 no-colour HUD shaders get their colour elsewhere; no mark ever reached their PS cb0 rows 0-1.
- Flat screens must scale TANGENTS, not angles (SKVR's SCREENTAN lesson applied here too).
- geo-11 stamps each rebuilt `.bin` with its `.txt`'s time; matching times = loaded. Restoring with `cp -p` keeps an
  old `.bin` valid.
- Memory updated: `ak-hud-steadiness` (whole HUD story), `preserve-hud-part-settings` (new).
