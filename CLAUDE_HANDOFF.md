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
**FARRAIN (2026-10-01 evening, deployed + fix step 1i applied, untested):** JJ on NEARRAIN3: still turns with the
head. His F2 (RAINWRITER) named the writer: the fix's CS a96594b16ceb399b, cb0[11] = camera forward x 512 incl. the head.
It pushes streaks 1024..2047 (a distant layer) 2 x cb0[11] further along the view = the swinging block. Step 1i swaps
cb0[11] for the game camera's forward x |cb0[11]| (cb13 from the mod around that dispatch) in all three uses. Panel
"rain stays in the world when you turn your head" (`farrain`, default 1); the draw-side modes are off (`nearrain=0`).
If the far layer still swims only during STICK turns, that is the 1-frame base-rotator lead (try "frames back").
**CAPTUREDIR:** F2 now writes akvr_captures\<date_time>\ (status.txt, *.csv, frame/katanga/hudlayer bmp, radar\);
shader dumps go to akvr_captures\shaders\; traces' working copies to akvr_captures\. 382 old capture items (583 MB,
incl. the FrameAnalysis folder) moved to akvr_captures\old\. JJ: keep the game folder clean.
**PARTSHUT:** the HUD section and "move / resize single HUD parts" no longer open themselves (JJ, while on the rain).
**JJ on FARRAIN: "I think he's fixed it."** (rain stays in the world under head turns.)

**HUDEYES + PAUSENOBACK + MENUFIRST (2026-10-01 late, deployed, untested):**
- HUDEYES — applies **VR_HUD_GUIDE.md section 1** ("HUD depth: per-eye quads, never depth baked into the pictures";
  SKVR HUDEYES3, skvr/SEKIRO_PLAN.md runs 107-108). `hud_eye_pair()` used to move the two quads by +-g_hudHalfIpd along
  the quad's own right axis, i.e. fixed to the direction faced at the last recentre (the HUD is room-fixed, 4 m), so a
  turned + tilted head gave an up/down difference between the eyes at the HUD edges. Now: eye positions from
  `xrLocateViews` (LOCAL space, display time, position-valid only) into `g_eyeLocPos[2]`; each eye's quad = position -
  (eye - midpoint). Only for a LOCAL-space quad (attach-to-head = view space keeps the old, exact shift). Panel "HUD depth
  follows your eyes" (`hudeyefollow`, default on; off = old method). The measured eye distance is logged once in the mode
  timeline ("HUD eyes NN.N mm", expect ~63) and shown on the "headset HUD layer" status line with the method in use. HUD
  picture flat: JJ's F2s of 2026-10-01 show "HUD distance: LIVE, shift 0.00000" (geo-11 HUD shift 0 in the layer), so
  depth comes only from the quad pair. Test with JJ: far-left / far-right HUD pieces with the head turned and tilted
  should converge.
- PAUSENOBACK — JJ: "remove the gradient dark HUD layer and darken the actual world instead". Pause F2 (akvr_captures\
  20261001_173020_454): layer 46.8% covered, 46.7% see-through = the pause menu's black gradient backing on the room layer.
  The part tree is capped (2040, PauseMenu not in it) and the draw list is ambiguous, so the HUD colour conversion has a
  second pixel shader used only while the pause view is live: pixels with alpha < 0.98 and brightest (un-premultiplied)
  channel < 0.12 are dropped. PAUSEDIM's view-wide dim stays the darkening. Panel "pause: leave out the menu's dark
  background" (`pausenoback`, default on).
- MENUFIRST — JJ: Batman at the very bottom for "a second or two" coming into the main menu. Main-menu F2 (20261001_
  172932_977): the camera ran from 12.72 s (raw verdict 1) but the smoothed verdict came at 13.23 s, so the first 3D menu
  frames were shown as the flat start-up screen. In menu phase 0 a 3D frame (g_anamorphic) or a running camera (3
  finalize changes within 0.5 s, newest < 150 ms) now counts as gameplay for the display only (phase detection unchanged).
  The camera trace itself showed nothing jumping (base pitch 0.3 deg throughout).
**PANELTIDY2 + PAUSEFAST + PAUSENOBACK2 + PAUSESIZE (2026-10-01 late, deployed, untested):** JJ on HUDEYES: "much better
when it's on" (confirmed) - switch now advanced-only. Pause: the black backing still flashed (pause view waited ~0.2 s
for the smoothed verdict) -> pause view now starts and ends on the camera itself (3 Presents without a finalize /
finalize ticking again). The pause colour pass drops dark pixels at ANY alpha (the top-left corner piece) and lifts dim
pixels so the brightest channel reaches 0.6 ("PAUSE MENU" title). The pause menu has its own size (`pausemenusize`,
default 0.5, panel "pause menu size %"); "pause / map size" is now "map size". Panel: settled / test controls shown only
with `advancedpanel=1` (code + ini keys unchanged): decouple pitch + stick height, extra view top/bottom, each-eye view,
HUD on its own layer, HUD depth follows your eyes, HUD layer settings, menus use full height, whole HUD in the room,
vignette threshold, RAIN LAYERS (tests), floating-screen head tracking + shape, pause view / dark-background switches,
pause/map/loading shape, live 3D main menu, flat floating screens, main-menu pose delay, Diagnostics. Visible: status,
Recenter, Capture; VIEW (world scale, extra view at the sides, picture height, where you stand); HUD (size, up/down,
distance, attach to head, HUD parts); ZOOM VIGNETTE (strength, clear centre, preview); MENUS AND SCREENS (float now,
floating/main menu/map/pause menu sizes, pause darken, start-up/loading sizes, loading up/down); FRAME RATE (hold at,
between frames, pose delay); Keyboard shortcuts. **README's panel tables need the same edit before the next release.**
**CAMRUN + UNPAUSE + MENUSTART (2026-10-01 late, deployed, untested):** JJ: leaving the pause showed "the entire thing
framed in a small window before it pops back out to the full 360" (PAUSEFAST ended the pause on the first camera tick,
gameplay came back only with the slower verdict) -> the pause holds until the camera is clearly running (3 finalize
changes within 0.5 s, newest < 150 ms = CAMRUN), and CAMRUN && 3D now counts as gameplay for the display everywhere.
JJ still sees "the view of Batman from the top at the bottom of the screen" entering the main menu (MENUFIRST did not
cure it; his earlier F2 trace showed the hooked camera level from its first tick, so the top view is drawn before the
hook or by another camera) -> MENUSTART: when the 3D menu first comes up, hooks.cpp saves the katanga picture 10x over
3 s plus display state (shots.txt) and the camera / mode traces into akvr_captures\menustart_<time>\. Read those next.
(JJ's message about dialogue-choice backdrops / subtitles was about Sekiro, not AKVR - left for SKVR.)
**RECENTERTIME + UNPAUSE2 + MENUFLICKER (2026-10-01 night, deployed, untested):**
- The top-down Batman entering the main menu = the CAMERA HEIGHT, not a game shot: MENUSTART (menustart_20261001_182258)
  showed delta_z 118 (lean_y 1.157 m = head height above the tracking origin) until 6.07 s, when the auto-recenter finally
  fired - it waited for 120 FRAMES of valid position, and the start-up / menu-load screens present few frames. Now 1 s
  by the clock (hooks.cpp). The pictures: Batman's head and shoulders from above at the bottom of each eye.
- UNPAUSE2: the pause regressed to the window - CAMRUN's camTicking still counted the pre-pause ticks (newest < 150 ms)
  and ended the pause on its next frame. It ends only on a NEW tick (s_pStill == 0) with the camera running.
- HUDEYES eye distance measured: 63.6 mm (mode timeline "HUD eyes 63.6 mm").
- MENUFLICKER: JJ "Batman flickers for the first ten to twenty seconds of being in the menu" - MENUSTART now also takes
  back-to-back frame pairs at 5 / 8 / 12 / 16 / 20 s (shot10..19). Compare each pair's eyes next.
**RECENTERTIME result (menustart_20261001_182853, build RECENTERTIME):** camera height offset within -2.1..+0.9 units from
the menu's first finalize (was 118) - the top-down Batman is gone in the data; Batman centred in the shots.
**MENUFLICKER analysis (same capture, scratchpad pairdiff.py):** back-to-back frame pairs at 5/8/12/16/20 s: both eyes
change by the same amount (so not an eye-order problem); ~25% of Batman's lit pixels change by >12 (max ~250), and the
difference picture puts them on the suit's specular highlights (ears, shoulders, rims) while the menu's intro camera
glides in for ~20 s. Reads as highlight shimmer during the camera move, which ends when it stops - matching JJ's
"first ten to twenty seconds". Not yet known whether JJ means that sparkle or the whole figure juddering: asked.
**RESIZESTOP (2026-10-01, deployed, untested) - the real MENUFLICKER cause.** JJ: "It's the entire rendering. It flickers
on and off for the first 10 to 20 seconds ... worse when you're moving your head more." The mode trace's frame gaps: from
7 s on, EXACTLY two ~114 ms stalls per second (73 frames/s otherwise). That is `resync_client_size()` (hooks.cpp): the
window client is stuck at 2560x1440 (start-up log: "CLAMPED, does not match", wanted 2888x2860), so it retried a
SetWindowPos every 500 ms for 40 tries = 20 s, every one refused, and each made the game re-run its display-change
handling (EnumDisplayMonitors / GetMonitorInfoW / GetClientRect repeated in the start-up log) = a stall with the
picture dropping out. Now it stops after two refusals in a row (start-up log line "window resize refused twice").
The render size comes from the ResX/ResY lever, so nothing depended on the resize. Check after JJ's run: mode trace has
no 114 ms gaps, and the HUD / render size are unchanged. Backup: diagnostics/before-RESIZESTOP-20261001.
JJ on RESIZESTOP: "Okay, that's better."
**CLEANUP (2026-10-01, deployed, untested).** JJ: "check that there's no other processes running during the gameplay,
such as analyzing HUD elements, trying to force different things ... old redundant stuff from earlier tests. Even
captures running in the background. Clean it all up." One switch, `testtools=1` in akvr_settings.ini (or the box at
the top of the advanced "RAIN LAYERS (tests)" section), now gates every leftover test recorder; OFF by default:
- DRAWPROBE rain probe: per-draw shader lookups with locks on EVERY game draw (probe_skip), blend/vertex/texture
  tracking, indirect-draw sorting. Pixel/vertex shader tracking also stays on if nearrain != 0 or the rain-parts cut is set.
- RAINWRITER: compute UAV/constant-buffer tracking, a malloc'd copy of every compute shader's bytecode.
- SHADERDUMP: 3 shader .bin files written to akvr_captures\shaders at every launch.
- MARKREC: ~1.3 KB copied per HUD draw every frame (F2 hudmarks.csv is empty without test tools).
- HUD-004 coverage: the HUD layer read back to the CPU every 90 frames (LAYERSHOT on F2 still works).
- observe_rts (dead scene tap): GetResource/QueryInterface/GetDesc on every OMSetRenderTargets.
- RADARREC: a 1024 px copy every frame into 90 textures (~380 MB video memory); freed when the switch goes off.
- frame_early.bmp at 6 s and the MENUSTART 20-shot capture at every launch.
- present probe pixel read-back every 30 frames (its CPU timing ring, frames.csv, stays).
Also: FRAMEID's memory scan (3 x up to 64 MB copies ~3 s into play) runs only with poseauto=1 (0 on JJ's game; it only
fed that option); the retired HUDPROBE tick is no longer called; NEARRAIN binds nothing in mode 0; geo-11 `d3dx.ini`
restored from before HUNTRAIN (`hunting=0`; FIX_CHANGES 3c). Kept on purpose: FARRAIN (needs the compute-shader
name per dispatch), PSMARK constant-buffer shadows (HUD routing), the camera/mode/frame timing rings (cheap; F2 needs
them), the start-up log (3 writes). Backup: diagnostics/before-CLEANUP-20261001 (dll, settings, HUNTRAIN d3dx.ini).
Check on JJ's run: HUD in the room as before (compass marks, reticle on target), rain hangs in space (FARRAIN), F2
still writes its folder.
**BANDOFF + RETNEAR (2026-10-01, deployed, untested).** JJ (4 F2s, 19:55): (1) the grapple reticle "splits into its
various elements when it reaches the edge of the field of view"; (2) the target-distance number "floats around a
general area ... doesn't stay locked onto anything". Findings from hudlayers.txt: in all 4 captures the reticle part
(`K2/0.0.0.0.1.0.0.0`, 4-5 concentric kids + one at +1525 twips) sat at the TOP of the HUD (y -6835..-8997 of a 7680
half-height). RETSQUASH is OFF since PANELTIDY (g_squashWant=false), so not the edge scaling. The live suspect is the
28% top band (1c): pieces whose origin is above it go flat (to the layer); with ROOMALL every other part is marked
anyway, so the band only caught the on-target parts. BANDOFF (hudsplit.cpp layer_begin): band sent as 0 while
hudroomall=1. The distance part (`K2/0.0.0.0.1.0.0.1`) is at the stage centre with an identity matrix and the number
is drawn away from it (capture 1: "51m" below centre while the reticle is at the top), so RETFLAT's origin search gave
it the scene depth at the middle of the view. RETNEAR (fix step 1j, 2 shaders): vertices farther than 0.15-0.25 clip
from their origin search from their own corner. ASSUMPTION: the number is drawn by 9938094a / 05154232 - MARKREC came
back EMPTY in all 4 captures (hudmarks.csv "frame (0)", no rows: mark_record never fills, cause unknown; now also
needs testtools=1). If the number still floats, fix the recorder and capture with test tools on to find its shader.
Also note: the katanga.bmp in F2 is only 962x476 - too small to see the reticle; a full-res crop would help next time.
JJ on BANDOFF + RETNEAR (2026-10-02): "The grapple points seem okay" (CONFIRMED). The distance target "is not okay
still ... a combination of both sticking to its target and sticking to my face"; asked: it "appears wherever the world
object is that it's pointing to, but it moves around when you move your head" and "should be at the depth of what's
behind it, what it's pointing to". RETNEAR's premise was WRONG: the widget is `K2/0.0.0.0.1.0.0.1` -> `.0` (moves; at
the 51m in F2 #1) with ~20 pieces (digits, mirrored bracket halves at +-604 twips, lines), each at its own origin.
**TARGETDEPTH (2026-10-02, deployed, untested):** earlyres `akvr_hud_target_points` walks each top-level "stays on its
target" part down the LIVE tree while a node has one child (widget -> 813; reticle stays 803), then up through the
parent matrices (node+0x20) to the movie root (root matrix = stage twips -> game-target pixels) -> clip. Hand-check on
F2 #1: widget 0.530/0.667 vs "51m" at 0.530/0.651; reticle 0.501/0.243 vs 0.52/0.248. hudsplit layer_begin sends them
in cb13 rows 1/2 (buffers now 48 bytes; radius 0.12; only with hudroomall=1) every frame; fix step 1k (all 13 shaders):
pieces whose origin is within the radius take that point's depth (one rigid shift) and follow depth (gameplay, not
room-marked). Status line: "target points N: (x, y) (x, y)" in the HUD layer diag. RISK: the 4 no-colour shaders
cannot see room marks, so a room part drawn by them within 0.12 of the reticle (compass at the top?) is pulled into the
3D picture while the reticle is near it - watch for compass pieces doubling/flicking near the reticle.
RAINCHECK: the rain section shows without advancedpanel again ("RAIN TEST"); ticking either hide box turns on the
test tools (JJ: the option "is gone in the overlay").
JJ on TARGETDEPTH (2026-10-02): the distance target "is still not locked in world space on the object ... still
moving around with the head". So depth was not (only) it: the widget's 2D position likely comes from the game's
camera WITHOUT AKVR's head turn, or one frame late. Not guessed this time - **TARGETTRACE** (test tools only): one
row per HUD frame (on-target clip points, camera yaw/pitch WITH the head, the game's own yaw, base forward, camera
position, finalize count) -> F2 `targets.csv`. Analysis plan: with the stick still and the head turning, if the widget
point does not move with (cam yaw - base yaw) -> the HUD ignores the head (fix: shift on-target pieces by the head
rotation reprojection in the shaders, cb13 rows have room); if it moves one row late -> lag.
Rain: JJ "whilst it's good, it's still hanging in world space", but isolated in the pause it "jitters a bit ... kind
of follows a little bit" on head turns. **RAINSTRETCH** (fix step 1l, A/B switch in RAIN TEST, default off): drops
the rain VS's `cb0[12] * 0.5` head-of-streak term (looks like per-frame camera motion). Hypothesis only.
JJ's F2 20261002_002826 was taken at 00:28 on the TARGETDEPTH build (TARGETTRACE deployed 00:34), so no targets.csv
yet - MARKREC DID record this time (hudmarks.csv / hudcb.txt filled; with testtools=1). Rain, JJ: isolated it is
"more solid", then "slowly moving with the head ... laggy ... following slowly a little bit", jitters, then "goes back
to hanging in space solidly"; confused by the options. Note: "show only the rain" removes almost all GPU work, so the
frame rate / pipeline depth changes and the pose pairing (posedelay) can be off for a while - judge rain in the normal
picture too. **RAINTIDY (deployed):** RAIN TEST now has 4 explained boxes (show only the rain, FARRAIN keep ON, the
stretch TEST, test tools); the rain-hunt controls are folded under advancedpanel=1.
JJ 2026-10-02 (later): RAINSTRETCH "doesn't seem to do anything" (hypothesis rejected); rain no longer flickers but
"very gently following head movements. A small amount. It's not rock solid." Reticle AND distance widget "still
following head movement". No F2 with targets.csv was taken. **TARGETMOVE (fix step 1m, deployed, untested):** assumes
the HUD uses the game camera without the head turn; each on-target point is re-projected through the drawn camera
(camera pair k Presents back, k = pose delay - 1 by default) and the pieces near it are moved by that offset in the
shaders. Panel HUD: on/off "reticle and distance marker stay on their target when you turn your head" + "marker
timing". If ON is clearly worse (markers swing twice as far), the hypothesis is wrong - then F2 with test tools on
gives targets.csv (points, offsets, cam yaw with head, base yaw) to decide. Rain open: candidates - the rain sim's
region uses the camera POSITION incl. AKVR's head position offset (dPos), or FARRAIN uses the newest base axes while
the CS draws an older frame (lag): try FARRAIN with `nearrainlag` 1-3 / compare.
JJ on TARGETMOVE: before, moving the head up/down carried the markers along ("if you moved your head up, they would
come up"); with the move "it does the opposite and more exaggerated". => the HUD DOES include the head turn, only
late. **TARGETMOVE2 (DLL only, deployed, untested):** the source camera is now the DRAWN camera (with head) of
`markerhudlag` Presents earlier (default 1, panel slider "how far the markers trail your head", 0 = no correction);
zero offset when the head is still. Still no targets.csv capture to confirm the lag size.
JJ on TARGETMOVE2: "when my head is still yes the marker sits still but it doesn't mean they're at the location that
the game has put them. That new setting appears to do nothing at all." **TARGETSTOCK (DLL only, deployed, untested) -
the real cause:** ROOTSHRINK draws the HUD movie in the ~60% HUD box, and the game places world markers in STAGE space
for the full screen, so every marker is pulled toward the box centre (head up -> target drops, marker drops only 60%:
"follows the head"; TARGETMOVE undid the whole head turn: "opposite and more exaggerated"). `akvr_hud_target_points`
now also returns, per point, (movie's own matrix view+0x110 applied to the stage point) - (drawn point); hudsplit sends
that as cb13 row 3 (fix step 1m moves the pieces + depth sample). Hand-check on F2 00:28: reticle drawn 0.356, stock
0.586 clip x (x1.65 = 1/0.6). Lag sliders removed; panel box "reticle and distance marker sit on their target".
NOTE: all other world markers marked for the room (enemy/objective icons in other movies) are pulled inward the same
way - if JJ wants them on target too, they need the same treatment.
Rain in the PAUSE (frozen): isolated it is "rock solid for the first couple of seconds, and then it slowly starts to
drift with your head". Theory: "show only the rain" removes the GPU load, the frame rate jumps, the fixed pose delay
pairs frames with the wrong head pose -> the picture trails the head. Ask: does the normal (not isolated) pause view
drift? F2 during the drift (test tools on) -> frames.csv / present.csv show frame times and the pose delay used.
JJ on TARGETSTOCK: "I think we're making an improvement", but the markers are "jittering when you move your head";
rain "still drifting"; and a black pause entering the main menu with its audio already playing. **MENUBLACK:**
frames.csv (F2 20261002_013133) - OUR Present hook took 2197 ms on that frame (106 ms on the one before): MENUSTART's
synchronous 66 MB picture grabs, active because testtools=1. The menu/start-up pictures now need `menucapture=1`
(settings file only). The September 2.07 s gap (menustart_20261001_182853) was the same thing. **TARGETSCALE (fix step
1n + DLL):** the per-frame tree read raced the game's next layout -> jitter; now each on-target vertex is mapped in the
shader through the fixed affine (movie's own matrix vs AKVR's root): cb13 row 4. Markers return at the game's size.
targets.csv from that F2 had n=0 (menu), so the jitter cause is reasoned, not measured.
**JJ FOUND THE RAIN/MARKER CAUSE (2026-10-02):** "Head pose delay of 2 makes the rain go rock solid and stable, and
it makes the [reticle] and the distance HUD marker go rock solid ... the world becomes jittery." = UE3's one-frame thread
lag: the world uses the camera one frame older than the rain draw and the HUD markers. OneFrameThreadLag=False would
align them but costs 44 vs 84 fps (earlyres.cpp note), so **FRAMEPAIR (fix step 1o + DLL, deployed, untested):** the
rain VS re-places each streak end for the world's camera (cb12 rows 6-8 = M | t, row 4 .w on); the markers get the
one-frame turn shift in cb13 row 4 .zw. Camera ring now stores the drawn position (`akvr_camera_pose_ago`). Panel HUD:
"rain and markers match the world's frame" + "timing" slider (`frameworld`, world camera k back, default 1 - a reasoned
guess; JJ may need 2). posedelay set back to 3 in JJ's settings. The earlier FARRAIN fix stays. Likely the same
one-frame offset hits other game-thread-placed effects (particles, world markers) - check if JJ reports them.
JJ on FRAMEPAIR: rain "I don't think that fixed" (can't tell: "the pause menu is not pausing the rain anymore" -
unexplained, the rain simulation was not touched; ask whether it falls in the pause with the box OFF); markers
"definitely not fixed. They still jitter when moving the head". **FRAMEPAIR2 (DLL only, deployed, untested):** no more
game-camera ring guess - it reproduces JJ's test: xr `akvr_xr_pose_pair(a, b)` gives the head quats the next submit
picks at the world's delay a (posedelay, 3) and the rain/markers' delay b (`framedelay`, default 2). Rd = R(qa)^-1
R(qb) in OpenXR view space: markers - their point re-projected by Rd (shift in cb13 row 4 .zw); rain - M = A Q A^T,
Q = C^T Rd C (UE3 camera axes A of the drawn camera, ring index 0), t = P - M P, into cb12 rows 6-8 (fix step 1o
unchanged). Head rotation only (no head-translation difference). Panel slider renamed "head pose delay for the rain
and markers". Old `frameworld` key ignored.
JJ on FRAMEPAIR2: "World space locked. HUD elements are still jittering when moving [the head]." The rain switch "makes it
continue to rain when the pause screen is on. When I turn that off, the rain pauses as well" - cause unknown (positions
are plain UE3 world units ~4000/11000/2400; no d3dx.ini rule on the rain shader). **RAINSPLIT (DLL only, built,
deployed by a watcher when the game closes):** rain correction has its own switch in RAIN TEST (`rainframefix`), the
markers keep `framefix` in HUD; readout "rain frame fix: last turn X deg (... 3 m away moves Y units), N draws, M while
paused" in the HUD layer diag / panel; targets.csv now has frame_shift_x/y. Next: F2 in the pause with the rain fix on
(is the turn tiny while the head is still?) and F2 in gameplay with markers (is frame_shift non-zero, does its sign
oppose the marker motion?). Cheap sign test for JJ: delay slider 4 = the same correction reversed.
JJ: "Clearly, the rain and the HUD elements need to somehow be on the same frame timing as the 3D elements." **THREADSYNC
TEST (2026-10-02, set on JJ's game, untested):** BmSystemSettings.ini `OneFrameThreadLag=False` + `posedelay=2` (with
framedelay 2 the FRAMEPAIR corrections are no-ops). Judge: world, rain and markers all steady at delay 2? Frame rate /
smoothness vs before (F2 frames.csv intervals). If too slow: back to True + posedelay 3 (backup before-THREADSYNC).
**THREADSYNC RESULT (2026-10-02):** JJ: "the grappling hook and the distance marker look perfectly stable now. And locked
to their targets, which is amazing. But after playing for a very short time, the game crashes." Two UE3 "Fatal error!"
crashes after the 02:18 switch (02:20:27 and 02:27:18; none all day with True). Dump (BmGame\Logs\unreal-v10246-
2026.10.02-02.25.38.dmp, read with python `minidump`): EXCEPTION_ACCESS_VIOLATION reading 0xffffffffffffffff at
BatmanAK+0xF1E3B6 (base 0x7FF6169B0000 = log setter 0x7FF617B7DC70 - 0x11CDC70), on a worker thread whose loop is
+0xFA592B/+0xFA5AD3/+0xFA7F20 (the same loop as ~6 idle sibling threads); the first crash ended in geo11.dll from the same
worker loop. NO dinput8 frame on either crashed stack. Hypothesis: with the threads unstaggered, something racing on the
worker threads (deferred contexts) - our worker-thread hooks grew a lot since Codex ran False under geo-11 on 09-25
(test tools' DRAWPROBE/RAINWRITER, PS tracking for the rain options). **TEST:** testtools removed, rainnostretch=0,
rainframefix=0 (framefix already 0), still False + posedelay 2. If it still crashes -> back to True + posedelay 3 and
find another way to align the rain/HUD (FRAMEPAIR2 did not fix the markers; JJ never ran the slider-4 sign test).
Ghidra MCP was not running (127.0.0.1:8080 refused) - function names for the crash offsets unknown.
**2026-10-02 late (deployed, untested):** JJ: no crash with test tools + rain options off -> THREADSYNC kept (mod forces
OneFrameThreadLag=False under geo-11, posedelay default 2; test-tools box warns it can crash). Panel: rain stretch, rain
frame fix, marker frame fix + delay slider REMOVED (defaults off). SCALE110: world scale rebased (old 1.1 = new 1.00;
d3dxdm convergence 2272.7; slider now also saves 1 s after a change). MARKFIRST: until the first HUD-part read of a
gameplay stretch has landed its marks (0.5 s + 200 ms), every HUD draw goes whole to the room layer (JJ: HUD pieces
"attached to the face and then quickly snap out"). TARGETMULTI (fix step 1p): 8 points; a part with 6+ children is a
list, one point per visible child; JJ's hudlayers now has K4/0.0.0.0.1.0.0.0 on target (74 world markers; the Batmobile
marker JJ saw moving "opposite" to the head was .15 of it). Open: Batmobile enter/exit camera snap (the game's own fast
camera swing) - offered a comfort fade, not built.
Lesson (cost two rebuilds): in PowerShell `@('a', 'b' + "x")` is an ARRAY of 3 ('a','b','x'), the comma binds
before `+`; scripted replacements built that way silently lost lines. Use the Edit tool or parenthesise.
HUNTRAIN d3dx.ini lines: already restored (build CLEANUP, FIX_CHANGES 3c); checked again 2026-10-02 - JJ's d3dx.ini
equals `diagnostics/before-HUNTRAIN-20261001/d3dx.ini`.
**VRLAUNCH (2026-10-02, build VRLAUNCH, deployed, untested in the game):** JJ: "update the package now ... The installer
also needs to create a way for the game to be launched in VR, leaving launching it from Steam to just play normally in
2D mode." The mod decides in DllMain (src/vrmode.cpp): VR only via the "Batman Arkham Knight (VR)" shortcut
(`AKVR-Launch-VR.bat` -> fresh `akvr_launch_vr.flag` + `steam://rungameid/208650`) or `-akvr`; a Steam start runs
nothing of AKVR. BmSystemSettings.ini + the NvGsa store are swapped per mode via `akvr_profiles\{2d,vr}` + `last.txt`
(log `akvr_mode_log.txt`). Installer: VR graphics go into the VR copy only; makes the launcher + shortcuts. Uninstaller:
removes them, restores 2D by last mode. Details + practice tests: FIX_CHANGES section 6 "VRLAUNCH". README rewritten
(VR shortcut, panel tables = the panel with advancedpanel off, pose delay 2). Package rebuilt (`dist/`, NOT uploaded:
JJ decides releases). On JJ's game: new DLL, launcher, Uninstall-AKVR.ps1, both shortcuts, `testtools=0`; backup
`diagnostics/before-VRLAUNCH-20261002/`. Ask JJ to check: (1) the shortcut starts VR (first start migrates: log line
"first start since VR/2D launching"); (2) a Steam start is plain 2D fullscreen 2560x1440 at HIS settings (30 fps cap from
his pre-mod backup); (3) back to VR keeps his VR settings. Untested: the fix's nvapi64.dll in 2D.

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
**TARGETFACE (2026-10-02, build TARGETFACE + fix step 1q, deployed, untested in the headset):** JJ: the reticle and the
distance marker "turn on their y-axis as you turn your head". Cause: flat stickers on a flat picture that turns with the
head - off-centre they are seen on a slant (45 deg: 50% width, 71% height). Step 1q lays each on-target piece on the plane
facing the eye, about its target point (cb13 row 3 = game-frame tan half-angles + on; panel "... and face you when you
turn your head", key `markerface`, default 1). Practice copy 13/13 + rerun clean, driver 13/13, applied to JJ's game
(identical to tested). Also fixed: a repair re-run of the patch script reported 1m "not patched" (exit 2) on an already
patched set. Package rebuilt with it (not uploaded). Ask JJ: markers keep their shape and stop turning when off to the
side? Compare with the box off. Rollback: `diagnostics/before-TARGETFACE-20261002/`.
**TARGETANCHOR (2026-10-02, DLL only, deployed, untested):** JJ on TARGETFACE: reticles hold their orientation but
"sometimes appear to separate their elements a little" at the view edges; the distance marker does not face him and "is
now flying off its fixed point in object space when turning the head". F2 x5 (18:34): the widget's point was (0.000,
0.080) = screen centre in EVERY capture - the part K2/...1 has two children now, so the one-child walk stopped at the
part, which never moves; the game moves K2/...1.1 (translations differ in each capture). Its "115m" label was drawn at
its SHRUNK-box spot = not taken as on-target, except near the screen centre (in and out as the head turns). Now: each
single on-target part's point = the node the game MOVES (earlyres part_anchor: the part if its translation changed in
the last 3 s, else the one child whose subtree moves; 2+ moving children = their parent; remembered while still).
Single parts' points are sent LAST (rows win over list markers: "last match wins"). Radius 0.12 -> 0.15 (the reticle's RB
prompt sits 0.105 from its point). F2 status now lists "all points". Ask JJ: distance marker on its object, facing him,
label in the right place; reticle elements together at the edges. Rollback DLL: `diagnostics/before-TARGETANCHOR-20261002/`.
**TARGETANCHOR2 + TARGETUP (2026-10-02, build TARGETUP + fix step 1r, deployed, untested):** JJ: on foot the distance
marker is better but "still rotating on the z-axis when rolling your head"; in the Batmobile it still turns and "flying
off to different positions". F2 18:51 (Batmobile): the widget part held TWO moving children (.1.0 and .1.1, two
objectives) and "2+ moving children = parent" fell back to the still part -> point (0.000, 0.080) again. Now every moving
child is its own point (up to 4 per part, near-duplicates < 0.05 merged); a node counts as moving for 30 s after its last
move (was 3 s). TARGETUP (fix 1r) replaces 1q's block: the facing plane's up = the world's up (cb13 row 11 from the drawn
camera, one Present back), so head roll no longer rolls the markers. cb13 is now 12 rows / 192 bytes. Rollback:
`diagnostics/before-TARGETUP-20261002/`. Ask JJ: roll the head (marker stays upright), Batmobile (marker on its object).
**TARGETUP2 (2026-10-02, DLL only, deployed, untested):** JJ: rolling the head, the reticle and the distance marker are
"kind of trying to roll but then trying to stay upright at the same time". TARGETUP's world-up came from the camera ring
1 Present back; the ring is written at Present after the head update for the NEXT frame, so it trailed the drawn frame
and the correction lagged the roll. Now read from the live camera fields at HUD draw (camera.cpp akvr_camera_live_axes).
Panel slider "upright timing" (key `markeruplag`, 0 = live, k = ring k-1; 2 = the old build) for an A/B if it still
fights. Unproven assumption: the live fields still hold the drawn frame's camera when the HUD draws (OneFrameThreadLag
False). Rollback: `diagnostics/before-TARGETUP2-20261002/`.
**LISTMOVING + MENUSIDE (2026-10-02, build MENUSIDE, DLL only, deployed, untested):** JJ: TARGETUP2 "much better"; left:
"a small narrow band within the top half of the field of view" where a marker "loses its lock on and kind of moves a
little". F2 x7 (21:27): 6-7 of the 8 points identical every capture at y 0.1-0.2 - K4/0.0.0.0.1.0.0.0 then held 30 STILL
pieces of a centred, symmetric HUD element (not the 74 world markers it held before: container content changes), and
pieces of the real markers near them took their point. List children now count only if they moved (node_moved_recently,
earlyres). MENUSIDE: JJ wants the main-menu camera moved left so Batman sits right of the menu items. camera.cpp slides
the camera along its right vector by `menuside` (default -80 UU) while the main menu is detected; panel "main menu
camera: left / right" in MENUS AND SCREENS; README row. Rollback: `diagnostics/before-MENUSIDE-20261002/`.
**MENUTEXT + ANCHORLOST (2026-10-02, build ANCHORLOST, DLL only, deployed, untested):** JJ on MENUSIDE: "when you move
your head right the camera moves towards Batman in a weird motion", the menu text needs to go further left, and "Batman's
camera needs to be fixed to how it was before". The slide rode the head-turned right vector; now the base camera's level
right and default 0 (JJ's live menuside set to 0.0). MENUTEXT: the live main menu's text box (hud_rect, sized by the main
menu size) slides left by `menutextleft` x width (default 0.10; panel "main menu text: move left"). ANCHORLOST: the
distance marker lost its fix at "a certain position" moving the head up and down: in the Batmobile the still reticle part
fell back to a fixed point (0.000, 0.215), and the 199m marker (widget child .1.1) passing within 0.05 of it was dropped as
a duplicate and took the reticle's spot and depth. Now: no point for a part with nothing moving; remembered anchors only
if they moved in the last 2 min; duplicates merged within one part only. Rollback: `diagnostics/before-ANCHORLOST-20261002/`.
**TARGETDIST + MENUSIDE3 (2026-10-02, build TARGETDIST + fix step 1s, deployed, untested):** JJ: ANCHORLOST "much
better", but the distance marker "took on the depth of the Batmobile" passing in front of it (the fix searches the depth
behind the point). earlyres `dist_update` triangulates each anchor node's object from the camera rays (camera position +
live axes + game tan + layout map), sends the view depth in the point row's .w; fix step 1s replaces the search result with
sep * (1 - conv / depth). Panel box "... at their object's own distance" (`markerdist`). F2 status: "marker distance: N
units (baseline B, R rays)" (last point updated). MENUSIDE3: JJ - MENUTEXT left the menu "exactly how it was before";
"just shift the camera over a little bit to the left without breaking anything" - menuside -80 again on the fixed (base
camera) direction; the text slider removed (menutextleft 0). Start-up stutter (JJ: very stuttery on the first run after a
reboot or other games, better on later runs): the game is on E:, a SATA HDD (ST8000VN004) with 33 GB free - cold UE3
streaming from a spinning disk; later runs hit the Windows file cache. NVIDIA DXCache is 88 GB (not the limit). geo-11
also rebuilds ShaderCacheDM after every fix-shader edit (one stuttery first run per update). Advised: Steam "Move install
folder" to L: (P3 Plus NVMe, 95 GB free; game 63 GB), then repoint the two VR shortcuts. Rollback:
`diagnostics/before-TARGETDIST-20261002/`.
**HUDWAIT + MENUSIDE4 (2026-10-02, build HUDWAIT, DLL only, deployed, untested):** JJ: TARGETDIST "much better"; on
entering gameplay "the distance marker is in the middle of the screen and not locked onto its target, and then it quickly
jumps to the target" ~0.5 s later = the MARKFIRST wait, when the whole HUD went to the room layer at its shrunk spots. Now
the HUD is not drawn at all during that wait (hudsplit hud_wait_skip, capped at 1.5 s so it can never stay hidden), and a
node counts as moving at 5 units (was 20) so markers get their point within a frame or two. Menu: JJ - the menu camera no
longer "wigs out", but at -80 "Batman is way off to the right"; -35 now (live + package). He also said the menu "looks fine
when coming back to the menu from gameplay": the slide applies only to the launch-order main menu (g_menuPhase 1), so the
menu after gameplay has no slide. Rollback: `diagnostics/before-HUDWAIT-20261002/`.
