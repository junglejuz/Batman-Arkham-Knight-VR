# AKVR — Batman: Arkham Knight true-6DOF VR mod

Our code inside `BatmanAK.exe`: a `version.dll` proxy that injects real 6DOF head
tracking into the game's (third-person) camera and displays the game per-eye in a
Quest 3 via OpenXR. In the spirit of Luke Ross's R.E.A.L. mods — third-person
preserved, no vorpX, no 3D-screen compromise.

Full project plan & milestone status: [`../ARKHAM_KNIGHT_VR_PLAN.md`](../ARKHAM_KNIGHT_VR_PLAN.md)

## Where it's at (2026-07-24)

- **Working now:** our DLL hooks DX11 Present; reads the Quest head pose (OpenXR,
  driven from the Present hook — one predicted pose/frame); injects head
  rotation + positional lean into the UE3 camera fields (additive — the controller
  still steers); submits the game frame to the headset **world-locked and stable**.
- **Stereo (AER) — working & sharp, this is the shippable path.** Alternate-eye
  rendering with **per-eye render-time pose** submitted to the compositor (each eye
  timewarped independently to "now"), which removed the turn-blur. Needs no geo-11.
- **Native stereo (geo-11 katanga capture) — SHELVED.** With our DLL co-loaded,
  geo-11's shared-texture pipeline never activates (mapping `err=2`, geo-11's own log
  stays empty) — a hook-chain conflict between our kiero hook and geo-11's own DXGI
  hook. An OpenVR presence "beacon" (connected fine) ruled out the missing-consumer
  theory. Fixing it would need live debugger work for an optional feature; AER is the
  path forward. (Beacon disabled after it caused a SteamVR-launch crash; code kept.)
- **In-headset settings menu (M6):** the floating overlay panel is now an interactive
  settings menu — **hold Menu + View (Start+Back) ~0.5s** to open it, navigate
  with the stick/D-pad, and the game ignores the pad while it's open. Sliders for 3D
  depth, lean/world-scale, FOV trim, and menu-screen size; live square-resolution
  presets; all settings persist across launches (`akvr_settings.ini`).
- **Head-turn roll bug — FIX BUILT + DEPLOYED 2026-07-25, awaiting in-game test.**
  Two causes, both in `akvr_head_update`: (1) head yaw was applied about the *base
  camera's own up* axis, but the third-person camera is usually pitched down at
  Batman, so turning about its tilted up tipped the horizon — yaw now turns about
  world up (+Z); (2) the recenter reference stored the full head orientation, so any
  pitch at recenter time tilted the axes the Euler extraction read — recenter is now
  **yaw-only** (`delta = Ry(-refYaw) * cur`), leaving pitch/roll gravity-absolute.
  This is why the two earlier quaternion tweaks failed: `conj(ref)*cur` leaks a
  recentre pitch into yaw and `cur*conj(ref)` leaks a recentre yaw into roll, so no
  choice of multiply order was ever going to be clean. Watch the "ROLL DIAG" overlay
  line: with the camera pitched down, sweep your head left/right — `camera-final`
  should stay ~0 while `head` stays ~0.
  **First test 2026-07-25:** roll not reported back yet, but the compose path pitched the
  wrong way (`PITCH_SIGN` +1 -> -1, see CAMERA_MAP) — fixed, rebuilt, redeployed.
- **Loading-screen head-pinning — FIX BUILT + DEPLOYED 2026-07-25, awaiting test.** The
  loading screen dragged around with your head for 5-10 s before floating free. Cause: the
  "are we in gameplay?" test only asked whether the camera-finalize counter was advancing,
  and on AK's loading screens the engine keeps ticking the camera for several seconds behind
  the 2D image. Added a second test — the camera VALUES sitting bit-identical for 30 frames
  (~1/3 s at 90 Hz) also counts as not-gameplay. Live play never does that for long because
  of the game's own idle camera sway. Risk: a cutscene holding a genuinely locked camera
  could trip it; F6 still overrides either way.

- **Frame rate: the 90 cap is soft — UNLOCKED 2026-07-25.** `MaxFPS=90` under
  `[SystemSettings]` in `BmGame\Config\BmSystemSettings.ini` is the live cap (UE3's
  `bSmoothFrameRate` is FALSE, so that path isn't involved). Raising it to 120 works:
  measured **117.8 fps, 58.9/eye** vs 90/45 before. Virtual Desktop must ALSO be set to
  120 Hz — we pace to the headset via `xrWaitFrame`, so VD's refresh is its own ceiling.
  Original file kept as `BmSystemSettings.ini.akvr-backup-90fps`. Watch for frame-rate
  -dependent physics/animation, the usual reason such caps exist.
- **Render resolution: SQUARE IS NOT REACHABLE without patching the game (settled
  2026-07-25).** The creation hook works — swapchain comes out 2048x2048 — but the game
  does NOT render square, so it's a net loss. Measured: the game asks DXGI for
  **2423x1363**, which is exactly the largest 16:9 that fits the desktop WORK AREA
  (`1392 - 29` px of caption/border `= 1363`; `1363 * 16/9 = 2423`, confirmed to the
  pixel). It **ignores `ResX`/`ResY`** in `BmSystemSettings.ini` and hard-wires 16:9 in
  windowed mode. Force the buffer square and the scene is still drawn at 2423x1363 into
  the top-left of it: clipped on the right, sitting high, and sheared because the
  projection is built for a shape the buffer doesn't have. `g_forceSquare` is therefore
  **default OFF** and persisted (`square=` in `akvr_settings.ini`). Remaining options:
  (a) render a much wider 16:9 FOV and crop the sides on the display side — no RE, costs
  effective sharpness; (b) patch the game's aspect/sizing logic — RE job; (c) test
  whether exclusive fullscreen (`Fullscreen=True`) escapes the work-area clamp. For
  reference, Luke Ross forces `2048,2048` per game, so he solved this somehow too.
- **Earlier dead ends for the same problem (kept so we don't retry them):**
  Calling `ResizeBuffers` ourselves returns `DXGI_ERROR_INVALID_CALL` (game holds
  backbuffer refs). Resizing the WINDOW succeeds — client really becomes 2048x2048 —
  but the backbuffer stays put and **the game's ResizeBuffers count stays at 0**: this
  title never resizes its swapchain and ignores `WM_SIZE`. Writing `ResX`/`ResY` to the
  .ini doesn't take either. The only remaining lever is a **swapchain-creation hook**
  (intercept `D3D11CreateDeviceAndSwapChain` / `IDXGIFactory::CreateSwapChain` from our
  proxy at startup and rewrite `BufferDesc`). Not built yet; the panel now just reports
  the state instead of retrying uselessly.

## NEXT UP — the SPIN FLICKER

**The problem, in JJ's words (2026-08-05):** *"one of the biggest visual issues is when spinning
the camera — the frame rate of the environment can't keep up with the speed of the camera, and it
can cause headaches."* Then, crucially: ***"it's more of a spin flicker than a spin smear."***

**JJ's call, and it is the right one: the cause is the same either way.** I briefly re-ranked
this on the word "flicker", reading it against a BerZerker passage about pipeline-depth jitter.
JJ pushed back — *"I think the cause is the same regardless"* — and that holds up: when the two
eyes sit at a large horizontal angle apart, the visual system cannot fuse them and alternates
between them instead. **Flicker is what a big uncorrected inter-eye offset looks like**, and it
scales with spin speed, which matches exactly what JJ sees.

**So §1 (yaw folding) is the work.** §2 and §3 are cheap secondary checks, worth keeping because
they cost almost nothing and would compound the same symptom — not because they replace §1.

**Status 2026-08-05: §1 is BUILT and DEPLOYED (build YAWFOLD), awaiting the first in-game
test.** §2 and §3 are still untouched — do them only once §1 has been judged in the headset.

---

### 1. Yaw folding (camera-rotation compensation)  ← BUILT 2026-08-05 (build YAWFOLD), UNTESTED

**As built.** Three small pieces, no new rendering and no extra GPU cost:

1. `camera.cpp` publishes `akvr_camera_base_yaw_deg()` — the game camera's own heading,
   taken from the base rotator the epilogue stub saves *before* our head rotation is
   composed in. Head rotation is deliberately excluded: the runtime already corrects it,
   and including it would rotate the stale eye twice.
2. `xr.cpp` records that heading in `g_eyeBaseYaw[e]` at the same instant it records
   `g_eyePose[e]` (the last camera-finalize before a Present *is* the frame being copied,
   so the two are from the same moment by construction).
3. At layer time both eyes are re-pinned to the heading the camera has **now**:
   `views[e].pose.orientation` is pre-multiplied by a yaw of `(baseNow − baseAtRender)`
   about the tracking space's own vertical. The just-drawn eye's delta is zero by
   construction, so only the stale eye moves.

**Sign.** Game yaw and tracking yaw run opposite ways (`YAW_SIGN = -1`), and the
correction is itself the negative of the camera's turn — the two negatives cancel, so the
tracking-space turn is `+delta`. If a spin looks *worse*, that is the only thing to
change: type **−1.00** into the panel's **spin correction** slider (no rebuild).

**Guards.** Gameplay only (never on menu/loading screens), never in native-stereo mode,
and a delta larger than **25°** applies nothing at all — past a fast spin's worth of
angle that is a cut or a teleport, and swinging the image by it would be far worse than
the flicker.

**Controls / readout.** Panel tick box **spin correction (turn OFF to compare)** — the A/B
switch, kept separate from the strength slider so turning it off never loses a tuned value
(persisted as `spinfoldon`) — plus a **spin correction strength** slider (−1.5…1.5, default
1.00, persisted as `spinfold`). Both are live, no restart. A live line under them reads:
`spin correction: L +0.00  R -1.83 deg   peak 4.10`. The peak decays slowly so a number
survives long enough to read after a spin — a peak that stays 0.00 while spinning means
the fold never fired. Also written to `akvr_startup_log.txt` under `patches:`.

**The trade it makes, and why the tick box exists.** A yaw of the whole stale eye aligns
everything at infinity — the skyline, the buildings — but it does NOT correct the boom's
sideways slide, which is what near objects need. Batman is the nearest thing on screen, so
**expect the fold to swap a whole-frame flicker for a local mismatch around the character**.
Whether that is a better trade is a stereographer's judgement, not a measurement, so JJ needs
to see both (his call, 2026-08-05, before the first test — and he predicted this artifact from
the description alone). If the character mismatch is the worse evil, the next move is a
partial gain (~0.5) rather than off: it splits the error between the far field and the near.

**How to test.** Stand still, spin with the right stick, and flick the tick box back and
forth — that is the comparison. Then sweep your head while spinning: the **ROLL diagnostic** must stay level
(`camera-final` ~0). Sign and multiply-order errors here reproduce the old
roll-in-stereo bugs exactly, and that line is the fastest way to catch one.

### 2. Eye identity — publish it, don't infer it  (cheap secondary check)

**Found 2026-08-05, untested.** `xr.cpp` (~line 914) decides which eye a frame is by taking the
**parity of a counter**:

```
uint64_t fc = akvr_camera_finalize_count();
g_submitEye = (int)(fc & 1ULL);
g_injectEye = (int)((fc + 1ULL) & 1ULL);
```

That is inference. The BerZerker doc's rule is the opposite: **the camera cave should publish
which eye it actually applied, and Present should consume that value.** We already have the
value — `camera.cpp` sets `g_curEye` immediately before it applies the eye offset. If the
finalize function ever runs twice in a frame (a second camera, a re-finalize) or a frame is
dropped, parity slips, the eyes swap, and you get a flicker that appears **exactly under heavy
motion** — which is when extra camera evaluations are most likely.

- Publish the applied eye from the cave alongside the finalize count.
- Have Present consume it instead of recomputing parity.
- Add a counter of "frames where inferred parity disagreed with the published eye" to the panel.
  If that number is non-zero while spinning, this is the whole bug.
- Guide 07 also describes the companion mechanism we do NOT have: on detected slip, **skip
  exactly one present** to re-phase. Consider adding once the disagreement counter proves the slip
  is real.

### 3. Steady the pacing / pin the pipeline depth  (cheap secondary check)

Comfort tracks frametime **variation**, not frame rate: measured uncapped 2.6–16.8 ms (jitter
14.2) vs capped-120 7.7–9.0 ms (jitter 1.3) — ~11x steadier at a *lower* average. Two cheap
experiments: cap to a steady 90, and hold the CPU no more than one frame ahead of the GPU with a
fence. The cap is a five-minute test and needs no code.

---

### Why the spin flicker happens

Under AER each eye is always one frame behind the other, so the two eyes look at **two different
moments**. The headset already hides that for *head* turns: it rotates the older picture by the
difference between the head pose the picture was drawn at and the head pose now (Asynchronous
Time-Warp). We feed it what it needs for that — `xr.cpp` submits `views[e].pose = g_eyePose[e]`,
the per-eye render-time pose. That is the same mechanism Luke Ross relies on.

**Stick spins get none of that.** The world turns but the head does not, so the runtime is told
"nothing changed", applies no correction, and the stale eye keeps showing the old camera angle.

### The fix

Add the game camera's own turn into the head angle we report, so a stick spin looks to the
runtime exactly like a head turn and its existing correction handles it. Ross calls this
**camera rotation compensation / "yaw folding"**. No new rendering, no extra GPU cost — we are
just feeding better information to a correction that already runs every frame.

### Yaw-folding steps

1. **Publish the finalized camera rotation.** `camera.cpp` computes `fyaw/fpitch/froll` in the
   epilogue (line ~980) but never exposes them. Add an accessor + a per-eye snapshot taken at the
   same instant the eye's head pose is captured, so the two are always from the same frame.
2. **Record it alongside `g_eyePose[e]`** in `xr.cpp` (where the per-eye pose is already stored,
   ~line 1061). One struct per eye: head pose + game-camera rotation.
3. **Bias the submitted pose.** At submit time compute the delta between the eye's recorded
   camera rotation and the current one, and rotate `views[e].pose.orientation` by it, so that
   `display_pose ⊖ submitted_pose` equals `head_delta + camera_delta`.
4. **Verify before believing it.** Watch the existing **ROLL diagnostic** (head vs game vs
   display roll) — sign and multiply-order errors here reproduce the old roll-in-stereo bugs
   exactly, and that is the fastest way to catch one. Test: stand still and spin with the right
   stick; the smear should drop sharply. Then sweep the head while spinning — the horizon must
   stay level.
5. **Then** try the two secondary checks (§2 eye identity, §3 a steady 90 cap) and see whether
   anything is left.

### Expect a partial win, not a cure

Ross states smooth stick rotation works *"as long as you're in first person"*, and the reason is
geometric. In first person a stick yaw is a **pure pivot about the eye point**, which is exactly
what time-warp corrects. Arkham Knight is third person: the camera **orbits Batman on a boom**, so
a yaw is a pivot **plus a translation along the arc**. Time-warp is rotation-only and cannot fix
translation without depth.

Order of magnitude: at ~180 deg/s and 85 fps the eyes are ~12 ms apart = ~2.1 deg of arc; on a
2–3 m boom that is **7–11 cm of camera travel, comparable to the whole IPD**. So expect the
rotational component (the dominant, most nauseating part) to go, leaving a smaller parallax
mismatch.

**Fixing that remainder properly needs a depth layer** (`XR_KHR_composition_layer_depth`) so the
runtime can reproject positionally — and we measured **zero depth binds ever reaching our hooks**
in this game. Do not plan around getting AK's depth buffer. The full answer would be AER v2-style
frame generation (optical flow; Ross ships `cudart64_12.dll` for it), which AK could in principle
support since it exposes motion vectors — but that is a project, not a patch.

Background and sources: memory `aer-v2-and-yaw-folding`.

## Build

```powershell
cd akvr
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # first time only
cmake --build build --config Release
```

Output: `build\Release\version.dll`

## Deploy (every build)

The DLL must be copied into the game's exe folder after **every** build, and the game
must be **closed** while copying or the file will be locked.

The CMake build now tries to copy the DLL automatically after linking. If the game is
running, it prints `Deploy skipped: file locked or game running` and the build still
succeeds. To copy manually, run:

```batch
akvr\deploy.bat
```

If your install path is different, edit `akvr\deploy.bat` and `AKVR_GAME_DIR` in
`akvr\CMakeLists.txt`.

## Install

Copy `build\Release\version.dll` into the game's exe folder (next to `BatmanAK.exe`):
`E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\`

- AER stereo works standalone (no geo-11 needed). The geo-11 native-stereo path is
  shelved — the full geo-11 stack currently lives in
  `..\_disabled_mods_backup\native_stereo_experiment_2026-07-24\`.
- Run Virtual Desktop (VDXR as the active OpenXR runtime) so a Quest 3 OpenXR runtime
  is available.
- Disable Steam / GeForce / RTSS overlays for the game (overlay stacking is the #1
  false-alarm crash source).
- **Uninstall = delete `version.dll`.**

## Controls

**Settings menu (recommended):** hold **Menu + View (Start + Back) together for ~0.5s**
to open the interactive menu. (Moved off L3+R3 on 2026-07-25 — the stick clicks double
as a game camera action. The chord is hidden from the game while held so Start doesn't
open the pause menu underneath ours.) Navigate with the left stick / D-pad, **A** to edit a
slider then left/right to change it. The game ignores the pad while the menu is open;
hold L3+R3 again to close. All changes persist across launches.

**Keyboard shortcuts (still work alongside the menu):**

| Key | Action |
|---|---|
| F8 | show/hide the AKVR panel (passive, no pad capture) — also mirrors it to the monitor for screenshots |
| F11 / F12 | toggle VR head-tracking / recenter forward |
| F7 | freecam (debug: we take full control of the camera) |
| F9 / F10 | retry camera hook / retry OpenXR connect |
| F6 | screen-mode (float the image world-fixed — for menus/loading) |
| F5 | wide render / centre-crop (escape the 16:9 mail-slot) |
| `[` `]` | AER stereo depth (half-separation); 0 = flat |
| PgUp/PgDn | lean gain (world scale) |
| Home/End | VR FOV trim |
| Insert/Delete | menu/loading screen size |
| `\` | switch stereo mode (AER ⟷ native; native shelved) |

**Settings menu** also adds a **convergence** slider (next to 3D depth) that slides the
stereo window forward/back by applying a per-eye NDC X offset to the projection matrix.

## Source layout

| File | Role |
|---|---|
| `src/hooks.cpp` | Present/ResizeBuffers hooks, ImGui panel + interactive settings menu, per-Present orchestration, settings persistence, live resolution |
| `src/xr.cpp` | OpenXR: session, per-Present frame loop, head pose (seqlock), headset display (AER eye swapchains + projection layer with centre-crop for wide-render), overlay quad layer |
| `src/camera.cpp` | camera find/read (M2), freecam (M3), additive head-pose injection at the finalize epilogue (M4), projection-matrix convergence patch (RE04) |
| `src/gamepad.cpp` | XInput interception (game ignores pad in menu mode) + controller→ImGui nav feed |
| `src/openvr_beacon.cpp` | OpenVR presence beacon (diagnostic, currently disabled) |

## Design notes

- **Why `version.dll`, not `d3d11.dll`/`dxgi.dll`?** geo-11 occupies both of those.
  Proxying `version.dll` (a direct Batman import) lets us coexist with a geo-11 stack.
- **Two hooks:** kiero + MinHook detour the DXGI swapchain `Present`/`ResizeBuffers`
  (frame loop + display); a hand-assembled code-cave at the camera-finalize epilogue
  (`0x1401297f7`) adds our head-pose delta to the camera fields (`0x574`–`0x58C`).
  Camera details in [`CAMERA_MAP.md`](CAMERA_MAP.md).
- **Shared D3D11 device across threads** needs `ID3D11Multithread` protection (set in
  `init_imgui`) or the game hard-freezes at splash.
- **Smoothness is a hard requirement** — one OpenXR predicted pose per frame feeds
  BOTH the camera injection and the submitted layer pose; no low-pass pose filtering.
- Save backups: `Steam\userdata\<id>\208650\` before testing.
