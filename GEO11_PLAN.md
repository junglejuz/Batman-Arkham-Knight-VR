**Current build PRESENTPAIR, 2026-09-25:** HUD correction confirmed much better.
Jitter unchanged with OneFrameThreadLag=False; splash/loading images rock solid.
Latest F1 (23:50:35.552) preserved in `akvr/diagnostics/jitter-20260925-235035/`.
Last 15s have constant base rotation, stable gameplay/FOV, one camera finalize per
Present, median 22.022ms cadence. No evidence for damping. Deployed a reversible
native Katanga submission timing test: `nativeafterpresent=1`, panel checkbox
"frame timing test: capture after Present". Compare ON/OFF and F1 in both states.
F1 now writes `_present.csv` with sparse pre/post GPU sample hashes and CPU
xrWaitFrame/begin/submit/Present durations. Producer timing remains a hypothesis,
NOT a confirmed fix. Release and 3 tests passed; headset test pending.
Preserve projvr=1, swapeyes=1, hudaspect=1, wide=0. See PLAYBOOK_REVIEW.

**Current build HUDASPECT, 2026-09-25:** User confirms swapped eyes and projVR
improve VR. Preserve `projvr=1`, `swapeyes=1`, `wide=0`; old claims below that
projVR should be off are historical. Remaining: severe jitter/lag and stretched
HUD. F1 preserved/analyzed in `diagnostics/jitter-20260925-233832/`.
Deployed reversible gameplay HUD pixel-aspect correction (`hudaspect=1`, panel
checkbox), timestamped F1 captures, and a separate generated-config comparison
`OneFrameThreadLag=False`. Build + 2 tests pass; headset acceptance pending.
No damping added. Trace shows quiet headset signal in a steady window, long
45 Hz sections, and unresolved base-camera jumps. See latest PLAYBOOK_REVIEW
for measurements, source evidence, backup and next test; do not call jitter fixed.

# AKVR — the geo-11 fork: TRUE simultaneous stereo

> Current status reviewed 2026-09-25: the integration below subsequently worked
> in the headset (see `../RESUME.md`). The remaining issue is vertical view
> coverage. See [the restart review](PLAYBOOK_REVIEW.md) for the checked baseline,
> playbook findings and next controlled test. The dated milestones below are
> historical; their "blocked" and "untested" labels are not the current status.

**Status: planned, not started. 2026-08-05.**
Backup of the working AER build: `backups/akvr-AER-working-2026-08-05/` (source + deployed
`version.dll` + settings). If geo-11 fails, restore that and fork again to the Luke-Ross
half-rate method instead — the alternative is written up at the end of this file.

---

## 1. Why we are doing this

Not a preference — a measurement. From `ak-inter-eye-baseline-measured` (build TRACE, 7162 rows
of real play):

| game camera turn rate | camera movement BETWEEN the two eyes | vs real IPD (6.5 cm) |
|---|---|---|
| not spinning | 5.5 cm | 0.8x |
| medium 60-150 deg/s | 13.6 cm | 2.1x |
| fast 150-300 deg/s | **22.0 cm** | **3.4x** |
| very fast 300-900 deg/s | **36.4 cm** | **5.6x** |

Under AER the two eyes are one frame apart in TIME, therefore one frame apart in SPACE. During a
spin Arkham's camera swings up to 22 cm along its boom arc between them, so the brain is handed a
stereo pair with a baseline 3.4x too wide, pointing along the arc instead of along the eye line.
**That is the spin flicker, and it is translation, not rotation** — which is why yaw folding could
only ever be a partial fix.

**It is also unfixable by moving the camera.** Every rendered frame is displayed twice (fresh eye,
then stale eye of the next pair), so a per-frame position correction would have to satisfy
`P_N + f(N) = P_{N-1} + f(N-1)` for all N — i.e. the camera never moves. Any alternating
correction makes one pair perfect and the next twice as bad. Proved before building; do not
re-derive.

**geo-11 renders the scene TWICE per frame, so both eyes come from the same instant.** It removes
the cause rather than trading against it: no stale eye, no flicker, no yaw folding, and no per-eye
temporal-history problem if DLSS ever follows.

---

## 2. What we already have (do not rebuild these)

- **A working Katanga consumer** in `xr.cpp`: `katanga_update()`, `g_katMap` / `g_katView` /
  `g_katTex`. The *handshake* is verified line-by-line against VRScreenCap's
  `katanga_loader.rs`: open `Local\KatangaMappedFile`, read a HANDLE at offset 0,
  `OpenSharedResource` it as a D3D11 texture. **The handshake is not in doubt. The LAYOUT is.**
- **A native-stereo display path**: `g_native` with `eyeW = g_native ? w/2 : w`.

### ✅ THE SHARED SURFACE IS SIDE-BY-SIDE — SETTLED BY LOOKING AT THE PIXELS, 2026-08-06

Do not reopen. A standalone probe (`scratchpad/katprobe.cpp`, MSVC 2022, ~90 lines) opens
`Local\KatangaMappedFile`, reads the HANDLE at offset 0, `OpenSharedResource`s it, prints the
desc and saves the image. Result, live: **5120 x 1440, DXGI_FORMAT_R8G8B8A8_UNORM (28)**, and the
saved picture shows **two eyes left/right** with visible horizontal parallax between them.

**Note the trap that nearly cost us this:** geo-11's MONITOR preview in katanga mode is
**over/under**, while the SHARED surface is **side-by-side**. Two different surfaces. Judging the
shared layout by what is on the screen gives the wrong answer — which is exactly how the
over/under belief arose. Always probe the surface itself.

geo-11's own log names the surface **`DoubleTex`** and prints its desc:
**5120 x 1440, R8G8B8A8_UNORM**, with the game window at 2560x1440. That is the window
**doubled in WIDTH** — side by side, one eye per half, 2560x1440 per eye.

- **`xr.cpp` halving the width (`eyeW = w/2`) is CORRECT as written.** No change needed.
- The pre-test expectation was over/under (from hands-on recollection); VRScreenCap's
  hard-coded `StereoMode::FullSbs` in `katanga_loader.rs` was right all along, and so was the
  Titanfall note in `tf2vr-geo11-katanga`.
- Making the split a setting rather than a constant is still worth doing as cheap insurance,
  but it is no longer load-bearing.
- **The camera already centres itself for native stereo** — `camera.cpp` applies no AER eye offset
  when `g_curEye < 0`, and `xr.cpp` sets `g_injectEye = -1` in the native branch. So switching
  modes does not need camera surgery.
- **We own the game's render size outright** — the engine-res patch (`install_engine_res`,
  earlyres.cpp) writes AK's own ResX/ResY globals before the engine reads them. **This is the key
  to the square-resolution question in §4.**
- **We are `version.dll` on purpose**, precisely so geo-11 can own `d3d11.dll` / `dxgi.dll`.
- The geo-11 package is at `references/Geo-11_0.6.198/` (d3d11.dll, d3dx.ini, d3dxdm.ini,
  nvapi64.dll, d3dcompiler_47.dll).

---

## 3. Milestone 1 — get geo-11 to run AT ALL, on its own

**Re-open the "shelved" verdict with fresh eyes.** The README says geo-11's shared-texture
pipeline never activated with our DLL co-loaded (`err=2`), blamed on a hook-chain conflict with
kiero. **Evidence found 2026-08-05 says that diagnosis was probably wrong:**

- `Binaries/Win64/d3d11_log.txt` from that July attempt is **19.5 MB, not empty** — geo-11 ran and
  logged heavily. The "geo-11's log stays empty" note is contradicted by the file itself.
- That log records **`direct_mode=11`**. The shipped default is `direct_mode = katanga_vr`, and
  reading the enum list in `d3dxdm.ini`, 11 lands on one of the **nvidia_dx9 / nvidia_dx11**
  modes — "requires 3D Vision Driver", "works up to driver 452.06". In that mode geo-11 would
  never publish the Katanga mapping at all, and `err=2` (file not found) is exactly what you would
  expect.

So step 1 is not debugger work. It is:

1. Install geo-11 into `Binaries/Win64/` (d3d11.dll, d3dcompiler_47.dll, nvapi64.dll, d3dx.ini,
   d3dxdm.ini). **Move our `version.dll` out first** — one variable at a time.
2. Set `direct_mode = katanga_vr`, and `shader_regex_patch_mode = 4`. That second one is not
   optional: geo-11's own d3dxdm.ini calls out **Batman Arkham Knight by name** as a game that
   loads its entire shader set at launch, where mode 5 causes "unacceptable launch delays".
3. Launch with NO AKVR. Verify: `d3d11_log.txt` shows `direct_mode` parsed as katanga_vr, and
   `Local\KatangaMappedFile` exists while the game runs.
4. Confirm stereo is actually being produced (geo-11's own overlay / hotkeys, or point JJ's
   existing Katanga reader from the Titanfall work at it).

**Exit criteria:** the mapping exists and carries a stereo image, with AKVR absent.
**If it fails here, geo-11 is dead for AK and we go to §7.**

---

## 4. Milestone 2 — ⛔ SQUARE OUTPUT IS NOT AVAILABLE. TESTED 2026-08-06.

**This section was wrong. Square is reachable and it DESTROYS the depth.**

- **Size is free and needs no patch at all.** The game honours `ResX`/`ResY` in
  `BmSystemSettings.ini` on RESTART (it never resizes its swapchain while running, so the change
  looks ignored until you relaunch). 1920x1440, 2560x1440 and 2688x2688 were all accepted —
  **including sizes larger than the desktop.** `install_engine_res()` is therefore NOT needed for
  render size on a clean install. The old "the game ignores ResX/ResY" measurement was taken on
  the install that later turned out to be poisoned.
- **Shape is NOT free.** At 2688x2688 the stereo broke in a specific way, on the MONITOR as well
  as in the headset: a foreground car reading as a flat card in front of everything, background
  trees in the wrong plane, the whole depth field compressed. Convergence and separation could not
  touch it — and they never could, because they slide the zero plane and scale depth *globally*;
  neither can misplace one object relative to another. **2560x1440 fixed it completely.**
- **Why:** the Arkham fix's shader corrections are written against a 16:9 frame, and AK constrains
  its camera to 16:9 anyway (`arkham-16-9-aspect-lock`). Hand them a square frame and their depth
  maths lands in the wrong place.

**Consequence: §5 is no longer optional.** We cannot get a square per-eye render out of the game,
so the per-eye shape has to be reconciled at the projection or at submission — which is precisely
what projVR exists for. Plan for a 16:9 render feeding a square-ish lens, not for a square render.

**JJ's calibrated stereo, saved in `d3dxdm.ini`:** `dm_separation = 30`, `dm_convergence = 100`,
`dm_auto_convergence = 0` (auto-convergence OFF — a self-moving depth plane is unpleasant in a
headset and would fight projVR).

Two things to watch on size:

- **Pick N from the headset, not from habit.** VD's per-eye surface was measured at 2496x2688 and
  2688x2880 depending on VD quality. Start at **N = 2688** (7.2 MP per eye) and tune.
- **Cost: geo-11 renders the scene twice.** Our AER build pushes 11.1 MP once; two 2688-squares is
  14.5 MP. Expect to trade resolution for true stereo — and note that true stereo at a lower
  resolution may well beat AER at a higher one, which is the whole point of the experiment.

---

## 5. Milestone 3 — who owns the projection?

**This is the real collision, and it is not resolution.** Both sides touch the projection:

- **projVR** (ours) MinHooks `BuildProjectionMatrix` and rewrites `m00`/`m11` to the Quest's
  per-eye half-FOV.
- **geo-11** derives its stereo from the projection/view and its shader fixes assume the game's
  own matrices.

Test in this order, one at a time:

1. **projVR OFF, square render on.** Let geo-11 see AK's untouched projection. If the game renders
   a square and geo-11's stereo is correct, we may not need projVR at all — a square render with a
   square FOV may already match the headset.
2. **projVR ON.** Watch for geo-11's stereo separation going wrong (flat, doubled, or
   depth-inverted), which would mean its shader fixes are keying off the matrix we rewrote.
3. If they fight, prefer **geo-11 owning stereo, us owning FOV** — i.e. keep the FOV rewrite but
   make sure it is applied identically to both eyes so it cannot introduce vertical disparity.

Also settle **who owns separation/convergence**: geo-11 has `dm_separation` (TF2 runs 25) and its
own convergence. Ours (`akvr_head_stereo`, the ±NDC-X jitter-slot convergence patch) must be
**disabled** in this fork, or the two stack. The world-scale calibration of 1.00 = the old 2.8
does not carry over — geo-11's separation will need recalibrating from scratch.

---

## 6. Milestone 4 — integration back into AKVR

**BUILT 2026-08-06, build `GEO11FEED`, deployed, UNTESTED.** What changed:

- **AKVR now detects geo-11** by the presence of `d3dxdm.ini` next to us, in DllMain, before the
  engine reads anything. When it is there, three things stand down automatically, no ini editing
  and no ticking of boxes:
  - the **engine-res patch and the fake monitor size** (`g_engW/g_engH = 0`, `g_enabled = false`)
    — §4 proved a square render destroys geo-11's depth, so the game keeps its own 16:9 ini size;
  - **`OneFrameThreadLag`** — an AER-only measure (see below);
  - **`Fullscreen` / `WindowDisplayMode`** — forced windowed only ever *because* we rendered
    bigger than the desktop. We don't, now. The known-good geo-11 config is left untouched.
- **The eyes now come from geo-11's shared surface, not the swapchain** (`g_katFeed`, on by
  default, `katfeed=` in the ini, and a panel tick box **"true stereo from geo-11 (turn OFF to
  compare)"** that only appears when geo-11 is installed). This is THE integration: in katanga
  mode the swapchain carries only geo-11's over/under monitor preview, while the real pair lives
  on the 5120x1440 side-by-side `DoubleTex`. Everything downstream is unchanged — the existing
  native path already splits SBS and sizes the eye swapchains from the source desc.
- The backbuffer MONO/SBS probe is skipped on that path (it would stall on another renderer's
  texture for an answer we already know).
- `akvr_startup_log.txt` now prints **`geo-11 stereo feed: mode= / feed= / <katanga status>`** —
  that one line answers the coexistence question.

### ⛔ MILESTONE 4 IS BLOCKED — geo-11 STOPS US REACHING THE HEADSET AT ALL (2026-08-07)

The mod loads, hooks, and runs alongside geo-11 perfectly. What it cannot do is get a headset:
every `xrGetSystem` returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE`, all session long. Virtual
Desktop's own log names our app and says **"Virtual Desktop Server is not running"** — while the
same headset serves a separate viewer process at that very moment.

**Bisected to geo-11's `d3d11.dll`:** both geo-11 DLLs gone → VR works instantly; only its
`dxgi.dll` gone → still no VR (and, note, **stereo still worked** — so dxgi.dll is NOT required,
correcting an earlier finding). VDXR composites *inside the game process* and must create its own
D3D11 device there; geo-11 owns d3d11.dll and refuses that — exactly as it refused kiero's probe.

Ruled out, all tested: `load_library_redirect=0`; geo-11's private `nvapi64.dll` removed;
`allow_create_device=2`; claiming the headset early from the DLL-load thread (kept — better
design anyway); rebuilding the XrInstance every 4 s (kept); **SteamVR as the runtime — rejected
outright by JJ, never propose it again.**

Full detail and the ruled-out list: memory `geo11-blocks-inprocess-openxr`.

**Three ways out, JJ's call:** (1) load geo-11 under another name so it HOOKS instead of being
`d3d11.dll` — untried, and the only route that keeps everything; (2) headset side in its own
process via `batvision` (proven working, already publishes head pose) — costs the in-game panel
and world-locked menus; (3) retreat to §7.

### ROUTE 1 BUILT 2026-08-08, build `GEO11HOOK`, deployed, UNTESTED

**Do NOT confuse this with geo-11's `hook=` ini setting.** That setting makes geo-11 patch COM
vtables; it collides with our MinHook detours on the same swapchain slots and **crashed the render
thread on 2026-08-07**. It stays commented out. The mechanism here is different and needs no ini
key at all: 3DMigoto's own DllMain does
`if (hinstDLL != GetModuleHandleA("d3d11.dll")) HookD3D11(hinstDLL);`
(`references/3Dmigoto/DirectX11/DLLMainHook.cpp:335`), Nektra-hooking `D3D11CreateDevice` and
`D3D11CreateDeviceAndSwapChain` **inside the genuine d3d11.dll** instead of replacing the module.
The real DLL is then present and whole, and only those two entry points are intercepted —
`D3D11Core*`, the `D3DKMT*` kernel thunks and the DXGI device path all reach Microsoft's code
untouched. That is the entire hypothesis for why VDXR might find the headset this time.

- **Switching is two batch files in `Binaries\Win64`:** `AKVR-geo11-HOOK.bat` and
  `AKVR-geo11-WRAP.bat` (both call `AKVR-geo11-mode.ps1`; add `-Mode status` to just look).
  HOOK renames `d3d11.dll` -> `geo11.dll`, parks geo-11's `dxgi.dll` as `dxgi.dll.wrapmode`
  (**not needed for stereo — measured 2026-08-07**), and sets `load_library_redirect=0`.
  Nothing is deleted; WRAP puts it all back. **Currently switched to HOOK.**
- **`load_geo11_hooked()` in hooks.cpp** does the load. Three preconditions, all of which make
  geo-11's DllMain return FALSE (i.e. `LoadLibrary` just fails) if missed:
  `dxgi.dll` already in the process; `d3d11.dll` already in the process (HookD3D11 uses
  `GetModuleBaseAddress` and cannot LoadLibrary from DllMain); and **not from our own DllMain**
  either — loader-lock deadlock, guide 02 §3. BatmanAK.exe imports **neither** d3d11 nor dxgi
  statically (it imports **d3d9.dll**), so nothing has loaded them for us.
- **ORDER, and it matters:** on a shared function whoever hooks SECOND is OUTERMOST. So
  `install_creation_hooks()` runs first (it also loads the real dxgi/d3d11) and geo-11 goes on top:
  game -> geo-11 (draws both eyes) -> us (Present, both eyes done) -> real. Loading geo-11 first
  would invert the nesting and we would run *before* its stereo work.
- Both of those, plus the vp-square patch, were **moved ahead of the headset probe loop**, which
  can sit for 40 s — we were only winning that race because UE3 spends the time loading shaders.
- **Config the bisect left broken, both fixed:** `direct_mode` was still `sbs` (no Katanga surface
  at all; original kept as `d3dxdm.ini.sbs-bisect`) and `katfeed=0` in `akvr_settings.ini`.
- **Read this line in `akvr_startup_log.txt`:** `geo-11 install: HOOK/WRAP  d3d11.dll = <path>`.
  Under HOOK that path must be **System32**. `note()` also logs
  `geo-11 HOOK mode: geo11.dll LOADED/FAILED (dxgi=1 d3d11=1 err=N)` — FAILED with both
  preconditions at 1 means geo-11's DllMain refused for its own reasons, not a load-order bug.
- **Known risk, and the first suspect if the game dies at launch:** we and geo-11 both hook
  `D3D11CreateDeviceAndSwapChain` in the real d3d11.dll (MinHook, then Nektra on top). If that
  double detour is the problem, drop OUR hook on the geo-11 path — the factory-vtable hooks on
  `CreateSwapChain`/`CreateSwapChainForHwnd` still cover swapchain creation on their own.
- **Honest caveat:** VDXR's own device creation still passes through geo-11's hook of
  `D3D11CreateDevice`. If the blocker was that specific call rather than the module identity, the
  rename changes nothing and we go to route 2.

1. Re-introduce `version.dll` alongside geo-11 and re-check §3's exit criteria. **If the mapping
   dies the moment we load, THEN it is a hook conflict** — and only then is the debugger session
   justified. First things to try: delay kiero init until geo-11 has initialised; hook the game's
   real swapchain from our existing `CreateSwapChain` hook instead of kiero's dummy swapchain.
2. Force `g_native = true`: camera centred, no AER offset, no yaw folding, eye parity irrelevant.
3. Feed the Katanga texture to the OpenXR layer instead of the backbuffer. **We keep native OpenXR
   submission** — we do not hand off to an external desktop viewer. That is our advantage over
   BerZerker's Katanga-surface mod and it keeps per-eye poses, world-locked menus, the overlay
   quad and 6DOF injection intact.
4. Head tracking, HUD sizing, comfort settings and the render-size patch all stay exactly as they
   are — none of them touch stereo.

**Verification that this was worth it:** re-run the camera trace (F1) during spins. The inter-eye
figures in §1 should collapse toward 0, because both eyes now come from the same instant. That is
the whole thesis, and it is measurable rather than a matter of opinion.

---

## 7. If geo-11 fails — the fallback

Restore `backups/akvr-AER-working-2026-08-05/` and build **non-overlapping AER pairs**: display
frames in couples (N, N+1), then (N+2, N+3), so each rendered frame is used ONCE. Then a
per-frame position correction of `±(v·dt)/2` IS valid and both images of every pair are
simultaneous. Cost: the stereo pair updates at half rate (head rotation stays smooth, because the
compositor still time-warps each eye). This is almost certainly what Luke Ross's **"1/2 rate"** and
**"1/3 rate"** modes are.

DLSS is mutually exclusive with geo-11 (see `aer-v2-and-yaw-folding`), so it belongs to the
fallback branch, not this one.
