## 2026-09-25: HUD accepted; jitter persists; PRESENTPAIR comparison

Headset report: HUD much better, no jitter improvement after disabling the extra
queued game frame. Splash/logos and loading images rock solid. Keep HUD/projVR/
eye swap. OneFrameThreadLag=False retained for the next comparison to isolate
submission order; do not claim it fixed jitter or reapply it as a new approach.

Latest F1 is `akvr_20260925_235035_552_*`, preserved in
`diagnostics/jitter-20260925-235035/`. Analysis `analysis-last15s.json`:
770 rows /14.993s, median22.022ms, p95 23.053ms, max92.435ms. Exactly one camera
finalization per Present throughout this window. All three base rotation axes
constant; gameplay100%, zero mode transitions, FOV84.7 constant, three observed
projection builds per Present. Raw head and composed rotation step distributions
agree; no justification for damping from this trace. Unlike prior capture, no
base-angle discontinuities in the final test window.

Source inspection: splash uses g_fixedPose; gameplay uses previous Present's
pose g_layerPose. Existing hkPresent copies Katanga and ends XR before calling
oPresent. If downstream geo11 updates Katanga there, this consumes an older pair
with a newer pose. Actual installed hook-chain producer timing is NOT established
by source ordering alone. Stable fixed-pose splash frames are consistent with
this hypothesis but also with other render/pose mismatch causes.

PRESENTPAIR comparison: for native + Katanga only, defer XR copy/end until after
oPresent. Call original exactly once and preserve its HRESULT; fallback/AER keep
prior order. Setting `nativeafterpresent` and panel checkbox allow live ON/OFF.
Default/installed ON for next test. No additional camera math/smoothing changes.
Moving the XR end later could instead increase latency if Katanga was already
current; reject this branch if the measured boundary and headset comparison do
not support it. This does NOT establish camera-pose ownership of the image.

New diagnostic samples 16 pixels across both SBS halves before/after downstream
Present every30 Presents, into four reusable staging slots. Map DO_NOT_WAIT on
later frames; skip busy slots; no production Flush or blocking readback. F1 saves
those hashes and camera/Present counters with measured xrWaitFrame, XR begin,
XR submit, and original Present CPU times to a timestamped `_present.csv`.
Zero changed hashes is inconclusive for static scenes. A changed hash establishes
sample content changed across that interval, not the exact source frame or pose;
other-thread producers remain possible. CPU submit time is not GPU time.

Reference: playbook FAIL-PERF-014,
`docs/19-d3d12-and-performance.md#blocking-wait-floor`, subsections "Pace the game
to the runtime's request, and tag frames with the request they answer" and "The
wait time tells you whether you are slow or being held back"; checkout
`b955f2244f7c5d5f3b3e5f403b1b6721a3362423`. Also vrframework guide07 section3.
The native simultaneous-stereo path does not inherit AFR parity prescriptions.

Release built; 3/3 CTests passed: HUD math/state, DirectInput forwarding, and a
real D3D11 WARP test that detects a texture update across the probe boundary and
correctly reports unchanged content in the next sample. Not an in-game validation.
Deployed with game closed; backup DLL/settings in
`diagnostics/jitter-20260925-235035/before-PRESENTPAIR/`.
Deployed SHA256: `3727eb4caa07648498fcfce1bc7b7d856e0dac6057233b31c583c986adc61687`.

Next: slow head turns and F1 with timing checkbox ON, then OFF and repeat; report
which feels steadier. Inspect _present.csv to accept/reject boundary hypothesis.
If neither helps, use measurements to pursue image/pose ownership or actual
projection agreement, rather than inventing a smoothing fix.

---

## 2026-09-25: F1 recording, HUDASPECT and queued-frame comparison

User confirms the eye swap looks much better and projVR expands the frame well.
Preserve `projvr=1`, `swapeyes=1`, `wide=0`. Earlier claims that projVR never
reaches the scene are superseded as a recommendation by this headset report;
actual projection geometry still needs independent validation.

Remaining user symptoms: severe camera jitter even still, head-motion lag, and
vertically stretched HUD. User briefly moved the game camera deliberately at
the start of the recording. Do not classify the entire recording as still.

### Runtime evidence and limits

Preserved F1 capture at `diagnostics/jitter-20260925-233832/`, 6585 camera rows,
131.646 seconds. See `window_analysis.json` for reproducible selected-window
measurements. At 35-40 seconds: 449 samples, median Present interval 11.022 ms,
95th-percentile raw head steps about 0.019-0.021 degrees. Live camera matches
previous Present's computed orientation to about 0.003 degrees RMS. At 95-105
and 110-125 seconds, cadence is 22.052 ms (~45 Hz), previous-update matching
remains close. This is not evidence of a need for headset damping.

Large base-camera discontinuities occur in other windows, including near 44s
with pitch alternating around +/-54.93 degrees and substantial world-position
changes. Their cause remains unresolved; a brief intentional input at the start
does not prove these later changes intentional. Need a separately marked still
test. Trace snapshots are read at Present, not at camera finalization, and do
not establish which pose belongs to the rendered Katanga texture.

Source timing: camera composition is published at Present for a following engine
frame; XR submission assumes the image uses the previous Present's pose.
`OneFrameThreadLag=True` can add engine/render queuing. As a reversible comparison,
changed ONLY that key to False in the installed generated BmSystemSettings.ini.
No frame cap, resolution, default template, or runtime changes. This is a latency
hypothesis test, not a proven jitter fix. Existing geo11 code leaves the key alone.

Reference: playbook `docs/failure-atlas.md` FAIL-PERF-014 and
`docs/19-d3d12-and-performance.md#blocking-wait-floor`, subsection "Pace the game
to the runtime's request, and tag frames with the request they answer";
checkout `b955f2244f7c5d5f3b3e5f403b1b6721a3362423`. Also vrframework guide 07
section 3 (engine/render/presenter clocks). These establish an audit direction,
not evidence that another engine's fix transfers to AK.

### HUD change: source inspection, not headset acceptance

Offline disassembly of this BatmanAK.exe verifies Scaleform SetViewport VA
0x1411aeae0 compares/copies 0x34 bytes, with uniform scale at viewport+0x2c and
pixel aspect at +0x30. Layout function 0x1411ac780 uses aspect to derive per-axis
units per pixel (e.g. 0x1411aca4e and 0x1411acafd). Corrected our old 0x38-byte
structure to the verified layout. Exact-fit mode 2 can bypass aspect correction;
that mode and full-screen overlays remain runtime acceptance risks.

Added gameplay/projVR-only pixel-aspect compensation, based on source dimensions
and measured headset FOV: tan(halfH)/tan(halfV) * height/width, about 0.513 for
2560x1440 and 42.35/45 degrees. Keeps authored aspect per movie, prevents repeated
multiplication, restores when disabled/screen mode. Panel checkbox and persisted
`hudaspect=1` allow comparison. Reuses existing guarded/rate-limited live movie
re-push; it is still a cross-thread operation and should be watched for crashes.

Reference: playbook chapter 04 section `ui-not-one-class` (same checkout above),
and vrframework guide 11's per-axis HUD scaling discussion. Projection changes
require independent HUD correction; blanket changes can affect overlays. No
shader-based extraction or claim of separate stereo UI layers.

F1 now keeps timestamped camera/mode/status files as well as the latest names,
and forces a current status snapshot (old startup logs could miss toggled projVR).

### Build and deployment

HUDASPECT Release compiled; CTest 2/2 passed (DirectInput forwarding and HUD
geometry/restoration/non-compounding/invalid-input cases). Game was closed at
deployment. Installed dinput8.dll SHA256:
`5cfd5f0c48377b3e8b48d0fba8cfd264ef0e93ebcfae9f15e2c0606e2d61c1c2`.
DLL, mod settings and generated game config preserved in
`diagnostics/jitter-20260925-233832/before-HUDASPECT/`.

Next headset test: normal launch, check HUD circles/text and menus, then stand
still with mouse/controller untouched for 12 seconds and F1; slowly turn head
for 10 seconds and F1 again. No damping added. If worse performance/jitter,
restore only OneFrameThreadLag=True first. HUD can be toggled independently.
Do not restore a stale whole game config across template timestamp changes.

---

# AKVR restart review - 2026-09-25

## Stereo polarity and stationary jitter - EYEORDER

User confirmed MODEOBS improves framing; current log proves GAMEPLAY, no crop
(cropX=0, cropW=2560), observed/render-hook FOV 84.7x54.3 matching submission.
User now reports incorrect stereo depth and jitter even while holding still.
No fresh F1 trace exists yet, so no damping or timing change is justified yet.

Evidence for swapping the source eyes:

- AKVR previously routed the left Katanga half to the left eye unconditionally.
- `references/VRScreenCap/src/config.rs:29` explicitly says geo-11 has the eyes
  swapped (possibly GPU-dependent); its default is `swap_eyes=true`. Reference
  checkout: `a2866bd353f237b9f65969a879f78366b72c6a57`.
- The user's fresh `akvr_katanga_02.bmp` also supports reversed disparity. A
  horizontal normalized-correlation comparison on its downsampled 512x288 eye
  halves found median xLeft-xRight of 12 px on the car (33 accepted patches),
  15 px on distant structure (6), and 10 px on foreground floor (17). Method:
  grayscale, 9x9 patches, 9 px sample spacing, horizontal search +/-35 px,
  correlation >0.94 and margin >0.04 over alternatives more than 3 px away.
  Sample rectangles (x0,y0,x1,y1): car (180,120,345,195), distant structure
  (210,60,310,108), floor (120,225,350,265). The near-to-far trend is inverted
  under the previous eye assignment. This is an image-based inference, not
  headset acceptance; shader effects, depth identification and texture ambiguity
  remain limitations of this small sample.

Build EYEORDER adds a native-stereo source-half swap, persisted as `swapeyes`.
Ctrl+F5 or the panel checkbox toggles it without changing FOV. Poses, eye labels,
camera offsets and AER behavior are unchanged. Set `swapeyes=1` for the next
comparison; all other installed settings were checked unchanged. Release build
passed; deployed hash verified with the game closed. Previous build is saved as
`dinput8.dll.before-EYEORDER-20260925`. Evidence/settings/captures are preserved in
`diagnostics/2026-09-25-eye-order/`. **User stereo acceptance remains pending.**

Playbook routing: FAIL-STR-002 and chapter 09's five-minute alignment diagnosis
and eye-image-delta review. Revision remains
`b955f2244f7c5d5f3b3e5f403b1b6721a3362423`. Jitter guidance: vrframework guide 07,
three frame counters, and the existing `measure-head-vs-camera-1to1` project
memory. The existing Present-to-camera pipeline assumes the previous Present's
pose rendered the current image; actual image/pose ownership remains unproven.
`OneFrameThreadLag=True` is still present; it is a hypothesis to test, not a
reason to change the known configuration alongside eye order.

Next: compare stereo with Ctrl+F5. If stationary jitter persists, remain in
gameplay, keep head/controller still for about 12 seconds, then press F1.
This dumps camera and mode ring buffers. Analyse the last ten seconds with:

```powershell
python akvr/tools/analyze_camera_trace.py 'E:/Games/Steam/steamapps/common/Batman Arkham Knight/Binaries/Win64/akvr_camera_trace.csv' --mode-trace 'E:/Games/Steam/steamapps/common/Batman Arkham Knight/Binaries/Win64/akvr_mode_trace.csv' --seconds 10
```

The analyzer reports angular steps, lean range, sample cadence, finalize-count
steps, FOV range and mode transitions. Its stationary synthetic fixture, 10 ms
cadence, time-window selection and angle-wrap checks passed. It observes CPU
samples; it cannot establish GPU/compositor behavior. Do not add a head-pose
filter without preserving agreement between the rendered and submitted poses.

## Gameplay incorrectly treated as a fixed screen - MODEOBS

The user confirmed the dinput8 build starts in VR, with geo-11's over/under
desktop preview. In gameplay, head turns moved the camera, but the view remained
window-like and appeared cropped to the left. The fresh startup log confirms
Katanga SBS 5120x1440, 2560x1440 per eye, `projvr=0`, zero rewrites, and a submitted
full FOV of only 42.4x27.1 degrees. The later raw-frustum log line falsely described
the headset's preferred FOV as submitted; MODEOBS fixes that diagnostic too.

Source-proven bug: `hkBuildProj` only counted projections inside `if (g_projVR)`,
while the gameplay classifier required that rewrite count to advance. Disabling
the ineffective projection rewrite therefore forced SCREEN after ten bad frames.
Head/camera injection is independent, explaining why head turning still worked.
SCREEN applies the saved 0.5 angular scale and a fixed pose. This explains the
small floating view; it does not yet prove the cause of the reported left crop.

MODEOBS counts matched projection observations regardless of rewriting. Only the
matrix writes and rewrite count remain conditional. Gameplay uses the independent
atomic observation counter with the existing cadence checks. Startup diagnostics
now report observation count, detector mode, effective display mode, actual
submitted FOV, and horizontal crop coordinates. Hook output is no longer labelled
as proven rendered projection. The panel no longer says to keep projVR enabled.

Release build passed. Deployed with the game closed and checked the file hash;
prior loader retained as `dinput8.dll.before-MODEOBS-20260925`. Original log/settings
are in `diagnostics/2026-09-25-screen-mode/`. Settings, stereo splitting and camera
injection are unchanged. **Headset validation remains pending.**

Next test: enter gameplay with projVR and wide mode off; verify observations
advance, mode becomes GAMEPLAY, cropX=0 and cropW=2560 at this resolution. The
baseline should submit about 84.7x54.3 degrees, retaining the known vertical
coverage limit. If left cropping persists, press F2 in gameplay and inspect the
new Katanga capture before altering eye layout. Only then test F5 for wider
vertical coverage. Do not infer a rendered scene's layout from the desktop preview.

## Launch regression and replacement bootstrap (later this session)

User reported the game ran on the desktop without VR. Steam's process log
confirmed the expected executable. Live PID 10832 loaded System32 VERSION.dll,
DXGI.dll and d3d11.dll, but neither the local AKVR version.dll nor geo11.dll nor
XR runtime modules. The AKVR logs were unchanged since August. The installed
version.dll hash matched the previous local build. This proves the bootstrap was
bypassed; the precise reason Windows selected the system version library is not
established. No global runtime or registry changes were made.

Following playbook FAIL-LOAD-001 and vrframework guide 02, added an alternative
DirectInput proxy using the same AKVR initialization and VR code. Static import
inspection confirmed BatmanAK.exe imports DirectInput8Create. The proxy forwards
the real system library by absolute path, preserves its six export names and
ordinals, and keeps the hooked mod resident. The Release build passed, as did an
isolated forwarding test creating/releasing a real DirectInput object, validating
the joystick format, resolving exports/ordinals and rejecting an invalid version.
The test intentionally does not initialize AKVR in a non-game host.

Deployed `build-dinput8/Release/dinput8.dll` with the game closed; verified hashes.
Original saved as `version.dll.akvr-before-dinput8-20260925` beside the executable,
and removed from the active `version.dll` name to prevent duplicate initialization.
**Fresh game and headset validation is pending.** No rendering settings changed.

Build: configure with `-DAKVR_PROXY=dinput8 -DAKVR_GAME_DIR=` into `build-dinput8`,
then build Release target `akvr`. `deploy.bat` now targets this build. Auto-deploy
configuration refuses an installed opposite proxy. Never run both proxies together.
Rollback with the game closed: move dinput8.dll out of the active name and restore
the saved original as version.dll. This restores the pre-change files, not a
proven solution to the current version-proxy bypass.

## Current baseline

Last recorded headset success: geo-11 simultaneous stereo with VirtualDesktopXR,
6DOF, the in-game panel and world-locked menus. The deployed/source build is
KATSHOT. No new game or headset test was performed during this review.

The outstanding issue is limited vertical coverage: about 54.3 degrees rendered
against about 90 degrees available in the headset. The last session disabled
`projvr` because changing its reported projection stretched the image without
producing the intended extra vertical scene coverage. The downstream overwrite
is a hypothesis; wrong view ownership or a discarded intermediate matrix also
needs ruling out.

Read-only inspection of the installed files on 2026-09-25 confirmed:

- `geo11.dll` and `version.dll` are present.
- `direct_mode=katanga_vr`, `shader_regex_patch_mode=4`, `calls=0`,
  `unbuffered=0`, `load_library_redirect=0`.
- `katfeed=1`, `projvr=0`, `vpsquare=0`, `wide=0`.
- Separation 30, convergence 100, automatic convergence off.

The stored startup log confirms the historical HOOK-mode run, a 7680x2160
shared stereo surface and 3840x2160 per eye. It was captured with `projvr=1`
before the setting was disabled, so it is not a current runtime observation.
The existing Katanga capture shows two full-height landscape eye images.
No BatmanAK process was found during this review.

## What the new reference changes

Reference: `references/vr-modding-playbook` at
`b955f2244f7c5d5f3b3e5f403b1b6721a3362423`.

- [CAM-014](../references/vr-modding-playbook/docs/pattern-catalog.md#cam-014)
  and [FAIL-RE-001](../references/vr-modding-playbook/docs/failure-atlas.md):
  establish that the modified view belongs to the world render. A successful
  write or plausible FOV is insufficient.
- [RE-004](../references/vr-modding-playbook/docs/pattern-catalog.md#re-004):
  require an independent visible effect with off/on controls. AKVR's immediate
  read-back in `camera.cpp` does not establish what reaches the rasterizer.
- [Projection authority](../references/vr-modding-playbook/docs/09-d3d11-openxr-injection.md#projection-authority)
  and [STR-011](../references/vr-modding-playbook/docs/pattern-catalog.md#str-011):
  submission must describe the actual world image. Declaring the headset's larger
  FOV cannot create missing scene coverage.
- [CAM-010](../references/vr-modding-playbook/docs/pattern-catalog.md#cam-010):
  if the engine overwrites a proven world projection, a later upload is a candidate
  intervention, with culling kept consistent. This is a fallback investigation,
  not a validated patch for AK or geo-11.
- [Subimage/FOV pairing](../references/vr-modding-playbook/docs/09-d3d11-openxr-injection.md#subimage-fov-pair):
  a crop must retain the FOV represented by its actual integer pixel rectangle.

The existing [vrframework engine-tweaks guide](../references/vrframework/guides/14-engine-tweaks-and-quirks.md)
also supports identifying and proving the engine-side framing branch before
patching it. Neither reference supplies an immediately proven AK fix.

## First bounded test: existing wide-render path

Source inspection found a lower-effort candidate already implemented:
`camera.cpp` computes a wider horizontal FOV to obtain the headset's vertical FOV
in a 16:9 render; `xr.cpp` crops each eye horizontally; F5 toggles `wide` in
`hooks.cpp`. The crop branch also serves the native stereo path. This is source
evidence only: no geo-11 runtime acceptance was found.

Hypothesis: widening the game's existing horizontal FOV will expose more vertical
scenery while preserving the 16:9 buffer shape that already works with geo-11.

Control: working 3840x2160-per-eye setup, `projvr=0`, `vpsquare=0`, `wide=0`.
Variable: F5 toggles only wide-render mode. Keep camera location, resolution,
separation and convergence fixed. Use a stationary gameplay scene with obvious
near/far landmarks; compare the Katanga captures made with F2 and the headset.

Pass: additional vertical scene coverage, stable object proportions and stereo
depth, no edge disappearance during head motion. Check HUD, Batmobile reticle,
menus and a cutscene before retaining the mode. Expect a sharpness cost because
the headset uses only the central horizontal portion of each rendered eye.

Fail: zoom/stretch without additional scene, clipping, or disturbed depth.
Toggle F5 off to return to the control. Do not treat a changed diagnostic alone
as success. If this fails, trace the main-world projection producer/consumer and
the existing disabled aspect-constraint patch before changing render dimensions.

Before shipping this path, verify per-eye asymmetry, integer crop/FOV agreement
and non-gameplay behavior; its current symmetric assumptions are not validated
by the playbook. The reported square-resolution depth failure remains a reason
to avoid jumping directly to a square buffer, not proof that all other ratios fail.


## 2026-09-26: PRESENTPAIR result and POSEDELAY test

Captures preserved in `diagnostics/presentpair-20260926-000542/` (ON at 00:05:42,
OFF at 00:06:59; the second present CSV also contains the ON rows).

Runtime measurement:
- Sampled Katanga pixels changed DURING the original Present in 0 of 124 (ON)
  and 0 of 40 (OFF) gameplay samples. The shared image is already final before
  our hook calls Present, so the PRESENTPAIR premise is not supported. Setting is
  now OFF (`nativeafterpresent=0`) and should stay off (it can only add latency).
- Frame interval 22.2 ms (45 Hz) in both states. xrWaitFrame holds 13-15 ms of
  each 22 ms frame: by chapter19 #blocking-wait-floor / FAIL-PERF-012, a large
  wait means the app is being held back, not arriving late. Half-rate cause
  (Virtual Desktop setting vs GPU) not yet determined.
- Camera trace, last 15 s of each: base camera constant, one finalize per
  Present, gameplay 100%, FOV 84.7 constant. Head moved slowly (median yaw step
  0.3-0.5 deg per frame).

Headset report (user): no ON/OFF verdict given yet. New observation: "the HUD
elements in game appear a lot more stable than the 3D elements."

Interpretation (hypothesis): the HUD is fixed in the image, so it follows the
SUBMITTED pose; the 3D scene follows the pose the game actually RENDERED with.
Jitter confined to 3D means those two differ frame to frame. `xr.cpp` hard-codes
the gap between publishing a pose and presenting its image as exactly 1 Present;
that gap has never been measured. Playbook lead: chapter19 "tag frames with the
request they answer" and FAIL-PERF-014 (checkout b955f2244f7c5d5f3b3e5f403b1b6721a3362423).

POSEDELAY build (source change, deployed, headset test pending): the submitted
pose now comes from a ring of recent poses, `posedelay` 0-3 (default 1 = previous
behaviour). Panel slider "head-pose delay, frames"; Ctrl+F7 steps 1->2->3->0.
Status file and `_present.csv` (new `pose_delay` column) record it. 3/3 tests
pass. DLL SHA256 31E98C67F48F32007C20F07105D4CE0DD39DAFD5E2ECA99F42C0E1C959AE140C.
Rollback: `diagnostics/presentpair-20260926-000542/before-POSEDELAY/`.

Pass: one delay value makes the 3D world as steady as the HUD during slow head
turns. If none does, the gap probably varies frame to frame; next step is a real
identity tag (pose id carried through the camera finalize to the image) rather
than a fixed delay.
**Headset result (user, 2026-09-26):** `posedelay=2` looks "steady as a rock". Gameplay jitter resolved by pairing each image with the pose from two Presents earlier. Saved as the setting; the hard-coded 1 was wrong for this pipeline. Not yet tested: fast motion, Batmobile, cutscenes, and whether the gap stays 2 at other frame rates (it is a fixed assumption, not an identity tag). Next issue per user: low resolution.

## 2026-09-26: LEVELRES — resolution ceiling and decoupled pitch

Resolution (runtime measurement): with BmSystemSettings ResX/ResY=3840x2160 the
startup log still showed backbuffer 2560x1440 and Katanga 5120x1440; user saw no
change. Source cause: `earlyres.cpp` forced the fake-monitor ceiling OFF whenever
geo-11 was detected, so the game clamped its ini size to the 2560x1440 monitor
(the clamp behaviour was measured 2026-07-27, build BIGRES-1). LEVELRES keeps the
ceiling under geo-11 but forces it to 16:9; the ini still sets the size, so the
square shape that broke geo-11 depth (GEO11_PLAN section 4) cannot come back.
`bigres=1` installed. Headset result pending; expect 7680x2160 Katanga.

Camera (user report): correct at eye level; with the camera lowered or raised,
looking left/right also rolls. Head yaw is applied about the game world's
vertical; a tilted game camera makes that axis differ from the player's neck.
Applied vrframework guide 09 section 7 "Decoupled pitch": in gameplay the game
camera's pitch is scaled by `pitchkeep` (default 0 = level horizon, 1 = old
behaviour); screen-mode menus keep the game's framing. Panel slider "game camera
tilt kept". Risks to check: gadget aiming, gliding/perch framing, cutscenes.
DLL SHA256 193B6117AA29B808333F95B53EAF7610152BA4EABE9F6E0D8BBDB3CF7B7AC180.
Rollback: `diagnostics/presentpair-20260926-000542/before-LEVELRES/`.
## 2026-09-26: FRAMELOG — Batmobile stutter; resolution still clamped

Headset report (user): LEVELRES still launched at desktop size. In a Batmobile
scene, ~90 fps while stationary; driving through the city is "a stutter fest",
unplayable. Performance now outranks resolution.

Resolution (runtime log): the fake monitor was active (GetSystemMetrics, work
area and monitor info all spoofed to 5248x3085) and the game still created
2560x1440 with ini ResX/ResY=3840x2160. Unspoofed: EnumDisplaySettings(current)
returned the real 2560x1440 mode. It is the leading suspect for the clamp, but
unproven. Parked: a bigger render adds GPU load.

Instrument before designing a fix (chapter19 #blocking-wait-floor): FRAMELOG
adds `_frames.csv` to F1, one row per Present (8192-frame ring, ~90 s): interval,
xrWaitFrame, XR begin, submit, original Present, total hook time, camera
finalize count. Large wait = held back; near-zero wait with long intervals =
genuinely slow; a long Present or hook time = our/driver cost.
DLL SHA256 82E3F6D2C12BB2F1ECCA38D52E5A6BCB4A80E4FF8BC90221B8C32EB69C74C9C1.
### FRAMELOG result (runtime measurement, `diagnostics/batmobile-20260926/`)

Second F1 (00:40:50), 113 s, 3692 gameplay frames. User: "not as bad this time,
but still stuttering". Headset link input: gameplay ran at a steady 45 Hz (median
22.2 ms), never 90. Game time per frame = interval minus xrWaitFrame: median
11.2 ms, p10 7.2, p90 15.4, so it narrowly misses the 11.1 ms budget for 90 Hz and
the runtime holds it at half rate. Stutter = 103 frames over 30 ms, clustered
while driving, up to 415 ms. In those frames our hook time was normal (median
10.9 ms, mostly the headset wait); the extra time was all OUTSIDE our hook. The
original Present median was 0.08 ms. The stalls are the game's own work, not
AKVR. One camera finalize per Present in 99.3% of frames.

Change: generated `OneFrameThreadLag` False -> True (the game default, and the
state before Codex's 2026-09-25 jitter experiment, which recorded no jitter
benefit). True lets the game and render threads overlap. Only that line edited,
no BOM, backup in `before-THREADLAG/`. Expected risk: the pose/image gap may
change, so re-check `posedelay` (likely 2 or 3) with Ctrl+F7.
**Headset result (user, 2026-09-26):** with OneFrameThreadLag=True the game ran "a lot better", up to 90 fps, much smoother, less stuttering. Keep True under geo-11. User now wants an AER/geo-11 choice at launch, and more resolution now that there is headroom.

### THREADLAG result and WINDOWMODE0 (2026-09-26)

Runtime measurement (`batmobile-20260926/threadlag-true/`, F1 00:46:50): 7458
gameplay frames in 89 s = 83.6 fps average (before: 44.3); 97% of intervals at
~11 ms; hitches over 30 ms fell from 1.24/s to 0.68/s. Game time median 10.8 ms.
New: camera finalizes per Present are 0 or 2 in ~8% of frames (before: 0.7%),
so the fixed posedelay=2 pairing can slip on those frames. If jitter returns,
try posedelay 3 or build the identity tag.

Resolution: generated config had `WindowDisplayMode=1` (the borderless,
desktop-sized mode) under geo-11, because the mod stands down on window keys when
geo-11 is present. The AER-era known-good big-render config was
`Fullscreen=False` + `WindowDisplayMode=0` (memory ak-comfort-settings-forced).
Changed only that line to 0; ResX/ResY 3840x2160, bigres=1 ceiling active.
Backup `before-WINDOWMODE0/`. Headset result pending.
## 2026-09-26: DISPMODE — display-mode spoof, launch focus; pose delay varies

Resolution: WINDOWMODE0 changed nothing (still 2560x1440 backbuffer; the log
showed only EnumDisplaySettings(current) reporting the real 2560x1440). No AK file
exists under AppData/Documents WB Games. DISPMODE fakes the current/registry
display mode when bigres is on and appends 3840x2160 and 5120x2880 to the mode
list; every list is logged. If this also fails, the remaining suspect is the
DXGI output (desktop coordinates / mode list), which IAT spoofing cannot reach.

Launch focus (user): the game window needs a manual taskbar click. DISPMODE
claims foreground for the first 20 s (AttachThreadInput + SetForegroundWindow,
once per second, stops after 3 in-front checks).

Pose delay (headset report): 3 steady in the garage, 2 steady outside. Runtime
(`windowmode0/`, F1 00:55:42): ~90 fps with xrWaitFrame at ~0 ms (the game is at
the budget); camera finalizes per Present vary between sections. A fixed
Present-count delay cannot hold in both. Planned fix (chapter19 identity tag):
use UE3's game-thread frame number and render-thread frame number, recording
which pose each game frame consumed and reading which frame the render thread
presents. Requires locating those counters (runtime scan) and a small stub
change. Not built yet.
DLL SHA256 B068BDEC878A1086C9BC4EA78A1D42D4B41144C7C2528D609533C3B7E61B13D9.
## 2026-09-26: ENGINE169 — engine-res patch under geo-11, 16:9 only

DISPMODE result (user): still desktop resolution, and the game started
minimized. Log: the display mode was faked, and the mode list already held
3840x2160 (377 real modes). The game still called AdjustWindowRect with
2560x1440, so it sizes itself from something import patching cannot reach (DXGI
is the leading suspect). No config file holds 2560x1440.

Source + static check: the engine-res patch (writes AK's ResX/ResY globals,
proven on the AER build at 3184x3500) was disabled under geo-11. Its "SIGNATURE
NOT FOUND" status was misleading: it printed because the patch was never
attempted. Its byte signature matches BatmanAK.exe exactly once (file offset
0xaeccc9). ENGINE169 runs it under geo-11, always 16:9 derived from `engineres`
(set to 2160, so 3840x2160). The focus claim now also restores a minimized
window. DLL SHA256 17843DB4469800C7DAC46BCABF47E0274D5E2F8C7D97B346C9213D841EB89186.
Rollback: `batmobile-20260926/before-ENGINE169/`. Headset result pending.
## 2026-09-26: ENGINE169 result; TIDYPANEL build

ENGINE169 WORKED (runtime log + user): backbuffer 3840x2160, game asked
3840x2160, Katanga 7680x2160 SBS, "engine render: FORCED 3840 x 2160". User:
sharper, with a performance drop (expected: 2.25x the pixels of 2560x1440).
Side effect: the desktop was left at 3840x2160 after the game quit (it was
2560x1440 earlier the same night). BatmanAK.exe imports no
ChangeDisplaySettings* function, so another module in the process changed it.
Suspects: geo-11 and the existing DXGI output hooks' injected mode list.

TIDYPANEL:
- Desktop guard: MinHook on user32 ChangeDisplaySettings{,Ex}{A,W}, installed
  from install_creation_hooks. It refuses any mode change with a non-null mode
  and logs the calling module in the startup log ("desktop guard: REFUSED ...").
- Controller: the overlay chord moved from Menu+View to BOTH STICK CLICKS held
  0.6 s. View alone opened the map before Menu could join the chord.
- Overlay rebuilt: status line; VIEW (world scale, horizon tilt, HUD size, menu
  screen size, floating-menus toggle, Recenter button); WHERE YOU STAND (offsets
  plus Reset); SMOOTHNESS (head-pose delay, geo-11 only); SHARPNESS (render
  height with next-launch size). Everything else is under Advanced (projVR, HUD
  proportions, geo-11 feed, eye swap, Present-timing test, AER spin correction,
  FOV trim, size helpers, image-detail tree), Diagnostics (incl. ROLL), and
  Keyboard shortcuts. No setting key was removed.
DLL SHA256 EB451AAA672CE2F0F78C00CBD06A77FF8F6ED06FD47105146A9C99E50DAF2BAC.
Rollback `batmobile-20260926/before-TIDYPANEL/`.
**CORRECTION (same night):** the desktop was NOT left changed. Win32_VideoController reports one mode per adapter, and it had read the second monitor (DISPLAY2, 3840x2160 native, scaled to 2560x1440 logical). The game monitor DISPLAY1 is still 2560x1440 current and saved. The rescaling the user saw during play was most likely the 3840x2160 window crossing onto the 4K, differently scaled monitor. The desktop guard stays as harmless insurance and logging. Check per-monitor modes with EnumDisplaySettings per device, never Win32_VideoController.

## 2026-09-26: GUARDLOG — the desktop guard broke the image; now log only

User: after TIDYPANEL the headset showed only the left part of the image, with
shearing and squashing on head motion. Log: "REFUSED ChangeDisplaySettingsExW
3840x2160 flags 0x4 from dxgi.dll". DXGI switches the GAME monitor to the render
size for the session (CDS_FULLSCREEN, temporary, undone at exit), which is the
rescaling the user saw under ENGINE169. Refused, the backbuffer stayed 2560x1440
(Katanga 5120x1440) while the engine rendered 3840x2160. Settings unchanged. The
panel rebuild was not involved. GUARDLOG passes every call through and only
logs it ("display mode: ..."). Consequence: at this render size the monitor must
accept the mode, and 3840x2160 is in its list. A size missing from the mode list
may fail the same way. Watch the startup log when trying 1800.
DLL SHA256 8082DF8BDF8253570DAAB43C149D38E6C8EE27828B71E9AEAF20EF1D57737430.
## 2026-09-26: WINDOWED — render size independent of the monitor; focus

User: no window focus, so the controller did not work, and the user was afraid
to click the window in case it dropped back to monitor resolution. Diagnosis
(source + log): the game ran exclusive fullscreen at the render size (dxgi.dll
switched the game monitor to 3840x2160, flags 0x4). Consequences: the render
size is limited to modes the monitor offers, so a plain 2K monitor stays at 2K;
and a fullscreen swapchain minimizes on focus loss (the "starts minimized" and
dead-controller reports).

WINDOWED: `g_forceWindowed` and `g_forceRes` now default ON (settings windowed=1,
forceres=1). That is the August AER toolkit: windowed swapchain, window and
buffer forced to the render size, max-track ceiling raised, caption stripped,
per-frame resync. keep-focus handles input for a background window; that was
the July failure that made windowed default off. New: SetFullscreenState(TRUE)
is refused (slot 10), Alt+Enter is disabled with MakeWindowAssociation, and
ResizeBuffers is pinned to the forced size. Expected log: no "display mode ...
from dxgi.dll" line; backbuffer 3840x2160; possibly "fullscreen request
refused". DLL SHA256 938633C501D8A5FB6B501D008A45CD3294D9C26B150C6872AB83C56616354AD6.
Rollback `batmobile-20260926/before-WINDOWED/` (restores windowed=0, forceres=0).
## 2026-09-26: NOMINIMIZE — windowed works for resolution; window minimized

WINDOWED result (user + log): no monitor mode change (good). Backbuffer
3840x2160, Katanga 7680x2160, headset GAMEPLAY. But the game hid in the
background, could not be brought forward manually, and the controller did not
work. Log: the game kept requesting fullscreen (refused), and the resync logged
"window client 0x0" with 117 attempts, i.e. the window was minimized.
Hypothesis: the game still believes it is fullscreen, and a fullscreen UE3 game
minimizes itself on deactivation; a minimized window loses pad input.

NOMINIMIZE: GetFullscreenState reports what the game asked for (the swapchain
stays windowed); ShowWindow minimize commands for the game window become
SW_SHOWNOACTIVATE (logged); WM_SYSCOMMAND SC_MINIMIZE is swallowed; the window is
restored whenever it is found minimized (every 1 s max, whole session);
GetFocus is faked for the game's own thread (keep-focus covered only
GetForegroundWindow/GetActiveWindow and the activation messages). All of it is
gated by keep-focus (F3).
DLL SHA256 FAB604745AED3789CB4041ECF4B37BDE3C954FD8B1A6D8E6B66B1C84C5BC28B9.
**NOMINIMIZE result (user, 2026-09-26):** "that's better now". Windowed at 3840x2160 works: controller OK, window stays up, no monitor mode change. Keep windowed=1, forceres=1, keep-focus on. New issues reported: (1) HUD scale-down still crops the edges; (1b) main menu too big and head-locked; (2) map/pause drops to a small floating window; (3) the outer edges of the view are stretched and blurred (geo-11's green FPS counter in a corner reads as a smudge).

## 2026-09-26: MENUMODE — main menu auto-detected; pause/map full size

Source (xr.cpp): automatic SCREEN states (pause, map, loading, where camera
projections stop) shared the manual screen size (screen=0.5), hence "a window in
front of you". The main menu runs a live camera, so the detector called it
GAMEPLAY: head-locked and full size.
MENUMODE:
- `pausescreen` (default 1.0) sizes the automatic screens; `screen` (0.5)
  sizes the main menu and F6.
- `automainmenu` (default on): the first camera-live stretch after launch is
  treated as the main menu (screen mode, camera frozen, world-fixed). The first
  non-gameplay stretch over 1.5 s after it (a loading screen) ends that phase for
  the session. F6 / the panel box leave the phase early. Startup-log mode label
  MAIN_MENU.
Heuristic, headset-unverified. Known gap: "quit to main menu" later in a session
is not re-detected (F6 works).
HUD edge crop and edge blur: NOT changed; waiting for F2 captures (Katanga
image) before touching them. Source note for the crop: scale_viewport shrinks
the Scaleform viewport but not its scissor rect, and hudaspect changes pixel
aspect; either could crop.
DLL SHA256 1DCDBE7F321184BC2659C543CBD598883C1A545996AFAA34EC63AEA6DE0DCE50.
## 2026-09-26: HUDFIT — HUD crop, squeezed pause/menu, main-menu timing

MENUMODE headset result (user) and F2 captures (`diagnostics/menumode-20260926/`):
- The splash suddenly grew, and the main menu was still big and head-locked.
  The phase heuristic fired on a startup blip and closed before the real menu.
- Pause and main menu looked horizontally stretched. Katanga captures show those
  3D scenes drawn anamorphically through projVR (like gameplay), but presented
  at the 16:9 screen shape because they are not GAMEPLAY: projections are built,
  but there is no camera finalize.
- HUD at a reduced scale (katanga_06 / frame_06): the Batmobile gauge is sliced
  at its outer edge.
- geo-11's FPS counter sits in the extreme top-left corner of each eye, at or past
  the visible edge (the view reaches about 39 deg up, the image 45 deg).
  Edge blur: not diagnosed; F2 images are downscaled to 480 px per eye.

HUDFIT (source changes, headset-unverified):
- Presented shape follows the picture: `g_lastAnamorphic` (10-frame hysteresis
  on projection builds) -> xr presents at the projVR FOV even outside gameplay,
  and the HUD pixel-aspect squash follows the same flag instead of gameplay and
  screen mode. The HUD size slider still applies in gameplay only.
- HUD crop: scale_viewport now also scales the viewport HEIGHT by the aspect
  factor. The pixel aspect alone made the logical viewport ~0.51x16:9, so the 16:9
  movie lost its sides inside any rectangle.
- Main-menu phase: opens after 3 s of continuous gameplay-looking frames and
  closes after 5 s of screen. The mode timeline is logged in the status file
  ("mode timeline:").
## 2026-09-26: FULLVIEW — edges, HUD flicker, main-menu memory

HUDFIT result (user): pause/map better, but some map icons were squished
vertically and flickered. The in-game HUD scaled better, but some elements
flickered. The main menu was scaled better, but started big and head-locked for
the 3 s confirmation window (timeline: game at 8.1 s, MAIN MENU on at 11.2 s).
Quitting back to the main menu later left it head-locked again. Black borders
remain at left, right and bottom (the top is actually overshot).
Mode trace (`hudfit-20260926/`): the squeeze flag flipped once in 187 s, so it
is not the flicker source. The main menu cross-fades between two camera
positions (user).

FULLVIEW (source, headset-unverified):
- HUD flicker suspect: the game re-sets every movie each frame from the global
  UI rectangle, which was scaled but not squashed, and the hook then squashed
  it. The global rectangle now carries the squash too.
- The main menu floats from launch (phase 0 counts as screen). While confirmed
  it records up to 16 camera positions (1 s samples, merged within 200 uu). After
  any loading screen of 5 s or more, a camera within 500 uu of one of those spots
  in the first 2 s re-enters the main-menu phase ("MAIN MENU again" in the timeline).
- Full view (`fullview`, default on): horizontal render = the widest outer eye
  edge (49.34 deg, both eyes share one projection), which spends the 16:9
  frame's surplus width. Vertical = an off-centre projection, m[9] += (tD-tU)/(tU+tD)
  (= 0.193), and the layer is submitted with the exact up/down edges. Status
  line "full view: ON (vertical centre shift ...)".
  Risks: the geo-11 fix shaders may assume a centred projection (watch shadows,
  lighting and depth); the wider FOV may change perceived stereo depth and world
  scale; the HUD now spans the wider view.
Answer to "is the aspect optimal": no. 16:9 (required by the geo-11 fix, since a
square render broke depth on 2026-08-06) oversamples horizontally about 1.9x
relative to vertical at 85 x 90 deg. FULLVIEW turns that surplus into
coverage.
## 2026-09-26: VPLOG + SIZELATCH

FULLVIEW result (user): the start was right (straight into the menu, correct
size, not head-locked). Loading screens started right, then grew before the
game (the main-menu phase ended 5 s into the loading screen, switching from menu
size to pause size). Pause and map at 100% were too big. HUD still flickers; map
icons are still stretched and flickering. The user set posedelay=3 in this session.
Runtime log: the headset reported LARGER frusta this session than earlier the same
night: L -54 / R 40 / U 44 / D -55 (round numbers; earlier -49.34 / 35.36 / 39.43 /
-50.57). FULLVIEW followed them: 108 x 99 deg submitted, so pixel density dropped.
Cause of the change is unknown (Virtual Desktop setting?); asked the user.

VPLOG (installed on game exit by a background waiter):
- SIZELATCH: a screen's size is chosen when it appears and held until gameplay
  returns; pausescreen default and installed value 0.5 (= the main-menu size).
- Viewport log: every Scaleform SetViewport (V = game through the vtable hook,
  F = our post-fix, P = live re-push) with its in/out rect and aspect, thread and
  QPC time (same clock as _frames.csv). F1 writes `akvr_*_viewports.csv`. The
  flicker fix waits for this data; two reasoned fixes did not cure it.
## 2026-09-26: HUDUP

User: "the very top HUD element feels too low now". FULLVIEW's off-centre
projection puts the frame's middle row about 12 deg below straight ahead (the
tan midpoint of +44/-55 deg), and the squashed HUD was centred on the frame.
HUDUP raises squashed HUD viewports, and the global UI rectangle, by
offset*H/2 px so the HUD is centred on straight ahead. Includes VPLOG and
SIZELATCH. DLL SHA256 55CECB423315F9330A4A7030AF6245FED3AE320904AF3AB998F5FF429531050A.
## 2026-09-26: MENU3D — flat floating screens, experimental live 3D menu, Meta recenter

User: in the floating main menu, the 3D Batman "is fixed and kind of warps when
your head moves", while the menu text is right. Explanation: the text and the
3D scene are ONE image from one camera. A stereo image frozen in the world warps
under head motion (the 3D-TV effect). The user asked for both options:
- MENUFLAT (`menuflat`, default on): while a screen floats (!effGameplay), both
  XR eyes copy the same Katanga half, so the picture is flat.
- MENU3D (`menu3d`, default off, experimental): during the main-menu phase the
  view stays live and head-tracked (not screen mode; base pitch kept). Every
  Scaleform viewport, and the global UI rectangle, is offset by the camera's
  applied head yaw/pitch since the menu appeared (tan(angle)/tan(half-FOV) x
  half the buffer), so the text reads as pinned in the world. It is sized by the
  main-menu size and re-pushed every frame from the Present thread. Known risks:
  per-frame Scaleform re-layout (menus crawled on 2026-08-05); cross-thread
  push (guarded by __try); no roll compensation.
- Meta-button recenter: the mod ignored XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING.
  For LOCAL space it now waits until changeTime, then calls
  akvr_head_recenter(), re-hangs the floating screen and re-bases MENU3D.
  Logged as "runtime recenter" in the mode timeline. Whether Virtual Desktop sends
  the event is unverified.
## 2026-09-26: UIGATES — the flicker, measured and fixed

MENU3D result (user): MENU3D "seems good" for the text, but the 3D element's
camera ignores head movement. The top HUD element is still a bit too low. The
map HUD still keeps a 16:9 band. The "overlay on the menu screen gets cropped"
(unclear whether that means the menu text or the AKVR panel). The screen edges
are still stretched and blurred. The user asked for separate HUD width and
height sliders.

Viewport log (menu3d-20260926/*_viewports.csv), runtime measurement: several
Scaleform movies have their OWN buffers (1024x768, 1024x720, 256x192): they are
drawn into textures (map icons etc.). The same movie alternated between the
game's texture-sized viewport (V rows) and the screen HUD rectangle (P rows /
global rect). Cause (source): the HUD gates (gate 3 = [movieDef+0x110] non-null,
the render-target movies) were opened whenever the HUD size != 1. Since HUDFIT
also opened them for the squash, they were effectively always open. The
flicker began in the HUDFIT session, the first with HUD size 0.98.

UIGATES:
- The gates never open. scale_viewport touches only viewports whose buffer
  equals the game's UI base rect (the screen). Render-target movies are left
  alone entirely.
- One hud_rect() computes the rectangle for every movie AND the global
  rectangle: width = W x `hudscale`; height = H x `hudscalev` x f; on menu
  screens with `menuuifill` (default on) height = full H (pixel aspect keeps it
  unstretched). HUDUP raise, MENU3D shift, then clamped inside the frame.
- Sliders "HUD width" / "HUD height"; save on release (IsAnyItemActive).
- F2 also writes akvr_katanga_NN_full.bmp at full resolution (for the edge blur).
- MENU3D: the main-menu camera does not take head injection (a different camera
  from the hooked one; this also explains the original "stuck to the face").
  The deploy resets it to off. Finding that camera needs RE.
## 2026-09-26: HUDPOS — MENU3D crash, HUD sliders, edge-blur verdict

UIGATES result (user): map icons no longer flicker. Entering and leaving the map
shows a brief re-scale flash. The bottom-right HUD radar still flickers. The
width slider scales everything proportionally; the height slider scales
everything and crops. MENU3D was "all sorts of weird", and pressing F1 with it
on crashed the game: "Pure virtual function being called" (MSVCR100, all
frames in BatmanAK.exe; no AKVR frames on that thread).
Diagnosis:
- MENU3D pushed Scaleform viewports from the Present thread every frame while
  the game built and destroyed menus on its own thread. That is the likeliest
  way to leave the game calling into a half-destroyed movie. __try cannot catch
  _purecall. MENU3D is REMOVED: the setter forces it off.
- The viewport log (uigates-20260926/*_viewports.csv) shows no per-frame
  re-sets in gameplay, only changes on slider or mode moves. So the radar
  flicker is NOT the HUD code. Cause unknown; asked the user to describe it.
- Separate HUD height: the game lays out its HUD as one picture (proportional
  scaling, or a crop when the logical aspect changes). Per-axis insets are not
  reachable through the viewport. Replaced with "HUD up/down" (`hudraise`,
  fraction of height, gameplay HUD only); height follows width again.
- Edge blur VERDICT: the full-resolution Katanga capture (akvr_katanga_01_full)
  is sharp to the corners; geo-11's FPS counter is crisp. The stretch and blur
  seen in the headset is added downstream: Virtual Desktop encode/resample and
  the lens. Next levers: VD quality, bitrate and sharpening settings.
## 2026-09-26: MENULIVE + PROJTIGHT

Runtime measurement (uigates-20260926, F1 at 03:02:56, main menu re-entered at
249.5 s with MENU3D on): head tracking DOES reach the main-menu camera in live
mode. Final yaw = base + head (head -9 deg); the game's camera reads back the
same yaw. The earlier "ignores head" report is unconfirmed; the crash is blamed
on the per-frame text pinning, not the live camera.
MENULIVE: `menu3d` switch restored as "main menu: live 3D, follows your head".
No pinning (the offset always reports no shift) and no per-frame push; the text
is in view at the main-menu size (hud_eff = menu zoom), clamped like the HUD.
Set on for the user.

Radar: the user reports it "blinks on and off, randomly, very very quickly".
Source hypothesis: projVR's main-camera test accepted ratios 0.45-0.72, and a
1024x720 render-target view is 0.703, so the radar's own projection could get
the headset FOV plus the FULLVIEW m[9] offset. PROJTIGHT: the 16:9 band is now
0.55-0.575 (main camera 0.5625). Every projection shape built is counted in the
status file ("projections built (ratio@HFOVxcount)") to confirm or refute.
**Decision (user, 2026-09-26):** pause, map and loading screens at full size (pausescreen=1.0) to avoid the resize flash. HUD layout stays one picture: use HUD size plus HUD up/down only; no vertical stretch. Installed build PROJTIGHT, untested in the headset.

## 2026-09-26: FRAMEID — exact pose/picture pairing from the engine's own counters

Why: a fixed pose delay was steady at 3 in the garage and 2 outside (user). UE3's
game thread runs ahead of its render thread by a varying amount. Reference:
vrframework guide 07 section 3 (engine / render / presenter clocks; "the mod never
invents a frame number, it borrows the engine's"); playbook ch19
#blocking-wait-floor "tag frames with the request they answer" (checkout
b955f2244f7c5d5f3b3e5f403b1b6721a3362423).
Design (src/frameid.cpp, source only, headset-unverified):
- After 300 steady gameplay Presents: three snapshots of the exe's writable
  sections, 32 Presents apart; candidates = dwords that rose by 20-44 both times.
- 300 Presents of tracking. RENDER = +1 on every Present. GAME = within 1 of the
  camera-finalize count, and NOT strictly +1 (it follows the finalize 0/2 steps;
  up to 6 retries if a stretch is perfectly steady). Pair with RENDER - GAME
  in {0, -1}. c = max(finalize - GAME).
- xr.cpp: g_seqForFin[finalize] = the pose sequence that finalize consumed
  (recorded each Present before the new pose is written). The pose ring is now
  64. Presented picture's finalize = RENDER + c, so the layer pose is exact.
  Delays outside 1..30 are rejected. Falls back to the fixed `posedelay` when not
  locked or when `poseauto` = 0.
- Status: "head-pose delay: N fixed, M used now (EXACT|fixed); exact matches so
  far: d:count", plus the frameid state line. _present.csv pose_delay column =
  the delay actually used.
Validation to do: the matched-delay histogram should read mostly 2 outdoors and 3
in the garage, matching the user's manual finding.
## 2026-09-26: GEOSHAPE — non-16:9 render test, pause pose, menu pitch

PROJTIGHT runtime log: every projection built has ratio 0.562 (main camera at
99 deg; a 75 deg one ~2.5k times, probably before the FOV lock; transition
FOVs). NO 0.703 radar projection exists, so the PROJTIGHT radar hypothesis is
refuted. The radar blink cause is unknown; next suspect is geo-11 per-eye
handling.

User, FRAMEID session: the main menu looks better, but the live menu with the
game's full pitch rolls Batman on head yaw. Entering pause, the frozen view
appears slightly rolled versus what was being looked at (F2 before/after).
Smoothness not yet tested. The user added a 3Dmigoto override to d3dx.ini:
[TextureOverrideAllNonSquareRT] match_type=Texture2D,
match_bind_flags=+render_target, match_width=!height, StereoMode=1.
Relevant: 3Dmigoto treats SQUARE surfaces specially (surface_square_createmode;
d3dxdm.ini carries a commented square-RT mono override). That is the likely
reason the 2026-08-06 2688x2688 render lost its depth, rather than "16:9 only".

GEOSHAPE:
- `geo11shape` (width/height x1000, default 1778) replaces the forced 16:9
  under geo-11; persisted by hooks.cpp. Test value 1143 with engineres 2688 =
  3072x2688 (8.26 MP, about the same as 3840x2160). Under FULLVIEW the per-eye
  tan-space shape is ~2.33 x 2.04 = 1.14, so this gives equal detail per radian
  both ways (~1318 px/rad; was 1648 horizontal / 1060 vertical).
- Pause pose: on gameplay -> screen with an anamorphic frame, the screen hangs
  at the pose the LAST finalize consumed (g_seqForFin) instead of level(head).
- Main-menu pitch: the live menu now uses `pitchkeep`, like gameplay.
Geometry note (for the user's "other mods offer not decoupling" question): the
game's pitch cannot be kept without either rolling the horizon on head yaw
(local composition) or the image not matching head motion (world-up
composition). Non-decoupled modes in other mods accept the horizon roll; there
is no third composition.
## 2026-09-26: UNLOCK169 — 3072x2688 keeps geo-11 depth; enable the aspect un-constrain

GEOSHAPE result (user, headset): 3D stereo "seems fine" at 3072x2688 with the
non-square-RT override. The first non-16:9 render that keeps geo-11 depth;
supports the square-surface explanation for the 2026-08-06 failure. But the
field of view is too small and "everything is up high": a small window you
look up to. Consistent with AK's bConstrainAspectRatio: the game draws a 16:9
slice inside the taller frame. Startup log: engine FORCED 3072x2688, Katanga
6144x2688, game UI rect 2560x1600, vpsquare wanted=0.
UNLOCK169:
- `vpsquare=1`: RE02 patch at 0x140112dd0 (je -> jmp to the un-constrained
  path), applied at startup. First use under geo-11. projVR's frameShaped band
  (eye H/W +-12%, 0.875 here) should match the now-unconstrained main camera.
- HUD screen-size test compares movie buffers with the forced engine size, not
  the game's UI rect (2560x1600, which no longer equals the render).
Check in the next log: "vpsquare ... applied=1 on=1", the projVR "game built"
ratio ~0.875, and gameplay framing filling the view.
## 2026-09-26: MODELIST — the 2560x1600 fallback

UNLOCK169 result: still a small high window. The desktop preview shows black
framing inside each eye half. Log: vpsquare applied=1 on=1, but every
projection was built at ratio 0.625 (16:10) and "game asked at creation:
2560x1600", so projVR matched nothing (rewrites=0). The game config had been
rewritten at 03:54 to Fullscreen=True / WindowDisplayMode=2 (plus
quality-option changes: rain FX on, shadows 1, LOD 1, texture filtering 4, draw
distance 1.0, which look like in-game options and were left alone). Exclusive
fullscreen accepts only monitor modes, so the game fell back to 2560x1600 and
drew the 3D in that corner of the forced 3072x2688 frame.
MODELIST:
- Config: Fullscreen=False, WindowDisplayMode=0, ResX/ResY=3072x2688
  (backup: unlock169-20260926/before-MODELIST/).
- Comfort forcing again enforces the four window keys under geo-11 whenever the
  render size is forced (OneFrameThreadLag still stands down).
- The fake display-mode list leads with the forced render size (then 3840x2160,
  5120x2880).
## 2026-09-26: DXGIMODES

MODELIST result: still a high window. Log: the comfort pass flipped 6 lines to
windowed + 3072x2688 at launch, but by the end of the session the game had
rewritten the config AGAIN to Fullscreen=True, WindowDisplayMode=2,
ResX/ResY=2560x1600. "game asked at creation: 2560x1600"; projections built at
0.625. The Win32 EnumDisplaySettings list (now containing 3072x2688) did not
change it, so the game validates against DXGI.
Source: the DXGI output hooks (GetDesc / GetDisplayModeList /
FindClosestMatchingMode) were deliberately NOT installed under geo-11 ("we do
not force the render size in geo-11 mode"), which has been false since ENGINE169.
DXGIMODES: install them under geo-11 whenever `engineres` > 0. In geo-11 mode
the list only ADDS the exact forced size (inheriting the biggest mode's refresh
and flags; no AER-style 16:9 replacement). FindClosestMatchingMode returns our
exact size when it is asked for. Config reset again to windowed 3072x2688. Log
lines to check: "DXGI ModeList: added 3072x2688", "DXGI ClosestMode".## 2026-09-26: geo-11 0.7.11, projVR off, lean, pose matcher

- User updated geo-11 0.6.182 -> 0.7.11; the release reinstalled d3d11.dll/dxgi.dll
  (WRAP mode, xrCreateSession -2). Swapped back to HOOK by hand (the HOOK script says
  "already hooked" while a stale geo11.dll exists); load_library_redirect=0;
  shader_regex_patch_mode 5 -> 4 (geo-11's ini names AK as stalling in mode 5).
  Backups: Win64\_geo11_backup_before_update_swap_20260926\.
- Lights and decals sliding with head motion: GONE with projvr=0 (user, headset). The
  FULLVIEW off-centre projection (m[9] += 0.193) was the risk flagged in FULLVIEW: the
  fix-pack lighting/decal shaders assume a centred projection. With vpsquare=1 the game
  itself builds 99 x 91.3 at ratio 0.875, equal to what we submit, so projVR is
  redundant at 3120x2730. Keep projvr=0. Cost: symmetric +-45.7 deg vertical instead of
  the headset's +39/-50 edges.
- Lean ran ~2x: settings held leanscale=1.96 = the code default 200 / kLeanAt1 102
  (calibration lost when the file was regenerated). Set 1.00; default now 102 in
  camera.cpp (next build).
- Main-menu head stutter: present.csv shows FRAMEID auto-matched delay 1 only during
  the main menu (15-33 s); gameplay fell back to the fixed 3. Manual gameplay truth was
  2-3, so the matcher's "1" is suspect. Test: poseauto=0 (set).
- Batmobile flicker on camera spins: camera.csv base-yaw steps alternate ~3 / ~4 deg per
  game frame (e.g. 4.13, 2.99, 4.23, 2.99), uncorrelated with frame dt (~17 ms, 58 fps
  into 90 Hz). Unconfirmed; next evidence: F2 mid-spin (per-eye position), and the
  Virtual Desktop SSW off/on comparison.
- Open: map Scaleform overlay still 16:9 over the taller map; map icons and radar still
  flicker. Waiting for F2 in the map.
## 2026-09-26: MAPFILL — map black box, keys moved off geo-11's

F2 17:40 (katanga_01, map): an opaque black box with a 16:9 window, exactly the
gameplay HUD rect (406,219 2308x2020). mode.csv: verdict 0 for the whole map, but the
map movies set their viewports at 124.99 s, inside the ~10-frame detector lag, so
scale_viewport gave them the gameplay box; on the mode flip, repush found hud_eff()=1
and returned "hands off", so the box was never undone. Radar (katanga_02-04): same in
both eyes in stills; flicker is temporal, still undiagnosed.
MAPFILL:
- scale_viewport: in the hands-off branch, a viewport exactly equal to the last rect we
  set is restored to the full frame (g_lastSet*). Others are left alone.
- Keys: F2 = pictures + logs (F1 freed; geo-11 0.7.11 binds Ctrl+F1). Ctrl+F5 (eye swap)
  and Ctrl+F7 (pose delay) removed; they are geo-11's convergence / save-settings keys.
  Panel keeps both controls. Note: the fix pack's d3dx.ini also binds plain F5 (ivy)
  and F6 (DOF), which overlap ours.
- Lean default 102 (from the earlier entry) is in this build.
Build installed (backup _geo11_backup_before_update_swap_20260926\dinput8.dll.DXGIMODES),
SHA256 843b781ff8432769f0d30772849f7772e687a5fe4e8008cc2f1f1f70ccd9b008. Headset-untested.
## 2026-09-26: RADARREC — flight recorder for the radar blink

User: the blink is too fast to press F2 during it. The 17:42 viewports.csv has no
viewport sets at all during gameplay (every set is logged, ring 4096, 107 rows total),
so our HUD resizing is not driving the radar blink.
RADARREC (hooks.cpp radar_rec_tick/dump): each Present copies a 512x512 square per eye
from geo-11's shared surface (centre 0.765/0.564 of an eye, where the radar sat in
katanga_02-04) into a 120-slot GPU ring (~240 MB VRAM). F2 also writes the ring, oldest
first, to Win64\akvr_radar_NN\f000.bmp... (~190 MB per press). Press F2 within ~1.5 s
after a blink. Suggested A/B with no build: geo-11 Ctrl+T (stereo off) — does the blink stop?
Installed over MAPFILL (includes it); SHA256 64167f2d8b77da47c29698f2f9141de66fd2129ca903098ccf52ac79b339deee.
No ctest tests are registered in build-dinput8. Headset-untested.
## 2026-09-26: RADARREC result; PAUSEROLL

Radar recordings (akvr_radar_01..06, 18:09-18:10): blink frames drop the radar-square
mean from ~72 to ~40 in BOTH eyes on the same frame; the image shows the radar (and its
side bar) entirely absent while other HUD pieces remain. In radar_06 it alternates every
2-3 frames for >1 s. User: worst when another UI element appears with audio; katanga_04
shows "LOCAL SURVEILLANCE / RANGE" appearing. So: not an eye mismatch, the radar is not
composited on those frames. Our HUD gates are stock and no viewport sets occur in
gameplay. Remaining suspects: game-side (UI budget/order with our global UI rect write),
or geo-11. Tests (no build): Ctrl+T (geo-11 stereo off); HUD size 1.00 + raise 0 (we then
write nothing to the game's UI rect).
Spin captures katanga_01-03: the car sits consistently in both eyes; the flicker is
frame-to-frame, not eye-to-eye. Still pending: Virtual Desktop SSW off/on.
PAUSEROLL: the pause hang used g_anamorphic = "3D scene drawn" AND projVR; with projvr=0
it was always false, so the frozen 3D picture hung LEVEL and lost the head roll it was
drawn with (user). hooks.cpp now passes the plain 3D-scene signal; xr.cpp's projVR uses
of g_anamorphic are already gated by g_projvrOn. SHA256 149edbfd...c3ce. Headset-untested.
- User tests: Ctrl+T (geo-11 stereo off) -> radar STILL blinks, so not geo-11's stereo. Virtual Desktop SSW was already OFF, so the Batmobile spin flicker is not SSW. HUD size 1.00 test in progress.
- HUD size/position at defaults -> radar does NOT blink (user). So the cause is our HUD
  resize. No per-movie viewport sets occur in gameplay, so the suspect is the per-Present
  write of the game's global UI rectangle (from our thread; the game resets it, e.g. on
  popups).
## 2026-09-26: HUDAREA — A/B switch for the global UI rectangle write
- `hudglobal` (panel: "HUD: also resize the game's HUD area", default ON = old
  behaviour). OFF: the game's rectangle is restored once and never written again; the
  per-movie SetViewport scaling still applies the HUD size.
- Status "hud:" line now ends "HUD area write ON/OFF, game overwrote it N times" (frames
  where the game had replaced our rectangle).
Expected: OFF stops the blink; check the HUD keeps its size and position. If the HUD
snaps back to full size with OFF, the per-movie path alone does not reach the HUD movies.
Built, not yet installed (game was running).
HUDAREA installed 079370fb...8753 (headset-untested), then superseded by TILTFILL (includes it).
## 2026-09-26: TILTFILL — fill the bottom with a pose tilt, not an off-centre projection
User: thin black band at the bottom with projvr=0. Quest 3 frusta U 39.4 / D 50.5; we
submit a centred +-45.7 (picture aspect), so the bottom ~5 deg is empty.
TILTFILL (xr.cpp): where the head pose is sampled, q = q * Rx(-t), t = (atan(tD) -
atan(tU)) / 2 (~5.6 deg) from the measured frusta. The tilted pose feeds BOTH the camera
injection (publish_pose) and the layer (g_poseHist -> g_layerPose), so render and display
agree by construction; the projection stays centred (geo-11 fixes happy). Covers ~40.1 up
/ ~51.2 down. Active only: tiltfill=1, fullview=1, projvr=0, geo-11 native. Recenter keeps
yaw only, so it does not cancel the tilt. Side effect: screen-space HUD sits ~5.6 deg
lower in view (HUD up/down can compensate). Panel checkbox + "N deg now"; status line
"bottom-fill tilt".
Car judder on spins (frames.csv 18:10, gameplay): presents last 1 vsync 69% / 2 vsyncs
30% in an irregular mix (1212111111221111111211212...), while the game advances the
camera by its own time step. Uneven display duration per step = back-and-forth judder.
SSW is off (user). Options offered: steady 45 (every frame 2 vsyncs; needs the frame-
timing guide 07 before building a pacing change) vs keep ~60-84 uneven.
## 2026-09-26: FPSLOCK — steady 45 / 40 / 30 as a panel option
User asked for a lock with 45, 40 and 30. References: playbook ch09 #runtime-owns-pacing
(FAIL-XR-024: an engine cap and xrWaitFrame are competing schedulers) and
#wait-frame-is-not-a-pacer (pace only when early, no-op otherwise), checkout
b955f2244f7c5d5f3b3e5f403b1b6721a3362423; vrframework guide 07 section 8 point 6 (verify with a
per-frame counter readout, not by eye).
Design (xr.cpp frame_begin): N = round(refresh / target), 1..6. After xrWaitFrame, while
the slot is earlier than lastNewDisplay + N*period - period/2, begin/end a repeat frame
with the previous layers (copies kept after each xrEndFrame; the swapchain's last released
image is resubmitted), then wait again. The game renders freely; only an early frame is
held. A late frame is counted (lockLate) and goes out at the next slot. The engine MaxFPS
stays forced at 120 (effectively unlimited here).
40 fps divides 80 and 120 Hz, not 90: at 90 Hz the lock runs 45 and the panel says so.
Panel: "steady frame rate (smooth spins): off / 45 / 40 / 30" + "headset X Hz: each frame
shown N times = Y fps; late frames". Setting `fpslock` (default 0). Status line
"steady frame rate".
Validation: frames.csv interval_ms in gameplay should read 2,2,2... vsyncs at 45 (90 Hz),
with a low late count; the spin judder should be gone. Headset-untested.
FPSLOCK installed, SHA256 30bf5ab9d4ce5cb2923e69cc045b71221cc0bff34506a004474abf79ce4e0c7a.
## 2026-09-26: FPSLOCK result; radar and slide-back narrowed
- FPSLOCK works (status 18:35): 90 Hz x2 = 45.0 fps, late 10 of 3138. User: camera
  "feels a bit better". Tilt 5.57 deg applied.
- Radar still blinks with HUD area write OFF; status "game overwrote it 0 times", so the
  global-rect write never fought the game and is NOT the cause. Only 49 viewport rows
  all session (creation-time sets), none per blink. With HUD size/raise at defaults the
  per-movie hook is hands-off and there is no blink, so the trigger is movies living in a
  shrunk/offset viewport at DRAW time (suspect: Scaleform masking/scissor for the round
  radar when another movie draws). Next split: size 1.00 + raise only vs size only.
- "Camera slides back when a spin stops" (user): camera.csv 18:35, four clean stops
  (132.57, 133.75, 136.57, 153.11 s): base yaw AND camera position stop dead within one
  frame and stay (<0.5 uu drift), head yaw steady. The game camera does not slide back.
  Whatever slides is downstream (display path) or in the scene (car/turret settling).
  Tests: world vs car; frame lock off; head-pose delay 2 with the lock on.
## 2026-09-26: FPSLOCK verified; poseauto off; open questions
- FPSLOCK verified by counter (guide 07 s8 p6): the last 120 presents before F2 (18:35)
  were ALL exactly 2 vsyncs (xr_wait ~16-17 ms each). User then set it off (fpslock=0).
- Slide-back: user says it is the CAR that slides back when the camera stops; the camera
  itself stops dead (log). Likely the Batmobile body/turret catching up with the camera
  (game behaviour). Check: battle mode only?
- Radar: HUD size 1.00 + raise only -> raise has no effect at 1.00 (hud_rect is not used
  when unscaled); size changed + raise 0 -> blinks. So the trigger is the SHRINK.
- poseauto: auto matcher picked 2 where only 3 is steady (user); earlier 1 where 2-3 was.
  Consistently one low. Default now OFF in code, panel label says experimental.
- First main menu stutters, second is fine: no data (frames.csv ring = last 157.8 s).
  Suspect geo-11 first-draw shader patching (mode 4) on first visit. Needs F2 in it.
- User asks: F6 floating screen WITH head tracking (behaviour to clarify), and the world
  scale slider wired to geo-11 (it only drives the AER eye offset; inert under geo-11).
  Zero-build test first: geo-11's Ctrl+F3/F4 separation, Ctrl+F5/F6 convergence.
## 2026-09-26: GEOSCALE — world scale drives geo-11 convergence
User measured with geo-11 keys: Ctrl+F6 (convergence up) = world smaller; Ctrl+F5 the
opposite; separation not needed. New src/geo11conv.cpp: ~300 Presents after start, via the
in-process nvapi64.dll: nvapi_QueryInterface -> Initialize, Stereo_CreateHandleFromIUnknown
(game device), Get/SetConvergence (IDs from references/3Dmigoto nvapi_interface.h; 3Dmigoto's
command-list `convergence` is the same call, DirectX11/CommandList.cpp ~1519). scale 1.00 =
convergence read at start; conv = base / scale; written only on change so geo-11's own keys
still work. Setting `geoscale`. Panel: in native mode the world-scale slider is this one, with
a diag line (start / set / read-back / rc / module). Calls are SEH-guarded. UNPROVEN that
geo-11 DM honours our handle: verify on geo-11's Ctrl+F1 readout, not the read-back.
Also in this build: poseauto default OFF (POSEOFF). Installed.
## 2026-09-26: HUDPIECES — per-movie shrink switches (guide 11 movie identification)
Radar blink frames (katanga_04/05) always show another element up ("LOCAL SURVEILLANCE /
RANGE", "122M" waypoint); the radar is absent from the whole frame (a round shape in the
corner of katanga_05 was a manhole cover). Hypothesis: the game's own HUD logic hides the
radar (overlap/visibility decided in full-screen coordinates) once we shrink movies.
RE (static, capstone, BatmanAK.exe): the hooked movie-view vtable is 0x142CC3150 (runtime
0x7FF695003150 - load delta 0x7FF552340000); slot 1 = 0x1415447f0 `mov rax,[rcx+0x48]; ret`
= GetMovieDef; slot 14 SetViewport 0x1411aeae0, slot 15 GetViewport 0x1411a4540.
HUDPIECES (earlyres.cpp): per view, guarded scan of the def (+0x48) for a C string or GFx
GString (+0x0C/+0x10) containing .swf/.gfx, depth 2; re-read on every viewport set. Each
screen-sized piece gets a panel checkbox ("HUD pieces"); unticked = left full size (our box
undone). Excluded names persist as `noshrink=;a;b;`. Status file lists every piece with
pointer, size, shrink/FULL and name. pushKey includes a generation so switches apply live.
The name scan is heuristic and unverified: if names read "(name not found)" or all the same,
the scan needs work; switches still work per piece in-session.
Installed (includes GEOSCALE + POSEOFF).
## 2026-09-26: CONV500 — world scale 1.00 = convergence 500 (user). conv = 500 / scale, fixed reference (not the launch value); start value still shown in the diag as a unit check. No d3dx_user.ini exists (no saved geo-11 convergence). Installed.
## 2026-09-26: GEOKEYS — world scale via virtual keys only geo-11 sees
CONV500 result (19:15 status): "stereo handle FAILED (init 0, create -140) via
C:\WINDOWS\SYSTEM32\nvapi64.dll" - the real driver, not geo-11. NvAPI route dead.
GEOKEYS: 3Dmigoto reads hotkeys with GetAsyncKeyState (references/3Dmigoto/DirectX11/
input.cpp VKInputButton::CheckState); geo11.dll imports it. geo11conv.cpp patches ONLY
geo11.dll's IAT entry and reports F13..F24 held per a mask (F13 = on, F14..F24 = 11-bit
v = 1000/scale). d3dxdm.ini patched (backup ...\d3dxdm.ini.before-akvr-conv): globals
$akvr_on/$akvr_base/$akvr_b0..b10, [KeyAKVRConvOn]/[KeyAKVRConvB0..10] (type=hold), and at
the top of [Present]: base = convergence on first activation, convergence = base*v/1000.
Units-free: world scale 1.00 = geo-11's starting convergence (dm_convergence 500). geo-11
polls keys only with the game window in the foreground (check_foreground_window=1).
Also from the 19:15 session: HUD piece names ALL "(name not found)" (URL scan fails; the
user identified the radar as the 4th piece from the bottom by toggling); settings had
fov=-2 (Home-key trim: 97 deg render -> 1 deg black at each eye's outer edge and a thin
top gap; reset to 0) and poseauto=1 (reset to 0; menus stuttered again). Installed.
## 2026-09-26: LINKTEST — is the key route reaching geo-11 at all?
GEOKEYS result: world scale does nothing (user). startup log: "geo-11 world scale 1.80:
convergence = start x 0.556" - IAT patch applied and mask set, yet no change. Unknown
whether (a) geo-11 never sees the faked keys or (b) the ini rules / `convergence =` in
[Present] do nothing in direct mode. LINKTEST: panel buttons fake geo-11's OWN Ctrl+F6 /
Ctrl+F5 (proven when the user presses them) for 2 s through the same IAT hook. Moves ->
(b); nothing -> (a).
Radar piece: third from the bottom this time (it was fourth): list order is creation
order, not stable. Unticked = full size = NO blink (user). Name scan widened: pass 0 .swf/
.gfx, pass 1 any name-like ASCII/UTF-16 text (6-90 chars, letter + . _ / \), from the def
(+0x48) and the view itself, two hops. Installed.
## 2026-09-26: KEYHOOK + SCREENTRACK + Pause key
LINKTEST result: faked Ctrl+F6/F5 via geo11.dll's IAT did NOTHING -> geo-11 does not read
keys through that import entry. KEYHOOK: MinHook detours on user32 GetAsyncKeyState and
GetKeyState in-process; only calls whose return address lies inside geo11.dll get the fake
answer (F13..F24 mask, link-test Ctrl+F5/F6); counter "geo-11 key reads N" in the diag
says whether geo-11 uses these APIs at all. A SendInput fallback was written and REMOVED
before shipping (user worried about keyboard input outside the game; it would have sent
real key events system-wide). d3dxdm.ini rules switched to geo-11's own preset pattern:
[Present] computes $akvr_want and raises $akvr_apply on change; [PresetAKVRConv]
(condition $akvr_apply == 1) sets convergence and $adj_conv, clears $akvr_apply
(backup d3dxdm.ini.before-akvr-preset).
User: keyboard entry outside the game misbehaved with the game CLOSED (processes checked:
only Virtual Desktop running) - not the mod; user suspects Virtual Desktop.
Radar: unticking the radar's piece (full size) still blinks this time -> the trigger is
ANOTHER shrunk piece. Piece order changes per session; names still not found.
SCREENTRACK: option C "a window into the world": the floating screen stays fixed and head
rotation + lean still drive the camera (akvr_xr_screen_frozen: forced screen + screentrack
= not frozen; pause/map/menus still freeze). Exact portal (off-axis) would need an
off-centre projection, which broke geo-11 lighting (projVR), so not done.
Screen toggle moved F6 -> Pause/Break (F6 also fired geo-11 Ctrl+F6 and the fix pack's
DOF toggle). Installed.
## 2026-09-26: NOF5 — vertical squash was wide render toggled by F5
KEYHOOK session (startup log 19:50): wide=1 -> game built H 122.1 x V 115.4, submitted 99 x 91
with a centre crop (cropX 551, cropW 2018): the whole 115 deg of height squeezed into 91 =
"squashed vertically" (user). Plain F5 toggled the obsolete pre-geo-11 wide render (also the
fix pack's ivy key; Ctrl+F5 is geo-11's convergence). wide=0 set; the F5 hotkey removed
(panel setting kept). Installed.
Radar: unticking HUD pieces ONE BY ONE did not stop the blink (user). Next split: ALL pieces
unticked with the HUD size still shrunk (and HUD area write on/off). If it still blinks, the
trigger is outside the per-movie list (global UI rect write, or movies we never list).
## 2026-09-26: KEYWHO — who reads keys in the game process?
KEYHOOK result (startup log 19:50): "key hook on, geo-11 key reads 0" while the user's own
Ctrl+F5/F6 works -> geo-11's key reader is not code inside geo11.dll's image calling
GetAsyncKeyState/GetKeyState. KEYWHO: every caller is classified by return address
(geo11.dll / BatmanAK.exe / our dll / other); "other" allocation bases are listed with counts
and module names (or "private memory"), and get the fake answer too (F13..F24 exist on no
keyboard; the Ctrl+F5/F6 link test fakes only for 2 s). If "other" stays empty as well,
geo-11 reads raw input or a hook and the fake-key route is dead (SendInput was rejected).
Installed.
## 2026-09-26: KEYWHO result -> GEOFILE (world scale applies next launch)
KEYWHO (status 20:03:59): "key reads: geo-11 0, game 1872400, mod 339010, other 58 [MSCTF.dll
x55, dinput8.dll x3]". geo-11 never calls GetAsyncKeyState/GetKeyState from anywhere; it reads
the keyboard another way (raw input suspected; GetRawInputData is in geo11.dll's strings).
Faking it would need system-wide SendInput (rejected by the user). Live world scale through
geo-11 is DEAD by three routes (NvAPI, IAT keys, global key detours).
GEOFILE: all key hooks and the link-test buttons removed; d3dxdm.ini restored from
d3dxdm.ini.before-akvr-conv (verified: every differing line was ours; mode 4 and
dm_convergence = 500 intact; the rules copy kept as d3dxdm.ini.with-akvr-keyrules). The slider
now shows 500 / (dm_convergence at launch) and, on release, rewrites that one line
(dm_convergence = 500 / scale, temp file + MoveFileEx, CRLF/no BOM kept). Applies at next
launch; live tuning = geo-11's own Ctrl+F5/F6. Warns if d3dx_user.ini holds a Ctrl+F7 save.
Radar (same session): pieces 0-9 FULL + 10-14 shrunk = no blink; radar was piece 5 from the
bottom. The trigger needs the radar AND earlier-created pieces full size; one-at-a-time never
isolated it. Piece order is creation order and changes per session; names unreadable.
Installed.
## 2026-09-26: RADARTEX + SCREENWIDE + SLIDERSTEP
Radar (user): it flickers between the SCALED and the NON-scaled position (not on/off). The
radar recording (akvr_radar_01, 20:14) frame 60 shows the radar lower-right at the edge of the
crop = full-size position. SetViewport impl 0x1411aeae0 has NO direct callers (only its vtable
slot), so the flip is not a bypassed setter. Viewport log 20:14: the radar's view 0x40743F60
(and four more HUD views) is set with BOTH a 3120x2730 screen buffer and a 1024x720 buffer of
its own. Own-texture viewports were left full layout (UIGATES rule), and that texture is shown
over the whole screen -> full-size radar whenever the game uses the texture path (popups).
RADARTEX: a view once seen screen-sized also gets the same RELATIVE hud box inside its own
buffer (hud_rect(W,H) of that buffer); own-texture-only views (1024x768/256x192 map icons) stay
untouched. Panel "HUD: shrink pieces also when drawn to their own texture (radar fix)", `huddual`.
Also answered: resolution unchanged (3120x2730/eye); the 45 lock was active (2,2,2, 626 late of
7248); "higher frame rate" feeling = floating screen mode, where the compositor handles head
motion at 90 Hz.
SCREENWIDE: floating-screen shape picture / 16:9 / 21:9 (`screenaspect`): crops the picture's
top/bottom and shrinks the layer's vertical angle to match. The bottom-fill tilt is now off in
the floating screen (the screen hangs level, a tilted camera would lower the horizon).
SLIDERSTEP: all 12 panel sliders snap to their displayed precision (1.00 reachable).
Build error lesson: the install step ran after a FAILED build and copied the previous DLL; now
the DLL's build tag is checked before installing. Installed SLIDERSTEP.
## 2026-09-26: VPWATCH — the radar flip, instrumented
SLIDERSTEP result: radar still flips small <-> normal. Viewport log 20:32: the 1024x720 own-
texture sets happen ONCE per movie at creation (then 3120x2730); the flips produce no
SetViewport calls, so RADARTEX's rule never fires during a flip. The full-size radar comes from
a path we cannot see yet. VPWATCH: once per Present, every known movie's stored viewport
(view+0xa0, 0x34 bytes) is compared with the last read; changes are logged as 'W' rows in
viewports.csv (same QPC ms clock). Recorder widened: one eye, 1024 px square centred at
0.80/0.62 of the eye (covers both radar positions), 90 frames, times.txt with QPC ms per frame.
Next: correlate blink frames (recorder) with W rows. W at blinks = direct writes (re-apply);
no W = a separate drawing path (texture composite) to find.
Also: D-pad / arrow nudges now move a slider exactly one displayed step (ImGui nudged 1% of
range = 2-3 steps); "floating screen size %" slider (g_floatScreenScale, `floatscreen`,
default 50%) separate from the main-menu size. 45 lock note: VD's counter shows ~90 because
each game frame is submitted twice; the game still draws 45. Installed.
VPWATCH installed after the game closed.
## 2026-09-26: VPWATCH result — no game-side viewport writes at Present granularity
F2 20:45 (akvr_radar_01 with times.txt; viewports 204511): all 26 'W' rows follow our own 'P'
pushes by one Present (mode flips and the user's panel toggles). The game NEVER changed a
movie's stored viewport between Presents. Yet the radar still flips to full size. So the
full-size radar comes from a path invisible at Present granularity: a temporary viewport swap
inside the frame (set/draw/restore), or the radar drawn via its own texture and composited.
Notable: movie 0x6873D3D0 was once set with buffer 1024x720 and viewport 256,152 512x360 (a
sub-rect inside its texture) at creation.
Next step (not started, needs the user's go-ahead): hook the movie draw itself. Candidate
vtable slots from the static dump (vtable 0x142CC3150): 26 0x1411b5870 (float use early:
Advance?), 27 0x1411ad270 (large frame: Display?), 25/31/32. Plan: pass-through MinHook
counters on 26 and 27, logging per call the view, its +0xa0 viewport and the bound render
target size (OMGetRenderTargets), into a per-frame ring dumped on F2.
## 2026-09-26: HUDPROBE + ENTRYFIX (built, waiting for the game to close)
HUDPROBE (src/hudprobe.cpp): pass-through MinHook detours on movie-view vtable slots 0,2-7,
9-11,16,18,20-27,31,32,34,35,37-39 (vtable from earlyres g_vtHooked[0]). Each detour is a
hand-written stub (saves rcx/rdx/r8/r9 + xmm0-3, calls probe_log(this, slot), restores, jmp
to the MinHook trampoline) so no signature is assumed. probe_log records time, thread,
slot, movie, its +0xa0 viewport and (render thread only) the bound RT size into a 16k ring;
slots over 3000 calls/s are only counted. F2 writes akvr_<stamp>_hudprobe.csv; the status
line lists calls per slot. Goal: find the call that draws the radar full size (viewport or
RT differs from the shrunk screen draw).
ENTRYFIX: (1) hud_eff uses the main-menu zoom only while the menu camera is live
(g_hudGameplay); the loading screen after the main menu was drawn at menu text size for the
5 s the main-menu phase lasts, then jumped to full size (user). (2) gameplay verdict after
10 perfect frames instead of 20 (0.45 s of floating window at 45 fps on game entry, user);
0.19^10 = 6e-8 per window in loads.
## 2026-09-26: TIDY2 — SSW choice for the frame lock + panel tidy (includes HUDPROBE, ENTRYFIX)
FPSLOCK-SSW (xr.cpp g_lockSkip, `fpslockssw`): "in-between refreshes: repeat the frame /
Virtual Desktop SSW". SSW mode resubmits nothing: before xrWaitFrame the Present thread sleeps
until lastWaitReturn + (N-1 + 0.25) periods (timeBeginPeriod(1), Sleep then spin; never >100 ms),
so the wait returns the slot N refreshes on and the runtime sees a half-rate app it can
synthesise for (user must set VD SSW to Always). Counters: late / early / frames. Unverified
that VDXR's xrWaitFrame returns the next slot after a late call - check the early count and
frames.csv intervals (should read 2,2,2 with no repeats logged).
Panel TIDY2: top level VIEW (world scale, camera tilt, Recenter, fold "where you stand"), HUD
(size, up/down, fold "HUD fixes" incl. pieces list), FLOATING SCREEN (toggle, head tracking,
shape, size, fold "menus, pause, map and loading screens"), SMOOTHNESS (lock + fill mode,
head-pose delay), SHARPNESS. Moved to Advanced: projVR (label corrected to "keep OFF under
geo-11" - it said keep ON), fill-whole-view and bottom tilt ("keep ON"), automatic pose match
("keep OFF"). Installed.
## 2026-09-26: TIDY2 results -> SAVEFIX
- HUDPROBE (hudprobe 205949): only slots 26 and 27 (1 call per movie per frame, 7758/7761) and
  11 fired; ALL on a non-render thread (game thread), rtW/H therefore 0. Every call saw the
  shrunk box (624,437 1872x1638) for all five screen movies. So the per-movie object never
  sees a full-size viewport: the full-size radar is produced on the render side.
- Setter 0x1411aeae0 disassembly: memcmp 0x34 -> copy to +0xa0 -> layout 0x1411ac780 ->
  push to [+0x88] (0x1411cd700 viewport, 0x1411a4b60, 0x1411cdc70). This is Scaleform 4
  (MovieImpl + display handle; render tree drawn on the render thread from a snapshot).
  Layout fields: +0xcc Scale, +0xd0 AspectRatio, +0xe8 ScaleMode, +0xec Align, +0xf0..fc
  VisibleFrameRect; Scale is used only in the NoScale branch (0x1411acb14, jump table on
  align). Hypothesis (unproven): a Scaleform filter / cached-bitmap render target places
  content without the viewport offset -> full-size radar when a popup with filters shows.
  Candidate fix: shrink via ScaleMode NoScale + Align Center + Scale, keeping a full-screen
  box (vtable slot 16 SetViewScaleMode / 18 SetViewAlignment). Needs the modes first: the
  probe now logs scale, aspect, mode, align and visible frame per call.
- SSW mode (fpslockssw=1, lock 45): game side verified 2,2,2 vsyncs (mean 22.23 ms) over 200
  presents; the VD overlay showing ~50 (and ~40 at 30) is VD's own counter.
- SAVEFIX: the render height (and world scale) never saved from the controller (only on
  IsItemDeactivatedAfterEdit). Every slider now sets g_sliderDirty; draw_panel saves (and
  commits dm_convergence, skipped when unchanged) once no item is active. Settings file still
  had engineres=2730 after the user's change. VD's quality preset does not cap the render
  (engine-res patch forces it); Godlike raises the streamed detail. Installed.
## 2026-09-26: SCALEMODE — shrink the HUD without an offset box (experimental) + capture button
Probe 21:09: all five screen HUD movies run ScaleMode 1 (ShowAll), Align 0 (Center), Scale 1,
Aspect 1, visible frame 0..20480 twips wide (1024 stage px) x -1760..16160 (the 1024x768
stage letterboxed to the box's shape). Render height now saved (engineres=3120).
NoScale branch (0x1411acb14): visible frame = viewport size (twips) x Scale (x Aspect for
width), centred per Align -> Scale = stage px per screen px.
SCALEMODE (earlyres.cpp, panel HUD fixes "shrink with Scaleform's own scaling (experimental
radar fix)", `hudscalemode`, default OFF): for screen movies in ShowAll, write +0xe8 = 0,
full-screen box (top = -raise px), Scale = stageVisPx / (hudScale x W) -> same size as the
box method. The original mode is remembered per movie and restored when the switch is off,
the HUD is at 1.00, or the piece is unticked. repush compares Scale too. Unknown: whether
the HUD's ActionScript re-lays out for NoScale (elements pushed to screen edges).
Capture: panel button "Save a recording + pictures (same as F2)" (capture_all()).
Game-entry side borders (user): the ~10-frame detection window shows the game at the
pause/loading size (80%); advised pause/loading size 100 until a better "game started"
signal is found.
## 2026-09-26: TEXDRAW — the radar flip's source (static RE) and a targeted fix
SCALEMODE result (user): worse - the radar jumps between the new scaled spot and one ABOVE
it. Probe 21:13: the game-thread state was steady in NoScale (the "half/half" split was
before/after ticking, not alternation). The second position = the HUD laid out with a
full-screen box at scale 1 (half size about the centre in NoScale). Same signature as the
box-mode flip (full layout).
Static RE: Scaleform 4 TreeRoot::SetViewport 0x1411cd700 (prologue 40 53 48 83 EC 20) has 3
callers: 0x1411aec7f (the movie's SetViewport - ours) and 0x141207d73 / 0x1412081c1, which
build a fresh viewport from a render target's size (virtual +0x28 twice; full rect, flags 1)
= render-to-texture draws. These bypass every movie-level lever we had.
TEXDRAW (earlyres.cpp): MinHook on 0x1411cd700 (prologue + both call sites verified at
runtime, else not hooked); calls whose return address follows either site get the RELATIVE
hud_rect box when in gameplay with the HUD shrunk and the viewport is the full rect. Panel HUD
fixes "shrink HUD drawn into off-screen pictures (radar fix)", `hudtexfix` default ON. HUD diag
line: "texture draws hooked: N, shrunk M, last WxH". hudscalemode set back to 0 in the
settings (user found it worse). Installed. Unverified: whether that texture is composited
full screen (relative box correct) - check the F2 diag counts and the radar.
Game-entry borders: user reports still visible briefly (less). pausescreen is still 0.8.
- TEXDRAW result (status 21:30): hook installed at 0x7FF69350D700 but "texture draws: 0" - the
  two render-to-texture sites never ran; they are NOT the flip source. hudscalemode was 1 again
  in the settings (user re-enabled or it was re-saved), so the flip was SCALEMODE's version.
  Everything that sets a movie or render-tree viewport has now been instrumented and the radar's
  movie never gets a full-size or unshrunk viewport. Remaining explanation (unproven): the radar
  element is POSITIONED by game/ActionScript logic in full-screen coordinates on some frames
  (e.g. an anchor computed from the real screen size or a projected point), alternating with
  its normal stage layout; HUD 1.00 makes the two coincide (no flicker). Viewport-level fixes
  cannot correct that.
## 2026-09-26: ROOTPITCH — HUD scripts read; shrink inside the movie; pitch unlink
Latest F2 (21:30, akvr_radar_01, SCALEMODE was ON): the "Powerslide" prompts stay put while
only the round radar/compass jumps (normal spot / higher / off the recorded square). The
21:09 recording (box mode) shows it absent on some frames instead.
HUD scripts (STATIC): extracted from BmGame.upk with Gildor extract + JPEXS, kept in
`diagnostics/hud-scripts-20260926/` (README has the recipe). The new-style HUD is one AS3
movie (ModularHudBm3, stage 1024x720) loading every HudModule into itself. No script
positions anything from the screen size per frame (last session's leading theory: ruled
out). Every module is a 3D panel (matrix3D + own perspectiveProjection); the prompts are
a separate AS2 movie with no 3D and never jump. Working explanation (UNPROVEN): Scaleform
projects 3D panels inconsistently inside an offset, shrunk viewport; HUD 1.00 = stock
viewport = no flicker.
Scaleform RE (STATIC): TreeNode::SetMatrix 0x1411cdc70 (Change_Matrix 1, 2x4 floats at
NodeData+0x10); the movie's SetViewport pushes ViewportMatrix view+0x110 onto its render
root view+0x88 (call 0x1411aeca5). Layout 0x1411ac780 has one caller (the setter). Slot 27
per-frame indirect-transform loop walks node parents only up to the root (never writes it).
Slot 68 edits +0x110 in place (rare). Movie vtable slots 2-10 forward to view+0x50 (main
movie sprite). SetVariable is not a movie vtable slot (UE3 execSetVariableNumber
0x14061aa90 goes through UGFxMoviePlayer vtable +0x318).
ROOTSHRINK (earlyres.cpp, `hudrootmode`, default ON, panel HUD fixes "shrink the HUD
inside the movie (radar flicker fix)"): the box logic still decides where; the viewport
goes back to full (stock), and the rect becomes the render root's matrix (stock
ViewportMatrix scaled by w/W,h/H plus l,t), re-applied after every SetViewport and when
+0x110 changes. Falls back to the box when projVR squash is active or the setter's bytes
don't match. SCALEMODE is ignored while it is on; TEXDRAW skipped. Diag line: "shrink
method: INSIDE the movie (root sets N, movies M)". Expected small static offset (~1% of
width) on tilted panels whose projection centre is not the screen centre (Health).
PITCHUNLINK (camera.cpp `pitchunlink`, panel VIEW "right stick tilts the view (horizon
stays level)"): the old tilt-kept mode rolled because past 40 deg the head yaw blended onto
the camera's own tilted up (menu diorama rule). Unlinked = full game pitch + head yaw
always about world vertical in gameplay (not frozen screens, not the main menu), pole
clamp 80 deg. Rendered roll = head roll only. Cost: with a tilted camera the scene sweeps
slightly instead of sliding on head turns. Correction of the GEOSHAPE note: a third route
exists and is what third-person UEVR does - level view, stick pitch drives the boom HEIGHT
(our hook is after the game placed the camera, so pitchkeep=0 already does this).
Settings changed: hudscalemode 0, pitchunlink 1, hudrootmode 1. Build ROOTPITCH, SHA256
171c3a86c2d9be81a05925715ff3ddfe8430173ec85a05f44f477fc677488af2. Rollback:
`diagnostics/before-ROOTPITCH-20260926/` (DLL + settings). Headset test pending.
## 2026-09-26: PIECES — per-piece HUD move/size, tipped pitch mode, entry hold
ROOTPITCH result (user): radar flicker FIXED by shrinking inside the movie. Pitch mode 1
("horizon kept level", status confirmed ON) still rolls on head turns with the camera
raised/lowered. Measured numerically (scratch check, build_basis maths): with the camera
pitched 45 deg, mode 1 turns 0.354 deg of every 0.5 deg head yaw into ROLL about the view
axis (w*sin p); a rigid composition gives 0 roll, 0.5 yaw. Level camera: both identical
(max diff 1e-16).
TIPPED (camera.cpp, `pitchunlink` now 0/1/2, panel radio "right stick up/down"): mode 2 =
camera = game heading + game pitch, then the full head rotation in that frame (UEVR style
with decoupled pitch off). Head turns slide the picture, never rotate it. Cost: the game's
tilt is a real tilt of the world, so its horizon is sloped when looking 90 deg sideways.
Playbook check (commit b955f22): ch01 / coverage "LockVerticalCamera" and vrframework
guide 09 s7 both keep gamepad pitch OUT of the view; no source offers pitch + level horizon
+ rigid motion, and the holonomy argument says none can. Mode 0 remains that route.
PIECEID (earlyres.cpp deep_scan): BFS from the movie def (depth 5, 0x200 bytes/node,
VirtualQuery page check, no guard pages, busy flag for the cross-thread push) for (a) text
naming a HudModule, (b) the movie's FILE SIZE (all HUD .gfx sizes from the extraction)
with a stage width in twips (20480/25600 float) within 0x40. Logged as "hud-piece: <view>
= <name> (found N pointers deep)" in the startup log. Unverified at runtime.
PIECEXF: each piece gets size (about the HUD box centre) and left/right, down/up shift
(fraction of screen) composed into its root matrix; works at HUD 1.00 too (gameplay only).
Panel: HUD fixes -> HUD pieces -> "move / resize". Saved by name as `piecexf=`; unnamed
pieces are session-only. Settings line buffer 128 -> 2304 (noshrink lists were cut).
ENTRYHOLD (xr.cpp g_holdScreen, hooks.cpp): while a run of gameplay-looking frames is
being confirmed (!verdict && goodRun > 0, up to 10 frames), the floating screen keeps its
last picture, so pause/loading at 80% goes straight to full VR without the small-window
frames. pausescreen back to 0.8, pitchunlink=2. Tests 3/3 pass (build-proxy-test).
Build PIECES SHA256 7e0829d6c48f76c51ca77e38cdd0c7928d5c1f167569e21a5664adfd7abf6d96.
Rollback: `diagnostics/before-PIECES-20260926/`. Headset test pending.
## 2026-09-26: HEIGHTPITCH — the other mods' route is camera height, which mode 0 already is
User: other mods "recouple the pitch to the height of the camera" with no roll. Camera traces
(6 captures, game position = cam - delta): a stick tilt down RAISES the camera and up LOWERS
it, the same sign in every capture (e.g. -19.6 deg -> +236 uu, +57 deg -> -232 uu): AK's boom
orbits. Our hook runs after the game places the camera, so mode 0 (`pitchunlink=0`,
`pitchkeep=0`) keeps that height and a level view = UEVR decoupled pitch; head yaw is about
the true vertical, so no roll. Modes 1/2 put the tilt in the view and carry the trade-off;
kept but no longer recommended. Panel label now says "raises / lowers the camera, view stays
level (like UEVR, no roll - recommended)". Settings set to mode 0. Headset check pending.
## 2026-09-26: LAYERS — stick height boost; HUD split phase 1 (read-only layer dump)
User results (PIECES): pitch modes 1 and 2 both roll on head turns (as predicted: 1 = roll
rate, 2 = tipped world). Mode 0: NO roll, but "pitch feels dead". HUD pieces: now named -
FrontendBGFader, FrontMostLayer x3, RadialGadgetSelect x2, "/ package/ModularHudBm3/Image" x4
(file URL found 2 pointers deep from the def by deep_scan text match); the compass and radar
scale together because modules are loaded INSIDE ModularHudBm3 movies (compass = Targets
module's "Compass" child of its GrandMaster). Per-movie transforms cannot split them.
STICKHEIGHT (camera.cpp `stickheight`, metres, default 2.0, panel under the top pitch
option): extra camera lift = -sin(game pitch) x value x lean scale, mode 0 + gameplay only,
on top of AK's own boom lift. View stays level, so no roll.
HUDLAYERS phase 1 (earlyres.cpp akvr_hud_layers_dump, F2 -> akvr_<stamp>_hudlayers.txt):
read-only walk of each ModularHudBm3 render tree from view+0x88 (node 0x38 bytes, parent
+0x20, data [[page+0x20]+idx*8+0x28], flags +0x0a, matrix +0x10; children found as pointers
reachable from the data whose parent field is the node, offset logged) plus a BFS from the
main sprite (view+0x50) matching display objects to layer nodes with their ASString names.
Goal: learn the node offsets and which layer is "Compass" / the radar, then phase 2 moves
individual layers via TreeNode::SetMatrix. Build LAYERS SHA256
9abfad05b3288b641e307ed416e01a01c949f7aa8e4d86cf3cae53d1a17f2421, rollback
`diagnostics/before-LAYERS-20260926/`. Needs one F2 in gameplay with the HUD showing.
## 2026-09-27: TIDY3 — camera measured clean; HUD layers phase 2; panel tidy
Camera (F2 23:59, 6364 frames, game pitch -55..+55): rendered roll = head roll to 0.00 deg
in every frame EXCEPT t 69.1-73.0 s, where final pitch = game pitch + head (e.g. -41.1 -
7.6 = -48.7) and roll differed: the user had the "world tipped" mode on then. Mode 0 is
roll-free by measurement. Tilt modes removed from the panel and forced off at load
(pitchkeep/pitchunlink ignored); stick height boost kept.
HUDLAYERS phase 1 result: only the roots were found (root matrix 0.109, tx 742, ty 712 =
our ROOTSHRINK), no children; the dump did show nodes whose +0x20 was the root. Static RE:
TreeContainer::Insert 0x1411cd480 (GetWritableData Change 0x100) / Remove 0x1411cd4f0
(0x200): children at data+0x90: 0 = none, untagged = inline at +0x90/+0x98 (max 2), bit 0
= array ptr (count +0x08, items +0x10). Helper 0x1411cd080.
Phase 2 (earlyres.cpp HUDLAYERS): walk each ModularHudBm3 tree to depth 7; names by BFS
from view+0x50 (hashed seen-set + node hash; heuristic first identifier ASString in an
object pointing at the node); per-part size (about stage centre 10240,7680 twips) /
shift (fractions of 20480 x 15360) / hide (zero 2x2) applied by TreeNode::SetMatrix on the
GFx thread from a movie vtable slot-27 hook (0x1411ad270, rcx/dl only), base re-read
whenever the node no longer holds our matrix; 3D nodes (flags 0x200) skipped. Reset
restores on the game thread. Applied state carried across rediscovery. Auto re-find 3 s
into each gameplay stretch when adjustments are saved (`hudlayers=` key:size:x:y:hide).
F2 writes the full layer list (akvr_<stamp>_hudlayers.txt).
Panel TIDY3: removed tilt radio + tilt slider, HUD fixes (off-screen, inside-movie,
Scaleform scaling, own-texture, HUD area), HUD pieces list; Advanced hidden unless
`advancedpanel=1` in the settings file. hudscalemode forced off, hudrootmode forced on.
Build TIDY3 SHA256 2fc572729d3f6d9ff9844468b69bc1a61b52a5995b5700a8632b2db89d0db89f.
Rollback `diagnostics/before-TIDY3-20260927/`. Tests 3/3.
## 2026-09-27: ROLLSIGN — the UEVR-default camera fixed; HUD part moves actually hooked
User: UEVR's default third-person camera pitches down as the camera lifts and does NOT roll
on head turns; wants that back. It is the rigid composition (game rotation, then the head
in that frame) = our mode 2, which rolled. ROOT CAUSE (numeric check vs UE3 FRotationMatrix
axes): camera.cpp build_basis(y,p,r) == UE(y,p,-r) - our roll runs the opposite way to
UE3's. Head-only roll hid it (ROLL_SIGN and the written froll cancel), but mode 2 CREATES
roll from yaw x pitch and wrote it mirrored: F2 23:59, pitch -41.1, head yaw 12.6 wrote
+15.5 deg, UE needed -8.6 (24 deg wrong, the roll JJ felt). Fix: head built with -gr,
composed, froll negated back into UE's sense. Verified in UE convention: level = old path
exactly; tilted = UE(head) x UE(game) rigidly; 0.5 deg head yaw -> 0.000 roll. Mode 2 back on
the panel as "follows the game camera (like UEVR's default)"; mode 0 kept as "always level
(like UEVR decoupled pitch)" with the stick height boost; mode 1 retired. pitchunlink=2 set.
My earlier "zero roll" check for mode 2 used only our own convention, so it could not see
this: check composed rotations against the ENGINE's rotation matrix, not our own inverse.
HUD layers (F2 00:25): 69 parts in 4 containers, names include StoryModeHud, DetectiveMode,
RightSideParent (Targets); walk capped at depth 7 hid the deeper parts; hide did nothing
because the slot-27 vtable swap never ran (the game calls it directly). Now: MinHook on
0x1411ad270, depth 12, 2048 parts, event-name strings filtered, live counters in the panel
line ("frame hook ON, calls N, matrix sets M"). HUDPROBE retired (its code patches collided).
Build ROLLSIGN SHA256 a749d6acf0afe010568468904a66119408dc6d54aa47dd3f04ad889050307b7e.
Rollback `diagnostics/before-ROLLSIGN-20260927/`.
## 2026-09-27: HIDE3D — camera confirmed fixed; hide via the visible bit, 3D parts movable
User: ROLLSIGN camera FIXED ("well done"). HUD parts: the radar is container c2's content
("c2.0" hides it); the compass and other parts were not separable - their hide boxes did
nothing. Cause: most module parts are 3D nodes (flag 0x200) and HUDLAYERS refused them;
hide worked by zeroing a 2D matrix. Static RE: TreeNode::SetVisible 0x1411ccd80 (flags
bit 0 = visible, GetWritableData Change 4; found as the callee fed flags&1 by the tree
copy at 0x1411cef00); TreeNode::SetMatrix3D 0x1411cdce0 (copies 0x30 bytes = 3x4 to
data+0x10, sets 0x200; 0x1411cdbe0 = back to identity 2D). Now: hide = SetVisible(false),
re-applied each frame, the game's own state restored on untick; move/resize works on 3D
nodes (3x4: scale the 3x3, translation [3]/[7]). Both prologues verified before use.
Build HIDE3D SHA256 1975e1bd8c372b9f19d223c50422c57072ad8278843b0e254cae206c775b3042.
Rollback `diagnostics/before-HIDE3D-20260927/`.
## 2026-09-27: HUDKEYS — the containers identified; saved adjustments keyed by identity
User (HIDE3D): hide works; container c0 = surveillance radio (AlertsAndSurveillance, also
holds StoryModeHud/DetectiveMode names), c1 = compass (Targets: RightSideParent), c2 = radar,
c3 = unknown (same shape as c2: one module, one part centred at 10240,7680 - probably a
situational overlay). Moving whole containers already gives separate radar/compass/radio.
Fix: container numbers follow creation order, which changes per launch, so keys are now
"<container key>/<part>": container key = first part name found inside, else
"s<child counts of the first 16 nodes>@<tx,ty/100 of the first depth-7 part>" (module
placement, static; deeper parts can animate). Container rows show "HUD container N (key)".
Build HUDKEYS SHA256 2852f8f83d9d6450a19e3d3a9c2f884f5e32ae1b65061c729be1ec3ba173993d.
Rollback `diagnostics/before-HUDKEYS-20260927/`. Check after the next launch: saved moves
land on the same container.
## 2026-09-27: HUDFP — containers recognised by fingerprint, not names or order
User: after relaunch "the parent nodes were named differently" and the settings were lost.
Saved keys were "RightSideParent/0" and "antiAliasType/0" - the second a TextField property
picked up by the ASString heuristic. Names are labels only now. CONTAINERFP (earlyres.cpp):
a container's fingerprint = set of 16-bit hashes of (depth, x/50, y/50, has-children) for its
parts at depth 6-11 (module content; the ModularHudBm3 frame above is shared by all four and
made c1/c2/c3 83-100% similar on the 00:25 map). All containers are matched against saved
fingerprints at once, best Jaccard pairs first (>= 0.35), fingerprints refreshed each
discovery; unmatched get new ids K1, K2... Part keys = "<K id>/<path inside container>".
Saved as `hudcontainers=K1=hhhh,...;K2=...`; F2 dump lists ids + part counts. Old name keys
cleared from the user's settings. Unverified: that the compass (c1) and c3 separate at
depth 8+ (the 00:25 map stopped at 7) - check the next F2 dump if a match goes wrong.
Build HUDFP SHA256 248b81cb7536dd5541980f8c42369b0f306781b803f4c9fd73e4dc784bcde69b.
Rollback `diagnostics/before-HUDFP-20260927/`.
## 2026-09-27: SLIDEROWN — controller slider steps owned by the mod
User: HUDFP kept the HUD positions across a relaunch (works). "All of the sliders were
fighting me" on the D-pad. A stand-alone ImGui 1.92.9 harness (scratchpad slidertest, same
SliderStep) steps cleanly on hold and release for %.2f/%.1f/%.0f, and the settings file is
loaded once, so the second writer is somewhere in the live panel input; not identified.
SLIDEROWN (hooks.cpp SliderStep): while a slider is active and not mouse-dragged or typed
into (TempInputIsActive), ImGui's change is discarded and the mod steps exactly one
displayed unit per press from the REAL pad (akvr_gamepad_diag: D-pad or left stick past
16000) and keyboard arrows, repeat 350 ms then 70 ms. Harness-verified. Return value =
value actually changed. Menus: startup main menu stutters, fine after gameplay; with
menu3d=1 it uses the gameplay display path; geo-11 cache_shaders=1 with 355 cached fixes.
Needs F2 in both states (frames.csv) - requested.
Build SLIDEROWN SHA256 c2890520147de4b49b39c33e0759048ea1a743c1b2efae08ce91c60f10b9a414 (installed after the game closed); rollback
`diagnostics/before-SLIDEROWN-20260927/`.
## 2026-09-27: TILTBOX + FPSNOTE
User: one tick box instead of two camera-tilt options, default = follows the game camera.
Panel VIEW: "decouple the camera pitch (view stays level, like UEVR's option)"; unticked =
pitchunlink 2 (ROLLSIGN rigid), ticked = 0 (+ stick height slider). Code default is now 2.
User: switching SSW -> "repeat the frame" kept 45. By design both modes hold the GAME at the
lock rate (xr.cpp FPSLOCK: repeat = each game frame shown div times; SSW = sleep before
xrWaitFrame so VD synthesises). Only lock "off" frees the game. Also VD's own SSW "Always"
(set for the SSW mode) holds the app at half rate independently of the mod. Panel relabelled
("hold the game at:", "between game frames:") with both notes. Settings: fpslock=45,
fpslockssw=0. Build TILTBOX SHA256 2b408faff2aacc9bf81218bef698e0ffbc4cc51cd93c65af666c64978ec43176,
rollback `diagnostics/before-TILTBOX-20260927/`.
## 2026-09-27: OFXR — OFXR Bridge frame generation: the frame lock stands down
User wants OFXR Bridge (github.com/tig3rmast3r/OFXR-Bridge, cloned to
references/OFXR-Bridge at dad56ac, public v0.2.1/V116; the user runs V277) to work with
AKVR; frame rate "tanks". Source review (STATIC): an implicit OpenXR API layer
(XR_APILAYER_XRFrameBridge_diagnostic.dll); the app sees a virtual half-rate loop and
the layer submits a synthetic + the real frame per app frame; projection layers are
copied D3D11->D3D12 (interop) and run through color-only optical flow; quad layers pass
through. Conflicts with AKVR: FPSLOCK repeat mode resubmits the previous projection layer
(looks like a new real frame -> generation between identical pictures + an extra paced
cycle); SSW mode's pre-wait sleep stacks on the layer's half rate. Plus GPU cost at
3560x3120/eye on top of geo-11. On the user's PC: tray V277, NVIDIA medium 50%, one test
2026-09-26 02:07-02:09 (older AKVR build), flight recorder off (no logs). Another implicit
layer is registered (OpenXRFreePIE, VRCompanion). Change (xr.cpp): after xrCreateInstance,
g_ofxr = the layer DLL is in the process; fps_lock_eff() = 0 then (panel amber note, status
line "OFXR Bridge frame generation: LOADED (mod frame lock paused)"). Not headset-tested.
Next if it still tanks: OFXR flight recorder log + our F2.
## 2026-09-27: SBSONE — one side-by-side swapchain (OFXR stutter)
OFXR test 1 (NVIDIA, our F2 03:15): game own time median 7 ms; our submit (xrEndFrame
through OFXR) median 46 ms, bursts 130-260 ms, one 5.3 s; 18-22 real fps. Flight log
(FidelityFX run, 03:22-03:23, V277): OFXR GPU work per synthesis median 1.18 ms (p90 2.6) -
not GPU-bound. 64 s steady: 1428 app frames (22/s) vs 5521 presenter submissions (86/s).
One app_end_frame traced (42.6 ms): per APPLICATION SWAPCHAIN a private_swapchain_acquire
wait ~10 ms + synthesis_pair 0.35 ms + private_swapchain_release wait ~9.6 ms - our two
per-eye swapchains = ~4 refresh waits. Quarantines (generation_prepare 18) only at startup
(swapchain recreation). Also noticed: OFXR ini has motion_vectors=dlss (V277 option; AK
has no DLSS) - unverified effect.
SBSONE (xr.cpp): in native (geo-11) mode ONE double-width swapchain; one full
CopySubresourceRegion of the katanga surface; each projection view reads its half via
subImage.imageRect offset (swap eyes / MENUFLAT = which half). `sbsswapchain` setting
(default 1); status line "headset images". Expected: OFXR's per-frame waits halve.
Build SBSONE SHA256 6881b7256dd230bc8b33f17a708b72690289bf9c64ebb3bd7b12cd1d73a882a8,
rollback `diagnostics/before-SBSONE-20260927/`. Headset test pending (with and without OFXR).
## 2026-09-27: SBSONE result + session handoff
User: "better, but still stuttery". Flight log 03:53 (V277, SBSONE, 65 s steady): real app
frames 30.7/s (was 22), app_end_frame median 12.2 ms / p90 22.3 (was 19.9 / 43.2),
presenter 87 submissions/s; private_swapchain acquire/release p90 ~9.7/9.4 ms remain,
app_swapchain_acquire median 3.3 ms. No AKVR F2 from this run. Handoff written:
akvr/CLAUDE_HANDOFF.md (old one archived in diagnostics/).

## 2026-09-27: ORBITFIX — stick-orbit jitter on Batman / the Batmobile

User: spinning the game camera around Batman or the Batmobile with the right stick jitters the
subject, not the background; nothing to do with the head. Source + trace evidence:
- The finalize-epilogue stub OVERWROTE the camera rotator with a full value composed on the
  Present thread from the PREVIOUS finalize's base, while the position was the game's current one.
- F2 traces `akvr_20260927_032352_265_camera.csv` / `..._031549_166_camera.csv`: for every spinning
  row, (drawn yaw) - (this finalize's base + head) = -(base step), exactly (median 2.0 deg/step at
  normal spin). So the camera sat on the new orbit point aimed with last frame's stick angle; the
  orbit centre (Batman) moved by that varying step while the far field barely did.
- Playbook `docs/01-camera-and-tracking.md` #delay-synthetic-latch-head (checkout b955f22): the
  game's synthetic motion must stay on the engine's clock; only head motion is ours.
Fix: the slots now hold the head DELTA (composed - saved base) and the stub ADDS it to the game's
fresh base (`add [rbx+off],eax`). Exact for heading changes (the composition is heading-invariant);
a game pitch change between frames is carried to first order. Lean still maps through last base
yaw (sub-cm). Pose pairing (posedelay) untouched: the head part is unchanged.
Source inspection + trace analysis only; headset test pending. Check with a new F2 during a spin:
the same script (drawn minus correct) should read ~0. Rollback `diagnostics/before-ORBITFIX-20260927/`.
Installed SHA256 eeabe925492c1c77405118dce67b6709bdd3f1e400378f62840c01cf212671fa.

OFXR retest after ORBITFIX (user, 2026-09-27): still more stuttery with OFXR than without. Parked.

## 2026-09-27: GEOLIVE — world scale slider drives geo-11 convergence LIVE

User asked for the world-scale slider to work interactively (earlier routes, all dead: NvAPI,
geo11.dll IAT key faking, process-wide key detours; system-wide SendInput rejected by the user).
New route: write geo-11's own convergence value in memory. Static RE of geo11.dll 0.7.11
(capstone/pefile, scratch scripts, no debugger):
- Ctrl+F5/F6 are d3dxdm.ini presets (`convergence = convergence * 1.05`). Their setter writes
  float [G]+0x3054 (current) and [G]+0x3008 (loaded dm_convergence); G = geo11+0x87E5B0.
- geo-11's per-frame direct-mode update (the "dm_convergence - %f" log, ~geo11+0x2078e0) compares
  its stereo object's +0x640 with [G]+0x3054 and pushes the value itself when it differs, so
  writing +0x3054 is exactly the key's effect.
- G and both offsets are found at runtime by pattern: ini read of L"dm_convergence" -> mov
  rax,[rip+G]; movss [rax+base],xmm0; log site loading G + nearest movss xmm2 load = live. The
  offline simulation (same logic) on the installed geo11.dll gave G 0x87e5b0, base 0x3008, live
  0x3054. Not found -> the slider stays next-launch (old GEOFILE behaviour).
Behaviour: first contact adopts geo-11's running value (nothing moves); slider eases 15%/frame
with a 0.2% settle (playbook ch09 #convergence-implementation, checkout b955f22); values geo-11's
own keys set are adopted into the slider; the ini line is still written on release. Panel label
reads "world scale (live)" + a live-link diag line. Source inspection + offline simulation only;
headset test pending. Rollback `diagnostics/before-GEOLIVE-20260927/` (includes d3dxdm.ini).
Installed SHA256 7b402ad0bd88f7bea469a8ec09ab2e291c6a3138931a5224d1a7ebc380b268aa.

## 2026-09-27: SLIDERFAST — sliders slow / "pushing against" (GEOLIVE result)
User: GEOLIVE works (world scale changes live), but sliders still fight and are very slow.
Causes addressed (source reasoning, not measured): (1) SLIDEROWN steps one displayed unit per
70 ms repeat = ~14 units/s (world scale 0.14/s); holding now accelerates x5 after 1 s, x25
after 2.5 s (first repeat 300 ms). (2) GEOLIVE adopted any value it had not written into the
slider; if geo-11 moves its own value, that drags the slider back. Adoption now waits 1.5 s
after a slider change; the diag line counts "changed outside the slider: N". If N climbs
while only the slider is used, geo-11 is rewriting convergence (find the writer: preset
transitions at geo11+0x183540 are the first suspect). If other sliders still fight, the
cause is elsewhere (second writer never found, SLIDEROWN notes).
Rollback `diagnostics/before-SLIDERFAST-20260927/` (= GEOLIVE). SHA256 94117212d78137f0bc20dce4c274ceed23b0abc7912bef269a22a494cc798219.

## 2026-09-27: ONESTEP — the slider "fight" root cause (two writers per press)
User (SLIDERFAST): better, but "the values go up then back down by a tiny amount each time I
click"; it started with the one-step (SLIDERSTEP) change. Source (references/imgui
imgui_widgets.cpp SliderBehaviorT ~3172): ImGui's own gamepad tweak moves a float slider 1% of
its RANGE per press and DRAWS that value inside SliderFloat; SLIDEROWN then replaced it with our
one-unit step, so each press showed ImGui's value for a frame, then ours. Before SLIDEROWN,
SLIDERSTEP's rounding fought the same 1% tweak. Fix: gamepad.cpp no longer feeds D-pad / left
stick left-right to ImGui while a SliderStep slider was controller-active on the previous panel
frame (g_sliderOwnFrame), and SliderStep applies its step BEFORE drawing (active = GetActiveID()
== id). Keyboard-arrow tweaks by ImGui are still discarded after the draw. The two SliderInt
widgets (pose delay, render height) are untouched. World scale range now 0.50-1.50 (1.00
centred, user). Rollback `diagnostics/before-ONESTEP-20260927/` (= SLIDERFAST). SHA256
ee606d8e97da783dec602f25054363bb0d6caeca27c41cae6d7190071648ec88. Headset test pending.

## 2026-09-27: GEOSEP — separation slider under world scale (live)
User asked for a separation slider (geo-11 Ctrl+F3/F4). Static RE, geo11.dll 0.7.11: ini read of
L"dm_separation" stores [G]+0x300c (convergence base +4); the Ctrl+F3/F4 presets
(`separation = separation +/- 1`) and the command setter (~geo11+0x186296) write +0x3050 and
+0x300c; the per-frame direct-mode update compares +0x3050 with its stereo object's +0x644 and
pushes it (same function as convergence). Runtime: the dm_separation ini read is found by the
same pattern; separation is enabled only when its base is exactly convergence base +4, and its
live slot is then convergence live -4 (0x3050). geo11conv.cpp refactored to one Chan per value
(ease, settle, adopt-after-1.5 s, outside-change counter). Slider 0.0-10.0 (%.1f, ini value 5
centred); saved as dm_separation with world scale. Diag shows outside changes per slider.
Rollback `diagnostics/before-GEOSEP-20260927/` (= ONESTEP, includes d3dxdm.ini). SHA256
bab036c8333538b24f29d7cae634933ae2495973eb87f546055f56043c466b9c. Headset test pending.

## 2026-09-27: RANGES — world scale 0.25-2.00, separation 0-50 (user)
GEOSEP sliders worked; saved values confirmed on disk (dm_convergence 500 = world scale 1.00, dm_separation 5.0). Ranges changed on request. Rollback `diagnostics/before-RANGES-20260927/` (= GEOSEP). SHA256 e85da42c826202841a18405f1f4db2574012fcf280a841cea4ce06696cc203cf.

## 2026-09-27: assessment — Sekiro EYEVIEW ("NOSEWASTE") on Arkham (not built)
Source: skvr/HANDOVER.md "Each eye's own view" (geo-11 x += S(w-C); large S + negative C turns
each eye outward by S NDC; render half-width tan (tO+tI)/2; submit each eye off-centre by -/+k*h;
culling widened by (1+k); swap eyes off; playbook A3.1/A3.3 per SKVR).
Arkham today (startup log): 3560x3120/eye, HFOV 99 (symmetric to the OUTER edge, xr.cpp FULLVIEW),
vertical already off-centre via TILTFILL (5.57 deg). VDXR Quest 3: outer 49.34, inner 35.36 deg ->
tO 1.1643, tI 0.7096, k 0.2426, width factor 0.8048: same sharpness at 2865 wide (19.5% fewer
pixels, x2 eyes) or +24% horizontal sharpness at today's cost. Render half-FOV 43.1 instead of 49.3.
Portrait 2865x3120 is non-square (geo-11's square-RT rule is what broke depth before).
Arkham advantages: sep/conv are LIVE here (GEOLIVE/GEOSEP), so S/C need no restart (Sekiro writes
them before geo-11 loads); only the render width does.
Work items: (1) calibrate geo-11 NDC per separation unit on AK (SKVR: 0.00105; measure from F2
Katanga pair); S = k/unit, C = -(sep*conv)/S keeps world scale; world-scale slider -> the S*C
product, separation slider meaningless in this mode. (2) per-eye off-centre submission in xr.cpp
(port SKVR g_eyeView). (3) swapeyes auto-off. (4) HUD doubled (SKVR HUDFIX: UI shaders branch on
StereoParams.y<0); AK HUD = Scaleform 3D panels through the fix pack, unknown effort. (5) pause/map
pictures and menus need the per-eye turn. (6) outer-edge culling: UE3 culls with the game
frustum; objects wholly in the outer ~6 deg may pop; needs RE for a wider cull frustum.

## 2026-09-27: EYEVIEW — each eye's own view (SKVR NOSEWASTE port), built for SPEED
User: "yes start, aim for more speed". Ported from skvr (xr.cpp g_eyeView / hooks sync) per the
assessment above; SKVR cites playbook A3.1/A3.3 (checkout b955f22).
- xr.cpp: inner tangent measured; with eye view on (launch setting) and a narrow render
  (eyeW <= height), g_headHalfH = atan((tO+tI)/2), which also narrows the game's FOV lock.
  Once geo-11 is confirmed at S/C (g_eyeApplied), each eye is submitted off-centre by -/+k*h
  (gameplay, live menus, floating screens; a flat one-half screen stays symmetric) and the
  SBS halves swap relative to swapeyes (negative C flips geo-11's eye translation; swap_eff()).
- geo11conv.cpp: eye-view mode holds separation S = k_headset / eyeunit (0.00105, SKVR value,
  UNVERIFIED on AK) and convergence C = -(sep * 500/scale) / S (keeps S*C = the world scale),
  written instantly, never adopted back; the ini keeps the user's pair. S/C change live, only
  the render width needs a restart (SKVR needs both at load).
- earlyres.cpp: geo-11 render shape floor 1000 -> 700 (exact square nudged to 992).
- Panel (VIEW): "use each eye's full view (~20% fewer pixels, restart)" (sets geo11shape x
  (tO+tI)/(2tO), restores eyebaseshape when off), "distance alignment" k (submission only) +
  reset, "flip eye turn", diag line; separation slider hidden in eye view.
Installed with eyeview=1, geo11shape 1143 -> 920 (next launch 2864x3120/eye, was 3560x3120).
Expected first run: HUD doubled (SKVR needed UI-shader fixes), possible pop-in at the outer
edge (game culls with its narrower frustum), pause/map screens may need checking. Calibrate:
F2 in gameplay looking at distant scenery -> block-match far disparity in the Katanga pair.
Rollback `diagnostics/before-EYEVIEW-20260927/` (settings: eyeview absent, geo11shape 1143).
SHA256 03a71caad14009aa9c3c1df589e95f4a2aadf86f47f1f50bd1bd6163578165c2.

## 2026-09-27: VRSEP — separation slider removed; world scale = geo-11's S*C product
User (RANGES run): in VR "convergence and separation appear to do the same thing" (scene size);
on the desktop separation differs. Explained by geo-11's shift S - S*C/w: constant slide S
(~0.005 of the half-width at 5, invisible) + parallax set by the product S*C. No screen plane in
a headset, so only the product matters. Normal mode now holds separation at 1.0 (far-field slide
~0.001) and convergence = 2500 / scale (2500 = 5 x 500, the user's 1.00 calibration); the ini is
committed as dm_separation 1.00 + that convergence. Moving S to 1 rewrites C in the same frame so
the product (size) never jumps. geo-11's own Ctrl+F3..F6 are adopted as a size change. User's
last pair (sep 5.5, conv 500) -> world scale ~0.91, same look. Eye view: C = -(2500/scale)/S.
Separation slider gone from the panel. Rollback `diagnostics/before-VRSEP-20260927/` (= EYEVIEW).
SHA256 de3a8b89bcfab52455d6c214b7a32bfc665b5dcdb7f95d5a89b6d22329ba7389. Includes EYEVIEW (untested).

## 2026-09-27: playbook sweep for missed high-value items (AKVR, SKVR, DS3VR) — leads, nothing built
Playbook b955f22, pattern catalog. SOURCE-read + our own code; none applied yet.
1. PERF-003 wait-ahead frame loop (+ PERF-006 floor vs own cost): all three mods call xrWaitFrame
   inside the game's Present hook on its render thread (akvr/skvr/ds3vr xr.cpp ~1140-1200,
   hooks.cpp akvr_xr_frame_begin). PERF-003's symptom = exactly half panel rate. AK measured 45 Hz
   with game own time ~7-11 ms and xrWaitFrame holding ~14 of 22 ms. Lead: one worker owns
   xrWaitFrame and releases the permit right before xrBeginFrame. Discriminate first (PERF-006):
   log wait/begin/end spans; if the game's own time < 11 ms yet 45 Hz, it is serialization.
2. STR-012 (+ STR-014) pose pairing: replace the fixed posedelay (AK garage 3 / outdoors 2; SKVR 3)
   with a push/pop ring: push the pose seq at each camera finalize (game thread), pop one per
   Present. A thread split delays but never reorders, so depth adapts per scene. Prove off-headset
   that finalizes per rendered frame ~= 1.00 (menus/loads may break it) before trusting.
3. CAM-010 / CAM-002 culling split for EYEVIEW pop-in: keep the engine's FOV wide (culls wide),
   narrow only the render in the projection matrix. AK has a projection-build hook (projVR) but its
   vertical write never landed - test horizontal. SKVR/DS3 already have culling-camera hooks.
4. STR-020 packed-pair seam: SBSONE (sbsswapchain=1, one SBS image) exists only for OFXR, now parked;
   any neighbourhood operator (compositor filtering, VD SSW) can bleed eyes at the nose edge. Cheap:
   sbsswapchain=0.
5. HUD-004 + ch04: HUD into its own transparent layer at a fixed depth - the alternative to per-game
   HUD shader edits for EYEVIEW's doubled HUD (all three).
6. STR-010 drop a bad frame on cuts/teleports (AK loading snap, cutscene cuts). STR-021 pacer only
   if OFXR returns.

## 2026-09-27: EYEVIEW first run -> EVGAME (eye view in gameplay only)
User, first EYEVIEW/VRSEP run: 3D looks fine; 2D menus and loading screens doubled, out of focus.
Log: eye view APPLIED (S 231, C -10.824, k 0.2425, narrow 1), render 2864x3120. F2 14:34
(akvr_katanga_02): 3D turned per eye as designed, "PAUSE MENU" at the same pixel in both halves:
Arkham's geo-11 fix leaves flat 2D unshifted (d3dxdm dm_hud_detection=0), so under turned frusta 2D
diverges by ~2k. SKVR shifts its UI in patched shaders (HUDFIX); Arkham does not.
EVGAME: geo-11 S/C are live, so the eye view now runs only in effective gameplay (not main menu,
screens, loading, pause, map): elsewhere geo11conv writes the normal pair (sep 1, C 2500/scale) at
once and the frustum is centred. The applied flag is delayed by the pose delay (per-Present history)
so frames in flight keep the framing/swap they were rendered with. Open: the in-game HUD during
gameplay is still doubled (needs a HUD shift = the slide, or a HUD layer; backlog item 6).
Second report: after leaving the in-game GRAPHICS menu without changes, the view became a small
window high in a corner. BmSystemSettings.ini was rewritten 14:36 to ResX 2560 / ResY 1440,
Fullscreen=True, WindowDisplayMode=2 (as in MODELIST): the game re-applies its own size in-session
and our engine-res patch only acts at startup. Launch-time comfort forcing restores the ini, so a
restart fixes it. Proper fix = RE the settings-apply path (backlog). Advice: avoid the graphics menu.
Rollback `diagnostics/before-EVGAME-20260927/` (= VRSEP). SHA256 8253f575e025c63a899b894d5ee967f09812549355475eff6651487a2fccde62.

## 2026-09-27: PAUSE169 + EVGAME delay + MENURES (graphics-menu reset experiment)
User (EVGAME run, eye view OFF for the test): the graphics-menu reset happens WITHOUT the eye view
too (not the portrait size). With the eye view on: the in-game compass (and other HUD parts) still
doubled (expected, HUD not yet handled); the first menu doubled briefly then normal; pause and map
screens too tall - they should match the game's 16:9 mask.
- EVGAME delay: eye view only after 1.5 s of steady gameplay, off at once.
- PAUSE169: non-gameplay screens cropped to 16:9 centred (xr.cpp scrAspect/vFrac); panel slider
  "pause / map / loading shape" (`pauseaspect`, 1.00 = whole picture) in case the prompts clip.
- MENURES (static RE, capstone): the menu's URGFxMovieUI::execSetResolution native (0x14051AE60,
  from the native table entry at rva 0x3036E40) passes a list index to vtable+0x4F8. The menu's own
  current resolution is two ints at VA 0x143123620 / 0x143123624 (list lookup 0x14001E6A0 against
  mode list [0x143100158] x count [0x143100160]); GFE option registration at 0x140C6F030 also reads
  them (strings "ResolutionX"/"ResolutionY", GFSDK_GSA). Separate from the startup ResX/ResY
  (0x143123CF0/CF4). Experiment: hold the menu pair at the forced engine size every Present
  (addresses decoded from the lookup's bytes, exact-match guarded); status line logs the original
  values, reset count, whether our size is in the game's list, and the list. If the reset persists,
  the apply path reads elsewhere: next target is vtable+0x4F8 (find URGFxMovieUI's vtable).
Rollback `diagnostics/before-MENURES-20260927/` (also has BmSystemSettings.ini). SHA256
a0d41945b97954e7a6a1c9c5e0bc16c4470602d623111bb0be7f58e027a1b95d. Installed with eyeview=0 (user).

## 2026-09-27: MENUMODE + HUDFIX experiment (geo-11 HUD detection at far depth)
MENURES result (startup log 15:13): hold ON, the menu pair was 5120x2880 (our largest faked mode),
reset twice to 3560x3120, our size IS in the game's list - and the reset still happened. Cause: leaving
the menu also sets EXCLUSIVE fullscreen (ini Fullscreen=True / WindowDisplayMode=2), which only takes
real monitor modes -> 2560x1440 (MODELIST mechanism). MENUMODE: also hold the menu's display mode
(int at VA 0x143123628 = resX+8, "Display_Mode" in the GFE registration cmp at 0x140C6F04D, pointer
check X+8) at its launch value (windowed, set by the comfort pass). Status line reports it.
SHA256 a4d6152f1554c34bffdc1f2fb031c1a9833934a1bc734b8b4b7a5728ad49c991. Rollback before-MENUMODE-20260927/.

HUDFIX (settings only, d3dxdm.ini [Stereo], backup before-MENUMODE-20260927/d3dxdm.before-hudfix.ini).
geo-11 0.7.11 RE: settings struct G (geo11+0x87E5B0): +0x3018 dm_hud_detection (int), +0x301C/301D
scissor/stencil flags, +0x302C dm_static_hud_depth (float, default 0.2), +0x3030 dm_auto_hud_depth
(bool, read only if detection on), +0x3040/+0x3044 HUD offset min/max, +0x3014 final separation
(= [0x3010]*[0x3050]*const). Per-view setup at geo11+0x1CD7C2: hud offset = clamp([0x302C]*[0x3014],
[0x3040], [0x3044]), written to the stereo params (+0x874/+0x870/+0x654), negated per eye.
So static depth is a FRACTION of the separation slide: 1.0 = HUD at the far-field slide = exactly the
eye view's per-eye turn (k) -> the HUD should fuse; lower = nearer. Set: dm_hud_detection 1,
dm_static_hud_depth 1.0, dm_auto_hud_depth 0, dm_auto_hud_offset_min 0.0, dm_auto_hud_offset_max 1.0.
Unknowns: whether [0x3014] follows our live separation writes every frame (else the HUD offset stays at
the launch separation), which draws geo-11 classes as HUD, first-launch shader patch time. Eye view
re-enabled for the test (eyeview=1, geo11shape 920).

## 2026-09-27: GSAHOLD (graphics menu, the real path) + EXPOSURE + EDGES
User (MENUMODE + HUDFIX run): leaving the graphics menu still breaks the view (now shifted up, top
clipped); some UI elements still widely separated; right eye darker; wants Sekiro's edge sliders.
- Graphics menu, RE (capstone, BatmanAK.exe): the UI natives table lists execSetDisplayMode
  0x14051AD40, execSetResolution 0x14051AE60, execApplySelectedOptions 0x140498500 (main and pause
  options) -> vtable+0x770. The options movie's vtable (0x141B978A0: slot 0x4F8 = 0x14001E770, 0x770 =
  0x14001F2D0) applies by reading "Display_Mode" (2 = fullscreen), "ResolutionX", "ResolutionY" via
  getters 0x140C67370/390/3B0, which call GFSDK_GSA_GetOptionValue from NvGsa.x64.dll (NVIDIA GeForce
  Experience settings store; SetOptionValue/Load/SaveConfigFile also imported). MENURES/MENUMODE held
  the GFE registration copies, which apply never reads. GSAHOLD: IAT-hook GFSDK_GSA_GetOptionValue in
  BatmanAK.exe (earlyres patch_import at startup, only when the engine size is forced) and answer
  Display_Mode 0 (windowed) and ResolutionX/Y = engine size; value at out+8. MENUMODE hold retired.
  Status line: "graphics menu: NvGsa answer ON, N lookups (store held mode m, WxH)".
- EXPOSURE: katanga pair 15:54 - same content 25-30% darker in the right eye while whole-eye means
  are equal = per-eye auto-exposure normalising each eye's own (now different) view (playbook STR-009).
  d3dxdm.ini: [TextureOverrideAKVRTinyRT] and [..TinyUAV] (Texture2D, width and height < 5,
  StereoMode = 2 mono). Unverified: whether AK's luminance chain ends in <=4x4 targets.
- UI still separated: the marker above Oracle has the far-field offset (HUD detection working); large
  holographic screens do not (drawn unshifted, not classed as HUD by geo-11). Needs per-shader work.
- EDGES: VIEW panel "extra view at the sides (deg)" (live FOV trim, `fov`, set to 5 as SKVR) and
  "extra view top and bottom (deg, restart)" + "same as sides" (`overscanv`), which set the next
  launch's geo11shape from the measured tangents (next_geo11_shape; the eye-view toggle uses it too).
Build GSAHOLD SHA256 48f32b485a07ab873d1cc25230acfc7bfffec04ad06c5a4e31edc21527fc10d3. Rollback
`diagnostics/before-GSAHOLD-20260927/` (dll, settings, d3dxdm.ini).

## 2026-09-27: VIGNETTE hunt (user: "make sure there is no vignetting on each eye")
- No config key; the game has post-process settings VignetteType / VignetteIntensity (and PMS_*),
  only in script reflection tables (no direct code refs).
- Measured on the 15:54 katanga pairs: the right/left brightness ratio of matched content drifts
  0.63 -> 0.84 with screen position (on top of the exposure offset) = consistent with a screen-centred
  vignette of ~15-25% at the edges. In the eye view a screen-centred vignette is off-centre to the lens.
- geo-11 fix's 3D Vision ShaderFixes label four vignette shaders, all special effects:
  a13376a20bb0bed1 (Harley detective vision; no DM copy), 07b441f67aabea9b (detective distortion +
  vignette), 72706aae5e913216 (disruptor gun), b054d718b3d653a6 ("vignette part"); DM copies exist
  for the last three. The ordinary gameplay vignette is not in the fix.
- Static searches found nothing decisive: ShaderCacheDM (10k patched shaders; only geo-11-patched
  ones are cached), GlobalShaderCache-PC-D3D-SM5.bin (2098 DXBC, none with uv-0.5 dp2 + cb scale);
  CombinedShaderCache upk is compressed (needs Gildor extract, not present now). Scanner:
  scratchpad gscan.py/dxbcscan.py (D3DDisassemble via the game's d3dcompiler_47; FNV-1a 64 hash).
- Next: geo-11 hunting (d3dx.ini hunting=2, backup before-GSAHOLD-20260927/d3dx.before-hunting.ini):
  Numpad0 on, Numpad1/2 cycle pixel shaders (marking_mode=skip hides the current one), Numpad3 marks
  (hash saved). Then skip it ([ShaderOverride] handling=skip if it is a separate pass) or a DM
  replacement with the vignette factor forced to 1.
VIGNETTE hunt, part 2 (user declined manual hunting; d3dx.ini hunting back to 0):
- Gildor decompress.exe (gildor.org/down/47/umodel/decompress.zip, -game=batman4) unpacked
  CombinedShaderCache-PC-D3D-SM5.upk (155 MB, 38,201 DXBC). 3Dmigoto/geo-11 shader ID = FNV-1 64
  with hval starting at 0 (3Dmigoto util.h fnv_64_buf) over the whole DXBC; verified: 306 of the fix's
  367 IDs found. Scanner: scratchpad gscan2.py (+dxbclib.py, D3DDisassemble).
- Arkham's vignette recipe (from the fix's Harley shader a13376a20bb0bed1: (uv-0.5)*cb.xy, dot, 1-d,
  max 1e-6, squares) matches 16 material shaders; all read as effects (ripples/distortion 408b51d5523f924c,
  afdc3ee0fd63e717; rotating/tunnel materials), none a plain screen darkening.
- Re-reading the matched-content ratio (0.63..0.84) with the exposure factor removed is not monotonic
  in distance from each eye's centre (some edges come out brighter), so it does NOT prove a gameplay
  vignette; content differences between the eyes explain it. Conclusion: no evidence of an always-on
  vignette; the game's vignettes are effect-driven (detective vision, Harley, disruptor, fear effects).

## 2026-09-27: GSAHOLD + EXPOSURE CONFIRMED; HUDDIST (HUD nearer/further)
User: both eyes the same brightness now (EXPOSURE tiny-RT mono works); leaving the graphics menu no
longer changes the resolution (GSAHOLD works); agrees there is no gameplay vignette. Asked for HUD
distance control in addition to the up/down/left/right controls.
HUDDIST: geo-11 HUD shift = clamp(depth * F, min, max) (F = separation slide). Offsets found by the
ini-read pattern (mov rax,[rip+G]; add rax,imm32; ... lea rdx,L"key"): dm_static_hud_depth +0x302C,
dm_auto_hud_offset_min +0x3040, max +0x3044 (verified offline). A world point at w is shifted
S(1 - C/w), so HUD at distance d = depth 1 - C/d with the LIVE convergence (negative in the eye view),
units 100/m. Written every Present with min/max opened to -2/+2. 0 = far away (depth 1, as before).
Panel HUD: "HUD distance (m, 0 = far away)" 0-20, saved as huddist. Needs dm_hud_detection = 1.
Unverified: that geo-11 recomputes the HUD offset every frame (the HUD fused after live separation
changes, which suggests it does). SHA256 in the handoff. Rollback before-HUDDIST-20260927/.

## 2026-09-27: crash report on HUDDIST ("Fatal error!", first load into the scene + camera moves)
The dialog lists truncated (low-32-bit) return addresses. Recovered the load base by brute force
(scratchpad crashmap.py: the one 64 KB-aligned base for which every frame lands right after a call;
unique, 15/15): runtime low bits 0x61FF0000. Stack, innermost first (VA):
0x141073010 (hash-table lookup: 13x4-byte key hash) <- 0x140005F70 <- 0x140877DF0 (uses global
[0x143AF6FD8] via 0x140005E90/0x140006230/0x140005F70) <- 0x140878BA0 <- 0x14080CDE0/0x14080C910 <-
0x140855C00 <- 0x140864885 <- 0x140865610 <- 0x1407E74F0/0x1407E7120 <- 0x140FA7D50/0x140FA5AC0/
0x140FA58D0 <- 0x140BF4030 (thread entry), faulting inside ntdll. A background (streaming/loading)
thread; none of these functions is hooked or patched by AKVR. Unproven cause: a one-off game crash,
or earlier memory damage. HUDDIST was the only change; HUDDIST2 now writes the HUD depth/min/max only
when they differ. If it repeats at the same point: bisect with before-HUDDIST-20260927/ (= GSAHOLD).
HUDDIST2 SHA256 bb6d51a5092e7816b3e5016b1c2185f87a526b33b4d64443273cc3d1e40915bd.

## 2026-09-27: HUDDIST2 result + TIDY4 (overlay rework)
User: no crash this time (the earlier crash was likely a one-off). The HUD distance slider does
nothing. Wants distance per HUD part, and the overlay cleaned up like Sekiro's.
- Why HUDDIST cannot work: geo-11's HUD shift is computed in geo11+0x1CD4B0 (writes the stereo
  object's +0x874/+0x870/+0x654 = clamp(depth * [G+0x3014], min, max)), called only from
  0x1801CEA00 <- 0x1800E4E10 (loads nvapi64.dll / nvapi.dll = stereo init) <- two functions with
  no direct callers (device/swap-chain setup). So the shift is fixed at startup, from the LAUNCH
  separation, and it is ONE value shared by every HUD-detected draw: per-part distance is impossible
  through geo-11's HUD handling. Consequence for the eye view: with the launch separation at 1-5.5,
  the fixed HUD shift is ~0.001-0.006, not k, so gameplay HUD parts (compass) stay doubled.
  Live route left to try: the per-frame copy (0x2078E0: obj = [ctx+0x1BD8]; obj+0x654 <- obj+0x874
  on update, then the params upload) - find obj at runtime (e.g. +0x640 == live conv, +0x644 == live
  sep) and write +0x874/+0x654 + dirty flag +0xCC8. That would give a live whole-HUD distance and a
  HUD shift of k in gameplay / ~0 in menus. HUDDIST writes disabled in code (TIDY4).
- TIDY4 overlay (SKVR order): status line; Recenter + Save capture; VIEW (world scale + 1.00, pitch
  decouple, stick height, extra view sides/top-bottom, picture height (restart), each eye's full view
  + alignment/flip, where you stand); HUD (size, up/down, menus full height, single parts); MENUS AND
  SCREENS (floating screen + menu/pause sizes and shape, live 3D menu, flat screens); FRAME RATE (lock,
  pose delay). Removed: HUD distance slider, the old FOV trim (= sides slider), long diag lines (now in
  Diagnostics). "spot the main menu automatically" moved to Advanced.
SHA256 2d4f8ef71bc668cfc405997e70e5f58ddd30d34276f894e7257d02012ef4fd9c. Rollback before-TIDY4-20260927/.

## 2026-09-27: PAUSESTUTTER + HUDLIVE (build HUDLIVE)
User: "a severe stutter of one to two seconds, consistently about three seconds after returning from a pause
screen or the map screen"; "dig deeper into how to move the whole HUD further or closer".
References read: playbook `docs/04-ui-and-hud.md` "Three ways to place a HUD in stereo" (fixed pixel disparity =
one apparent depth for the whole HUD; expose it as a number) and #binocular-panel-fit; vrframework guide 11 §5
("the per-eye offset is a fake": large HUD elements can ghost at the edges); guide 07 (frame timing). Playbook
checkout b955f2244f7c5d5f3b3e5f403b1b6721a3362423. Also SKVR/DS3VR's HUD route (patched Scaleform vertex shaders
+ live IniParams, sk_geo11.cpp) - kept as the fallback if this one fails.
- PAUSESTUTTER (SOURCE, not yet measured in-game): earlyres.cpp re-runs HUD-part discovery 3 s into EVERY
  gameplay stretch when saved part adjustments exist (JJ has them). Its name search (name_layers) walks ~24k
  objects per container and tests ~1M pointers, each through page_ok -> VirtualQuery, on the game thread = the
  1-2 s freeze at ~3 s. Names are panel labels only (keys = container fingerprint + path), so the automatic pass
  now skips the search and reuses names by key; the panel button and the F2 dump still search. The log and
  the panel show "search N ms (quick / with names)". The 22:43 frames.csv had no pause return (longest
  frame 97 ms), so there is no before-measurement.
- HUDLIVE (STATIC, geo11.dll 0.7.11 sha1 600ea47f..., same file in AK/DS3/Sekiro): the HUD shift lives in
  geo-11's stereo object obj = [swapchain wrapper + 0x1BD8] (wrapper Present 0x205AB0 / 0x206D00 / 0x232110
  call the per-frame update 0x2078E0 with it). Setup 0x1CD4B0 writes obj+0x874 = +0x870 = +0x654 =
  clamp([G+0x302C]*[G+0x3014]) and the eye blocks (+0x308 left: -S, C, 0, 1, +0x520, -h @+0x31C; +0x330 right:
  S, C, 1, 1, +0x520, +h @+0x344; third block -h @+0x36C). Per frame (0x207C28) geo-11 compares obj+0x654 with
  obj+0x874 (next to the conv/sep compares GEOLIVE uses) and, when they differ, rebuilds the blocks FROM +0x874
  and sets the upload flag +0xCC8. Nothing rewrites +0x874 per frame (only a profile load 0x208F99/0x2091A6 and
  the auto-HUD branch 0x2095C3/0x209731 gated by [G+0x3018]). The other update path (0x207A50, when [G+0x30B0]
  and [G+0x3020]|[G+0x3028]) copies 874->654 without rebuilding the blocks. So TIDY4's "fixed at startup"
  was only about the settings slots; the object value is live.
  geo11conv.cpp hud_tick: checks 7 code signatures at fixed RVAs (other build -> off), takes the swapchain from
  hkPresent (akvr_geo11_swapchain), accepts obj when obj+0x640/+0x644 match the live conv/sep, writes
  h = S(1 - C/d) (S = [G+0x3014], C = live conv, d = huddist x 100; 0 = far away, h = S) to +0x874/+0x870 when
  different; if the right-eye block has not followed after 3 Presents, writes the blocks + upload flag itself.
  Panel HUD: "HUD distance (m, 0 = far away)" 0-20 + "far away"; status line "HUD distance: LIVE, shift ...,
  writes N, eye-block writes N" (also in F2). Expected side effect: in the eye view, "far away" makes the
  gameplay HUD (compass) fuse, which the fixed startup shift did not.
  Open: whether the game's swapchain is geo-11's wrapper (else "stereo object NOT found" and the misses count);
  which draws geo-11 classes as HUD (large holographic screens were not, GSAHOLD notes).
Build HUDLIVE SHA256 44f8c5aed2c1806e912e4dd00f86dd18bc30fc3e1c02eb9e95a556097aea60a9.
Rollback `diagnostics/before-HUDLIVE-20260927/` (= TIDY4; dll, settings, d3dxdm.ini).
