# AKN handoff — Arkham Knight VR without geo-11 (engine stereo + DLSS)

Last updated 2026-10-09 22:50 (SteamVR route + suit gloss). Read this first, then the last sections of `NOTES.md` (full run log) and `DLSS_PLAN.md`.
Workspace rules: `VR_ESSENTIALS.md`, `VR_HUD_GUIDE.md`, `VR_WORLD_MARKERS_GUIDE.md`.

**2026-10-13: HUD / markers / detective mode / menu transitions -> read `HUD_REBUILD_PLAN.md` first** (JJ-approved plan: general
placement rule instead of per-element fixes; detective mode all at HUD depth; transitions from game state only).

## 1. What this is

- **AKN** = `akvr_native/` → `XINPUT9_1_0.dll` in the game folder. Makes the game render BOTH eyes itself (a cloned view
  family for the right eye, rendered first; the left eye is the game's own family), runs per-eye DLSS, captures the
  HUD into its own texture, logs to `Binaries\Win64\akn\akn_log.txt`, takes commands via `akn\command.txt`.
- **AKVR** = `akvr/` → `dinput8.dll`. OpenXR, camera/head, overlay panel, settings (`akvr_settings.ini`). With `-akn`
  it runs in "engine stereo" mode and takes the eye pair + HUD texture from AKN (`akvr/src/akn_link.cpp`).
- geo-11 is NOT used in this version (the geo-11 version is the released AKVR; see `RESUME.md` / `akvr/CLAUDE_HANDOFF.md`).
- Build: `cmake --build akvr_native/build --config Release`, `cmake --build akvr/build-dinput8 --config Release`.
  Deploy: `tools/Run-M5c-A.ps1` copies both DLLs (game must be closed).

## 2. State (all deployed to JJ's game, latest builds)

| Area | State | Key names |
|---|---|---|
| Native stereo, per-eye lighting | CONFIRMED (JJ "98 % better") | TURNEDEYE (centred lens on a turned eye camera), POSESTAMP, FRAMESYNC |
| Main menu in real 3D | built, fake-headset checked | MENU3D (old one-spot view = option `menuonespot`) |
| Rain (same in both eyes, off the head, with depth, pause/isolate) | CONFIRMED | RAINONCE, FARRAIN2/NEARRAIN2, RAINDEPTH, panel RAIN TEST |
| DLSS per eye (DLAA…Performance), transformer, CAMMV motion | CONFIRMED (JJ: background "so much better") | `dlss=` (restart), model live, CAMMV mode 1 |
| DLSS texture sharpness (mip bias, Luma's method) | fake-headset proven (blur test), JJ untested | MIPBIAS / MIPBIAS2, overlay "texture sharpness (live)" |
| HUD at full resolution under DLSS | fake-headset checked | HUDFULL |
| Lens flares off | config forced at start-up | LENSFLAREOFF (FIX_CHANGES.md) |
| Weapon wheel "posterised" | FIX BUILT, JJ untested | HUDINEYE: gameplay HUD drawn into each eye with the game's blend (not a VD quad layer); HUDGLOW, HUDBLEND |
| Distance marker / reticles on their objects | FIX BUILT, data-verified once, JJ untested | MARKERCUT (geo-11 on-target points + Canvas.Project world point, per-eye card) |
| HUD flicker (map / menu / pause text, right-side elements) | candidate fix built, untested | HUDHOLD (keep the last complete HUD sheet); gameplay covered by HUDINEYE |
| Batman's suit gloss differs per eye / slides with the head | SOLVED (JJ chose it in the panel, 2026-10-09 22:4x) | SUITGLOSS3: ~1010 pixel shaders take gloss from the camera's AIM (cb1[11..13]); their draws get the head-free game camera's rows. Default on (`gloss 3`, `glosssrc 1`), no extra pixels |
| Overlay + test code tidy | DONE 2026-10-09 (fake-headset smoke run clean) | PANELTIDY3 (126 -> 81 controls), CLEANTESTS (finished test runners, lens mode 3, session diagnostics removed; RAIN TEST kept) |
| SteamVR (same native build, own launcher) | CONFIRMED by JJ (fuses, colours right, no overlay) | SVRAUTO (DXGI 1.1 factory), SVRSPACE (STAGE space), SVRSTRAIGHT (turned eyes warped straight), launcher `AKVR-Launch-VR-EngineStereo-SteamVR.bat` |
| Desktop mirror under DLSS | FIXED | MIRRORLATE (drawn after the HUD is collected, else it lands on the HUD sheet), MIRRORHUD2 (no second HUD), STRBLEND |
| Map: right eye lacks the hologram look; map icons on the HUD plane | OPEN | families identical in gameplay; need a dump with the map OPEN |
| 120 Hz | OPEN | GPU: DLSS ~2.8-3.9 ms/eye; processor side ~8.6 ms |
| Markers turn with the head | SOLVED 2026-10-11 (JJ: "solid and not rotating ... perfect"): MKGS per-vertex perspective card (VR_WORLD_MARKERS_GUIDE §5b); earlier: | MARKERBASE: Canvas.Project answered head-free (same answer at head yaw 0/+-15) |
| Compass chunks / icons on objects | FIX BUILT | MARKERMATCH: match in game units via the layout map, 0.06; no fallback cuts |
| HUD stretched 14 %, right edge cut (map overlay, main menu prompt) | FIX VERIFIED | HUDSIZE: AKVR lays the HUD out at AKN's HUD texture size (akn_hud_out_size); compass centred |
| Weapon wheel banding | FIX BUILT, pause path verified | WHEELEYE: the wheel = pause look; HUD in the eyes there too, darkening under it |
| Map HUD misaligned / wrong size | FIX BUILT, map opened on fake headset | MAPFLAT: game's own lens on floating screens, HUD cropped + hung on the screen; map key = TAB |
| Wet ground reflections differ per eye | settles by itself a few seconds after entering the game (JJ) | the right eye's fresh history fills up; the 2026-10-09 "left eye only" numbers were a measurement bug + a live lens switch (NOTES CORRECTION 16:3x) |
| AFW rendering (alternate frame warping, JJ 2026-10-10) - overlay choice beside native | BUILT + deployed, not run yet | STEREO RENDERING section; `afw 0/1`, `afwmode 1-3`, `afwdbg`; src/afw.cpp; NOTES "AFW PLAN" + TEST PLAN AFW1 |
| Loading screen cropped (JJ 2026-10-08 23:5x) | OPEN | two capture runs failed (Enter on Continue did not land); unknown if he meant the desktop window or the headset |

JJ's report + 13 captures of 2026-10-08 19:45: `akvr_native/captures_jj_20261008/` (with that session's log).

## 3. How the newest pieces work (details in NOTES.md)

- **HUDINEYE** (`akvr/src/xr.cpp` `hud_in_eyes`): the HUD sheet (AKN's redirected HUD texture) is drawn into both
  halves of AKVR's side-by-side copy (`g_aknSbs`, 8-bit display values) with ONE/INV_SRC_ALPHA, at the room-fixed
  HUD quad's place, through each eye's own pose + lens (`hie_eye_view` = the submitted views). Why: VD blends quad
  layers in linear light → dark see-through HUD backings go milky and band (the sheet itself was correct). Gameplay
  only; menus / pause / flat frames still use the quad layer. Setting `hudineye` (default 1), test command `hudineye N`.
- **MARKERCUT** (same function): geo-11's on-target points (`earlyres akvr_hud_target_points`, live HUD tree) give
  where each marker is on the sheet; AKN's `akn_markers` gives Canvas.Project world points (RA 0x86DC0D, matched to a
  point within 0.25 NDC); AKN's `akn_pair_vp` gives both eyes' unjittered view×lens of the shown pair. The marker's
  circle (r 0.12 NDC) is cut out of the sheet and drawn into each eye centred on that eye's projection of the world
  point. Run MC 20:25: point (-0.034, 0.192) matched, left (-0.057, 0.187) / right (-0.339, 0.191). Markers with no
  Canvas.Project point (grapple?) stay on the sheet for now — next: depth from the left eye's depth buffer (geo-11
  TARGETDEPTH) or the 2nd projection caller seen in JJ's log (RA 0x5A2BB1). Setting `markercut`, command `markercut N`.
- **SUITGLOSS3** (suit gloss): AK's character shading takes gloss / rim from the camera's AIM - pixel shaders that read
  the view buffer's direction rows `cb1[11..13]` but never its position row `cb1[14]`. With turned eyes (lens mode 2)
  each eye's aim differs by ~14° → different gloss per eye; in geo-11 (one aim) it fuses but slides with the head (JJ).
  AKVR (`hudsplit.cpp` mc_worker) disassembles every pixel shader at creation on a worker thread and flags them
  (`akvr_shader_matcap` bit 2; ~1010 of ~30400). AKN `gloss_pre` gives those draws a copy of b1 with rows 11-13 from
  the head-free game camera (MARKERBASE `g_mkVbase`, built every stereo frame), like RAINDEPTH's per-draw cb copy.
  AKVR export `akvr_shader_camdir`; AKN command `gloss 0|1` (on by default). The test variants (matcap subset, head
  aim, narrowing groups, the overlay test section) were removed in CLEANTESTS 2026-10-09. Ruled out on the way: the matcap subset alone, wrong
  camera data in the lighting compute step (`dae26b50bf0927b8`, fully per eye), the right-eye-only reflection
  refresh (`15a1e2efe71084c6`), every compute step (JJ's skip sequence).
- **SteamVR** (detail NOTES "SVR"): DXGI 1.1 factory only when this process heads for SteamVR (geo-11 never loads
  here); STAGE space under SteamVR (its LOCAL is a seated universe that is ROLLED 180° on JJ's PC and swaps in ~3 s
  after start without an event); SteamVR cannot fuse eye views turned different ways, so `straighten()` rewarps each
  turned eye into the runtime's own straight lens and the views are declared straight (`svrstraight 1` = SteamVR only).
  Our private context state (`g_strState`) keeps whatever was set in it: reset blend / depth / raster after swapping it
  in (STRBLEND). Quitting from the game's menu is clean under SteamVR.

## 4. Testing (fake headset)

- Check `ListAgents` and running games first; Steam runs one game at a time (other sessions run Arkham City).
  `Run-M5c-A.ps1` waits by itself for another game to close. Announce each run with an honest time.
- `tools/Run-<X>.ps1` = part A (deploy, launch, menus, MENUCHECK) + `Run-<X>-B.ps1` (the tests). Templates: copy the
  first 28 lines of `Run-X1-B.ps1` (Cmd / XrShot helpers).
- The save now starts at the **Grapnel Boost training terminal**: its popup hides markers and takes input (M did not
  open the map). Walk back first: `python tools/akmenu.py hold 0x53 2.0`. Other helpers: `akmenu.py mouse dx dy`
  (turn the camera), `key`, `click`, `shot`.
- Ready but not run: `Run-MP2.ps1` (walk back, markers on/off + head turn, then map open with M + `famdump`).
- Useful AKN commands: `capmk <tag>` (pair + HUD + HUD-draw dump + projections — also the overlay capture button),
  `pairshot`, `eyesame 0-3`, `vs 0/4`, `famdump N`, `hudblend`, `hudineye`, `markercut`, `mipbias 0-3`, `dlsspreset`,
  `xeye N`, `projlog N`, `mkforce x y`.
- Traps met this session: Python/heredoc edits mangle backslashes (use the Write/Edit tools for Windows paths); files
  edited with Python lost their CRLF endings (exact-match edits then fail); functions placed inside xr.cpp's anonymous
  namespace are invisible to other files.

## 4b. 2026-10-09 night (runs HF, HF2, HF3 - NOTES.md "JJ's report 2026-10-09") - deployed, JJ to check in the headset
- HUD flicker FIXED + verified (HUDDSV: the sheet's own depth/stencil; the sheet steady every frame, more HUD shown).
- Pause / wheel world flicker FIXED + verified (PAUSESTAMP: pause-look writes count as camera updates; delay 1).
- Dark window behind wheel / pause: BACKEXT (the sheet's backdrop carried over the whole view on the pause look).
- Loading screen crop: LOADFIT (HUDFULL only while AKN's sheet is in use; the re-push applies it both ways) - HF3 shots:
  loading pictures whole, gameplay HUD in place right after the load.
- Still open: lens flare ring (c000751, LensFlares=False everywhere already - a different effect, needs a hunt). (Suit
  gloss: solved later that day, SUITGLOSS3 in §3.)

## 4c. 2026-10-09 ~01:40 (runs HB, HC) - deployed
- HUD flicker confirmed fixed by JJ. New: HUDVP (main menu prompts: window-size layouts stretched to the sheet,
  verified), HUDSKIP (pause backdrop removed on the pause look, verified; wheel vignette rule unverified - pad only),
  WHEELROUND (gadget wheel oval: the 16:9 answer rounded down, unverified), LOADFIT2 (loading screen right size from
  its first frame, verified in 3 shots).

## 5. Next steps (in order) - 2026-10-09
1. JJ's headset check of the 2026-10-08 late builds (markers, compass, wheel, map, main menu prompt).
2. (Done 2026-10-09: suit gloss SUITGLOSS3; SteamVR route.) SteamVR leftovers: end the session on EXITING (SteamVR
   kills an app ~5 s after a Quit it thinks was ignored; quitting from the game's menu is clean); the lighting step's
   DLSS jitter (its cb0 row 8: x 0 vs the projection's -0.00033, y sign flipped) - check whether it matters.
3. Loading screen crop: ask JJ where he saw it; capture with tools/Run-LD.ps1 (LOADSHOT job in part A).
4. Fake-headset runs SAVE engineshape (xrsim lens set too late): restore JJ's 918 after every run, or set the lens first.

## 5b. Older next steps

1. JJ's headset results for HUDINEYE (wheel), MARKERCUT (markers), HUDHOLD (flicker); act on his captures.
2. Map: capture/dump with the map open → copy whatever differs into the right eye's family/view before it renders.
3. Suit reflections: confirm `AllowImageReflections` on Batman; then decide setting vs per-eye fix with JJ.
4. Markers without a Canvas.Project point: depth-buffer fallback; map icons at the map's depth.
5. Performance toward 120 Hz.


## 2026-10-11 markers rebuilt (MKEYE)
Marker pieces are drawn per eye inside their own draw (AKN eye marker layers, card mapping per eye from the same frame's
world point + eye views); AKVR lays each layer over its eye 1:1. Replaces cards (MARKERCUT / MKSAMEFRAME / MKCARRY ...),
which stay behind "mkeye 0" / the panel box. Also new today: first person (akvr camera.cpp FIRSTPERSON). Details:
NOTES.md "RETHINK MARKERS 2", "MKEYE BUILD", "FIRSTPERSON".

## 2026-10-11 13:36 MKGS2
MKGS (per-vertex 3D marker placement by a generated geometry shader) never ran in JJ's 13:3x session (built 0, all draws on the
affine fallback): AKVR's 2048-slot shader table was full. Now an unbounded map + diagnostics (NOTES "MKGS RESULT + MKGS2").

## 2026-10-11 ~14:55 PACKAGE 0.3.0 (native stereo only) - installed on JJ's game, waiting for his check before GitHub
Folder moved: akvr_native -> akvr/native (junction at the old path). Commits 83c5e46, 7b8fe6b, 4894080 (local, not pushed).
Panel tidy (PANELTIDY4), installer/uninstaller rewritten (no geo-11), README in the Batman City layout, practice tests 35/35
(scratchpad Test-AKPackage.ps1). JJ's geo-11 / fix files kept in E:\...\Batman Arkham Knight\vrmod_geo11_backup_2026-10-11.
Open: first person does nothing in AK (JJ) - hidden from the panel; after JJ's check: push, release v0.3.0 (AKVR-ArkhamKnight.zip).

## 2026-10-11 22:14 MKFACEPOS + crash / prompt / detective findings (NOTES "JJ 2026-10-11 ~22:00")
AKN installed (22:14): markers now tilt to face the game camera's position (City MARKERFACE, roll-free sizing; "mkfacepos 0|1");
logs kept across launches (akn_log_prev*.txt); captures save both eye marker layers. JJ's SteamVR crash (19:01) = null read in
NVIDIA's driver while SteamVR took a frame; the 19:06 crash was VD exiting. Open, with ready runners: missing prompts
(tools\Run-PROMPT.ps1), detective-mode lights missing in the left eye (tools\Run-DET2.ps1). TEST PLAN JJ1011N in NOTES.

## 2026-10-11 23:07 DETSHARE (default) + DETHOLD; open DETCALLOUT
The right eye now shares the game's view state by default (vs 0): fixes detective mode's half-lit right eye and the right
eye's missing lighting after / before detective mode (JJ: "consistent now"). Pending JJ: smearing check, SteamVR crash
repro without view-state swaps. Next build: detective scan callouts (four-plus box + line + label) locked to their object
by a depth anchor (NOTES "DETCALLOUT"); they are not placed through Canvas.Project.

## 2026-10-12 04:20 DETCALLOUT (fake headset verified, JJ's headset check pending)
Detective scan callouts (box of four plus signs + line + label) are drawn as one world card at the scanned object: AKVR marks
the callout HUD parts (K1/0.0.0.0.3.0.9/.10/.4/.3/.5/.8, own mark) and reports the box point; AKN finds the object's distance by
marching the head-free camera ray through the left eye's depth (copied before UE3's mid-render foreground depth clear), then
places the pieces like every marker (MKGS, MKFACEPOS). Detective key for tests: X (pad injection is ignored). NOTES "DC1".
