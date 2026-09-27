# AKVR handoff to Claude ? 2026-09-26

## Start here

Continue fixing Batman Arkham Knight VR gameplay jitter/head-motion lag. User
requested this handoff, not further implementation in this turn. Read root
AGENTS.md, this file, then the newest sections of PLAYBOOK_REVIEW.md. RESUME.md
and GEO11_PLAN.md contain historical contradictions: the newest evidence here
supersedes older claims that projVR should be off or stereo is not working.

Workspace: `D:/Documents/Antigravity/VR_Modding` (not a Git repository).
Game: `E:/Games/Steam/steamapps/common/Batman Arkham Knight`.
Runtime: Quest 3 via Virtual Desktop / VirtualDesktopXR. Do not switch to SteamVR
or change the global OpenXR runtime. User prefers brief plain-language updates.

## Confirmed by the user

- Game launches into VR through local dinput8.dll. Local version.dll had been
  bypassed in favor of System32 VERSION.dll; live-module inspection proved it.
- Gameplay head tracking works. MODEOBS separated gameplay detection from the
  optional projection rewrite, correcting the cropped/fixed-screen behavior.
- Swapping native eye images improved stereo. Keep `swapeyes=1`.
- Experimental projVR expands the view well. Keep `projvr=1`, `wide=0`.
  This is user acceptance of framing, not independent proof of projection geometry.
- HUDASPECT makes HUD proportions much better. Keep `hudaspect=1`.
- Severe jitter, including while still, and slight head-motion lag remain.
- `OneFrameThreadLag=False` produced NO perceived jitter improvement.
- Startup logos, splash screens and loading images are rock solid.

## Exact stopping point

PRESENTPAIR is built and installed, but has NO user headset result yet.
Rechecked installed DLL SHA256 at handoff:
`3727eb4caa07648498fcfce1bc7b7d856e0dac6057233b31c583c986adc61687`.
Installed `nativeafterpresent=1`. No timestamped `_present.csv` files were found
in the game directory at handoff, so do not assume the new test has been run.
No game/config/code changes were made during the handoff turn.

The last request to the user was:
1. Relaunch; slowly turn head; press F1.
2. Open F8, untick **frame timing test: capture after Present**, close the panel,
   repeat slow turns and F1.
3. Report which feels steadier.

F1 creates timestamped `_camera.csv`, `_mode.csv`, `_status.txt`, and now
`_present.csv` in Binaries/Win64. It also maintains the old latest camera/mode
filenames. Preserve new captures into a dated diagnostics folder before analysis.

## Current hypothesis and test

Before PRESENTPAIR, hkPresent sampled a new XR pose, published camera changes for
a following engine frame, copied Katanga into XR and ended XR, then called the
next/original Present. Gameplay's submitted pose is g_layerPose (previous
Present's sampled pose). Splash uses a fixed world pose g_fixedPose.

If geo11 updates the shared image inside downstream Present, copying before it
could pair an older image with a newer pose. This is a hypothesis: the installed
hook chain's actual producer timing has not been observed. Stable splash screens
support investigating gameplay image/pose matching, but do not prove this cause.

PRESENTPAIR adds a reversible native/Katanga-only branch that submits XR after
oPresent. AER/fallback retain prior order. Original Present is called exactly
once and its HRESULT returned. Setting and panel checkbox: nativeafterpresent.
If the image was already current before Present, moving submission later could
add latency; reject it if unsupported. No damping was introduced.

`src/present_probe.cpp` samples 16 pixels across the SBS image before/after
Present every 30 Presents, using four reusable staging textures. Later frames
Map with DO_NOT_WAIT; busy slots are skipped; no production Flush or GPU wait.
F1 logs hashes, camera/Present counters, gameplay/timing switch, and CPU times
for xrWaitFrame, XR begin, XR submit, original Present.

Interpretation:
- Compare gameplay records under switch ON versus OFF and user observations.
- Changed hash means sampled pixels changed across that interval; it does NOT
  identify the exact rendered frame or its pose, or exclude another producer thread.
- Unchanged hash in a static scene is inconclusive. Slow head turns help.
- xrWaitFrame time distinguishes runtime waiting from arriving late. Total
  XR begin/submit times are CPU durations, not GPU timings.
- Camera trace is sampled at Present, NOT inside camera finalization. Do not
  treat memory readback or a projection-hook write as rendered-image evidence.

## Latest analyzed recording

`diagnostics/jitter-20260925-235035/akvr_20260925_235035_552_*`
plus `analysis-last15s.json`. Last 15 seconds:
- 770 rows, 14.993s; median Present interval 22.022ms (~45 Hz), p95 23.053ms,
  maximum 92.435ms.
- Exactly one camera finalize per Present; all base rotation axes constant.
- Gameplay 100%, no mode transitions, FOV84.7 constant, three projection builds
  per Present. Raw-head and composed-angle steps agree.

Prior capture `diagnostics/jitter-20260925-233832/` has a quiet 90Hz window and
other windows with large base-camera discontinuities. User briefly moved the
camera deliberately at the start. Do not label the whole recording still or
assume the later jumps explained. Latest final window does not show those jumps.
The evidence does not justify adding head-pose damping, which adds lag.

## Files and validation

- `src/hooks.cpp`: loader-independent Present path, mode observation, eye-swap
  settings/UI, HUD switch, numbered F1 dumps, new submission-order comparison.
- `src/xr.cpp`, `src/xr.h`: native eye swap, actual submitted-mode/FOV diagnostics,
  measured xrWaitFrame duration. Pose-to-image association remains an assumption.
- `src/camera.cpp`: independent projection observation counter; existing Present-
  side camera composition and trace. No new smoothing.
- `src/earlyres.cpp`, `src/hud_projection.h`: verified 0x34-byte GFx viewport,
  uniform scale +0x2c / pixel aspect +0x30. Gameplay/projVR correction ~0.5128
  for current source/FOV, tracks authored aspect and restores when disabled.
  Exact-fit movies/fullscreen overlays and inherited cross-thread live re-push
  remain potential limitations; do not undo the user-accepted HUD fix casually.
- `src/present_probe.*`: new sparse GPU boundary diagnostic.
- `tools/analyze_camera_trace.py`: stdlib trace summary; accepts --seconds and
  --mode-trace. New present CSV can be read with Python csv.
- `tests/`: 3/3 CTests passed: DirectInput forwarding, HUD geometry/state, actual
  D3D11 WARP probe test detects changed and unchanged image content.

Build (auto-deployment disabled in current cache):
```powershell
cmake --build akvr/build-dinput8 --config Release --target akvr --parallel 8
cmake --build akvr/build-proxy-test --config Release --parallel 8
ctest --test-dir akvr/build-proxy-test -C Release --output-on-failure
```
For a fresh configure: Visual Studio 17 2022 / x64, AKVR_PROXY=dinput8,
AKVR_GAME_DIR empty. Build emits known LNK4222 warnings for COM export ordinals
matching the genuine proxy. Do not install version.dll alongside dinput8.dll.
Deploy manually only with BatmanAK closed; preserve prior DLL/config; verify hash.
No headset acceptance of PRESENTPAIR follows from build/tests passing.

## Settings, rollback and references

Installed mod settings checked at handoff: projvr1, swapeyes1, hudaspect1, wide0,
katfeed1, nativeafterpresent1, spinfoldon0, hudscale1000, leanscale1.9608,
worldscale0.8929; offset sliders0. Current game source 2560x1440 per eye; Katanga
5120x1440 SBS. Desktop over/under is a preview, not the headset source.

Generated game config OneFrameThreadLag=False was retained for the new comparison
so submission order is the variable. Do not present disabling it as a new fix.
NEVER edit DefaultSystemSettings.ini on this existing install: timestamp-triggered
regeneration previously wiped tuned settings. Edit generated BmSystemSettings.ini
only; preserve IniVersion and encoding (UTF-8 without BOM). Do not restore a stale
whole game config across template timestamp changes.

Pre-PRESENTPAIR DLL/settings:
`diagnostics/jitter-20260925-235035/before-PRESENTPAIR/`.
Pre-HUDASPECT DLL/settings/game config:
`diagnostics/jitter-20260925-233832/before-HUDASPECT/`.
The checkbox OFF returns previous submission order while preserving accepted HUD.

Consult root references/README.md and playbook AGENTS.md before new techniques.
Playbook checkout: `b955f2244f7c5d5f3b3e5f403b1b6721a3362423`.
Relevant: FAIL-PERF-014; chapter19 #blocking-wait-floor (pose/image association,
request identity, measured waits); vrframework guide07 section3 (three clocks).
For HUD: chapter04 #ui-not-one-class and vrframework guide11. For earlier eye swap:
VRScreenCap default reversal was a lead, validated by the user's comparison.
Record source inspection, runtime measurements and headset reports separately.
Do not update the upstream reference checkout incidentally or put target notes in it.

Older Claude memory under
`C:/Users/jungl/.claude/projects/D--Documents-Antigravity-VR-Modding/memory/`
contains useful structural facts but obsolete conclusions. In particular, old
`projvr-vertical-never-lands` advice must not override the user's latest success.

## If the comparison does not help

Use the new measurements to reject unsupported Present-order changes. Next
investigate actual rendered image/pose ownership and projection agreement rather
than further queue guesses or smoothing. A trace of camera memory alone cannot
prove which pose geo11 rendered into the shared texture. No causal diagnosis of
the remaining jitter has yet been established.
