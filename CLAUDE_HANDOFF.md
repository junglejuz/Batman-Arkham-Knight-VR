# AKVR handoff — 2026-09-29 (end of session)

Previous handoff: `diagnostics/CLAUDE_HANDOFF_2026-09-28_archived.md` (HUD layer history, OFXR notes).
Build-by-build detail: `PLAYBOOK_REVIEW.md` (sections from "VSID3" onwards are this session).

## Rules (all still apply)

- **Packaging for any mod: read `../PACKAGING_GUIDE.md` first** (workspace root; everything learned packaging AKVR).

- Plain language and short replies to JJ; no code or jargon in chat. Never propose SteamVR or changing the
  OpenXR runtime (the README may *mention* SteamVR for other players - see below).
- **Every change to a file we did not write** (geo-11, the fix, the GAME's own config) goes into
  [FIX_CHANGES.md](FIX_CHANGES.md) in the same step. Fix-shader edits: patch a copy, assemble with geo-11 0.6.90
  `cmd_Decompiler -a`, load with `tools/shader_driver_test` (vstest.exe) BEFORE they reach the game.
- Build: `cmake --build akvr/build-dinput8 --config Release`. The post-build copy does not run: with the game
  CLOSED, back up `dinput8.dll` + `akvr_settings.ini` to `diagnostics/before-<BUILD>-<date>/`, run `deploy.bat`,
  compare hashes. Bump the build tag in hooks.cpp (`"   build: <TAG> "`).
- **Package ONLY with `python akvr/tools/make_package.py`** -> `dist/AKVR-ArkhamKnight/` + `dist/AKVR-ArkhamKnight.zip`
  (no top folder, forward-slash names; PowerShell's zip tools write backslashes that Windows' extractor rejects).
  Upload: `"C:\Program Files\GitHub CLI\gh.exe" release upload v0.1.0 <zip> -R junglejuz/akvr --clobber`, then
  download it back and compare hashes.
- Commit messages with quotes: write them to a file and `git commit -F` (PowerShell 5.1 mangles `"` in args).
- The command guard blocks some commands containing `'/c'` or `.Replace('\','/')` etc. - put them in a .ps1/.py file.
- JJ's Documents folder is **D:\Documents** (moved). Always use `[Environment]::GetFolderPath('MyDocuments')`.
- **Windows' built-in zip extraction is broken on JJ's PC** (confirmed by JJ; fails for every zip). He uses 7-Zip.
  Don't chase the zip format again.

## Where things stand

**JJ's PC right now (clean slate for a fresh-install test):**
- `E:\Games\Steam\steamapps\common\Batman Arkham Knight` = a fresh Steam install, **mod and fix uninstalled** with
  the new uninstaller. Graphics set by hand to JJ's flat-screen preference in BOTH places (BmGame\Config\
  BmSystemSettings.ini and `D:\Documents\WB Games\Batman Arkham Knight\GFXSettings.BatmanArkhamKnight.xml`):
  2560x1440 fullscreen, 16x anisotropic, all four GameWorks effects on. **Still at game defaults, waiting for
  JJ's answer: Max FPS 30, motion blur on, v-sync on** (his August settings had blur and v-sync off).
- `...\common\Batman Arkham Knight mod` = JJ's OLD tuned install (renamed): his tuned `akvr_settings.ini`
  (source of `install/files/akvr_settings.ini`) and old BmSystemSettings.ini.
- Safety copies: `diagnostics/gfxstore-safety-20260929/`, `gfx-before-restore-20260929/`,
  `gfxsettings-store-20260929/` (the store as it was with the mod's VR values in it).

**GitHub:** private repo `junglejuz/akvr`, all pushed. Release **v0.1.0 "AKVR 0.1"** holds `AKVR-ArkhamKnight.zip`
= mod build **GSACOMFORT** (not yet run in the game) + the current installer and uninstaller. READMEs of akvr,
skvr and ds3vr were rewritten this session (plain wording, "native stereo", headset-agnostic, short install).

## NEXT: JJ's clean fresh-install test (he was about to do it)

He follows the README from step 1 (download the release zip, unzip with 7-Zip, fix from HelixMod left in
Downloads, run Install-AKVR.bat, check the OpenXR runtime, play). Check afterwards:
1. Installer output: "your graphics settings saved (vrmod_graphics_backup)", "tested VR settings copied in",
   "tested graphics settings applied", "Uninstall-AKVR.bat put in the game folder", HUD edits applied.
2. `akvr_startup_log.txt`: build GSACOMFORT; engine render FORCED 2888x2860; native eye order SWAPPED;
   head-pose delay 3; comfort line; **graphics menu line "blur/vsync off N (store held blur X)" with N > 0**
   (proves the game asks NVIDIA's store and gets "off"; if N = 0 the game only reads the store in the menu).
3. JJ in the headset: eyes right, no blur on head movement, pose delay 3 right, pause/map at 70%, HUD layer +
   reticle/objective marker at depth, compass not split.
4. Then Uninstall-AKVR.bat: the game must come back EXACTLY to the settings above (restored from
   vrmod_graphics_backup), fullscreen 1440p.
If anything is wrong, compare with the old tuned install and with the practice tests in the scratchpad
(`cleanslate-test.ps1`, `gfxcycle-test.ps1` - scratchpad of session 3f3e048c, may be gone).

## What was done this session (2026-09-28 night -> 09-29)

| Build / change | What | Status |
|---|---|---|
| VSID4 | Doubled TARGET DETAIL panel: HUD shaders named on the GAME's device (delay-loaded D3D11CreateDevice slot) - geo-11 stores flags, not hashes | 13/13 recognised (log); cap raised to 1024 |
| PANELTIDY | Removed: reticle edge-size (never worked), colour/distance-fix/placement options | done |
| DEPTHALL (fix edit 1e) | Objective marker + other depth pieces kept in the 3D picture: all 13 HUD shaders pass the fix's own depth decision | driver-tested, applied; JJ saw it working |
| SESSBACKOFF | VR session refused after Onimusha: retries back off 3..30 s instead of every frame (was 1 fps) | root cause unknown, parked (VD restart doesn't help, reboot does) |
| HUDVIEW | Lazy follow removed (JJ: "cheap trick"); HUD quad in VIEW space (compositor places it every refresh) | JJ happier |
| BAND28 | Compass band fixed at 28% (split at 30), slider removed | JJ confirmed |
| ANYHEADSET | Per-eye HUD shift only when the runtime is Virtual Desktop (VD draws quads without parallax) | untested elsewhere |
| installer | Runs the fix-patch script; NonSquareRT block; finds the fix 1-2 folders up; tested VR settings on first install; JJ's graphics menu on first install; saves the player's graphics (ini + NVIDIA store) | practice-tested |
| uninstaller | In the game folder; fix + mod removed; skips backups that contain the mod; restores the player's graphics exactly (else resets); finds moved Documents; offers to delete backups; deletes itself | practice-tested |
| GSACOMFORT | Mod answers NVIDIA's store MotionBlur/Vsync/AdaptiveVsync = off (the game was putting blur back) | **installed in the release, not run** |

Stutter analysis (capture 00:19): 6 of 7 hitches > 50 ms were the game; the worst came from waiting in the hand-off
to Virtual Desktop (xr_submit 20-80 ms). Not split further; only if JJ asks.

## Update 2026-09-29 evening (fresh-install test, part 1)

JJ installed from the release at 17:20 and ran it: GSACOMFORT WORKS (log: "blur/vsync off 3 (store held blur 1)").
JJ reported the four GameWorks effects were NOT turned off: the installer's graphics list never had them (stock is off)
and NVIDIA's store was never written. Fixed in the installer (commit 5c9c080, FIX_CHANGES.md section 6), package
re-uploaded to v0.1.0 and verified by download. JJ's current install still has them on: Uninstall-AKVR.bat, then
install again from the new zip (the uninstall makes the reinstall a first install).

**Also 2026-09-29 evening:** JJ lowered the head-pose delay 3 -> 2 while GameWorks was on (README now says so).
Installer: picks the game from ANY folder of it (searches 4 levels down; several copies = ask again; Cancel has its
own message), and asks the picture sharpness on a first install: Low 2016 / Medium 2432 (Enter) / High 2860 =
engineres + rendersize (`-Picture` for scripts). Tested on a fake game folder in the scratchpad, JJ's store restored.

**Build HUDWORLD (2026-09-29 evening, deployed to JJ's game, NOT in the release yet):** JJ wants the HUD fixed in the
room by default (UEVR-style), glued to the head only as the panel option "attach UI to head movement" (`hudattach`,
default 0). xr.cpp g_hudSpace 2 = the HUD quad placed once in LOCAL space in front of the LEVEL head pose at the last
recenter (akvr_head_recenter -> akvr_xr_hud_reanchor) and left there. Backup: diagnostics/before-HUDWORLD-20260929/.
Check in the headset: HUD stays put when turning the head, F12 re-hangs it, depth still right (per-eye quads).

**Late 2026-09-29 (builds EDGEBAND -> LAYERSHOT -> MENUONE, installed):** with the HUD layer ROOM-FIXED, the split
(fix scene-depth pieces stay in the picture) tears elements apart: one element = pieces tagged differently, so part hangs
in the room and part follows the head = "double" (JJ; the desktop shows one copy). EDGEBAND (position strips, fix 1f)
made it worse and is undone. MENUONE: on the live main menu the whole UI goes to the layer, no split. F2 now also saves
`<capture>hudlayer.bmp` (the layer image). OPEN: gameplay compass/tips still head-locked; a correct split needs to tell
world markers from screen-fixed HUD per ELEMENT (e.g. per Scaleform movie/container), not per piece tag or position.

**Build ZOOMVIG (2026-09-29 late, installed, untested):** right-stick-click zoom = our FOV lock cancels the magnification,
so only the game's 2D overlay was left. New: our own vignette (view-space quad, black alpha ramp) while the game's OWN
FOV (camera.cpp `g_gameFov`, before the lock) < `vigbelow` (default 45 deg, GUESS - the panel shows now/lowest/widest; set
it from JJ's numbers). Panel section ZOOM VIGNETTE: strength, clear centre, threshold, preview. The game's overlay: JJ to
hide its HUD part (not yet identified). MENUONE2: phase 0 (start screen) counts as menu too.
JJ's HUD part map: C0.0.0 = gameplay tips; C1.0.0.0.1.0.0.0 reticle, .1 target distance, .2 compass. Menu 3D (Batman)
jiggles on menus only, not in gameplay - asked JJ to try pose delay 2 / 4 on the menu.

**Build PARTTAG (2026-09-30, installed, untested):** per-PART HUD sorting. Panel HUD parts list: new box "hang in the
room" per part (saved as 5th field in `hudlayers`). The mod writes add blue = -1/512 into the part's Scaleform colour
transform (GetWritableData 0x1411c96f0 change 2, data+0x68) every frame; fix step 1g makes the 7 colour HUD shaders
drop the scene-depth decision for marked pieces -> flat, whole part on the room-fixed layer. JJ ticks the compass first,
then tips etc. Also: MENUDELAY (menu pose delay 2, JJ confirmed the value), vignette default 1.0 / clear 18.
Still open: first-menu Batman flicker for a while; loading screens look vertically stretched (need F2 captures);
the game's zoom overlay = part C0.0.0.0.0.2 per JJ (tick hide).

## Open issues, in priority order

1. **The fresh-install test above.**
2. **The mod's built-in defaults are still not the tested setup** (~25 keys: swapeyes, engineres, posedelay,
   vpsquare, hudscale, ...). The installer's settings file hides this; a missing key still falls back to a wrong
   default. Offered to JJ, not started: change the defaults in code to match `install/files/akvr_settings.ini`.
3. Motion blur: if GSACOMFORT's counter stays 0, the game only reads the store from the graphics menu; then the
   blur came back through the file only (the mod flips it at every start anyway).
4. Compass band (1c) exists only in the 2 filter-42 shaders; if compass pieces from the other 11 flicker, add it there.
5. VR session refused after Onimusha/REFramework until reboot (VD OpenXR.log: QI on VD's own submission device,
   80004002). Parked by JJ.
6. Sekiro/DS3 installers lack their HUD edits and an uninstaller.
7. Before making the repo public: geo-11 is bundled (3Dmigoto GPL; credit + links are in the release notes).
8. OpenVR: JJ asked, then agreed it isn't needed (SteamVR runs OpenXR). Don't reopen without a concrete reason.

## Key facts learned this session

- **Arkham keeps its graphics menu in two places**: `BmGame\Config\BmSystemSettings.ini` (game folder) and NVIDIA's
  settings store `<Documents>\WB Games\Batman Arkham Knight\GFXSettings.BatmanArkhamKnight.xml` (UTF-16; read at
  every start, saved back). The mod's GSAHOLD answers (windowed + VR size) get saved into the store: after
  uninstalling, the game opened as a square window. Store option names: Display_Mode, ResolutionX/Y,
  Anti-Aliasing, Texture_Resolution, Shadow_Quality, Level_Of_Detail, Interactive_Smoke, Interactive_Paper_Debris,
  Rain_FX, Volumetric_Lighting, Vsync, AdaptiveVsync, MotionBlur, TextureFiltering (0 trilinear .. 4 = 16x).
- The stock 60 fps cap (mod only rescues < 60) + motion blur caused blur on head movement and a different
  pose delay on the fresh install. Tested menu: Max FPS 90, High, 2x aniso (VR); JJ's flat menu differs.
- On a fresh game install the first start picks the monitor's LARGEST mode (JJ's are 4K-capable, desktop 1440p).
- The fix was updated 2026-09-25 (AO, window cubemaps, non-16:9; same file name/URL); our patch script gives
  byte-identical HUD shaders on both releases.
- Memory: `vr-mod-installers`, `ak-gfxsettings-store-file`, `geo11-shader-hash-not-in-context`,
  `ak-hud-depth-decision-all-13`, `ak-hud-steadiness`, `vr-session-refused-pc-side`, `github-repos`,
  `openvr-backend-question`.
