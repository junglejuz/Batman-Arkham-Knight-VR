# AKVR handoff — 2026-09-27 (end of session)

## Start here

**RULE (JJ 2026-09-28): every change to geo-11, the fix, or any other third-party file is documented in
[FIX_CHANGES.md](FIX_CHANGES.md) in the same step. Another agent writes the install scripts from it.**

Batman: Arkham Knight VR mod (AKVR). Quest 3 through Virtual Desktop / VDXR, true
stereo from geo-11 (HOOK mode), AKVR loaded as `dinput8.dll`. Read the root
`AGENTS.md`, this file, then the newest sections of `PLAYBOOK_REVIEW.md` (ROOTPITCH
onwards). This file supersedes `RESUME.md`'s older paragraphs and the archived
handoff (`diagnostics/CLAUDE_HANDOFF_2026-09-26_archived.md`).

- Workspace: `D:/Documents/Antigravity/VR_Modding` (not a Git repository).
- Game: `E:/Games/Steam/steamapps/common/Batman Arkham Knight/Binaries/Win64`.
- Build: `cmake --build akvr/build-dinput8 --config Release --target akvr`. The
  post-build copy does NOT run; install by hand with the game CLOSED, back up the
  current DLL + `akvr_settings.ini` to `akvr/diagnostics/before-<BUILD>-<date>/` first,
  then confirm the build tag is in the installed DLL (`grep -c "build: <TAG>"`).
- Tests: `ctest --test-dir akvr/build-proxy-test -C Release` (3 tests).
- Never SteamVR; never change the OpenXR runtime. Plain language to the user, short.
- Patch files with a Python script WRITTEN TO A FILE (Write tool). Heredoc patches in
  Git Bash collapse `\\n` and break C string escapes (happened twice this session).

## Installed now

**RULE (2026-09-28): every fix-shader edit is assembled and loaded into the NVIDIA driver by
`tools/shader_driver_test` before it reaches the game (a bad edit killed the launch).** HUDSPLIT was re-applied
after that test (position output per shader).

Build **VSID2b** (2026-09-28, untested): split pairs game HUD shaders with geo-11's replacements (VSID found 0/13);
slider width 42%. DIAG RB REMOVED (RB icon = font glyph in the shared text cache, cannot be tagged alone).

Previous: Build **RETSQUASH** (2026-09-28, untested) = VSID + edge-squash correction (FIX_CHANGES 1d). VSID: split limited to the 13 patched HUD shaders (TARGET DETAIL panel was
doubled), font 0.85, band 30. **DIAG RB is ON in d3dx.ini** (hunting=1, F13 frame record of HUD draws): after
JJ presses 'record the HUD for geo-11', read FrameAnalysis-*, find the RB icon hash, tag it via the fix-patch
script, then `python akvr/tools/diag_rb.py off`.

Previous: Build **RETFLAT2** (2026-09-28 03:10, untested) = RETFLAT + adapter/window logging. The 'not entering VR' at
03:01-03:08 hit the previous build and the Sekiro mod too (PC-side); user rebooted. If it recurs: read the
session line (game GPU LUID vs headset LUID) and 'window at' in akvr_startup_log.txt.

Previous: Build **RETFLAT** (2026-09-28, untested) + fix edit 1c: one scene depth per reticle piece (tilt test) and a
compass band (the split made compass pieces flicker; confirmed by the user). Panel: HUD layer settings ->
compass band %. Frame-rate choice always shown (6c).

Previous: Build **HUDLAYER6b** (2026-09-28) + fix edit HUDSPLIT (FIX_CHANGES 1b, applied): HUD layer with the
reticle kept at scene depth (each HUD draw issued twice with a cb13 switch), eye shift + lazy 0.3 deg default
(user-confirmed), panel sections in closed drawers. After the next launch check the 13 HUD .bin files came back
(geo-11 assembled the edit). PLAYBOOK_REVIEW "HUDLAYER5 result -> HUDLAYER6".

Previous: Build **HUDLAYER5** (2026-09-28): plain-blend-only coverage (compass dark patch), "distance fix: add
the eye difference" option (VD may draw quads without parallax), "lazy follow" option. User: layer only
slightly steadier (repeat-frame mode), not a deal breaker. PLAYBOOK_REVIEW "HUDLAYER4 result -> HUDLAYER5".

Previous: Build **HUDLAYER4** (2026-09-28): HUDLAYER3 LIVE but not steadier (JJ on VD SSW mode), lost distance,
dark see-through pieces. HUDLAYER4: room-space placement per frame, colour conversion, additive blend fix.
Test in "repeat the frame" mode. PLAYBOOK_REVIEW "HUDLAYER3 result -> HUDLAYER4".

Previous: Build **HUDLAYER3** (2026-09-28): HUDLAYER2 found the real textures (+0x140, +0x190) but rejected them
(single-slice test); HUDLAYER3 accepts arrays, copies slice 0.

Previous: Build **HUDLAYER2** (2026-09-28): HUDLAYER turned itself off (geo-11 wraps textures);
HUDLAYER2 finds the real texture inside geo-11's stand-in. Panel line lists what it found.

Previous: Build **HUDLAYER** (2026-09-28): the HUD drawn into our own image and shown as a head-locked
quad (checkbox "HUD on its own layer (steady HUD)", default on). Check the two panel lines under it
(LIVE, coverage %, headset HUD layer frames sent). Next: RB icon depth (fix texture tag) and keeping the
depth-following reticle out of the layer. PLAYBOOK_REVIEW "HUDSPLIT3 result -> HUDLAYER".

Previous: Build **HUDSPLIT3** (2026-09-28): HUDSPLIT2 saw no HUD drawing on real contexts at all; HUDSPLIT3
also watches geo-11's game-facing context and counts draws per layer per frame. Ask for F2 in gameplay
(optionally a second F2 with the hide test ticked). PLAYBOOK_REVIEW "HUDSPLIT2 result -> HUDSPLIT3".

Previous: Build **HUDSPLIT2** (2026-09-28): HUDSPLIT's hide test CONFIRMED (whole HUD vanished) but the
immediate context saw 0 HUD binds/draws; HUDSPLIT2 also watches deferred contexts + command lists. Ask
for F2 in gameplay; read <stamp>_hudsplit.csv. PLAYBOOK_REVIEW "HUDSPLIT result -> HUDSPLIT2".

Previous: Build **HUDSPLIT** (2026-09-28): HUD distance CONFIRMED working by the user (HUDDEPTH), range now
0-20 m. HUD jitter = step 1 of the HUD layer: a PROBE on the one function that draws every Flash movie
(0x1405de350) + draw counters, and a never-saved test switch "TEST: hide the whole HUD". Ask the user to
tick it in gameplay (does the WHOLE HUD vanish?), untick, then press F2 in gameplay; read
<stamp>_hudsplit.csv. Details + step-2 plan: PLAYBOOK_REVIEW "HUDSPLIT". Rollback before-HUDSPLIT-20260928/.

Build **HUDDEPTH** (2026-09-28, untested) + fix-pack script `tools/AKVR-fix-patches.ps1` (APPLIED to the game):
HUD distance now through the fix's 13 HUD shaders (geo-11's HUD value was ignored by every shader). Steadiness
parked (slider removed). HUD jitter = 45 Hz picture shown twice at 90 Hz; real fix = HUD on its own layer
(backlog item 6) - the NEXT project. RULE: fix-pack changes only via install-time script (user).

Previous: Build **HUDSTEADY2** (2026-09-28): steadiness pairs with the NEWEST pose (HUDSTEADY lagged without
damping = stale pairing); HUD distance range 0-5 m (20 m looked like far away). Rollback before-HUDSTEADY2-20260928/.

Previous: Build **HUDSTEADY** (2026-09-28): HUD distance link fixed (hooks geo-11's per-frame update for the
wrapper), new "HUD steadiness" slider (HUD follows a smoothed head direction, 0 = off), slimmer F2. Pause stutter
CONFIRMED fixed. E: drive is nearly full (~470 MB). See PLAYBOOK_REVIEW "HUDLIVE result -> HUDSTEADY".
Rollback `diagnostics/before-HUDSTEADY-20260928/` (= HUDLIVE).

Previous: Build **HUDLIVE** (2026-09-27): (1) PAUSESTUTTER - the 1-2 s freeze ~3 s after pause/map was the
automatic HUD-part re-discovery's name search (VirtualQuery on ~1M pointers); the automatic pass now skips it.
(2) HUD distance slider back, now live through geo-11's stereo object (obj+0x874, see PLAYBOOK_REVIEW
PAUSESTUTTER + HUDLIVE). Check the HUD status line says LIVE. Rollback `diagnostics/before-HUDLIVE-20260927/` (= TIDY4).

Previous: Build **TIDY4** (overlay reworked like SKVR; HUD distance removed - geo-11 fixes its HUD shift at startup,
one value for all HUD parts). GSAHOLD (graphics menu) + shared exposure CONFIRMED. Eye view ON; gameplay HUD
(compass) still doubled in the eye view - next: live HUD shift via geo-11's stereo object (PLAYBOOK_REVIEW TIDY4).
Rollback: `diagnostics/before-MENURES-20260927/`, `before-PAUSE169-20260927/`, `before-EVGAME-20260927/` (= VRSEP). Older: `diagnostics/before-VRSEP-20260927/` (= EYEVIEW). Older: `diagnostics/before-EYEVIEW-20260927/` (= RANGES: world scale 0.25-2.00, separation 0-50).
Older: `diagnostics/before-RANGES-20260927/` (= GEOSEP). Older: `diagnostics/before-GEOSEP-20260927/` (= ONESTEP). Older: `diagnostics/before-ONESTEP-20260927/` (= SLIDERFAST, user: "a bit better"). Older: `diagnostics/before-SLIDERFAST-20260927/` (= GEOLIVE: world scale LIVE, user confirmed it works).
Older rollback: `diagnostics/before-GEOLIVE-20260927/` (= ORBITFIX, user confirmed "so much better").
ORBITFIX: ORBITFIX: stick-orbit jitter on
Batman/Batmobile = our rotation write was one game frame stale; see PLAYBOOK_REVIEW ORBITFIX.
OFXR parked on the user's request; camera is the focus.

Ranked next steps for all three games: `../VR_IMPROVEMENT_BACKLOG.md`. `sbsswapchain=0` since 2026-09-27 (OFXR parked; STR-020 seam).

Key settings (akvr_settings.ini): `pitchunlink=2` (camera follows the game's tilt,
UEVR-default), `stickheight=2.00`, `sbsswapchain=1`, `fpslock=0` (user turned the lock
off), `posedelay=3`, `projvr=0`, `swapeyes=1`, `engineres=3120` (render 3560x3120/eye),
`hudscale=600`, `hudraise=40`, `hudrootmode=1`, `pausescreen=0.800`, `menu3d=1`,
`hudlayers=;K3/0:...` + `hudcontainers=...` (the user's saved HUD part positions).

## Confirmed working by the user this session

1. **Radar flicker fixed** (ROOTSHRINK): HUD shrunk inside the movie via the render-root
   matrix, viewport left stock. Cause from the extracted HUD scripts: the modules are 3D
   panels projected inside an offset viewport.
2. **Camera fixed** (ROLLSIGN): the rigid "follow the game's tilt" composition had its
   roll written mirrored to UE3 (`build_basis(y,p,r) == UE3(y,p,-r)`). Now like UEVR's
   default: camera up = looks down, head turns never roll. Panel: one tick box
   "decouple the camera pitch" (unticked = default).
3. **HUD parts separable** (HUDLAYERS/HIDE3D/HUDFP): HUD -> "move / resize single HUD
   parts". Containers: surveillance radio, compass, radar, one unknown situational
   overlay. Hide = Scaleform SetVisible; 3D parts moved with SetMatrix3D. Saved
   positions **survived a relaunch** (fingerprint keys K1..K4, not names/order).
4. Menus at 80% with no small-window flash on game entry (ENTRYHOLD).
5. OFXR Bridge loads and runs with AKVR; better with SBSONE, **still stuttery**.

## Open issues, in priority order

### 1. OFXR Bridge still stutters (user: "better, but still stuttery")
OFXR = optical-flow frame generation, an implicit OpenXR layer. Source cloned at
`references/OFXR-Bridge` (public V116); the user runs **V277** from
`%LOCALAPPDATA%\OFXR Bridge\RuntimeLayer\v277` (flight logs there; recorder ON).
Measured progression (flight logs, 65 s steady windows):

| | real app frames/s | OFXR hold in xrEndFrame (median / p90) | headset submissions/s |
|---|---|---|---|
| two per-eye swapchains (03:22 log) | 22 | 19.9 / 43.2 ms | 86 |
| SBSONE, one SBS swapchain (03:53 log) | 30.7 | 12.2 / 22.3 ms | 87 |

OFXR's own GPU work is small (~1.2 ms per synthesis) - not GPU-bound on its side. The
remaining hold: `private_swapchain_acquire` and `private_swapchain_release` each wait up
to ~10 ms (p90 9.7 / 9.4) about twice per app frame, plus `app_swapchain_acquire`
median 3.3 ms. Target: <= ~15 ms hold so the game (own work ~7 ms, measured) makes 45.
Already done: AKVR's frame lock auto-pauses when the layer DLL is loaded (OFXR build).
Next ideas, none tried:
- Get an AKVR F2 from an SBSONE+OFXR run (the newest frames.csv is from before SBSONE):
  game-own time vs submit time per frame.
- Check whether VDXR releases swapchain images late (the ~10 ms waits track the
  90 Hz refresh); compare OFXR with another OpenXR game on the same PC.
- Our per-frame order is xrWaitFrame -> copy -> xrEndFrame inside Present, with the
  game rendering between Presents. OFXR expects wait -> render -> end; the hold may be
  its back-pressure. Read V277's pacing if the author publishes it; ask the user for
  OFXR's Discord / issue if not.
- `motion_vectors=dlss` is set in OFXR's ini; AK has no DLSS. Ask the user to set it to
  off in the tray and compare.
- Lower render height (SHARPNESS) as a GPU-headroom test.

### 2. Startup main menu stutters; fine after returning from gameplay
Not measured yet. With `menu3d=1` the startup menu is live 3D through the gameplay
path; pause/map afterwards are still pictures (cannot stutter), so the comparison may
be unfair. Asked the user to try unticking "main menu: live 3D, follows your head"
and/or press F2 while it stutters. No data yet.

### 3. Slider "fighting" on the D-pad (SLIDEROWN, installed, untested)
The user said all sliders fought. A stand-alone ImGui 1.92.9 harness
(scratchpad `slidertest`, not kept) stepped cleanly, so the second writer was never
found. SLIDEROWN discards ImGui's controller tweak and steps one displayed unit per
press from the real pad (350 ms then 70 ms repeat). Confirm with the user.

### 4. Frame rate readouts
The mod holds the game at the lock rate in both lock modes; "60 fps" seen earlier was
VD counting repeat submissions. VD's own SSW "Always" halves the app rate on its own.
The panel wording was clarified (TILTBOX). Nothing open unless the user asks again.

## Key technical facts learned (details in PLAYBOOK_REVIEW)

- HUD scripts: `BmGame.upk` -> Gildor `extract.exe -filter=SwfMovie` -> CFX/zlib carve
  -> JPEXS. Recipe + scripts in `diagnostics/hud-scripts-20260926/`.
- Scaleform render tree (BatmanAK.exe): TreeNode::SetMatrix `0x1411cdc70`, SetMatrix3D
  `0x1411cdce0`, SetVisible `0x1411ccd80`, TreeContainer::Insert `0x1411cd480` /
  Remove `0x1411cd4f0`; children at node data+0x90; node parent +0x20; movie render
  root at view+0x88, ViewportMatrix view+0x110; per-frame movie slot 27 `0x1411ad270`
  is called DIRECTLY (vtable swaps never fire; use MinHook).
- Heuristic Scaleform names are unstable across launches (one came out
  "antiAliasType"): never key saved settings on them.
- Camera: verify any composition against UE3's own rotation matrix, not our inverse;
  check F2 trace per frame (final roll vs head roll) before theorising.

## Memory

User memory index: `C:/Users/jungl/.claude/projects/d--Documents-Antigravity-VR-Modding/memory/MEMORY.md`.
New this session: `ak-roll-sign-vs-ue3`, `ofxr-bridge-frame-generation`; updated
`ak-radar-blink-captured`, `ak-geo11-res-ceiling-and-pitch`.
