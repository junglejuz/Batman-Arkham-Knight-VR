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

## HUNTRAIN (2026-10-01): shader hunt for the pause vignette and a head-locked rain layer

JJ on PAUSELOOK: "a good start"; wants the game's own "screen-sized black vignette-type overlay" in the pause gone
(only AKVR's whole-world darkening), and found "one particular layer" of rain attached to the head. No F2 from that
run. Temporary d3dx.ini edits on JJ's game (FIX_CHANGES 3c): hunting=2, marks dump asm only, frame dump on Scroll
Lock (log only), and a TEST with `[TextureOverrideRain1]` (eaa6f638 forced mono) commented out. Next: read JJ's
marked hashes (newest `*-ps.txt` in ShaderFixes / ShaderFixesDM, then delete them) or diff the two FrameAnalysis
logs (pause vs gameplay); skip the pause vignette with a ShaderOverride (gated to the pause if it is shared);
decide the rain fix; restore the hunting lines; add the kept changes to the patch script + FIX_CHANGES.

**HUNTRAIN results so far (2026-10-01):** panel section "FIND A SHADER (geo-11)" (SHADERFINDER/2) presses the hunting
keys by SendInput (rows: pixel 1/2/3, vertex 4/5/6, index buffer 7/8/9, vertex buffer / * -). JJ marked rain PS
**5d787946eda54077** (a particle streak shader): hiding it removes BOTH the world rain and the head-locked layer, so
the layer must be told apart by its vertex shader or buffers (likely a camera-attached emitter). geo-11 frame
analysis logs `hash=0000000000000007` for every shader (flags, not hashes: memory geo11-shader-hash-not-in-context),
so frame lists cannot identify shaders; the Scroll Lock button was removed. Marks dump asm into BOTH ShaderFixes and
ShaderFixesDM (`5d787946eda54077-ps.txt` so far): delete them at cleanup. Safety copies of both folders taken before
any VS marking: `diagnostics/before-HUNTRAIN-20261001/ShaderFixes*`.

**DRAWPROBE (2026-10-01, deployed, untested):** JJ: stepping the finder "took a lot of button presses". The mod now
names every VS and PS at creation on the game-facing device (new CreatePixelShader hook, slot 15; `sh_put` map), tracks
the VS/PS bound per game-side context (PSSetShader slot 9 hook + Det<0>::VSSet), and sorts every game draw using PS
`g_probePs` (5d787946eda54077, rain) into kinds = VS hash + PS t0/t1 SRV pointers. Panel (FIND A SHADER section):
one "hide" tick per kind (the draw is dropped); F2 status lists the kinds ("rain probe"). Session-only; once JJ finds
the head-locked kind, make it a fixed rule (by VS hash if that separates it; texture pointers do not persist).

**DRAWPROBE2 (2026-10-01, deployed, untested):** JJ's DRAWPROBE screenshot: ONE kind, VS f50d1365e929b3a0 (the
fix's ShaderFixesDM copy: instanced GPU particles from a structured buffer, placed through the view-projection), 6
indices = the WORLD rain. So PS 5d787946eda54077 is the world rain; the head-locked layer is a different draw (likely
a camera-attached rain mesh). Panel section is now "RAIN LAYERS" (the numpad step rows are gone; JJ won't use them):
kinds by PS+VS pair, tagged R (rain particles), T (binds one of the rain's PS textures, tracked by a new
PSSetShaderResources slot-8 hook), B/A (12 draws before/after the rain on the same context); a hide tick drops that
pair everywhere. F2 status lists them with hashes. All panel sections now start closed each launch (JJ: "close all
the HUD drawers").

**DRAWPROBE2 result (JJ):** he hid rows 2-14 — "didn't affect anything"; the face-locked rain HAS parallax (3D, not
screen depth). F2 status (akvr_20261001_*): two instanced rain systems, both 6 x 20480: row 1 PS 5d787946eda54077 /
VS f50d1365e929b3a0 (world rain, fix-patched VS) and row 11 PS 37313d9770da1c5e / VS 2aafb19df6567d30 (uses the
rain texture, drawn right after; NOT in the fix at all, only regex markers in ShaderCacheDM). Row 11 was among the
hidden ones. Open: does the hide tick work at all (never confirmed by JJ: ask him to tick row 1 alone and see if the
world rain goes)? SHADERDUMP (deployed): writes the ORIGINAL bytecode of those 3 shaders to
`Binaries\Win64\akvr_shader_<hash>.bin` at creation; disassemble with
`E:\Games\# MODS\Geo-11\geo-11+v0.6.90\cmd_Decompiler\cmd_Decompiler.exe -d` and read how row 11 places its streaks.

**DRAWPROBE3 (2026-10-01, deployed, untested):** JJ confirmed the hide tick WORKS (row 1 removed the world rain), so
rows 2-14 are not the stuck layer. SHADERDUMP read (asm in the scratchpad `shd/`): row 11's VS 2aafb19df6567d30 places
streaks at structured-buffer positions minus cb0[10] (camera position) through the ordinary view-projection =
world-fixed. The probe now also takes every see-through draw (blend enabled on RT0, tracked per context via the Blend
hook) as tag 16/"S"; 256 kinds. Panel RAIN LAYERS: "hide everything below except the world rain" (drops every tagged
non-rain draw), then groups by vertex shader with a hide each (hidden-VS list) and a tree of kinds. Next if the top box
does NOT remove the stuck layer: it is opaque or not drawn through the game-side context hooks.

**DRAWPROBE4 (2026-10-01, deployed, untested):** JJ: with "hide everything except the world rain" (all see-through
draws) the stuck layer STAYS. Blind spots found: the indirect draws (context slots 39/40) were never hooked, and compute
work was never looked at. Now hooked on the game side: 39/40 (tag 64 "I", recorded as kinds), Dispatch 41 /
DispatchIndirect 42 / CSSetShader 69, plus CreateComputeShader (device slot 18) for names. Panel: "hide EVERY draw
except the world rain" (drops every non-rain draw outside the HUD call), "hide all compute work", and a list of compute
shaders (the fix's rain CS a96594b16ceb399b / bc5c6aebf60c9308 labelled). F2 status lists the CS too.

**RAINPARTS (2026-10-01, deployed, untested):** DRAWPROBE4 results (JJ): hiding CS 92fb168506dc721a removed ALL rain —
it is a 2x2 downsample (8 a frame; the rain needs one of its images), not the simulation. "hide the world rain" removes
the stuck layer too, and with every OTHER draw hidden the stuck layer stays => the stuck streaks are INSTANCES of the
world rain draw (PS 5d787946eda54077 / VS f50d1365e929b3a0, 6 x 20480, positions from structured buffer t0 by
SV_InstanceID; VS offsets w>0 streak heads by cb0[12]). Test now: panel slider "world rain: eighths drawn" cuts the
instance count (only the tail can be cut: SV_InstanceID ignores StartInstanceLocation). If the stuck layer vanishes at
some step, it is an instance block: then cull that range in the fix VS (ShaderFixesDM edit + FIX_CHANGES + driver
test) or by count. If it thins evenly, the stuck streaks are interleaved: look at the buffer writer (a CS from the list).

**NEARRAIN (2026-10-01, deployed + fix step 1h applied, untested in the headset):** RAINPARTS2 (JJ): the stuck
streaks are the FIRST 2048 instances of the world rain draw (a few world drops among them). JJ wanted them hung in
space, not hidden. Fix step 1h (FIX_CHANGES 1h, `Patch-NearRain`) edits the fix's VS f50d1365e929b3a0: with cb12 bound
(mode 1) each of the first cb12[1].w streaks is re-expressed from the head-turned camera (derived in-shader from its own
view-projection cb0[6..9]) into the game camera's axes (AKVR sends them: camera.cpp `akvr_camera_base_axes`, the
stub's saved base rotator, UE3 FRotationMatrix convention); mode 2 hides them. The mod binds cb12 only around that draw
(hudsplit.cpp `rain_cb_pre/post`). Panel RAIN LAYERS "rain close to you" (setting `nearrain`, default 1). Known
limit: the base rotator is the latest finalize's, so during STICK turns it may lead the drawn frame by one frame
(slight swim of the near rain); head turns use the draw's own matrix, exact. If the block still follows the head in
mode 1, check the F2 "near rain:" line (binds / without axes) and the sign conventions first.
**NEARRAIN2 (deployed + applied, untested):** JJ on v1: head TRANSLATION fixed, but head ROTATION still turns the
block. Suspect: the game places the block with an older camera than the draw's. The mod now records (base, drawn)
rotators every Present (camera.cpp `akvr_camera_record_rotators`, ring of 16) and, with "camera frames back" N > 0
(setting `nearrainlag`), sends that frame's pair in cb12[0..5]; the shader (step 1h v2) uses it instead of its own
view-projection. JJ to find the N where the block stops turning; if none does, the block is not a rigid camera
rotation (e.g. wrapped in a camera-aligned box) -> recommend "hidden".
**NEARHIDE (built; deploys when JJ closes the game):** JJ on NEARRAIN2: no "frames back" value works; the block turns
"as if there's another camera orbiting it" - not a rigid rotation about the camera (maybe placed around the orbit
pivot / Batman). Default is now mode 2 = hidden (the first 2048 streaks off screen; ~90% of the rain stays); "hang in
the room" is labelled an experiment. Reopening it needs RE of how the game fills the first 2048 entries of the rain
buffer (the simulation CS or CPU upload), not more shader maths.
**RAINWRITER (2026-10-01, deployed, needs one F2 in the rain):** JJ: "I don't want them turned off. I want them fixed
so they hang in space." Reading of NEARRAIN2's failure: the streaks are world-placed; the REGION they are wrapped into
follows the (head-turned) view, so turning the streaks orbits them. The fix belongs in whatever moves the rain. This
build watches the rain VS's t0 buffer (+ CopyResource/CopySubresourceRegion sources), hooks CSSetUnorderedAccessViews
(68) / CSSetConstantBuffers (71), records each writer CS with its cb contents (snapshots via the Map/Unmap/
UpdateSubresource hooks), keeps every CS's bytecode from creation and saves the writer's as
`Binaries\Win64\akvr_shader_<hash>.bin`; F2 status "rain writer:" lists it all with the camera position/axes for
matching. Next: disassemble the writer, find the wrap region (box centre/size, from camera position + view direction?)
and make it use the game camera (not the head) - via cb from the mod around that dispatch, like cb12 for the draw.
**NEARRAIN3 (2026-10-01 late, deployed + applied, JJ tests tomorrow):** JJ: originally the block followed head
POSITION only; mode 1 added rotation ("mostly hanging in space" otherwise). Mode 3 "follow the game camera" (default,
set in JJ's ini with `nearrainlag=1`): the shader subtracts the head's camera position offset (stub dPos, recorded per
Present in the camera ring) from the first 2048 streaks, no rotation. If lean still drags the block, try frames back
0/2/3; if it swims only while moving the stick, that is the game's own look. RAINWRITER stays in the build (an F2 in
the rain still records the buffer writer, if a deeper fix is needed).
HUNTRAIN d3dx.ini lines (hunting=2 etc.) are still on JJ's game: restore `diagnostics/before-HUNTRAIN-20261001/d3dx.ini`
once the rain is settled; the finder-mark dumps (5d78...-ps) were deleted.

## PAUSELOOK + PAUSEDIM (2026-09-30, deployed to JJ's game only; JJ: "a good start")

JJ: pause as in Sekiro — "the whole 360 world to freeze, darken, and then add the pause screen menus over the top",
"not fully darken to black. just darker". Evidence it can work: the mode trace through a pause (SCREENTAN capture,
t 237-243 s) shows ms_since_finalize climbing (camera stopped) but proj_hits 2 per frame all along: the renderer
keeps building the main view. Freecam already proves the renderer reads the camera view fields (+0x574..+0x58C).
- xr.cpp: `g_pauseLive` starts within 1 s of gameplay ending if the frame stays 3D, menu phase >= 2, not F6, and
  camera.cpp `akvr_camera_main_view_live()` (main-band projection at the camera's own FOV in the last 0.3 s, none
  at another FOV for 0.6 s — the map's own camera should fail this and keep the old window). While live: shown as
  gameplay (effGameplay), HUD layer in menu mode (whole UI, no split) at the PAUSE size, anchored ahead of the head
  at pause start (anchor restored after), black view-space dim quad between world and HUD (`pausedim` 0.5).
- camera.cpp `pause_look_write()` (end of akvr_head_update): after 2 Presents with no finalize, writes rotator =
  stub-saved base + head delta, position = (fields at pause start - dPos then) + dPos now.
- Panel: "pause: world stays around you, menu on top" (`pauselook`), "pause: darken the world %" (`pausedim`).
  F2 status line "pause look: ..." (LIVE?, main view check, head writes count).
- Unknowns for the headset: does the paused render follow our writes (if not: the frozen picture swims with the
  head — untick and look for the renderer's own copy, as SKVR run 90); does the pause menu draw a full-screen
  dimmer (SKVR MENUDIM); is the map correctly left as a window; pose delay during pause.

## RELEASED 2026-09-30: build ROOTKIDS packaged (JJ: "really happy with this, package it up")

GitHub release v0.1.0 now holds the ROOTKIDS package (zip sha256 FE507AFE…FC75, downloaded back and matched).
`install/files/akvr_settings.ini` = JJ's live file byte for byte, INCLUDING `hudcontainers=` this time (PACKAGING_GUIDE
said drop it, but the world ticks and JJ's moves are keyed to those ids). NOT tested: that the fingerprints match on a
fresh install; if a player's reticle floats in the room, that is the first suspect. d3dxdm.ini and the game graphics
values matched the shipped ones; fix-patch status on JJ's game: all 13 shaders patched + split. README updated (HUD in
the room, zoom vignette, screen sliders). The mod's code defaults still lag the settings file (upgrade installs keep
their old file, so new keys fall back to code defaults: loadscreen 0.6, loadup 0, screen 0.5).

## ROOTKIDS + KEEPGLOW3 (2026-09-30, JJ confirmed: objective in the room, compass glows) — supersede KEEPGLOW/KEEPGLOW2

JJ on KEEPGLOW2: the launch objective is STILL on the head. Code-read finding: in a movie with no world part the
ROOMALL rule marked only the ROOT (autoRoom for p < 0), and apply_layers skips depth 0, so NOTHING in such a movie
was ever marked. Most of its text is flat anyway (layer); only pieces the fix gives scene depth, like the objective,
stayed in the head-locked picture. ROOTKIDS: roots are never auto-marked; their children are the top branches.
KEEPGLOW3: the mark overwrites all four adds again (the WORLDKIDS behaviour), and the glow is kept by never marking a
part under an already marked part (`s_markAbove` in apply_layers); the two compass glow pieces sit under the ticked
compass. If the objective still sticks: F2 while it is on screen and look for its lines in hudlayers.txt (MARK?).

## KEEPGLOW2 (2026-09-30, deployed, untested)

JJ on KEEPGLOW: the launch objective went back to the head. Likely cause (code-read, no F2): its text is coloured by
its own add and it fades through the alpha add, so keeping a non-zero alpha add left the mark in no channel it
inherits. Alpha now ALWAYS takes the mark (as before KEEPGLOW); only R, G, B keep the game's non-zero add. If the
compass glow darkens again, the glow pieces use alpha add too: then F2 there and read their add row in hudcb.txt.

## KEEPGLOW (2026-09-30, deployed, untested)

JJ: happy with WORLDKIDS + loading screens. Left: two compass pieces "should be a glow but look like a darkening
effect" = `K2/…2.0.6` and `.2.0.7` (mirrored end pieces, JJ's own "hang in the room" ticks). Cause: mark_rgb
OVERWROTE the node's colour add row, and Flash glows/brightness are add, so only the dimming multiply was left. Now
the mark goes only into add channels that are 0 (or already the mark); alpha only if 0 or no other channel took it.
`has_mark()` (any channel) replaces the old blue-only check. Every auto-marked branch was losing its adds too, so
other elements may look brighter now.

## WORLDKIDS + LOADALL + LOADUP (2026-09-30, JJ: "really happy with that") — supersedes the MARKCARRY diagnosis below

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
