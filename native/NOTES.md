# AKN — Arkham Knight native-stereo test (separate from AKVR)

Started 2026-10-06 at JJ's request. Question: can Arkham Knight draw both eyes itself (like Batman City), so each
eye can get its own DLSS, and is it worth leaving geo-11 for? **Decision rule (agreed with JJ):** measure a doubled
frame against the current geo-11 mod at the same picture size; within ~10% of geo-11's cost -> go on (DLSS per eye,
then lower internal resolution); clearly more expensive -> stay on geo-11.

Sources read first (VR_ESSENTIALS rule): `batman_city/RE_NOTES.md` "The viewport-draw root", `batman_city/
BATMAN_CITY_PLAN.md` §2.1-2.3 (BS Infinite: double the viewport root on the per-frame caller only; BL1: doubling the
client Draw corrupted the heap), playbook 17 #side-effect-gate. DLSS reference: `references/Luma-Framework`
(Custom MIT: credit Filippo Tarpini / Luma; commercial use needs permission) - `Source/Games/Batman Arkham Knight/
main.cpp` (SMAA pass hashes, MVs = the R16G16_FLOAT target, depth = SRV1 of SMAA edge detection, jitter at
BuildProjectionMatrix 0x1400104FF) and `Source/Core/dlss/DLSS.cpp`.

## Static map (STATIC; BatmanAK.exe Steam 54,632,960 bytes; image base 0x140000000, ASLR on -> use RVAs)

Tool: `re/ak.py` (pdata bounds, RIP-relative xrefs, call census).

| VA | What | How found |
|---|---|---|
| 0x140BF8000 | **FViewport::Draw(this, bShouldPresent)** - BeginDrawing cmd (vtable 0x1424E38E0), viewport vtbl+0xD8, canvas (0xB8 bytes, ctor 0x14087F260) at vp+0xB4, **ViewportClient (vp+0x5C) vtbl+0x18 (vp, canvas)** at 0x140BF83FF, canvas flush 0x140880000, EndDrawing cmd (vtable 0x1424E3908, present flags) | DescribeCommand strings "BeginDrawingCommand"/"EndDrawingCommand" -> describe fns 0x140BDDE60..90 -> vtables 0x1424E38B8/E0/3908 -> RIP refs |
| 0x140AD3EA0 | **UGameEngine::RedrawViewports(this, bShouldPresent)**: GameViewport = engine+0x2BC, its FViewport = +0x64, Draw at 0x140AD3EE0 (RA 0x140AD3EE5); secondary viewports engine+0xBEC/0xBF4 | only callers of Draw |
| 0x140AF003F | the per-frame call of RedrawViewports inside 0x140AEE980 (engine tick), RA 0x140AF0044 | callers of RedrawViewports (other: 0x140AF2827 in 0x140AF2040) |
| 0x1409F6220 | **ULocalPlayer::CalcSceneView(player, family, &loc, &rot, viewport, drawer)** - calls BuildProjectionMatrix 0x140010420 | Draw's no-client player loop passes exactly these args |
| 0x1409F87B0 | viewport-client Draw A (vtable 0x141B906F8 slot +0x18) | caller of CalcSceneView, in a vtable |
| 0x14041BF00 | viewport-client Draw B (vtable 0x141D7C278 slot +0x18) | same |
| engine+0x84C / +0x854 | GamePlayers array / count | Draw's player loop |

## Milestone 1 probe (build 2026-10-06, not yet run)

`XINPUT9_1_0.dll` in the game folder (forwards to System32), active ONLY with `-akn` on the command line, so
Steam / the AKVR VR shortcut are unaffected (AKVR stays dormant without -akvr). Log: `<game>\akn\akn_log.txt`.
Run: `<game>\akn\AKN-Test.bat` (starts BatmanAK.exe -akn -akntest with SteamAppId 208650; Steam must be running,
no other Steam game). The auto test waits ~600 frames, then 25 s each of mode 0 (normal), 1 (second draw first,
no present), 2 (second draw first, with present), 0 again, with a SUMMARY line per phase: frames/s, ms per Draw,
second draws/s, faults, CalcSceneView per pass, which client Draw runs, process CPU.
To remove: delete `XINPUT9_1_0.dll` and the `akn` folder.

Read the result for: (1) faults = 0 and the game keeps running; (2) CalcSceneView runs in the second pass too (the
scene is really drawn twice); (3) frames/s mode 1-2 vs 0; (4) CPU. Same view twice = cost and stability only, not
stereo. Next: per-pass camera offset at the FSceneView inputs (Batman City "Stereo seam decision"), per-eye
ViewState, then the geo-11 comparison at the same size.

## LIVE results 2026-10-06 (flat 1280x720, 2D profile MaxFPS=30, gameplay in the city; logs in <game>\akn\)

- Run 1-2 (title screen): per-frame RedrawViewports caller is **RA 0xAF282C** (in 0xAF2040), not 0xAF0044 (once).
  FViewport::Draw takes 0.05 ms: it does NOT draw the world.
- **The split (LIVE + STATIC):** early in the frame UWorld::Tick (0xAA9960, from 0xAF21E1) calls the **scene entry
  0xBDDEC0(viewport)** at 0xAAA501: new FCanvas at vp+0xB4, then jmp client vtbl+0x10 = **client Draw A 0x9F87B0**
  (vtable 0x141B90700): builds the FSceneViewFamily (heap, ctor 0x821E90) and runs CalcSceneView per player.
  Late, FViewport::Draw (from RedrawViewports) calls client vtbl+0x18 = **0x9F1230**, which renders the family via
  **0x8E9E90(global, family=[r12+0x140], 0, float, int) at 0x9F1444** (RA 0x9F1449), frees it (dtor 0x823E10 at
  0x9F2406), then the HUD. A is called twice a frame (scene entry + from 0x9F1230 at 0x9F13C0); only the first
  builds views. The render call 0x8E9E90 also runs every frame from **RA 0x5B21F2** (in 0x5B1DA0, main loop).
- Client Draw A returns early at 0x9F88E3 while CanvasObject (UCanvas at client+0x48, "CanvasObject"/"Engine")
  holds its FCanvas (+0x94); the late pass clears it.
- Run 3 (double the scene entry, no canvas reset): stable 25+25 s, but the game's own pass skipped (the guard above).
- **Run 4 (scene entry doubled + canvas field reset): ~10 doubled frames, then a FATAL on a worker thread in
  Scaleform's AS3 VM** (crash report frames 0x1415DF0 "Can't dispatch '%s' - exception is not cleared", 0x13DAE50
  "getStackTrace"). Client Draw A does HUD/movie work too -> never run it twice (FAIL-HUD-014). Tool: re/crashmap.py.
- **Run 5 (render call 0x8E9E90 doubled at RA 0x9F1449, same family): stable all phases, but GPU load unchanged**
  (22.0 / 19.4 / 23.0 / 21.8 %), second call ~0.04-0.7 ms -> it returns early on a reused family. The render/s count
  from the 0x5B21F2 path also dropped in phase 1. Next: find the early exit (0x8E9E90 is 7 KB with many exits:
  0x8EBA51 when [rcx+0x3B8] != 0, 0x8EAE96/9F/A4...) - trace which exit the second call takes (hardware breakpoints
  via SetThreadContext + VEH on the game thread, 4 points a run), or build a fresh second family per eye
  (ctor 0x821E90 + CalcSceneView) instead of reusing one.

## 2026-10-07 — run 5 doubled the wrong function; run 6 built (STATIC, not yet run)

- **0x8E9E90 is not the world render.** STATIC: arg1 is a global (rip 0x14311C750), it walks a per-object array at
  [[[arg1+0x84]+0x54][0]]+0x680 (count +0x688) with flags +0xDC, and is called from 19 sites (engine tick, main loop,
  FViewport::Draw...). LIVE run 5 agrees: ~58 calls/s at 30 frames/s, 0.07 ms each. Run 5 measured nothing.
- **The world is queued by BeginRenderingViewFamily(canvas, family) = 0x81B670**, single caller 0x9F155E inside the
  late client draw 0x9F1230 (RA 0x9F1563), family = client+0x140 (built by client Draw A). Evidence: bumps a
  counter 0x143A3149C (frame number), allocates 0x1F0 bytes and runs ctor 0x818530(alloc, family, canvas+0x18
  hit proxies, top of the canvas transform stack canvas+0x38/+0x40, 0x50-byte entries), then enqueues a render
  command (the UE3 BeginRenderingViewFamily shape). Guard before the call: skipped when client+0xC0 bit 0x40, or the
  earlier per-player test set ebx, or a global count difference <= 0.
- **Run 6 probe** (XINPUT9_1_0.dll built 2026-10-07 01:44, installed): mode 1 calls 0x81B670 twice from RA 0x9F1563
  with the same family (two full world renders into the same target, same view = cost test, not stereo). The 0x8E9E90
  doubling is off. Auto test (-akntest, F10 in gameplay): 25 s each of normal, doubled, normal, doubled; SUMMARY lines
  now lead with WORLD/s and ms/world. Log GPU load beside it: `nvidia-smi --query-gpu=timestamp,utilization.gpu,
  memory.used,clocks.gr --format=csv -lms 500 > akn\gpu_log6.csv`.
  Read: faults 0; WORLD/s = 2x frames/s in doubled phases; GPU load roughly doubles (if not, the second render is
  culled/skipped on the render thread - next suspect the frame-number / occlusion state on the shared ViewState).
- After the cost figure: per-eye family (second CalcSceneView with an offset camera and its own ViewState), then the
  same measurement with geo-11 at the same picture size for the go/no-go.
- **Run 6 LIVE 2026-10-07 01:53 (log akn_log_run6.txt once archived):** stable. The second BeginRenderingViewFamily ran
  30/s in both doubled phases (second/s 30.0, faults 0, 0.03 ms to queue), 30 frames/s held throughout. Process CPU
  53/53 % (normal) vs 60/70 % (doubled) of one core - noisy, a hint only. **No GPU figure: the nvidia-smi logger was
  not started.** Fix: the probe now reads NVML itself (System32 nvml.dll) every 100 ms and logs GPU busy %, clock,
  busy x clock and VRAM per second and per phase (build "run 6b"). Same test otherwise.

## 2026-10-07 — JJ: "forget frame-rate tests, build it". M2 = engine stereo (two views, one family)

Plan: AK renders a double-wide window, left eye view in the left half, right eye in the right half, from ONE
CalcSceneView pass done twice (UE3 split-screen mechanics). AKVR's existing native-SBS path in xr.cpp (backbuffer
halves -> eye swapchains) then carries it to the headset; geo-11 not loaded in that mode. STATIC map (2026-10-07):
- ULocalPlayer: +0xA8/+0xAC Origin x/y, +0xB0/+0xB4 Size x/y (fractions of the viewport), +0xC8 ViewState,
  +0x5C ViewportClient. View rect = viewport size x those (top of CalcSceneView 0x9F6220).
- CalcSceneView(player, family, &loc, &rot, viewport, drawer): news 0xA20-byte FSceneView, ctor **0x82EB30**
  (34 args: family, ViewState=player+0xC8, -1, ..., a11..a16 = X, Y, X, Y, SizeX, SizeY floats, a17 ViewMatrix*,
  a18 ProjectionMatrix*; RA 0x9F7301), then appends to family->Views (family +0 data, +8 count, +0xC max), returns
  the view. Per-frame caller: client Draw A, RA **0x9F8B3A** (LIVE census run 6).
- **AllocateViewState(0, 0) = 0x82F860** (new 0x8850-byte FSceneViewState, ctor 0x82CE20); ULocalPlayer stores it
  at +0xC8 (0x9F93F8).
- A 0..3 counter at RVA 0x3098180 steps once per CalcSceneView (likely the temporal jitter phase): eye 2 gets eye 1's.
- M2 probe build: -aknstereo (or F9) -> stereo_calc: eye 0 Size.x/2; eye 1 Origin.x + Size/2 with the second
  ViewState; ctor hook moves each eye +-ipd/2 along view x (aknipd=<units>, default 6.4) and sets m00 = m11*h/w.
  Logs the first ctor inputs (rect, view, proj). Desktop check: both halves drawn, post effects per half.

### M2 LIVE 2026-10-07 03:05 — two views in one family: draws, but the post effects break
- aknvs=1 (own view state from AllocateViewState) -> render-thread FATAL on the first stereo frame: 0x7F4852 reads
  [obj+0x3B8] (0x83BF60) where obj = [[view+0x10]+0x750] (view+0x10 = ViewState); the ViewState ctor 0x82CE20 never
  writes +0x750 and GMalloc (0xFA2A40 -> vtbl+0x18) does not zero -> garbage pointer. Open: who sets it for the game's.
- aknvs=0 (eyes share the game's view state): stable, 31 frames/s, 2300+ stereo frames, both halves drawn
  (screenshot: pause scene, left and right views offset).
- JJ (live, at 2560x1440): left view "mostly correct but the glows are in the wrong place"; right view "Batman correct,
  everything behind him purple, masked by a really pixelated mask". = AK's post passes (bloom/glow, pause blur, the
  character mask) assume ONE view filling the target. Two-views-in-one-family is a dead end for AK without rewriting
  the post chain.

### M3 (built 2026-10-07, not yet run) — one full-size family per eye
- stereo_calc clones the game's still-empty family (0x70 bytes) and builds the right eye's full-rect view into the
  clone; BeginRenderingViewFamily renders the clone FIRST at RA 0x9F1563, then the clone is freed (dtor 0x823E10 +
  free 0xFA2A80); the game's family (left eye) renders next and gets the HUD.
- Scene renderer ctor 0x818530(this, family, hit proxies, transform, int) tags each renderer by family; render-thread
  RenderViewFamily 0x81B3C0(renderer) (from command 0x81B650) -> after it, the bound render target = that eye's
  finished world picture. command.txt "capture" saves akn\eye0.bmp / eye1.bmp from there.
- SWBF2 lessons to carry: shared auto exposure (one EV for both eyes), keep the TAA jitter in the projection,
  vendor effect libraries keep one buffer per frame (AK ships GFSDK_GodraysLib, GFSDK_PSM: check for leaks).

### M3 LIVE 2026-10-07 03:16-03:20 (2560x1440 flat, aknvs=0 shared view state)
- Stable: right-eye family renders first each frame, 30 frames/s, eye renders on the render thread 1:1 with frames,
  2 "other" scene renders total (not ours). Screen shows the left eye + HUD normally (loading scene).
- No render target is bound when RenderViewFamily 0x81B3C0 returns -> OMSetRenderTargets (context vtbl 33) hooked
  on the render thread to list what each eye's render writes. Order (both eyes identical): 2560x1440 f24, f28, f28,
  f26 (R11G11B10 HDR scene, reused all through), f54, f34, 640x360 f41, 1280x720 f61, f24, f28, f34, 640x360 f26/f10,
  2560x1440 f28, f49, f28, then 1280x720/640x360 f10 (RGBA16F) chain, last 256x16 f28.
- After the left eye's render, before the next frame's right eye, ONE target is set: 2560x1440 f28 from render
  command 0x874140 (stack AA55 <- 8741DE <- DC815D command loop), called from 0x880226/0x880973/0x881184 = canvas
  flush region (0x880000): sets the window buffer + clear before the HUD. Where the finished world reaches the window
  is still open -> next build saves every >=1000-wide target per eye on "capture" (eye<N>_<idx>_f<fmt>.bmp, HDR
  formats tone-mapped for viewing).
- D3D11CreateDeviceAndSwapChain returns no swap chain (the game makes it via the factory).

### After-scene draw log (LIVE 2026-10-07 03:3x, command "after")
After the left eye's scene render, every draw is Scaleform HUD (stacks in 0x11Dxxxx, small 704x544 / 576x64 movie
targets, then the window buffer 2560x1440 f28 set by 0x874140 from the canvas flush). The PS slots t1/t2 1280x720 f10
and t5 2560x1440 f10 are stale bindings left by the scene render; t5 (full-res RGBA16F) was never a render target in
the eye's list -> the finished world reaches the window buffer INSIDE each eye's render by a copy or compute pass that
the OMSetRenderTargets list cannot see. So the finishing is per eye already; we must read the right buffer.

## TEST PLAN M3b — one launch (JJ 2026-10-07: plan first, many tests per launch)
Launch: -akn -aknstereo aknvs=0, 2560x1440 flat; wait until eye renders > 1000 (3D scene up). All by command.txt.
| # | command | proves | judge |
|---|---|---|---|
| T1 | trace | every Copy/CopySubresourceRegion/Resolve/Dispatch(+UAVs)/OMSetRTandUAV in one right-eye and one left-eye render | where the finished picture is written |
| T2 | capture a | window buffer at the end of each eye's render (eye<N>_a_bb.bmp) + every full-size target/copy/UAV destination | eye1_a_bb and eye0_a_bb both finished (graded, glow) |
| T3 | ipd 40, capture b, ipd 6.4 | the eyes really differ (exaggerated separation) | b offsets larger than a |
| T4 | (offline from T2) | mean brightness per eye | within a few % (else shared exposure needed) |
| T5 | stereo 0 (15 s), stereo 1 (15 s) | cost: frames/s + GPU busy x clock per phase from the 1 s lines | ratio |
| T6 | vs 2, capture c, vs 0 | second eye without view state: alive? picture? | alive + finished |
| T7 | vs 3 (LAST: may crash), capture d | own view state with +0x750 copied from the game's | alive 10 s |
Follow-ups pre-decided: T2 finished -> next build keeps eye 1's buffer copy every frame (the VR source) and starts
AKVR integration; T2 not finished -> T1 names the writer, capture its destination; T7 crash -> hardware write watch
on the game's view state +0x750 to find the setter.

### M3b RESULTS (LIVE 2026-10-07 03:32-03:35, one launch, tools/Run-M3b.ps1)
- **T1 trace: each eye's render ENDS by writing the finished picture into the window buffer.** Last steps of the
  render: compute Dispatch 160x90 -> 2560x1440 f28 (final combine), then CopySubresourceRegion into the window buffer
  (the buffer the HUD later draws on, found as the first target set after the left eye). Much of AK's post is compute
  (Dispatch), which is why the OMSetRenderTargets list never showed it. => at the end of RenderViewFamily 0x81B3C0 the
  window buffer = that eye's finished world picture (graded, glow, AA), before any HUD.
- **T2 capture a: both eyes finished, same look; mean RGB left 17.5/38.0/39.2, right 17.4/38.0/39.1 (T4: equal).**
  Small horizontal parallax visible (Batman shifted). Files akn\eye<N>_a_bb.bmp.
- **T3 ipd 40: both eyes heavily BLURRED** = motion blur: with a SHARED view state each eye's "previous frame" is
  the other eye's camera, so the eye separation reads as camera motion. Own view state per eye is required (and the
  VR profile forces motion blur off anyway). Brightness b: left 19.3/40.4/42.5 vs right 17.7/38.6/39.9.
- **T5 cost (30 fps cap, 2560x1440 per eye, same clock 2610 MHz): GPU busy mono 10.1 %, stereo 18.3 % -> a two-eye
  frame costs ~1.8x mono** (both renders fully per eye; shadows etc. not shared).
- **T6 vs 2 (no view state for eye 2): FATAL** on a worker thread (crash stack 0xCA33FC <- 0xCA7B50 <- 0x7E26D6 <-
  0x7E73AA <- 0x7E76BA <- 0xFA592B ... 0xBF4140). Ruled out. T7 not reached.

## TEST PLAN M4 — per-eye lens + per-eye frame history (tools/Run-M4.ps1, one launch)
Static finding first: view state +0x750 is written by 0x800850's chunk 0x800BF5 = Views[0]->State+0x750 = a light
(type byte 5, flag 0x1000 at +0x40) found this frame, only when found and only for single-view families; readers test
it for null. The game's own view state was allocated at start-up in fresh zeroed pages, so it reads null; ours came
from recycled GMalloc memory (garbage) -> crash. New modes: vs 4 = GMalloc 0x8850 + memset 0 + ctor 0x82CE20(m,0,0);
vs 5 = AllocateViewState + zero +0x750; vs 3 = copy the game's +0x750.
Lens: command "asym 1" = per-eye off-centre projection from 4 tangents (default Quest 3-like: outer 52, inner 42, up
45, down 50 deg, right eye mirrored; "fov o i u d" changes it), game's depth terms and x/y offsets kept.
| # | step | judge |
|---|---|---|
| T1 | capture a (shared history, centred lens) | baseline |
| T2 | asym 1, capture b | each eye off-centre: more picture on its outer side; no stretching artefacts beyond the 16:9 window |
| T3 | vs 4 (crash-prone) then capture c1, ipd 40 capture c2, ipd 6.4, 30 s soak, capture c3, stereo 0/1 | alive; c2 SHARP (no eye-separation motion blur) |
| T3' | if vs 4 crashed: relaunch vs 5 (e*), then vs 3 (f*) | same |

### M4 RESULTS (LIVE 2026-10-07 04:33-04:35, one launch, no crash)
- Game's own projection in that scene: m00 3.73, m11 6.63 (zoomed opening camera; m00/m11 = 9/16), depth terms
  [2][2]=1 [2][3]=1 [3][2]=-10 (near plane 10 units), x/y offsets 0 (no jitter seen in the matrix).
- **asym 1 works:** eye projections m00 0.917, m11 0.913, m20 +-0.174, m21 0.0875 (off-centre, mirrored per eye).
  Capture b (still SHARED history): both eyes heavily blurred (the lens change + other eye's camera read as motion).
- **vs 4 (zeroed GMalloc + ctor 0x82CE20) is STABLE**: 80 s incl. ipd 40, soak, stereo off/on. New view state
  +0x740.. all 0 (game's: 22E78640 2700000015 0 6C0C2AC0). Capture c (own history + off-centre lens, normal spacing,
  after the soak): **both eyes sharp, clean parallax, brightness equal (28.7/46.8/58.9 vs 28.5/47.6/58.4).**
- Lost: c1/c2 (the ipd 40 sharpness check) - capture tags were one character, all three "c" captures overwrote each
  other. Fixed (tags up to 15 chars). Re-check ipd 40 sharpness in the next plan.
- => per-eye view state = vs 4. Make it the default.

## M5 — first headset build (stage loop: finish a stage, write the next, run it; JJ 2026-10-07)
Design: AKN (XINPUT9_1_0.dll) owns the engine stereo and publishes pictures; AKVR (dinput8.dll) keeps OpenXR, panel,
head tracking (camera centred on the head, as in geo-11 mode: camera.cpp g_curEye -1), pacing. Joined by exports.
- **5a AKN publishes (desktop test):** side-by-side eye texture (2W x H, window format; left half = eye 0 copied at
  the end of its render, right half = eye 1), pair counter; HUD redirect ("hud 1"): every bind of the window buffer
  outside the eye renders goes to our own HUD texture (cleared transparent at the first bind of each frame), so HUD,
  menus and loading screens land there and the window keeps the clean left-eye world; exports akn_* (get pair, get HUD,
  set per-eye lens tangents + eye offsets in game units). Test: captures of the pair and of the HUD texture (alpha) in
  one launch with the pause menu up.
- **5b AKVR engine mode** (-akn on the command line, VR mode): geo-11 not loaded (g_geo11 false), window = one eye,
  near-square (engineres x engineshape 0.911), g_native with the AKN pair as the source (backbuffer to both halves when
  no fresh pair: logos/loading without 3D), projection views: each eye's real fov (g_rawFov) and pose (head pose +-
  half the measured eye distance along the head's right), AKN fed each frame with those tangents and offsets
  (+-ipd/2 x world scale g_posScale), HUD layer source = AKN HUD texture.
- **5c fake headset (xrsim) one launch:** session up, both eyes delivered, lens shapes declared = rendered, HUD on the
  layer, ipd 40 sharpness re-check, frame rate. Then JJ in the Quest.
Testing per stage (JJ: "make sure you include testing"):
- **5a test (desktop, one launch, tools/Run-M5a.ps1):** stereo + vs 4; T1 capture h0 with hud 0 (control: window has
  world+HUD); T2 hud 1, capture h1: pair texture left/right = the two finished eyes (differ by parallax), HUD texture
  = HUD/pause menu on transparent (alpha 0 elsewhere), window = clean world; T3 hud 1 for 60 s soak, alive, frames/s;
  T4 export table present (log "exports ready").
- **5b test:** builds clean; AKVR without -akn unchanged (geo-11 path log lines identical); 5b+5c run together on xrsim.
- **5c test (xrsim, one launch):** listed above; plus control: stereo 0 live -> both eyes the same picture.

### 5a test — measured pass/fail (judged by tools/judge_m5a.py from akn_log.txt + captures; JJ: confirm success or
failure with telemetry, and decide in advance what happens next)
| test | telemetry / measurement | PASS if | on PASS | on FAIL |
|---|---|---|---|---|
| T0 start | log: "hooked" lines, no "SIGNATURE MISMATCH", "D3D11 device", "exports ready" | all present | go on | stop; fix the named hook/export |
| T1 pair (hud 0) | pair capture: mean brightness each half; mean abs difference left vs right | both halves mean > 5/255, difference > 1.0, brightness within 5 % | T2 | identical halves -> eye offset/copy half wrong (check "view ctor" lines); black -> window buffer not found (check "window buffer =") |
| T2 HUD (hud 1) | HUD capture alpha coverage; window vs left half mean abs difference; log redirect counters per second | 0.05 % < alpha>0 coverage < 60 %; window ~= left eye (diff < 3); redirects > 0 | T3 | redirects 0 -> HUD binds the window another way (trace binds after eye 0 next); coverage ~100 % -> HUD writes opaque alpha (Batman City mask fix next); window still shows HUD -> missed path (Clear/Copy) |
| T3 soak 60 s | per-second: frames/s, stereo frames, faults, alive | alive, faults 0, mean frames/s >= 28, stereo frames +>= 28/s | stage 5b | crash -> crashmap.py, fix, re-run 5a only |

### 5a RESULT (LIVE 2026-10-07 04:51-04:54, judge: T0 PASS, T1 FAIL, T2 FAIL, T3 PASS) - and why the FAILs are the judge's
- Scene was the MAIN MENU's 3D background: an extreme close-up of Batman's belt centimetres from the camera.
- T1: halves differ strongly (13.0) and brightness gap 23.6 % - visually both eyes are correct; at that distance the
  eyes genuinely see different parts (right eye sees more of the lit buckle). The brightness metric is only valid on
  ordinary gameplay distances (M4 gameplay: 0.5 % gap). Judge fixed: measured in gameplay.
- T2: HUD redirect WORKS (pictures): HUD texture = the main menu on transparent (6.2 % covered, 1224 redirects in 8 s),
  the window shows the clean world. The failed number compared window and pair captured at different moments of an
  animated scene. Judge fixed: inside the HUD-covered pixels, the window WITH redirect must differ from the HUD picture
  far more than the window WITHOUT redirect does (d_h1 > d_h0 + 10).
- T3 soak PASS: 30.0 frames/s, 30.2 stereo frames/s, no faults.
- Decision: re-run T1/T2 with the fixed judge IN GAMEPLAY inside the 5c launch (one launch), not a separate 5a run.

### 5c run 1 (LIVE 2026-10-07 04:55-05:17, fake headset) - findings and fixes
- Crash 1 (render thread, first stereo frame): AKN took the FIRST D3D11 device = AKVR's kiero probe device, which is
  released -> hook_context called a dead device. Fix: every created device is held (AddRef); the LATEST is used to
  reach the context function table (the game's comes after the probe); the game device itself is taken from the
  render thread's context (GetDevice) in the first eye render.
- Crash 2 (render thread executes a heap address, first stereo frame during a 1-fps loading hitch): not reproduced
  after the start gate (stereo only after 30 steady frames at the normal render site; paused on a >250 ms hitch).
  Crash recorder (vectored handler, first 5 access violations, logs module+offset and our state) added; it only ever
  logged AKVR's own handled probe reads at DINPUT8+11390 (harmless).
- Hooks first landed on the probe device's table (pairs 0) -> latest device fix -> pair 4656x2860, HUD redirect live.
- Menus: title "CLICK HERE TO START" at client (0.352, 0.523); save select and "Continue Story" by Enter (a click on the
  save did not select it). Menus are read from the HUD texture (the window shows only the world now).
- Gameplay results (judge v2): T0 link PASS, T1 pair PASS, T2 HUD PASS (1.7 % coverage), T4 lens PASS (rendered and
  declared = runtime to 0.001 deg), T5 HUD layer PASS, T7 soak PASS (90 fps game and AKVR, no AKN crash).
  T3 FAIL = REAL BUG: headset right eye = horizontal streaks (90 % flat rows): xr.cpp engine-mode rect used offset
  e*width also with one swapchain per eye -> fixed (halfX). T6: lens declared symmetric when stereo off (correct);
  pair counter not logged while stereo off -> telemetry now always logged. Brightness: gap flips sign with content
  (sky: left brighter, whole: right brighter) -> new test T1b "eyesame" (same camera + lens both eyes).
- Next run (tools/Run-M5c.ps1): one launch, menus scripted, all T0-T7 + T1b.
- Prepared for "T1b FAIL" (per-eye exposure): the trace of one eye render shows AK's exposure chain as compute
  dispatches down to 1x1 R16_FLOAT (f54) and back up (adapted luminance). If eye 0 and eye 1 bind DIFFERENT 1x1 targets
  (each view state its own), share it: after the left eye's render, CopyResource(right eye's 1x1 <- left eye's 1x1),
  so both start every frame from one value (VR_ESSENTIALS STR-009; SWBF2 shared EV). Test: T1b gap < 2 %.
- Prepared for "5c PASS": install/AKVR-Launch-VR-EngineStereo.bat (game folder; starts BatmanAK.exe -akvr -akn
  -aknstereo directly, Steam running) for JJ's first Quest test via Virtual Desktop.

### 5c run 3 (LIVE 2026-10-07 05:27-05:33): ALL PASS except T1b
T0 link, T1 pair, T2 HUD (1.7 %), T3 headset stereo (non-black 90/84 %, no streaks, L-R 27.2), T4 lens (rendered and
declared = runtime to 0.001 deg), T5 HUD layer, T6 control (pairs stop, declared lens symmetric when off), T7 soak
(90.0 game / 90.5 AKVR frames/s, no AKN crash) all PASS. T1b FAIL: same camera both eyes, right eye 2.3 % darker
(difference 1.47) -> per-eye exposure. Menus: each step registered one screen late; the Enter retry loop got there.
### Stage 6a (built): shared exposure - 1x1 float compute targets noted per eye; after the left eye's render the right
eye's get the left's values (command lumshare 0|1, default 1; log "exposure targets (1x1)"). Test in the full 5c set:
eyesame + lumshare 0 (control, capture s0) vs lumshare 1 (capture s): PASS if gap < 1 % with sharing.

### Stage 6a RESULT (LIVE 2026-10-07 05:33-05:38): full 5c set PASS again; T1b 1.18 % with sharing vs 1.31 % without
- The right eye's 1x1 "exposure" target changes address between frames (pooled per-frame texture, not the adaptation
  history); copying into it changed nothing and could disturb a later user of the pooled texture -> lumshare default
  OFF. Residual same-camera brightness gap 1.2-2.3 % (left brighter in every run: systematic, small). Left as a known
  item for JJ's headset judgement; if he sees it, find the adaptation history (RenderDoc-free route: hardware write watch
  on the view state's exposure fields, or compare the two view states' floats frame to frame).
## Stage 6b plan - regression: AKVR WITHOUT -akn (geo-11 path) still works with the new dinput8 (fake headset)
- Launch -akvr only (AKN dormant), reach gameplay, headset shot: non-black > 50 % both eyes, no streaks; AKN log shows
  no hooks (dormant). PASS -> stage 6c (launcher in the game folder for JJ's Quest test). FAIL -> diff the startup logs
  against the SCENEFMT build's, the backup is in akvr/diagnostics/before-AKN5C-20261007.

### Stage 6b RESULT (LIVE 2026-10-07 05:39-05:42): PASS - AKVR without -akn (geo-11 HOOK path, AKN dormant) with the
new dinput8 (AKN5C-0.2.2) reaches gameplay on the fake headset: both eyes 83 % non-black, HUD layer LIVE, geo-11
"HOOK (alias geo11.dll)", feed NATIVE. JJ's normal VR shortcut is unaffected.
### Stage 6c (2026-10-07 05:43): Binaries\Win64\AKVR-Launch-VR-EngineStereo.bat installed (starts BatmanAK.exe -akvr -akn
-aknstereo, Steam running). Deployed: dinput8 = AKN5C-0.2.2 (backup of the previous one in
akvr/diagnostics/before-AKN5C-20261007), XINPUT9_1_0.dll = AKN (dormant without -akn).
## NEXT (needs JJ): first Quest test via Virtual Desktop with the engine-stereo launcher. What to look at: depth feels
right (world scale), both eyes equally bright, HUD readable on its layer, no double images, frame rate (panel), the
picture at the edges (asymmetric lenses). Then stage 7: DLSS per eye (Luma's AK findings: MVs = R16G16_FLOAT target,
depth = SRV1 of the SMAA edge pass, jitter at BuildProjectionMatrix 0x1400104FF; replace SMAA per eye).

## JJ's FIRST QUEST TEST (2026-10-07 ~20:23, Virtual Desktop) - and the cause
JJ: depth on title/logos, main menu HUD readable in depth but Batman "completely out of focus, can't see him at all";
in game MONO, and head movement stretches and shears.
Log (akn_log): AKN made a pair every frame in gameplay (pairs = frames, 60-90 fps) BUT "window buffer = 2688x2880 f91"
while AKVR's eye/window was 2328x2860: the window buffer was detected as "first render target bound after the left
eye" and Virtual Desktop's IN-PROCESS compositor device (a 3rd D3D11 device, created 20:24:15) binds its own targets
through the same hooked functions -> AKN copied VD's picture into the pair; AKVR's size/format check failed every frame
-> flat fallback (window picture to both eyes = mono) declared with the symmetric lens while the picture had the left
eye's asymmetric lens -> shearing with head motion. HUD redirect never matched (HUD frames 0).
Main menu: an extreme close-up (Batman's belt centimetres from the camera) -> unfusable disparity in stereo; AKVR's
flat-menu path must show it as a flat screen.
FIX (built): every context hook acts only on the game's immediate context (taken inside the first eye render); the
window buffer = the destination of the eye render's final full-size copy (M3b trace), not a later bind; AKVR reports
pair-vs-flat frames to AKN (akn_report) and logs why a pair does not fit (akvr_mode_log). TEST: "rogue 1" = a second
D3D11 device on its own thread binding/clearing/copying a 2048x2048 sRGB target every 3 ms all session (stands in for
VD); judge T8 (>95 % of gameplay frames shown as the pair), T9 (window buffer = eye size despite the rogue), T10 (main
menu shown flat in the headset).
### Run 7 (2026-10-07 20:36-20:40, rogue device on): T8 PASS (4732 pair frames, 1 flat = 100 % stereo), T9 PASS
(59625 rogue ops, window buffer still the game's 2640x2860) -> the Quest mono/shear bug is FIXED. All others PASS except:
T7 (2 caught access violations in AKN+62C0 = the BMP saver reading the 1-byte R8 working picture as 4 bytes/pixel
during the menu captures - fixed with per-format bytes/pixel, unknown formats skipped), T10 (menu shot taken while the
menu was still loading - now waits for steady frames), T1b (known 1 % residual).
### Run 8 (2026-10-07 20:42-20:46): T0-T9 PASS (T7 clean: 0 AKN errors; T8 100 % stereo; T9 rogue 60903 ops). T10:
the menu headset shot WAS written (xrsim log 20:44:01) - xrsim-shot.ps1 timed out on its own counter; copied by hand:
the main menu's 3D background is still a STEREO pair (each eye a different part of Batman's belt) because JJ's
akvr_settings.ini has menuflat=0 (stereo menus, fine under geo-11's gentle depth). MENUCLOSE (AKVR, built): in engine
mode the main menu (akvr_xr_main_menu_detected) renders both eyes from one spot with the game's own lens (AKN offsets 0,
asym 0) and AKVR shows the left half to both eyes; the per-eye gameplay override is off there. T10 now judged on the
AKN source pair at the main menu (pair_menu_main2.bmp): halves within 5 % (headset shots re-project per eye: the
stereo-off control differs 0.71 relative, so they cannot judge "flat").
### Run 9 (2026-10-07 20:52-20:56): ALL PASS except T1b (known 1.4 % same-camera residual). T8 100 % stereo with the
rogue device (68706 ops), T9 window buffer = game's, T10 main menu pair halves identical (MENUCLOSE works), T7 clean.
Deployed in the game folder (by the run): dinput8 (AKVR with MENUCLOSE + akn_report), XINPUT9_1_0 (AKN context filter).
-> Ready for JJ's second Quest test with AKVR-Launch-VR-EngineStereo.bat.

## JJ's 2nd QUEST TEST (2026-10-07 20:58) - still mono + shear in game; main menu: Batman visible after a moment but
head motion exaggerated; main menu "mono" = no depth (JJ: vision, NOT audio)
Log: pairs 0 all session, AKVR shown 0 stereo / 9033 flat; no "game device" line -> the context hooks never ran on
the game's context. Cause: "hook via the LATEST held device" (run-7 fix) picked Virtual Desktop's device (created last,
20:59:00, by VD's module); its immediate-context function table differs from the game's (D3D11 has different context
tables, e.g. single-threaded devices). The rogue test device (default flags) shared the game's table -> tests passed.
Main menu: MENUCLOSE drew it with the game's own narrow lens and AKVR declared the headset's view -> magnified motion.
FIX: AKVR hands the device behind the game's swap chain to AKN (akn_set_game_device in akvr_xr_set_device); AKN hooks
only that device's table (waits up to 15 s of 3D for it when -akvr); telemetry "context hooks installed via device ..."
and "HOOKS MISS THE GAME" after 240 blind eye renders. Main menu: both eyes from one spot (offsets 0) but each through
its own lens (asym 1), shown exactly as rendered (per-eye fov, head pose) - zero depth, 1:1 motion. Per-eye
presentation now also in PAUSELOOK; other floating screens show the left half to both eyes (flat) in engine mode.
TEST: rogue 2 = single-threaded stand-in created before any 3D (own context table, like VD); T11 (hooks via the AKVR
device, no MISS), T10 = main menu halves equal after the best sideways shift (no depth). (JJ clarified: "mono" in
the menu = no 3D depth, not audio - no audio issue.)
- JJ 2026-10-07 21:1x: 'Forget the main menu, just focus on getting stereo 3D in the game.' -> menu work parked.
### Run 10 (2026-10-07 21:04-21:09, rogue 2 = single-threaded VD-like device from launch): ALL GAMEPLAY TESTS PASS -
T11 hooks via the game's device named by AKVR (game device seen at once), T8 100 % stereo (4416 pair / 0 flat), T9 window
buffer = the game's 2640x2860, T3/T4/T5/T7 pass (74 fps). T10 (main menu) parked by JJ, T1b known residual.
-> JJ's 3rd Quest test.

## JJ's 3rd QUEST TEST (2026-10-07 21:16): stereo now (pairs + AKVR shown 5137 stereo), but the views "so far apart they
don't fuse" and head turns still shear/skew. Asked why SWBF2 was quicker (answer: SWBF2 was built for engine stereo from
the start; here AKVR carries geo-11-era picture corrections, and my fake-headset tests could not see them).
Cause (CONFIRMED by numbers): JJ's akvr_settings.ini swapeyes=1 (a geo-11 need) -> AKVR copied each eye the OTHER eye's
picture (sourceEye = 1 - e) while declaring each eye's own lens/pose -> crossed views (unfusable) and every picture
stretched into the wrong lens (shear that changes as the head turns). AKN's rendered lens was exact (matrices give the
headset's tangents to 4 decimals: m20 +-0.2425, m21 0.1932) - no doubled vertical shift.
GEO11OFF (built): in engine stereo AKVR never swaps eyes, adds no FULLVIEW vertical NDC shift to the game projection,
applies no bottom-fill tilt; AKN ignores the game's own x/y projection offsets. JJ's saved settings untouched.
Why the tests missed it: no test checked WHICH picture reached WHICH eye, and xrsim's default lens is vertically
symmetric (FULLVIEW/tilt corrections were zero there). New: fake headset gets JJ's Quest lens (49.48/35.50/39.38/50.52),
T12 eye identity (red/blue bands per eye in the pair, checked in the headset images), T13 head turn 1:1 (yaw 20, pitch
15 -> the world moves 20/15 deg in the headset), T4b lens actually rendered (from the logged matrices) = headset lens.
### Run 13 (2026-10-07 21:29-21:33, JJ's Quest lens on xrsim, VD-like rogue 2, GEO11OFF): STAGE PASS
T12 eye identity PASS (left eye red 255/blue 0, right eye blue 255/red 0), T13 head turn 1:1 PASS (yaw 20 -> +20.50 deg,
pitch 15 -> -15.00 deg), T4/T4b lens rendered = declared = Quest to 0.001 deg, T8 100 % stereo (5264/2), T11 hooks via
the game's device, T7 88.7 fps no errors. T10 main menu parked (JJ), T1b residual 2.3 %. Runs 11/12 died in the harness
(lens command before the session; focus click at the window edge) - fixed. -> JJ's 4th Quest test.

## JJ's 4th QUEST TEST (2026-10-07 21:51): "the game is showing in 3D now" (GOAL REACHED). Remaining: lighting and
reflections differ per eye; everything jitters on head turns ("maybe a head pose delay setting"); the double stick
click no longer opens the VR panel.
- Lighting/reflections: same-camera pair (eyesame) differs ONLY in the rain (diff map) -> per-render randomness: each
  BeginRenderingViewFamily bumps the frame counters 0x143A3149C / 0x143A314A0 -> the eyes get different frame numbers ->
  different rain and noise patterns (SSAO/reflection noise). FRAMESYNC: counters restored after the right eye's render.
  View-dependent screen-space effects can still legitimately differ a little; ask JJ for specifics if seen after this.
- Jitter: AKVR used the fixed geo-11 delay 2; FRAMEID never locked (its engine counters move twice per frame now).
  POSESTAMP: AKN stamps each pair with AKVR's camera-finalize count read when the eye views are built (export
  akvr_finalize_now in dinput8.dll); AKVR presents the pair with exactly the pose that finalize consumed (g_seqForFin).
- Panel chord: AKVR hooked XInputGetState in the game folder's XINPUT9_1_0.dll = AKN's forwarder stub. Now it hooks
  System32's copy (the forwarder calls it); startup log line "controller hook: ...".
Tests: T1b same camera identical (diff < 0.4, control with framesync 0), T14 pose stamp (> 95 % exact), T15 controller hook.
### Run 14 (2026-10-07 22:00-22:04): T14 POSE STAMP 100 % (4370/4370) - the exact delay in engine mode is 1, not the
geo-11 fixed 2 (= JJ's jitter); T15 controller hook ON in System32 xinput9_1_0; T12, T4/T4b, T8 (99.9 %), T9, T11, T7
pass. T1b: same-camera difference 1.148 -> 0.484 with FRAMESYNC; diff map: only some rain streaks remain (lighting and
reflections identical) - the rest of the rain draws its own randomness (open, minor). T13 failed on the SCENE: camera
close to Batman, head turns swing the third-person camera (translation), shots hit the +-40 deg search limit; run 13
(open spot) passed exactly. TODO: make T13 scene-proof (compare AKN's rendered view rotation with AKVR's head pose per
stamp instead of images). Deployed -> JJ's 5th Quest test.

## JJ's 5th QUEST TEST (2026-10-07 ~22:10): JITTER GONE. "Lighting, shadows and reflections are still completely
different in each eye" (JJ: like SWBF2's sun shadow missing in one eye). The same-camera test cannot see a leak
(borrowing the other eye's data looks right when both eyes are the same camera).
Theory: effects that keep data "from last time" continue from the OTHER eye's render (two renders per frame).
Built: EYESHIFT (move one eye's camera only), HISTORY detector (per eye render: every resource read - SRV/UAV/copy
source - before anything wrote it in that render, and written later = carried-over state; the ones in BOTH eyes'
renders are logged), HISTSWAP (each eye keeps its own copy of those: restored before its render, saved after),
pairshot (pair-only capture). Test T16 (SWBF2 Test-ShadowLeak method): move the right eye 3 m -> the left eye's picture
must stay at frame-to-frame noise, and vice versa; HISTSWAP off (control) then on, one launch.

### Run 16 (2026-10-07 22:21-22:27): T17 CONFIRMS the cause - same spot, same frame, each eye its own mirrored
OFF-CENTRE lens: aligned difference 18.80 vs same-lens control 1.42; the map shows a floor light pool present in one
eye only, shadows and wet-ground reflections different = AK's light culling / screen-space lighting assume a CENTRED
lens (geo-11 era note: "off-centre projection made lights/decals slide"). T16 leak test: no leak (other eye stays at
noise with or without HISTSWAP) - the per-eye difference is NOT shared state. T7 67 fps, T8 100 %.
TURNEDEYE (built, lens mode 2, default in engine mode): each eye renders a CENTRED lens just covering its field of view
(half-angles (R-L)/2, (U-D)/2) with the eye camera turned to the field's centre (yaw (L+R)/2 ~ 7 deg outward, pitch
(U+D)/2 ~ 5.6 deg down) - AKN multiplies the eye view by the turned frame [r u f]; AKVR declares the same: fov
symmetric, pose = eye pose * yaw * pitch. Test T17b: same spot + frame, warp the right eye's picture onto the left
eye's direction (exact ray maths) -> must match (vs control); T4b now checks rendered lens = DECLARED layer lens.
### Run 17 (2026-10-07 22:31-22:36): TURNEDEYE FIXES THE PER-EYE LIGHTING (fake headset)
T17b PASS: same spot + frame, turned eyes, right eye warped onto the left: difference 5.37 (same-lens control 2.26;
off-centre lenses gave 18.80); diff map = rain streaks + edge resampling only, the floor light pool, shadows and wet
reflections now match. T4 declared lens = centred (-42.5 42.5 45.0 -45.0) to 0.000 deg, T4b rendered = declared
0.001 deg, T6 control, T12 eye identity, T14 pose stamp 100 %, T15 controller, T16 no leak, T8 100 % stereo, T7 62.9
fps (heavy-rain spot; AKVR 63.2), xrsim shows each eye's layer at its turned angle (honoured). T13 pitch -15.00 exact;
yaw -36 = scene (camera wedged near Batman by a wall, third-person swing - see run 14), informational. Judge updated
for turned eyes (T4 expects the centred lens, T4b reads 'turned lens' lines, T6 compares on/off vertical lens);
T1b made informational (rain randomness only). STAGE 5c PASS -> JJ's 6th Quest test.

## JJ's 6th QUEST TEST (2026-10-07 ~22:45): lighting "98 % better" (TURNEDEYE confirmed in the headset). Rain may differ
per eye; the double stick click still does not open the VR panel.
### Runs 18-25 (22:50-23:31, fake headset, one launch each)
- PANEL: opens fine by AKN's switch ("panel 1") and by F8 - headset quads 1 -> 3 (D1/D2 PASS); the display path works.
  Chord: JJ plays with Quest Touch controllers that Virtual Desktop turns into a pad INSIDE the game (Batman City). With
  AKN's XINPUT9_1_0.dll in the game folder, AKVR read Windows' copy directly and never saw that pad. GAMEROUTE (AKVR
  gamepad.cpp, engine mode only): wrap BatmanAK.exe's own import of XInputGetState; the game's reads pass our filter and
  the chord read calls the top of that chain. Verified: wrap installed, game reads go through it; no pad on the test PC,
  so the chord itself needs JJ. AKN logs "pad route: ..." every 10 s (which route sees a pad).
- RAIN: same camera + shared frame number -> nearly every streak differs (D4 map). NOT the rain compute steps of the
  geo-11 era (a96594b1/bc5c6aeb never run here; enhanced rain bEnableRainFX=False), NOT the view state (vs 0 shared:
  1.23 vs 1.53), NOT UE3's GSRandSeed (0x143A28C24, found from appSRand's constants) nor the C runtime rand() state
  (RANDSYNC, self-tested, no effect), NOT the jitter counter (already shared). No draw has 256+ instances here; the big
  non-indexed Draw of 103530 vertices (vs 62c3cb6af9d6796c ps 0538920e4247cdf5) gets different VS b2 c9-c14 (3x4
  transforms) per eye and b1 c32.y 7/2 - NOT yet proven to be the rain (next: skip that draw -> rain gone?; then find
  who fills b2: per-render counter or the real-time clock). Diagnostics stay in, off in normal play.
### Runs 26-32 (2026-10-07 23:55 - 10-08 00:55): RAIN PER EYE SOLVED (fake headset) - JJ's tip "SWBF2 particles"
- Run 26 pause test: same camera both eyes: playing 1.23, PAUSED 0.02 -> the rain advances per RENDER, not per frame
  (playbook side-effect gate "advances twice"; SWBF2: particles spawned per view).
- Runs 27 (zero elapsed time for the left render) and 28 (game thread waits for both renders) changed nothing.
- Runs 29-31: bisection saw only 23 vertex shaders -> AK records most drawing on DEFERRED CONTEXTS (worker threads,
  own function table); our hooks were on the immediate table only. DEFERRED hooks (dispatch / draws on the deferred
  table, eye tagged by g_eyeNow from the render thread): 20.8 M recorded draws, 1.5 M playbacks in the run.
- Run 32: the geo-11-era rain compute steps a96594b16ceb399b (simulation) + bc5c6aebf60c9308 (splashes) DO run, once
  in EACH eye's render (recorded). RAINONCE 1 = skip the simulation in the second (left) eye: same-camera difference
  0.009 (vs 1.12-1.33); mode 3 (+ splashes) 0.004-0.016. Bisection over 81 vertex shaders independently found the rain
  draw f50d1365e929b3a0 (hidden: 0.014). Normal stereo with it on: rain in both eyes. DEFAULT rainonce 1, deployed.
  Side effect expected: rain falls at normal speed again (it was stepped twice per frame).
### Runs 33-35 (2026-10-08 01:05-01:22): RAIN PANEL + RAIN OFF THE HEAD (fake headset), MARKER PROBE
- PANELRAIN: AKVR's panel RAIN TEST in engine mode drives AKN through akn_cmd: "show only the rain" (rainview 2),
  "hide the rain" (rainview 1), NEW "pause the rain" (rainpause 1: the simulation CS a96594b16ceb399b is skipped in
  both eyes; only-rain pictures 3 s apart: 0.17 paused vs 1.18 running), "rain stays in the world" (farrain).
- RAINCB dump of the simulation's cb0 (head yaw 20 / pitch 15 / lean 30 cm): cb0[10] = camera position (w 1),
  cb0[11] = camera forward x 512 (w 0), BOTH with the head (and the right eye's turned lens). FARRAIN2 + NEARRAIN2
  (default on): the dispatch gets a copy of cb0 with rows 10/11 = the game camera WITHOUT the head for that frame
  (view location after CalcSceneView minus AKVR's head offset; AKVR base axes), carried game thread -> render tag ->
  render thread -> recording workers. Verified: lean 30 cm moves the original row 10 by 30.7 units, the patched row
  stays (-75999.1); turn 20 deg changes the original row 11, the patched one stays. (Run 34 bug: the location was read
  BEFORE CalcSceneView fills it = 0,0,0 - fixed in run 35.)
- MARKERPROBE: FSceneView::Project = 0x81FF10 (view, out FVector4, world). Callers: only UCanvas::execProject
  (RA 0x86DC0D), always with the LEFT eye's view (the head included). In the test spot it projects one point: the 45 m
  objective marker (-75279, -34793, 3526). No grapple point came into view in 7 head poses - JJ / a better spot
  needed. ULocalPlayer::execProject -> 0x9F8220 not seen. Next: VR_WORLD_MARKERS_GUIDE §3-5 for AK engine mode
  (HUD is drawn once into the redirect texture: marker pieces must be drawn per eye into the pair instead).
### Runs 36-38 (2026-10-08 01:30-01:43): RAINDEPTH - JJ: "paused / isolated rain is on a flat plane, all one depth"
- Rain VS (f50d1365e929b3a0, decompiled in akvr/diagnostics/before-HUNTRAIN-20261001): positions from structured
  buffer t0 (offset 48), clip = pos x cb0[6..9]. RAINVP per eye: rows 6-8 per-eye (turned lens), row 9 ~(0,0,-10,0)
  in BOTH = the buffer positions are relative to the camera that draws them. One shared simulation (RAINONCE) ->
  each eye drew the same drops relative to itself = zero parallax, rain at infinity (paired-tile disparity 0 for 100 %).
- RAINDEPTH (default on): per eye, rows 6-9 = translate(O) x that eye's view x lens (doubles), O = the simulation's
  origin (the frame's head-free camera, NEARRAIN2; frozen while the rain is paused). Check: our rows 6-8 = the game's;
  row 9 with O = the eye reproduces the game's (0, 0, -10, 0). Paused rain-only pair: disparity median -10.5 half-px,
  10-90 % -25.5..-3 (fix off: 0 for all 106 tiles); running the same.

## DLSS stage D0 (2026-10-08 02:22-02:27, fake headset unpaced at 240 Hz, rogue device on) - DLSS_PLAN.md
- SMAA swap points found per eye, all on the IMMEDIATE context, every frame (77/78 per s each eye): edge PS
  0x067FF80F (t1 = depth f19 R32G8X24 typeless, eye size), weights PS 0x2F55F98E, neighbourhood CS 0x42C0137E
  (t0 = scene f26 R11G11B10_FLOAT? (fmt 26), u0 = output same size). Names via AKVR akvr_shader_crc (CRC-32).
- Native 2640x2860 per eye: 77.0 frames/s, GPU busy 99.8 % = GPU-BOUND; eye renders: processor L 5.2 / R 3.5 ms,
  graphics card L 5.6 / R 6.2 ms.
- "67 %" run INVALID: engineres 1916 -> AKVR forced 1552x1916, but the game's window fought to 891x2044 (resync
  attempts 113) and the eyes rendered 891x1100. At that tiny size: 113 frames/s, GPU busy 46 % = PROCESSOR-BOUND at
  ~8.8 ms; eye renders processor 2.7 + 2.7 ms. => 120 Hz needs DLSS AND ~1 ms off the processor side.
- NO R16G16_FLOAT motion-vector clears (MotionBlur=False in the VR settings). Next: find velocity via the motion-blur
  groundwork CS ed72bff815f35134 (still runs per eye) inputs; fix the window-size fight for small eye sizes.
### D0b (2026-10-08 02:31): UE3 ScreenPercentage=67 + motion-vector hunt
- ScreenPercentage 67: 108.5 frames/s, GPU busy 62 %, eye graphics-card ms 2.5 + 4.0 (native 5.6 + 6.2) - BUT the
  picture is BROKEN (pair_sp67: scene in a small rect, layers misaligned, grey blocks) - same as M2 (sub-rect views
  break AK's post chain). NOT a route. Processor-side limit again ~108-113 frames/s.
- MOTION VECTORS: R16G16_FLOAT eye-size texture = input slot 1 of the motion-blur groundwork CS ed72bff815f35134 and
  of b7f14acc058bce77, every eye render, even with MotionBlur=False (not cleared via ClearRenderTargetView - why Luma's
  grab saw nothing). Next: verify it holds camera motion (background moving on a head turn), not only moving objects.
- Route for D1: the game renders each eye truly small (engine size = render size), DLSS at the END of each eye
  (finished small picture -> full-size headset image). Needs the small-size window fight fixed (D0: the game asked
  for 891x1100 and won) and the headset image size decoupled from the engine size.

## DLSS D1 (2026-10-08 03:08-03:33, fake headset, unpaced 240 Hz, rogue device on) - DLSS_PLAN.md
- DLSS WORKS per eye: NGX 310.9.1 (from Luma) in akn\, one feature per eye, jitter in AKN's lens, motion vectors =
  input 1 of CS ed72bff815f35134 (R16G16F), depth = SMAA edge PS input 1, SMAA edge/weights skipped, neighbourhood
  = copy; DLSS at the end of each eye (window buffer copy -> evaluate -> full-size eye -> pair). Picture clean (DLAA
  crop, pair_d1a).
- Fixes on the way: AKVR read only the first 4 KB of akvr_settings.ini (dlss= past it) -> read from the whole file;
  small render sizes fought the window (game fell back to a 1080-tall mode, drew 998x1080 in 1760x1904) -> the fake
  screen is sized from the headset eye (DLSSSCREEN); that centred the window half off the monitor (title clicks
  missed) -> AKVR puts the window top-left (WINDOWHOME); AKVR writes ResX/ResY = forced size (ENGINESIZE); DLSS
  ignores one-off odd-size renders (30-frame rule); AKVR stretches flat menu frames to the DLSS pair size (DLSSPAIR).
- MEASURED at Quality (render 1760x1904 -> 2640x2860 per eye): 71 frames/s (native full size 77; Quality size
  without DLSS 116). DLSS evaluate = 3.87-3.91 ms PER EYE on the graphics card for presets K, M, L and E alike (310.9
  seems to run its transformer whatever the hint) = ~7.8 ms per frame for the two eyes. 120 Hz (8.3 ms) is out of
  reach with this DLL at this output size. Levers: a smaller output (VD default ~2448 tall: -27 % pixels), a CNN
  DLSS DLL (3.8.x, cheaper - JJ asked for the newest non-5 version), FSR (cheaper), or accept DLAA-quality AA only.
### D1 follow-up (2026-10-08 03:36-03:41) + JJ's headset build
- JJ: transformer model only (no CNN). Performance (render 1312x1424 -> 2640x2860): 99 frames/s, DLSS 2.80 ms per
  eye; Quality at a 2448-tall output (1504x1632 -> 2264x2448): 92 frames/s, 2.88 ms per eye (native full size 77).
- DLSSMIRROR: the desktop window shows the DLSS left eye (scaled) - the game's own window picture is the small,
  un-anti-aliased render (JJ: "AA looks bad still" was the window, not the headset).
- DLSSPANEL: overlay radio "DLSS (restart)": Off / DLAA / Ultra quality / Quality / Balanced / Performance, saved
  as dlss= in akvr_settings.ini (AKVR's save writes it now; it used to drop unknown keys).
- Deployed for JJ: dlss=5 (Performance), engineres 2860, engineshape 925.
### D1m (2026-10-08 03:55, JJ in the headset: "the background is still shimmering" with the rain off)
- CAMMV: own motion vectors per eye (depth + this eye's camera, reprojected with the same eye's previous-frame camera,
  jitter removed, render pixels, MVJittered off). Modes: 0 the game's velocity buffer (Luma), 1 camera only,
  2 camera + the game's buffer where non-zero.
- Fake head, rain hidden, Performance: still-head background change 0: 1.84, 1: 1.07, 2: 2.21; detail still 2.63 /
  3.11 / 2.85. Mode 1 = steadiest + sharpest -> DEFAULT. The game buffer adds noise even on static pixels (encoding not
  understood yet - maybe biased / scaled) and mode 0 had MVJittered on with zero static motion (= shimmer). Open:
  decode the game's velocity for moving objects (Batman / cape may smear with camera-only motion).
- AKVR: PANELONE (one panel quad for both eyes, Batman City distance), DLSSPANEL2 (own DLSS section, open).
- Deployed for JJ: dlss=5 Performance, motion mode 1.
### JJ's DLSS headset test (2026-10-08 ~04:10) + HUDFULL
- JJ: background "so much better" (camera motion); slight jitter on diagonal bridge pylons; HUD "very low resolution";
  asks for on-the-fly preset; Batman's reflections mismatched per eye (like Arkham City's puddles); he ran Balanced.
- HUDFULL: AKVR scales every SCREEN-sized Scaleform movie viewport (buffer == engine render size) to the headset size
  (x DLSS factor, rect + scissor); AKN's HUD texture = DLSS output size. Fake headset 13:54: HUD texture 2320x2860,
  tutorial popup text crisp at full size (hud_rgb_hfgame). Own-texture movies untouched. Pause capture empty (timing).
- DLSSMODEL: overlay "DLSS model (live)" default/K/J/M/L -> AKN dlsspreset (feature rebuilt live).
- Test tools: akmenu click brings the game in front first (FRONTFIRST - VS Code took a title click); menu timing
  back to the proven gaps (MENUTIMING); LOADWAIT never presses Enter into a started load.
- Open: reflections on Batman per eye (next), DLSS render scale needs a restart (game render size fixed at launch).
### Batman reflections per eye (2026-10-08 14:00-14:20)
- XEYE (textures one eye reads but only the other draws): 256x256 RGBA8 7-mip = tiled rain ripple/drip pattern
  (picture: blue streaks + ring drops; right-only because RAINONCE runs the rain once) - eye-independent, harmless;
  8192x8192 R16G16F = shadow/light atlas (light space); rain buffers, 1x1 exposure (expected). No planar reflection /
  scene capture shared between eyes (unlike Arkham City PERCAPTURE).
- R1 (Run-R1.ps1, judge_r1.py, one launch): same spot + frame, turned eyes, right warped onto left. Control 0.20;
  DLSS motion 1 still 5.85 / head sweep 6.70; mode 2 6.95 / 6.50; mode 0 6.09 / 6.16 (no-DLSS T17b: 5.37). Worst blocks
  at the left eye's right edge (0.92-0.97, 0.34-0.39) in every case; Batman's body dark in the maps. -> DLSS and its
  motion modes add ~0.5-1.3 only; no Batman-specific mismatch at this spot (his suit barely lit here; the AR scanner
  covers his head). Not reproduced: need JJ's picture of the case.
- CAPPAIR (AKVR): the panel capture / F2 in engine stereo also saves BOTH eyes (akn\pair_c<hhmmss>.bmp). Deployed.
### Main menu goes flat after a second (JJ 2026-10-08 ~14:25) - MENU3D
- JJ: first menu "Batman looks great for about a second then it switches and the depth and scale look noticeably worse".
  Log (R1 run): eyes from AKVR #601 offsets -3.213/3.213 -> #1201 (14:13:19, main menu detected) offsets 0/0 = our own
  MENUCLOSE (2026-10-07, "Batman completely out of focus" in the menu, before TURNEDEYE / POSESTAMP fixed the lens and
  jitter): both eyes from one spot = no depth, scale cue lost.
- MENU3D (AKVR xr.cpp/hooks.cpp): real eye separation in the main menu by default; old one-spot view = advanced option
  "main menu: both eyes from one spot" (setting menuonespot, default 0). Fake headset 14:29: mode MAIN_MENU, offsets
  stay -3.213/3.213 through the menu (#601..#4201). Deployed, headset check pending.
- Seen in passing: AKVR "AKN pair 4640x2860 f26 does not fit the side-by-side" at load - window buffer format flips
  f26/f28 (pair re-made each time); check later.
### MIPBIAS - DLSS texture sharpness (2026-10-08 15:00-15:30, JJ: "what about dlss? check the notes")
- DLSS_PLAN D1 listed "a negative mip bias for the smaller render (Luma does the same)" - never built. Built now:
  PSSetSamplers (immediate + deferred) swaps samplers bound in eye renders for twins with MipLODBias +=
  log2(scale) - 1 (Luma core.hpp CreateCustomSampler mode 4, super_resolution.h GetMipLODBias); twins made on first use
  (the game creates its samplers before our hooks). Live "mipbias 0|1|2|3" (off / full = default / mild log2(scale) /
  +4 blur diagnostic); overlay "texture sharpness (live)" in the DLSS section.
- Census: ~21 samplers in the eye renders, all with the game's own bias -0.25; 6 anisotropic (0x55), the rest trilinear
  0x15 / 0x14 and point. Blur test +4 on the anisotropic ones only: ground barely changed -> the rooftop ground uses
  TRILINEAR samplers. MIPBIAS2: also tiling (wrap / mirror) linear samplers, filter kept (14 twins, ~100k swaps/s).
  Blur test then flattens the ground (proof of reach); full vs off: CAUTION lettering + gravel crisper (MB_crops /
  scratch mb3_crops), still-head change not higher (off 7.3 / full 6.0 / mild 4.5 - the scene's light flicker
  dominates). Deployed (Balanced, full).
### World markers in engine mode - MK1-MK3 probes (2026-10-08 15:30-15:57, VR_WORLD_MARKERS_GUIDE.md)
- MK1 MARKERBASE check: AKVR's axes (new export akvr_marker_axes) + view location rebuild the game's left-eye view
  EXACTLY (rotation diff 0.00000, signs right +1 up +1); head-free view = base axes + NEARRAIN2 base position. Head
  turned 15 deg: the game's Canvas.Project result jumps 3.83 -> 63.5 (it projects with the LEFT EYE view incl. head),
  the head-free reference stays 2.192 -> the guide's jitter fix holds here.
- HUD pieces (VS cb0 read back per HUD-phase draw): rows 0-6 a shared header (identity, camera position, 1/w 1/h
  w h); 2D Scaleform pieces carry x-row (a b 0 tx) / y-row (c d 0 ty) in rows 7-8 (NDC; e.g. health widget tx -0.4155
  ty 0.2301 = its spot in the HUD capture); 3D-tilted pieces a full 4x4 in rows 7-10 (row7.z != 0, large w) - skip.
  cb0 is written whole by the game (Map) - port AKVR hudsplit's Map/Unmap/UpdateSubresource shadow (MARKREC) rather
  than read back per draw.
- MK2 calibration (Project result forced to 4 known points): NO HUD piece moved -> the objective marker this spot
  projects (-75279,-34793,3526, ~40 m) is not drawn (hidden by the game here). The 32x64 pieces near (0, 0.17) are a
  fixed element. Grapple points never came into view. Need a visible reticle / distance marker.
- Harness: training terminal popup hides markers (walk back: akmenu hold 0x53); camera turn by relative mouse
  (akmenu mouse) toward the projected point; MENUCHECK: AKN status line "main menu N" from AKVR (akvr_in_main_menu) -
  part A re-presses Continue when still 1 (a stuck main menu had passed as gameplay).
- MK3: AKVR panel capture / F2 -> AKN "capmk <time>": pair + HUD + 80 HUD-draw dumps + 40 projections + caller census
  (verified: pair_t1, hud_*_t1, 81 lines). NEXT: JJ's capture with a grapple reticle + distance marker on screen ->
  which pieces, which projection (Canvas.Project 86DC0D or another path), then MARKEREYE/BASE/CARD in AKN (marker pieces
  drawn per eye into the pair, card mapping from the head-free reference).
### HUD see-through pieces "posterised" (JJ 2026-10-08 ~16:45: weapon wheels, "wrong blending mode") - HUDBLEND
- Cause found in the route change: geo-11 era hudsplit gave every HUD blend state a coverage variant while the HUD went
  to the layer (HUD-004 / HUDLAYER5 blend_variant); AKN's HUD redirect (engine mode) never did - the game's blends write
  alpha for an opaque target, AKVR's layer divides colour by alpha (HUDLAYER4).
- HUDBLEND (AKN): OMSetBlendState hooked (slot 35); while our HUD texture is bound, plain see-through blends get alpha
  ONE/INV_SRC_ALPHA + alpha written, other modes leave alpha alone; game blend restored when the target changes. Live
  "hudblend 0|1" + AKVR HUD section checkbox "see-through HUD fix". Default ON.
- HB run (16:59, training terminal panel, Run-HB.ps1 / judge_hb.py): colour-above-alpha 0 % both ways at this panel;
  preview of AKVR's conversion: OFF shows a dark smeared box behind the "COMPLETED" panel, ON is clean - but pieces that
  DARKEN the scene (multiply-type blends) cannot be carried by an over-layer and drop out (known HUDLAYER5 limit).
  Weapon wheel itself not reproduced (no pad injection; gadget wheel binding unknown). AKVR HUD layer swapchain is
  _SRGB (dark tones keep precision). NEXT: JJ A/B with the wheel open + panel capture (capmk) -> blend states of the
  wheel draws; if its backing is a multiply, convert grey multiplies to black-with-coverage.
- HB2 (17:03): capture now logs each HUD draw's blend (capmk). At the panel: colour SRC_ALPHA/INV_SRC_ALPHA or
  ONE/INV_SRC_ALPHA, but the ALPHA blend is ONE/ONE (additive) in 68 of 132 draws -> overlapping see-through pieces
  pile alpha up to ~1 on our sheet = semi-transparent areas turn solid/flat (JJ's "posterised", "wrong blending mode").
  HUDBLEND turns those into ONE/INV_SRC_ALPHA (true coverage). Deployed, default on, headset A/B pending.
- JJ: the wheels looked wrong in the GEO-11 version too -> the missing coverage blend (HUDBLEND) cannot be the main
  cause (geo-11 hudsplit had it). Shared by both: AKVR's layer conversion (HUDLAYER4) lin(saturate(rgb / a)) * a -
  every pixel whose colour is ABOVE its coverage (light added on a see-through backing: glows, additive strokes) is
  clipped to one flat value = bands. HUDGLOW (AKVR xr.cpp, all 3 conversion shaders): base = min(rgb, a) converted as
  before + added light max(rgb - a, 0) in linear on top; nothing clipped. Old shader kept (hud colour mode 2), overlay
  HUD "keep HUD glows" (live, default on). Shaders fxc-compiled offline OK; fake headset 18:48: layer "colour converted",
  2645 frames. The test panel has no glow-over-coverage (0 % colour above alpha) - the wheel A/B is JJ's.

## JJ's headset report 2026-10-08 ~19:45 (13 captures, saved in akvr_native/captures_jj_20261008/ with the log)
1. Weapon wheels posterised (also under geo-11). Captures c194443/c195922: the HUD sheet is RIGHT (composited the
   game's way it matches the flat game); the headset blends the HUD quad in LINEAR light, the game in display values ->
   the dark see-through backing goes milky, world shows through, gradients band in VD's stream.
   -> HUDINEYE (AKVR xr.cpp hud_in_eyes): in gameplay the HUD sheet is drawn into each eye picture (our side-by-side
   copy, 8-bit display values) with the game's blend, at the room-fixed quad's place through each eye's own pose and
   lens; no HUD layer then. Menus / pause / flat frames keep the layer. Setting hudineye (default 1), overlay HUD
   "HUD drawn into the eyes", test command "hudineye N". Fake headset 20:11: in-eye = 1 layer 0 quads, XP widget in
   place. HUDGLOW / HUDBLEND stay (shared conversion / coverage).
2. Lens flares: c194645 green flare ring in the LEFT eye only -> LENSFLAREOFF (AKVR config force LensFlares=False,
   DefaultLensFlares=False; FIX_CHANGES).
3. Batman's suit reflections still differ per eye - open (XEYE found no shared texture; R1 no extra from DLSS).
4. Grapple / radar / distance markers not on their objects; the distance marker moves OPPOSITE to head turns (rock
   solid under geo-11). Canvas.Project uses the LEFT EYE view (head included) and the HUD hangs in the room.
   Plan (geo-11 + Arkham City): (a) MARKERBASE - Canvas.Project answered from the head-free game camera (Arkham City);
   (b) the marker's HUD area from the geo-11 on-target points (earlyres akvr_hud_target_points, TARGETANCHOR/MULTI);
   (c) MARKERCUT in hud_in_eyes: that area cut out of the sheet and drawn into each eye at the object - world point
   from Canvas.Project (exact, Arkham City) or the left eye's scene depth at the point (geo-11 TARGETDEPTH) for markers
   without a projection call (grapple). AK's HUD pieces are partly 3D Scaleform (4x4 in cb0 rows 7-10, layouts differ
   per shader - MK4 calibration), so Arkham City's per-piece cb0 shift does not carry over; the cut works on pixels.
   MK4: the 52 m marker is on the HUD sheet (hud_rgb_he1) but fades in and out (absent in mk4).
5. HUD flicker (right-side elements, map elements, main menu + pause text) - not flat frames (gameplay 1/s); next:
   count HUD draws to the window outside the HUD phase.
6. Map: c194503 left eye shows the hologram map, the RIGHT eye the plain city top-down - the map's look reaches the
   game's view only; map icons on the HUD plane, not at map depth.
### Runs 2026-10-08 20:10-20:42 (HE, MK4, MC, MV x2, RF, RF2)
- HE: HUDINEYE works (in-eye: 1 layer 0 quads; XP widget in place). MK4 calibration: AK's HUD pieces are partly 3D
  Scaleform with per-shader cb0 layouts - per-piece cb0 shifting (Arkham City) not usable; the 52 m marker fades in/out.
- MC (MARKERCUT live, 20:25): geo-11 target point found in engine mode (-0.034 0.192), the game's Canvas.Project point
  (-0.057 0.187) matched (layout map k-1 0.667 = AKVR's HUD shrink), world point -> left (-0.057 0.187) right
  (-0.339 0.191) in each eye's own turned picture (the 0.28 gap = the eyes' +-7 deg lens turn). ~160 cards/s.
- MV x2: no visual verdict - the save now starts at the Grapnel Boost training terminal; no Canvas.Project calls (the
  52 m marker never showed), even after walking back. Needs JJ's headset check or a spot with a marker.
- RF / RF2 (Batman's suit gloss LEFT eye only, JJ c194852): per frame, bright share in the picture: normal L .071 / R
  .033; eyesame 1 (same camera + lens) equal; eyesame 3 (own spots, the left lens for both) equal; eyesame 2 (same
  spot, own lenses) differs (L .058 / R .038). Warped right-onto-left at the same spot (rfs2_warp): the left lens shows
  clearly more gloss on Batman's shoulder / arm / chest. -> the suit reflections depend on the LENS DIRECTION, not the
  eye position = screen-space (each eye reflects what is on its own screen; the left eye is turned left and sees more
  of the bright sky / moon). Candidates: BmSystemSettings Reflections=True (line 190) / AllowImageReflections=True;
  test each off (restart) with RF2; a real fix would need both eyes to reflect the same screen content.
- RF (vs 0 shared view state vs 4): inconclusive (Batman animated); RF2 rules the view state out anyway (same camera
  + lens = identical with separate view states).
- New AKN test mode eyesame 3 (own spots, both the left lens).
- MP (20:46): M did not open the map (Batman at the Grapnel training terminal); families left/right identical in
  gameplay (FAMDUMP 0x10-0x70). Map needs JJ's capture with the map open.
- RF3 (20:46-20:52, one setting off per launch, file restored byte-exact both times, hash D89ED1B64890): whole-picture
  bright share per eye - Reflections=False: left still brighter in eyesame 0 and 2 (not the cause); AllowImage
  Reflections=False: the consistent left bias is gone (differences either way). Batman-region warp check failed to
  align this run (Batman elsewhere / mode not settled) - unconfirmed. Candidate: UE3 image reflections (screen-space,
  per lens). Not changed in JJ's config; offer as a test.
- HUDHOLD (AKVR hudsplit): a fresh-pair frame with no NEW HUD sheet shows the previous complete sheet instead of no
  HUD layer (menus / pause / map flicker candidate); gameplay uses HUDINEYE.

## JJ's headset report 2026-10-08 ~22:45 (21 captures c223421-c224433 in Binaries\Win64\akn + akvr_captures)
JJ: distance marker on its object now, but it turns on its own axis with the HEAD (should only with the stick/mouse) and
flies off near the LEFT edge of the view (not the right); compass broken (chunks cut out, elements stuck to world
objects); weapon wheel still banding; suit reflections (dark suit, bat symbol, arm guards) still flicker per eye; map
HUD misaligned and the wrong size; main menu button-prompt element out of position.
Findings (from the captures + logs, before any change):
- MARKERS: Canvas.Project (RA 86DC0D) was answered with the LEFT EYE view (head + turned lens) - the 59 m marker wanders
  over the HUD sheet as the head turns (c224419-433). The geo-11 on-target points include COMPASS icons (they move with
  the view), and MARKERCUT matched them to the objective's projection within 0.25 in SHEET units (no layout map) ->
  circles cut out of the compass and pasted at the objective. Fallback (no target points) cut circles at bare projected
  points too.
- HUD SIZE: AKN's HUD texture = 2320x2860 (engineshape 811 read at launch) but AKVR's HUDFULL laid screen movies out at
  engineres x LIVE engineshape = 2640x2860 (AKVR re-measures the eye shape from the headset after the session starts and
  SAVES it: 925; the same launch rendered 1344x1656 = 811). HUD draws: viewport 2640 wide on a 2320 sheet (akn_log "vp
  0,0 2640x2860") = 14 % sideways stretch, right edge cut (map overlay size, main menu prompt, right-side elements).
  The shape value flips between launches (811 at launch both sessions today, 925 saved) - cause not chased; the HUD
  now takes AKN's real size, so it no longer matters for the HUD.
- WEAPON WHEEL: opening it stops the game camera -> PAUSE LOOK goes live (19:44/19:59 captures: "pause look LIVE now",
  camera 3.5 s without a finalize; 22:3x timeline: 1-2 s pause-look episodes) -> HUDINEYE was skipped (!g_pauseLive)
  -> the wheel went back to the linear-blended quad layer = the banding. HUDINEYE never reached the wheel.
- MAP (mode SCREEN, left half to both eyes): the picture was drawn through the headset's TURNED 85x90 eye lens + eye
  offset, the map's icons by the game for its own 75 deg lens; the screen crops the picture to a 16:9 slice while the
  HUD quad squeezed the WHOLE sheet into that slice; the HUD quad hung at the gameplay HUD's room anchor, the screen
  where the head looked when it opened.
- SUIT: pair c223928 / c223947 crops: bright gloss on the bat symbol / shoulders / arm guard in the LEFT eye only -
  matches RF2 (lens-direction dependent = screen-space image reflections; AllowImageReflections=False removed the bias).
Built (AKN + AKVR, 2026-10-08 23:2x):
- MARKERBASE (AKN hk_Proj): Canvas.Project answered from the head-free game camera (g_mkVbase) x the game's own lens
  (g_mkGameP), UE3 Project form (x/w, y/w, z/w, w). Command "markerbase 0|1" (default 1). PROJ log keeps the game's own.
- MARKERMATCH (AKVR hud_in_eyes): target points mapped through the layout map (txf) into game units, matched within
  0.06; no fallback cuts. AKN "mark" command no longer swallows "markercut"/"markerbase" (it did - markercut never ran).
- HUDSIZE: AKN export akn_hud_out_size; AKVR HUDFULL lays screen movies out at exactly that size and rescales the HUD
  shrink box (root state) into the same pixels when it was chosen in render pixels.
- WHEELEYE (AKVR): HUD in the eyes during the pause look too; the darkening (pause dim + DOF-off exposure) is drawn into
  the eye pictures UNDER the HUD and the dim layer is skipped that frame.
- MAPFLAT (AKVR + AKN): on a floating screen over a 3D picture AKN renders the game's own lens (asym 0, DLSS jitter
  kept), both eyes at the head; the HUD layer gets the screen's crop and hangs on the screen's pose. Command "mapflat".
- SAMEDIR (lens mode 3, AKN + AKVR): both eyes face the head's way (no yaw), centred lens wide enough for the outer
  edge (~28 % more width). Command "lensmode 2|3" (not yet a setting). Test: does it make the suit gloss equal?
Test: tools/Run-HM.ps1 (+ main menu capmk/headset shot in part A), judge tools/judge_hm.py.
### HM / HM2 / RF4 runs (2026-10-08 23:33-23:47, fake headset)
- HM: MARKERBASE verified - the objective (-75279,-34793,3526) gets 7.129/0.144 at head yaw 0 / +15 / -15 (the game's own
  left-eye answers 160.4 / 3.99 / 3.99); markerbase 0 -> the game's own. HUDSIZE verified: HUD texture 2640x2860 and the
  layout 2640 (compass now centred, u 0.348-0.667; before 0.397-0.759 on the 2320 sheet). WHEELEYE: pause = 1 layer, 0
  quads, pause menu crisp in both eyes over the darkened world. SAMEDIR facing Batman: lens 2 centre bright share
  L .016-.018 / R .032-.034, lens 3 L .026-.028 / R .028-.030, pictures visibly equal. No crash.
- JJ: lens 3's extra width is "way too costly" for some gloss -> default back to 2 (option + panel checkbox stay).
- RF4 (AllowImageReflectionShadowing=False one launch, file restored byte-exact D89ED1B64890): lens 2 still differs
  (L .042-.050 / R .015-.016), lens 3 equal (.036-.040) -> the shadowing is NOT the cause; the reflections themselves
  follow the camera direction (RF3: AllowImageReflections=False removes the bias).
- Map key is TAB (BmInput.ini OpenLevelUpOrMap), not M; akmenu key now sends scan codes (KEYSCAN). Fake-headset runs
  had been saving engineshape 811/813 into JJ's ini (memory akn-test-runs-touch-jj-settings).
- HM3 (23:48): map opened with TAB. mapflat 1: the map drawn through the game's own lens, HUD sheet + picture one layout
  (comp_hm2map1), headset: map + icons on one screen. mapflat 0: wider picture, layouts differ.
- LD / LD2 (23:55-00:02): loading-screen capture failed twice - the Enter on Continue did not land (LD2 never reached
  gameplay); the background shot job may disturb focus. Peer session's Arkham City then took the PC. JJ's settings
  restored after each run (engineshape 918, lensmode 2).

## JJ's report 2026-10-09 00:0x (12 captures c000734-c000931) and runs HF / HF2 (00:26-00:47)
JJ: wheel/pause "sitting in a dark window"; ALL HUD elements flicker (compass, level info, distance marker, wheel, pause
and map text) - "seems to only happen after we told DLSS not to scale the HUD"; lens flares still there (c000751, green
ring, left eye); pause/wheel: looking around flickers the world; loading screen "too big, most of it cropped".
- HUD FLICKER = HUDDSV (VERIFIED). HUDSTAT (60 frames of the sheet per setup): the sheet switched between 4 and 10
  covered cells (16x16 grid) every few frames with the SAME 28 draws; hudall / huddef / hudfull 0 changed nothing. Cause:
  since HUDFULL the sheet is the DLSS output size but the game binds its window buffer WITH its window depth/stencil at
  the render size (log: 1328x1656 f19 vs sheet 2296x2860) - D3D11 refuses the mismatched pair, the stencil-masked HUD
  draws miss the sheet. Fix: the sheet's own depth/stencil (sheet size, cleared with the sheet, the game's HUD-time
  depth clears redirected to it). HF2: huddsv 1 = 24 cells every frame (25 when a marker moves); huddsv 0 = 2/10/24
  jumping. The steady sheet also carries far more HUD (24 cells vs 4-10). Command "huddsv", default on.
- PAUSE / WHEEL WORLD FLICKER = PAUSESTAMP (VERIFIED): the pose stamp was the camera-finalize count, which stops in a
  pause (the wheel stops the camera too) -> the matched pose grew 1..30 frames old, then the geo-11 fixed delay 2. Now
  AKVR counts its pause-look camera writes as finalizes (akvr_camera_write_count, also the AKN stamp); HF: during the
  pause every picture matched, delay 1. Engine-stereo fallback delay 1 (was JJ's geo-11 posedelay 2).
- DARK WINDOW = BACKEXT: the game's backdrop (50 % black over the whole sheet + a darker 16:9 band, c000911) ends at the
  sheet's edges; on the pause look the sheet's edge texels are carried on over the whole view. HF2 pictures: the window's
  side edges gone, the pause backdrop runs as a band across the view. Command "backext".
- LOADING SCREEN = LOADFIT: HUDFULL laid every screen-sized movie out at the headset size, also on loading screens where
  nothing is collected (flat frames) -> drawn big into the small window, top-left part only. Now only while AKN pairs
  flow (akvr_xr_akn_3d), and the re-push applies HUDFULL both ways (it never did: HF2 showed a panel small top-left for
  ~15 s after the load). Not yet seen on a real loading screen (the shots came after the load).
- Reflections VIEWSCAN: the finished eye views hold only the turned axes (no unturned copy) - theory not confirmed there.
- Map key TAB; wheel has no keyboard key (pad only).

## JJ 2026-10-09 ~01:10: flicker RESOLVED (HUDDSV confirmed in the headset). Open: main menu button prompts too high and
## smaller; loading screen first too big, then rescales; pause + wheels in dark windows, the gadget wheel an oval.
- MAIN MENU PROMPTS = HUDVP (verified HB 01:22): JJ "sitting within a layout that is for a smaller resolution ... the
  buttons do look smaller". HUD parts outside AKVR's Scaleform resize draw with the WINDOW's whole viewport (render size)
  onto the sheet -> laid out in its top-left 58 %. AKN (RSSetViewports / RSSetScissorRects hooks, slots 44/45): while the
  sheet is the target, a viewport of exactly the whole window (+ its scissor) is stretched to the whole sheet; the game's
  own request is put back when it binds another target. mmain (on): prompts bottom left, full size; mmainvp0 (off):
  small, halfway up. Gameplay HUD unchanged (30 cells on and off, ~10 viewports stretched per frame). Command "hudvp".
- LOADING SCREEN BIG FIRST = LOADFIT2: the 3D flag waited 30 frames without a pair and the re-push had a 200 ms gate ->
  4 frames, and a 3D/flat change re-pushes at once.
- BACKDROPS = HUDSKIP: HUD draws dropped by vertex shader + texture size, only while the pause look is live (AKVR sends
  "hudskipon"). HB pause candidates: vs 3b819a7e86e9d631 + 256x1 gradient removed the pause backdrop (cells 120 -> 23,
  edge alpha 122 -> 0, menu text kept); 4b432a87 / 54cd897e / 9689d605 / 94f88871 changed nothing on the pause. Wheels:
  vs 9689d605c06946bf + 256x16 gradient (first visible draw in both wheel captures) - not verified (no key opens a
  wheel: arrows / Equals tried; pad only). Defaults: both rules.
- OVAL GADGET WHEEL = WHEELROUND: MENUFIT2 answered ResolutionY = round(1528 x 9/16) = 860 -> 1.7767 < the script's
  SIXTEEN_NINE_ASPECT_RATIO, so RadialGadgetSelect.as ResizeComponents still stretched GrandRightParent (ring 0.81
  wide:tall in c010816, text unstretched). Now floor -> 859 -> 1.7788. MENUFIT2 counters: 4 answers changed of 4 asked.
- HC (01:36-01:39): pause with the default rules: rules ON at the pause look, 1 backdrop draw dropped per frame, sheet
  23 cells / edge 0, headset: pause list + prompts over the evenly dimmed world, no window. Loading screen (LOADFIT2):
  3 headset shots during the load, the picture whole at its proper size; the gameplay HUD in place on the first frames
  after it. Wheel fixes (WHEELROUND, wheel backdrop rule) need JJ's pad.

## JJ 2026-10-09 ~01:47 (captures c014423-c014609): wheel no longer oval (WHEELROUND confirmed); every pausing screen
## (pause, weapon wheel, other wheels, mission dialogs) still in black backdrops with hard edges; detective mode: vignette
## per eye, and after leaving it the RIGHT eye stays half in it (orange x-ray on objects, flat blue light; c014609).
- Wheel backdrop: the 9689d605 + 256x16 rule did fire (the draw is gone from the wheel frames) but the backdrop stayed -
  it is another draw, beyond the 80 the capture dump logs. Pad injection (AKVR PADINJECT, game reads pad 0 ~80/s
  through the wrapped import, "connected") opened NO wheel and did not toggle detective mode (D-pad up per JJ) - the
  game ignores injected buttons; keyboard: the gameplay keys are not in any ini (only debug keys), V/Q/R did nothing.
- BLACKKEY (AKVR, default on): on the pause look the HUD sheet is drawn with pure-black see-through texels removed (the
  backdrops are rgb 0 with coverage, c000911); BACKEXT off. HF4: pause menu intact with it on (its own backdrop is
  already gone by HUDSKIP) - the wheels / mission dialogs need JJ's check.
- Detective mode right eye: the right eye's own FSceneViewState (vs 4) keeps detective state the game resets only in
  its own. CAPDET (AKN): every panel capture now also saves both view states (vs_<tag>_<eye>.bin) and the eye renders'
  pixel shaders; shaders not in akn\ps_normal_ref.txt (718, recorded HF4) are hidden one at a time with a pair picture
  each (vignette hunt). HF4: end to end OK (10 candidates in the pause). Next: JJ's captures normal / in detective /
  after exit -> tools/vsdiff.py (STUCK fields) -> carry them from the game's view state each frame; the vignette = the
  CAPDET picture without it.
- Harness: the first Esc after part A is often not taken - hf4_driver.py pause() confirms via the log and retries.

## MIRRORHUD 2026-10-09 (parked)

AKVR draws the HUD sheet over the desktop mirror after the DLSS stretch (mirror_hud in xr.cpp, g_mirrorHud=1). The diagnostics show it drawing every frame (5922/5922), but the desktop shot still had no HUD. JJ: stop spending time on it; for HUD checks, ask JJ to take headset captures instead. Not investigated further.

## RAINPAIR + VSFRESH 2026-10-09 (03:00)

JJ 02:54: right eye still wrong after detective mode (c025418: right eye has no fog, light shafts or rain), left-eye rain frozen, and a crash on returning focus to the game.
- Rain: after the detective exit the one remaining rain step per frame was tagged LEFT (worker-thread recording, so the eye tag is a guess) and was skipped, leaving no step at all. RAINPAIR: the first step of each stereo pair runs (the pair number advances at the start of the right eye render). Fake headset HF6: about 77 steps run and 77 skipped per second; still correct after a pause.
- VSFRESH: on detective exit (the composite PS is no longer seen) the right eye gets a NEW view state (vs mode 4 recipe; the old one is leaked on purpose). Commands: "vsfresh", "vsfreshdet 0|1". HF6: 3 forced resets, no crash, both eyes match. The detective exit itself is UNTESTED (pad only).
- View-state diff c025418 against normal pairs: only flags +0xB0/+0xC0 and a few counters differ, nothing consistent. Fog/rain state is probably held in pointed-to objects.
- Crash 02:55 (dump unreal-...02.52.58.dmp): access violation in 0x806870 (per-primitive view relevance, worker task 0xFA5xxx), when calling the virtual function at primitive+8 (proxy). The vtable read was 0x7ff6df5c7e (a shifted pointer) = a deleted or overwritten primitive. The stack holds stale AKN addresses (mip_twin, load_settings; map of the 02:37 build in the scratchpad). The 01:42 crash was a different spot (0x11C84DE, a write to null). The cause is not found. Tool: scratchpad dmp.py reads UE3 dumps (python minidump). The current build has a map (scratchpad akn_hf7.map). Repro driver tools/hf7_driver.py (Notepad focus out/in) is written but not run; JJ is testing by hand.

## VSCOPY 2026-10-09 (03:10-03:20)

- VSFRESH (a fresh right-eye view state on detective exit) is now OFF by default. JJ 03:09: with a fresh one, detective mode turned lights white in the right eye and the map showed the real city instead of the hologram. The game sets up every view state it already knows when those modes switch; a fresh one misses that setup (c030900/c030937: dozens of fields stay at start-up values, 0x1f at +0xC/+0x54/+0x574..).
- VSCOPY: copy view-state ranges game -> right eye each frame. Commands "vscopy <hex off> <hex len>" and "vscopy clear".
  - Copying +0xB0 and +0xC0 together CRASHED at the first stereo frame (03:14, a write to 0x24 at 0x1064C80).
  - HF8 fake headset: +0xB0 alone ran fine; +0xC0 alone crashed within seconds. +0xC0 is not a plain flag.
  - Default is now { +0xB0 }. Whether it brings back fog, light shafts and rain after the detective exit is untested (needs JJ, pad only).
- MENUFAST: the main-menu Enter retry now comes after 12 s instead of 45 s (JJ: "ridiculous amount of time").
- Marker cut-out moves other HUD (JJ c031225: an "INCOMPLETE" challenge panel lost a circle that moved with the marker card). Planned structural fix, per VR_WORLD_MARKERS_GUIDE section 3.3-3.4: route the marker's own HUD draws (placement within reach of the target point) into a separate marker sheet instead of cutting a circle out of the HUD sheet.
- To do: the main menu Batman head pops right after the logos; gameplay entry shows a split second of flat picture.

## Live detective test 2026-10-09 03:22-03:25 (JJ in the headset, VSCOPY +0xB0 build)

- Pictures for ready, on and off: pair_r1ready/r1on/r1off; view states vs_r1*_{0,1}.bin.
- Detective ON looked right in both eyes.
- Detective OFF: the right eye still had no fog and no rain, and kept an orange x-ray strip. Copying +0xB0 does NOT fix it.
- Fields equal between the eyes before the switch and different after the exit:
  - +0xC0: game 0, right 1.
  - +0x3B4 (float): game -86 in detective and -105 after; the right eye stays -0.
  - Everything else was counters.
- A one-off copy of +0xC0 (writing 0 into the right eye) CRASHED within 1 s (dump 03.20.27: exe+0x1070959 reads null+0x24). Never touch +0xC0, in either direction.
- Next: HWDET. "hwdet 3b4 c0 b0" arms a hardware write-watch on the GAME view state and logs the exe offsets of the code that writes those fields. Toggle detective on and off, decompile the writers in Ghidra, then call the same reset on the right-eye view state.
- LOADSEEN (wait_steady.py): the menu "missed Enter" was not a miss. The VR loading screen runs smoothly, so the hitch-based load detector never fired. A frames/s < 1 line now also counts as the load starting.

## HWDET result + VSSPLIT 2026-10-09 03:55-04:05

- HWDET (JJ live, toggling detective on and off): game view state +0x3B4 and +0xC0 are written EVERY FRAME by the renderer (0x8145DE, 0x803789, 0x80551D). Detective entry added only 0x814909, which writes +0x3B4 from view+0x8DC negated. Nothing writes these fields at the exit. +0x390..+0x3B8 are previous-frame history copies (+0x3B4 <- +0x3A8). There is no exit reset routine to call; dead end.
- VSSPLIT, built and installed but untested: the right eye keeps two view states.
  - The ORIGINAL view state is used while detective mode is up (shader seen within 100 ms) or a flat screen is up (map; AKVR export akvr_flat_screen_now).
  - A second view state, made at the first detective exit and never used in detective mode, is used in normal play.
  - Why: the original is right in detective mode and on the map but stuck after the exit; a fresh one is right in normal play only.
  - Command "vssplit 0|1". Log "VSSPLIT: right eye view state ..." at each swap.
- VSCOPY default list is empty again.

- MARKERRING 2026-10-09 (AKVR xr.cpp mcps): a marker circle is cut only when the ring just outside it (1.2 r and 1.5 r, 24 samples each) has no HUD (alpha < 0.04). Otherwise the marker stays flat in the sheet: no holes, but no depth while it overlaps other HUD. This is the interim fix for c031225; the structural fix is still a separate marker sheet (guide 3.3-3.4). The shader was compiled offline (hlslcheck.py in the scratchpad) before shipping.
- VSSPLIT bug fixed (04:20): the exit test read detWas after it had been overwritten, so the split never engaged in the 04:17 session.

## MARKSHEET 2026-10-09 04:20-04:40 (structural HUD-hole fix, in progress)

- JJ 04:2x: detective mode FIXED (VSSPLIT). New report: the distance marker sits at the right depth, but up close a camera move snaps it to HUD depth and the next move puts it back. Likely MARKERRING: the marker's own edge reaches the ring. Log 04:22 also shows one cut whose eye projection is far from its HUD position (hud -0.198,-0.038 vs L 0.147,-2.920): the game may pin markers on screen up close.
- AKN: CPU shadow of HUD-phase constant uploads (hooks VSSetConstantBuffers 7, Map 14, Unmap 15, UpdateSubresource 48; first 128 bytes). A piece that is a marker draw (VS 9938094a, position rows cb0[6] / cb0[7], translation .w) within 0.08 of an AKVR marker centre goes into the marker sheet (g_mkTex, cleared with the HUD sheet). Export akn_get_marker_hud. Commands: marksheet, mkreach, mkpieces N.
- AKVR: exports akvr_marker_centres (last frame's cut centres, sheet NDC). When AKN routed pieces last frame, the HUD sheet is drawn whole and the cards are drawn from the marker sheet (radius 0.15, no ring test). Otherwise it falls back to MARKERCUT + MARKERRING.
- AK HUD vertex shaders (from the geo-11 fix ShaderFixes):
  - ONE piece per draw: 9938094a and 3b819a7e (the pause backdrop), position rows cb0[6] / cb0[7].
  - BATCHED, per-vertex row index: 05154232 (cb0[r+14]), 7d8fcdc2 (cb0[r+14]), ef1c1604 (cb0[r+10]). These cannot be routed per piece.
  - Which kind the distance marker uses is UNKNOWN: "mkpieces 200" with a marker on screen.
- Fake-headset HF9: no crash; no markers at the test spot, so routing is untested. The first shadow layout guess (Batman City rows 0 / 3) read world numbers; fixed to rows 6 / 7.

### SVR (2026-10-09): engine stereo through SteamVR
JJ: get the NATIVE mod working in SteamVR. Known blocker (akvr/CLAUDE_HANDOFF.md DXGI11): SteamVR's in-game client needs
a keyed-mutex sync texture, which AK's device (DXGI 1.0 CreateDXGIFactory) cannot make -> every submit
SharedTexturesNotSupported. The fix (answer CreateDXGIFactory with CreateDXGIFactory1) only clashed with geo-11, which
engine stereo never loads. SVRAUTO (akvr/src/earlyres.cpp akvr_early_dxgi11_mode): no `dxgi11` key = on (game calls)
when -akn AND the process's runtime is SteamVR (XR_RUNTIME_JSON, else HKLM ActiveRuntime contains steamxr/steamvr);
otherwise off, so the Virtual Desktop route is unchanged. Status line: "on (auto: SteamVR)".
Launcher: install/AKVR-Launch-VR-EngineStereo-SteamVR.bat (installed in Binaries\Win64): XR_RUNTIME_JSON = HKCU's
steamxr manifest for this launch only. Test script: tools/Run-SVR.ps1 <scratch dir> (backs up/restores JJ's ini,
closes SteamVR if it started it).
Run SVR1 14:00: SVRAUTO on, 2 game factory calls upgraded (via gameoverlayrenderer64). SteamVR: HmdNotFound - Quest
not connected to VD ("Streamer did not return a connected headset"; forcedDriver vrto3d "driver not found", ignored).
So SteamVR CANNOT be tested without the Quest connected on this PC. Menu script also stalled (no session).
Still to check with the headset: xrclient_BatmanAK.txt 0 share errors, picture both eyes, pacing (playbook 09: SteamVR
xrWaitFrame is a poor pacer, reports multiples of the period), Touch controllers (VD's gamepad emulation under SteamVR?),
quit (vrserver kills apps lingering after VREvent_Quit).
JJ's 1st SteamVR run 15:09 (SVRAUTO build): pictures ARRIVE (xrclient 0 share errors; share probe keyed-mutex 0x0, factory1
yes). Reported: title screens "behind me", semi-transparent, upside down; main menu in 3D but upside down + unfusable.
xrclient: "xrLocateViews: Switching to TrackingUniverseSeated because XR_REFERENCE_SPACE_TYPE_LOCAL continued to be
requested for more than 3 seconds" (3 s after session start) - NO reference-space-change event reached us (timeline has
no "runtime recenter"); the screen was hung at 0.6 s. Lens identical to VD (49.48/35.50/39.38/50.52). Also at the end:
"Aborting due to ignoring Quit event after 5.0 seconds" - we never destroy session/instance on EXITING (open; ask JJ
how he quit). Reasoning: a level screen (pure yaw) can only show upside down + behind if LOCAL itself turned ~180 deg
about a horizontal axis after it was hung; head pitch/roll in camera.cpp are ABSOLUTE in LOCAL (only yaw is zeroed),
so a tilted LOCAL also turns the rendered world -> unfusable. UNPROVEN until the pose trace is read.
SVRSPACE build (xr.cpp): both LOCAL and STAGE spaces made; `xrspace` 0 auto = STAGE under SteamVR (no seated switch),
1 LOCAL, 2 STAGE; live AKN command "xrspace N" re-zeroes. POSEJUMP: >25 deg or >25 cm in one frame = re-zero (mode
timeline "pose jump"). POSETRACE: akn\akvr_pose_trace.txt, 1/s, head in both spaces + hung screen + eye-0 submit pose.
Fake headset 15:22: switches live 0->2->1->0 at the main menu, no crash, trace OK. (Run-M5c-A's menu Enter is failing
now: the game window loses focus - taskbar over it in menu_4 shots; harness issue, not the mod.)
JJ's 2nd SteamVR run 15:2x (SVRSPACE): title + loading screens now fine. POSETRACE PROVED the upside-down cause: SteamVR's
seated LOCAL on JJ's PC is ROLLED 180 deg (head ypr 0 0 180, y -1.27 m) while STAGE is upright - SVRSPACE fixed it.
New: everything with its own per-eye picture (menus, HUD, gameplay, "even the VR overlay") too far apart to fuse.
Reasoning: SteamVR's eye geometry itself is fine (identical flat pictures fuse); the only per-eye difference the flat
screens lack is the TURNEDEYE turn (each eye view yawed ~7 deg out, pitched 5.6 deg down vs the head). Hypothesis:
SteamVR's OpenXR->OpenVR path ignores/mishandles view orientation that differs from the head (OpenVR has fixed
eye-to-head transforms) -> each eye's content shown ~7 deg outward = divergent. (The panel is a plain quad; probably
judged against the unfusable world - ask.) SVRLENS: panel DLSS section "eye cameras" radio: turned out (2) / same way
(3) / straight (1, off-centre lens, no turn) - live. If 1 fuses and 2 does not: proper fix = warp the turned eye image
into the runtime's own unturned asymmetric lens ourselves before submitting (exact for a pure rotation), any runtime.
JJ's 3rd SteamVR run (SVRLENS): "same way" (lens mode 3) FUSES in SteamVR; turned out (2) does not. So SteamVR handles a
view orientation shared by both eyes (mode 3 still pitches both 5.6 deg down) but NOT eyes turned different ways (mode 2
yaw +-7 deg). Remaining in mode 3: some wet-ground reflections differ per eye. Not the suit cause (that was lens
DIRECTION, gone in mode 3) - left: eye POSITION (true reflection parallax, or SSR missing/noisy per eye). Discriminator:
AKN "eyesame 1" (both eyes one spot + one lens) - match = position only; still differ = per-eye render state (noise /
history) -> share it. Open choices for JJ: SteamVR auto mode 3 (+25 % pixels) vs mode 2 + our own warp to straight eyes.
JJ's SteamVR capture 15:31 (lens mode 3): wet-ground sheen in the LEFT eye only - slab luma L 68-103 vs R 26-34 over 11
frames. Not lens direction (mode 3). Left = the game's own family, right = our clone -> per-view reflection work done
for the game's view only. JJ: "check Arkham City's ground reflections" - BATMAN_CITY_PLAN PERCAPTURE: the reflect
probe's UpdateRequired (TimeBetweenCaptures/LastCaptureTime vs family CurrentWorldTime) skipped the 2nd eye pass; fix =
hook UpdateRequired, return TRUE in the 2nd pass. NEXT for AK: find the same (scene capture list before the main scene,
probe vtbl UpdateRequired) in AK's 64-bit exe; per-eye pass RT diff (an RT the clone pass reads but never writes).
SVRSTRAIGHT (xr.cpp straighten()): turned eyes rewarped to the runtime's straight lens + straight views when SteamVR
(svrstraight 1 auto / 2 always / 0 off, AKN command). Fake headset SVR2 (tools/Run-SVR2.ps1, into gameplay ~60 s, menu
steps <= 10 s per JJ): straightened shot lines up with the turned one; its blown-out cyan colours were a ONE-FRAME AKN
glitch - "window buffer" flips to a 1520x1656 f26 (R11G11B10 scene) buffer for one frame right when a capture stalls
the GPU (15:46:57 xrsim shot, 15:47:02 pairshot with straighten off) and the pair is rebuilt in f26 for that frame. A
real one-frame flash risk on hitches - guard: ignore a window-buffer change whose format differs. Clean "on" shots still
owed (Arkham City was started at 15:47, launch blocked).
CORRECTION 16:0x: JJ's SteamVR run (thought it was VD; startup log says SteamVR/OpenXR, STAGE, DXGI 1.1 on) with
SVRSTRAIGHT on = "super exposed, blown out to white, blurring when moving the head". So the fake-headset st_on cyan/white
WAS the straighten output, NOT the AKN one-frame f26 glitch (that glitch is real too - BBSTEADY now ignores it - but it
did not explain st_on). Straighten is BROKEN; cause unknown (shader math reads fine; suspect AKN's immediate-context
hooks acting on our draw at Present, e.g. sampler/blend swaps or HUD-phase binds - compare g_aknSbs vs g_aknStraight
dumps on the fake headset). JJ's ini set to lensmode=3 / engineshape=1172 / svrstraight=0 for SteamVR (works).
RF-SVR (JJ live, SteamVR, lens 3, straighten off, 16:12): fuses fine. pair rfA/rfC (own spots) and rfB (eyesame 1 =
identical camera + lens): LEFT brighter in all - slab 66 vs 47, grate 79 vs 39, sky glow 91 vs 60, rest of ground equal.
=> NOT eye position/direction: the RIGHT (clone) render lacks it. trace (16:13:06): right eye runs EXTRA steps the left
does not - Dispatch 16x16x1 into two 256x256 f28 UAVs (+ a 61x104 pass writing one of them as u1), and Dispatch 10x1x1
into 3 buffers incl. its own first buffer (view-state resources at 1F... addresses) = looks like per-view state treated
as new every frame (temporal effects never accumulate). View-state diff (JJ's capture vs_c153142_0/1.bin, 0 = left/game,
1 = right): 12 ranges set in the game's, ZERO in ours: +0x6c(16) +0x94(4) +0x108(8) +0x1d0(64) +0x3e8(8) +0x44c(16)
+0x474(4) +0x484(100) +0x4ec(28) (u16 pairs ~0x30-0xa6: occlusion/visibility history?), +0x52c = 1.0f,
+0x8834 = 1,1 (flags), +0x8844 (3 dwords). Test ready: tools/Run-VS1.ps1 (eyesame 1, vscopy groups flags / 1.0 /
u16 ranges / all, then vs 0 as positive control) - the 16:16 run failed: JJ's own window covered the game, menu keys
lost. Quit via the game menu under SteamVR: CLEAN (no 5 s abort).
CORRECTION 16:3x (JJ: "last launch looked good to me too"): the 16:12 regional numbers were a MEASUREMENT BUG ($W/$w
are one PowerShell variable). Whole-picture |L-R|: rfB (eyesame 1) 0.33 = identical; rfA/rfC 10.5 = normal eye-position
difference; 16:24 vA-vF all ~0.6 (copying view-state fields changed nothing). The right eye's extra trace steps / zero
view-state fields do NOT show in the picture - lead dropped. The 15:31 capture IS different: right eye ~20 % darker
overall (L 57-58 vs R 44-46, |L-R| 17-19) - JJ had switched eye cameras LIVE in the panel (turned -> same way) in that
session; since restarts it is gone. Suspect: a live lens change leaves the right eye wrong until restart (unproven).
SVRSTRAIGHT root cause FOUND (16:4x): g_strState (the private context state shared by flat_stretch, dlss_mirror and
straighten) KEEPS its pipeline state; MIRRORHUD (inside dlss_mirror) sets the HUD blend ONE/INV_SRC_ALPHA + its DS/RS
and never resets them, so straighten blended every frame onto the previous g_aknStraight (never cleared): colours piled
up to white (fake headset: off G 12 -> on G 255, R 0 -> 88) and old frames trailed = JJ's "blurring when moving your
head". STRBLEND: all three set opaque blend / no depth / default raster right after swapping the state in. (flat_stretch
and dlss_mirror's main draw were exposed to the same leftover blend.) SVRDUMP: "svrstraight 3" saves akn\st_in.bmp /
st_out.bmp once. akmenu.py OVERCHECK: no click unless the window under the point is the game.
STRBLEND VERIFIED (fake headset 16:39, tools/Run-SVR2.ps1 -> scratch svr10): svrstraight 0 vs 2 headset shots match -
mean luma L 46.4/45.5, R 38.7/36.1, same near-black share 3.6 %; leftover diffs = the animated Riddler hologram + rain.
JJ's ini: lensmode=2, engineshape=918, svrstraight=1 (auto: SteamVR only). Next: JJ's SteamVR headset test.
JJ SteamVR 18:44 (SVRSTRAIGHT + STRBLEND): fuses, but "a 2D flat copy of all 3D elements overlaid, moves opposite to
head". Engine pair + desktop fine; his capture c184453 HUD sheet (hud_rgb/alpha) held the WHOLE left-eye picture.
Cause: dlss_mirror binds the window buffer (g_bb) inside AKN's HUD phase (eye 0 end .. akn_present) -> AKN redirects
it to the HUD sheet (probe.cpp OMSetRenderTargets hook, g_hudPhase || g_hudAll) -> HUD-in-eyes draws the scene flat.
Before STRBLEND the leftover HUD blend hid it; opaque made it solid. Not SteamVR-specific (any DLSS run). MIRRORLATE:
dlss_mirror now runs after akn_send_present. Check queued: tools/Run-SVR3.ps1 (capmk fk -> HUD alpha must be HUD only).

## SUITGLOSS 2026-10-09 evening (JJ: suit chest/arm gloss differs per eye in turned-eye mode; wants it without pixels)
Static proof from the geo-11 fix's ShaderFixes (masterotaku labels the first line): the character "specular" family
(UE3_BatmanAK.ini ShaderRegex_SpecularPS1/2/3; e.g. 2d72637801b0a8a0 "Manual specular, Arkham Knight helmet") does a
VIEW-SPACE MATCAP: pos0 = v5 + cb1[21] (world-relative), ... r9.xy = r0.y*cb1[12] + cb1[11]*r0.x + cb1[13]*r0.z;
r9.xy = r9.xy*(0.5,-0.5)+0.5; sample t2 -> the reflection is looked up in the CAMERA's frame (cb1[11..13] = view
rotation rows). Depends on camera ORIENTATION only = RF2 (same spot + own lenses differ; own spots + one lens equal).
TURNEDEYE gives each eye its own +-7 deg yaw / pitch -> different matcap texel = per-eye gloss. Not screen-space.
Fix plan (no pixel cost): detect these PS at creation (disassembly pattern: add rX.xyz, vN.xyzx, cb1[21] ... cb1[11..13]
rotation feeding .xy*(0.5,-0.5)+0.5 -> sample), and for their draws bind a COPY of the view CB (b1) whose rows 11-13
are rotated back by the eye's lens turn (head orientation) - like RAINDEPTH's per-dispatch cb copy. Live switch, sign
A/B (turn vs inverse). NOTE: cb1[11..14] is also used with a translation row in the lighting CS 43a28c74 (positions) -
only the matcap draws may get the copy, never a global rewrite.
SUITGLOSS built + fake-headset run GL1 (19:08, tools/Run-GL1.ps1): AKVR flags 104 of 30440 pixel shaders as camera-frame
matcaps (D3DDisassemble on a worker thread, pattern rN.xy from cb1[11..13] then .xy*(0.5,-0.5)+0.5); ~320 matcap
draws/s in gameplay, all patched. LAYOUT PROVEN: the game's cb1[11..13] = the turned view's rows 0-2 exactly (.w 0),
row 14 = translation -> mode 1 (head view rows) is the right write. eyesame 2 pairs lined up (warpdiff.py, rotation
homography): Batman-area |L-R| gloss 0: 4.94 5.87 4.36 | gloss 1: 4.67 4.59 4.33 | gloss 2: 5.24 3.95 5.36 - points the
right way, but the suit is barely lit at that spot and resampling noise dominates: JJ's headset A/B decides (live
"gloss 0|1"). Default gloss 1. MIRRORHUD2: desktop mirror skips its HUD draw when HUDINEYE already drew it into the
eyes (JJ saw a doubled HUD on the desktop after MIRRORLATE).
JJ headset A/B 19:15 (gloss 0/1 x2, 10 s each): NO visible change -> the matcap family is not the visible suit gloss.
gloss default now 0. Next lead LIGHTCB: the deferred lighting/reflection CS 43a28c741999da36 converts tile/pixel
positions with its OWN cb0[13..16] (and lens cb0[6]/[7]); if that comes from the un-turned camera, image reflections
(AllowImageReflections=False removed the per-eye bias, RF3) are aimed wrong per eye. Test: tools/Run-LC1.ps1.
JJ 19:1x: on entering the game under SteamVR the ground reflections mismatched "for a couple of seconds, then fixed
itself" = temporal history of the right eye's fresh state filling up (harmless; same family as 16:24 note).
LC2 / OC1 (fake headset 19:37 / 19:40): lighting compute step in TODAY's game = dae26b50bf0927b8 (cb0 448 B, cb1 608 B, 21
inputs; the 2018 geo-11 hash 43a28c74 does not exist - JJ: don't get confused with the geo-11 fixes). Its cb0 rows 6-9 =
the eye's own lens (DLSS jitter x/y in row 8 differs slightly from proj: x 0 vs -0.00033, y sign flipped - minor, open);
its cb1 (view buffer, 608 B) is fully per eye: [9]/[18]/[21] eye position (6.4 units apart), [11..14] the TURNED view,
[15..17] its transpose. => NO wrong-camera bug in lighting. Right-eye-only compute step 15a1e2efe71084c6 (2 x 256x256
f28): called from FUN_140ced1c0 (per-eye tile shading setup) -> FUN_140ce66a0, only when the scene's vtbl+0x1f8 result
[0] == 0 (a once-per-frame "needs update" flag) - a reflection-environment refresh done in the first eye's render;
"skipcs" of it: no measurable change in either eye (6.9/7.6 vs control 7.6/8.3). Still unexplained: suit gloss per eye.
Remaining direction-dependent candidate: a SCREEN-SPACE reflection pass (half-res 61x52 dispatches into three 968x828
f10 UAVs in both eyes) - the chest plate reflecting on-screen content each turned eye sees differently; same-way lens
fixes it because both screens then cover the same view. If confirmed (live skip of that pass in JJ's headset), a fix
without any trade-off may not exist (screen-space data is per screen). New commands: skipcs <hex>, csstack <hex>,
lightinv N, lightcs <hex>, lightcb N. Scripts: Run-LC2.ps1, Run-OC1.ps1.
SUITGLOSS2/3 (20:0x-22:2x): JJ: geo-11's suit reflections "move with your head movements, but at least they kind of fuse"
= the suit gloss uses the CAMERA'S AIM (moves with head in VR; per-eye aim in turned-eye mode -> no fusion). New set:
AKVR flags pixel shaders reading cb1[11..13] (camera direction rows) but never cb1[14] (position row): 1010 shaders
(akvr_shader_matcap bit 2), ~5000 draws/s. gloss 3 = that set, 4 = both sets; glossbits <mask> <val> halves by name;
glosssrc 1 (default) = rows from the HEAD-FREE game camera (MARKERBASE g_mkVbase, built every stereo frame) so the gloss
also stops sliding with head turns; 0 = head aim. Fake headset GL2 19:59: no crash, picture normal (diffs = hologram).
Next: JJ headset A/B at his geo-11 capture pose (Batman facing camera, rooftop): gloss 3 off/on, glosssrc 0/1, halving.
JJ live gloss A/B 22:32 (off/on x2, head aim vs head-free): JJ called out "that one!" mid-sequence (phase unknown).
GLOSSPANEL (AKVR hooks.cpp, engine stereo): overlay section "Suit gloss test" - swap off / mirror (1) / direction (3) /
both (4), aim head-free (glosssrc 1) or head (0), group sliders A/B/C (glossbits: 4 name bits each, 0 = all) so JJ narrows
the swap to the suit shader himself. Live, not saved; game starts with gloss 0. Installed on game close.
JJ 22:4x CHOSE (own tests in the panel): "game camera without the head and direction shaders" -> defaults now gloss 3
(1010 direction-only pixel shaders get rows 11-13 from the head-free game camera) + glosssrc 1; panel starts on that.
Suit gloss: matching in both eyes, no longer tied to the turned eye's aim, no extra pixels. Open: narrow to the suit's
own shaders (group sliders) if a side effect ever shows; ground wet reflections are a separate (screen-space?) item.
PANELTIDY3 (2026-10-09 23:0x, JJ: "remove unnecessary, redundant and obsolete tests and options"): overlay 126 -> 81
controls. Gone: the 4 test buttons (blur / softness search, distance-blur A/B, DoF cost), the near-floor skip test, the
Batmobile transition test, the "Suit gloss test" section, the eye-camera choice, toggles of settled fixes (camrestore,
camsmooth, hudblend, HUD glows, hudineye, markercut, markerhead / markerface / markerdist + upright timing), the main-menu
"one spot" fallback, the head-pose delay sliders, and every "keep ON / keep OFF" switch in Advanced (now "Advanced
(numbers)"). Hidden in engine stereo only: the geo-11 each-eye-view experiment and "floating screens show a flat
picture". Kept: RAIN TEST (JJ: "might come in handy later"), DoF / AA / edge smoothing, DLSS + model + texture
sharpness, world scale, picture size, comfort / aim options, camera offset, HUD size / position / parts, post effects,
zoom vignette, menus, frame-rate lock, diagnostics. Settings files keep every value (JJ's: all settled values ON/OFF as
labelled). Backup of hooks.cpp before: scratchpad hooks.cpp.before-tidy3 (session-local).
CLEANTESTS (2026-10-09 23:0x, JJ: "the code for these tests ... should be removed from the mod"): removed SWEEP /
SOFTSWEEP / BLURAB / COSTSWEEP runners (+ Shift+F2 sweep, g_dofTest, the hudsplit drop-one-shader hook), CAMTEST,
lens mode 3 (AKVR akn_eye_lens + AKN TURNEDEYE `same`; a saved lensmode=3 falls back to 2) + AKN "lensmode" command,
the matcap recognition (only the camera-direction rule stays: export akvr_shader_camdir), glossbits / glosssrc /
glosslog / head-frame store, LIGHTCB / LIGHTINV / ONCECS (skipcs, csstack) / SUITPS (skipset, pscb) / VIEWSCAN,
SVRDUMP, POSETRACE, the AKN xrspace / svrstraight commands (the ini settings stay). Kept: RAIN TEST, the generic test
commands (eyesame, pairshot, trace, capmk, psinv, vsinv ...), all fixes. Smoke run 22:59: gameplay normal, suit gloss
2800-3000 draws/s corrected, no crash report. Source backup: scratchpad before-clean/ (session-local).
MARKERWORLD (2026-10-09 23:1x, JJ: distance marker "a little bit too small" and "rotating when my head rolls ... should
only rotate with camera movement"): the per-eye marker card was an upright square in each EYE's picture (the eye rolls
with the head -> the card rolled). Now a panel in the WORLD at the Canvas.Project point, sides along the head-free game
camera's right / up (akvr_camera_axes_ago base axes, signs checked by projection), world size = the old on-screen card
size (from the left eye: NDC per unit along the camera's right) x markersize (default 1.25, HUD slider "distance marker /
reticle size"); each corner projected through each eye's pair VP. Falls back to the old square if the axes are missing.
Not seen on the fake headset (the start save's training pop-up hides markers) - JJ to check.
JJ 23:04-23:07 (MARKERWORLD build): marker no longer rolls; but it flickers, near the objective it "snaps out to a new
location at a different size and standard HUD depth", and before that "missing the very left and right top corners ...
cutting out the text that's near it". Log: matched (pts 1 cut 1) most seconds until 23:06:57, then mostly pts 0 (the
HUD tree's on-target point gone near the objective = the game's near-range marker is not recognised -> flat on the HUD).
His HUD sheet c230453 HAS the 37m marker on the MAIN sheet = AKN did not route it that frame. Causes: routing reach 0.08
(bracket corners / distance text are separate pieces further out -> left on the HUD sheet), routing keyed to AKVR's LAST
frame centres (one unmatched frame -> nothing routed next frame), and the small-circle MARKERCUT fallback when not routed
(bites out of nearby text, corners lost, alternating = flicker). Fixes: MKREACH (AKN reach 0.16, card region 0.22),
MKHOLD (AKVR keeps the last centres 8 frames), MKNOCUT (sheet on + not routed = no cut, marker whole on the HUD that
frame). capmk now also logs 80 MKPIECE lines (piece placement / nearest marker / routed) - for the near-range marker.
JJ 23:12 (MKREACH/MKHOLD/MKNOCUT build): steadier but "not locked onto the target ... floats away when you move the
camera". capmk MKPIECE log (80 draws): 0 pieces routed to the marker sheet - the distance marker's pieces near its centre
are drawn by VS fd60f2d764b91756 / 4b432a87d072f988 / 91e2b2225bf3ea04 / 05154232 (batched), never by 9938094a, the only
VS MARKSHEET routes. So the marker sheet never had this marker; the circle-cut fallback was what kept it on target, and
MKNOCUT left it flat on the room-hung HUD. MKNOCUT REVERTED; cut radius 0.06 -> 0.08 (MARKERRING still guards nearby
HUD); MKHOLD kept. Proper fix next: cut the marker's exact rectangle from the HUD tree (earlyres target element bounds)
instead of a circle - no bites, corners + distance text included - and/or route those VS once proven single-piece.

### MKSINGLE 2026-10-09 23:25 (JJ: "cutting out with a circle seems like a pretty bad hack")
Disassembly (fxc /dumpbin of the dumped VS): fd60f2d7 (brackets) and 91e2b222 (text) place ONE piece per draw exactly
like 9938094a (position x / y = dp4 with cb0[6] / cb0[7], immediate indexing); 4b432a87 / 05154232 index cb0 per vertex
(batched, cannot be moved whole). AKVR (hudsplit.cpp) now disassembles every vertex shader at creation on the worker
thread and marks the one-piece ones (export akvr_vs_one_piece); AKN mk_route_begin routes ANY of them near a marker
(pause backdrop 3b819a7e still excluded). Fake-headset run MK1: 241 one-piece HUD shaders recognised, fd60f2d7 among
them; no marker on screen at that save, so routing itself is NOT yet proven - needs JJ at an objective (capmk / Save
capture logs MKPIECE). Status line: "markers: one-piece HUD shaders known N, pieces to the marker sheet/s N". Circle cut
kept for now for leftover batched pieces; remove it once the capture shows no marker piece is batched.
JJ 23:28 (MKSINGLE build, HEADSET): number on target but "small and hard to fuse"; "the border and the glow is at standard
HUD depth and floats around"; another HUD element broken at the start. capmk c232821 / c232839 (LIVE): marker at
(-0.092, 0.108); routed = fd60f2d7 piece d 0.012, number 91e2b222 d 0.055, AND fd60f2d7 pieces at (0, 0.178) d 0.11-0.12
(= the other broken element, pulled in by reach 0.16). Left on the HUD sheet (hud_rgb shows the bracket box + glow
without the number): batched 599bd060 / 05154232 / 4b432a87 at the marker.

### MKBATCH 2026-10-09 23:40 (route batched pieces one by one; no cut)
SOURCE: fxc of the dumps - 05154232 places piece k with cb0 rows 16 / 17 + 6k (index = v0.z UNORM x 1530.06 = 255 x 6);
4b432a87 with rows 12 / 13 + 4k (bfi of v3.x). AKVR `vslayout.h` follows the index arithmetic (mad / ishl / bfi / add /
utof / ftou / mov) to the position lines: STATIC test on all 56 dumps -> both layouts exact, nothing else mis-read.
Export akvr_vs_batch_layout. AKN: CB shadow now the whole buffer (<= 4 KB; Map during the HUD phase hands the game our
cached memory, Unmap forwards it - no slow read-back); for a batched draw near a marker: all pieces near -> whole draw to
the marker sheet; mixed -> SPLIT: two draws from our own cb0 copies, near pieces folded to a point in the HUD-sheet draw
and far pieces folded in the marker-sheet draw. Reach 0.16 -> 0.09. Status: "markers: HUD shaders known one-piece N
batched N | draws to the marker sheet/s N (split N), not routable/s N". capmk logs per batch "pieces near / far -> SPLIT".
Not fake-run: Arkham City was running (one Steam game at a time). Installed 23:38.

## TEST PLAN MKBATCH (JJ drives, one launch)
| # | what | proves | judge |
|---|---|---|---|
| 1 | gameplay, objective marker on screen, turn the camera | border + glow + number move together, on target | no part at HUD depth |
| 2 | look at the HUD elsewhere from the start | the 0.11 false route is gone | no broken element |
| 3 | panel Save capture at the marker (capmk) | MKPIECE: 599bd060 has a layout? SPLIT counts | log lines |
| 4 | walk close to the objective (near-range marker), Save capture | which shaders draw it | MKPIECE |
| 5 | marker size slider if the number is still small | g_mkScale | JJ |

### MKBATCH RESULTS 23:43 (JJ, HEADSET): still split; grapple reticle off its point and at the wrong depth
LIVE: every batched draw "layout 1, shadow 320 of 320 bytes -> hud sheet" = MY BUG: the game binds buffers holding only
the rows it uses (320 B), and the check wanted the shader's declared maximum (CB0[106] = 1696 B). Nothing was split.

## RETHINK MKTAG 2026-10-09 23:5x (JJ: "Seems we are just making it worse. Do you need to rethink your strategy?")
Looked up again: VR_WORLD_MARKERS_GUIDE §3.3 / §6 (pieces by position + reach; Batman City RETICLERUN had to add texture
fingerprints because position pulled in the location title), memory batman-city-hud-overlays (RETICLERUN), ak-markers-
face-the-eye (TARGETANCHOR / LISTMOVING: the HUD TREE knows the marker parts), earlyres PARTTAG / MARKREC (AKVR already
writes an invisible colour mark into a HUD-tree part, and every piece drawn from it carries the mark in its constants).
Why position failed: the HUD atlases are shared (one 2048x1984 sheet holds brackets and other HUD), batches mix pieces of
several elements, pieces of one marker spread 0.01-0.12 from its point, other HUD sits 0.11 away. Any reach is wrong for
some marker. Root fix: recognise a piece by WHAT it is. MKTAG: AKVR marks the "stays on its target" parts (L.world) with
kMkTag = -2/512 in all four colour adds (re-applied after the game's SetCxform, like PARTTAG); AKN routes a piece only if
its colour rows (single: all of cb0 but the placement rows 6/7; batch: each piece's own colour / uv rows, from the layout
reader) or the pixel shader's cb0 carry the mark; position only picks which marker (within 0.35). vslayout now reads all
six batched HUD shaders (05154232, 4b432a87, 54cd897e, 599bd060, 5c9618a3, 97657b85 = 3D 4x4 per piece, colour block at
row 112) - STATIC on the dumps. Fixed: pieces counted only inside the bound buffer.
Fake run 23:54 (sim): no crash, HUD sheet clean with the Map swap, "marks set 2"; no marker on screen (training popup).
Risk: 599bd060 (the glow) takes its colour from the VERTEX data, not cb0 - its pieces may carry no mark; the log will say.

## TEST PLAN MKTAG (JJ drives; I trigger through command.txt)
| # | step | command | proves / judge |
|---|---|---|---|
| 1 | distance marker in view, say ready | capmk t1 | MKPIECE: brackets / glow / number "tag vs N" or "tagged N" > 0, nothing else tagged |
| 2 | turn the camera | - | JJ: marker whole, on target, no other HUD broken |
| 3 | A/B | mkmode 0, then 1 | old position rule vs the mark |
| 4 | grapple reticle in view | capmk t2 | reticle pieces tagged? |
| 5 | near-range marker | capmk t3 | its pieces tagged? (has a point?) |
| 6 | if the glow is untagged | - | 599bd060 needs the vertex-colour route (next build) |

### MKTAG RESULTS 2026-10-10 00:0x (JJ, HEADSET + LIVE log)
HEADSET: distance marker STABLE (first time); number hard to read ("too close to my face"?); glow + border + text split
off and flash above it bigger now and then; compass flickers with a duplicated section near the marker when looking down.
LIVE: marks set 2; ~340 marked draws/s routed (number 91e2b222 tag vs 4-8, batches 05154232 / 54cd897e tagged per piece,
fd60f2d7 tagged) - the mark WORKS as identification. Causes found:
1. Flash GLOW = filter: marker drawn again into a 60x92 scratch target (599bd060 / c9b47e60), blurred, pasted back by
   c9b47e60 (mark at row 6, "other" shader = not routable -> glow stayed flat). The scratch draws themselves were rerouted
   into the marker sheet at the scratch place/size = "flashing above it at a bigger size".
2. Compass duplicate: (a) PS cb0 hits ("ps 3/4") came from the previous marker draw's leftover PS constants, marking whole
   batches drawn after it; (b) the circle-cut fallback on unrouted frames copied the compass area around the marker.
Fixes (installed 00:1x): MKSHEETONLY route only draws aimed at the HUD sheet; MKGLOW marked draws of other shaders go whole
to the marker sheet; MKPSOFF pixel-shader constants ignored; MKNOCUT2 (xr.cpp) no cut and no cards on unrouted frames with
the sheet on. Status adds "off-sheet draws skipped".

### MKSHEETONLY RESULTS 2026-10-10 00:16 (JJ, HEADSET): marker "rotated differently in each eye", cannot fuse
LIVE: routing clean (only marked pieces: number, batches 05154232 / 54cd897e, glow paste c9b47e60, fd60f2d7; no compass).
So the mismatch is in the per-eye CARD (MARKERWORLD corners), not the piece choice. JJ already called the number "small and
hard to fuse" with MARKERWORLD before MKTAG - likely there since MARKERWORLD. JJ's rule (same message): markers turn only
with the GAME camera, never the head = MARKERWORLD's intent. Eye order / VP checked in source: eye 0 = left in both.
No capture showed the HUD as the headset gets it -> EYESHOT: F2 / panel capture now saves eyes.bmp (both eyes with HUD +
cards, AKVR g_hieDst) and cards.txt (first card: W, camera axes, dR, h, per-eye centre + corners + VP). A/B: AKN command
"mkworld 0|1" (0 = the old flat per-eye square).
CAPSLIM: drive E FULL (0 GB) - 70 GB of akn\*.bmp: CAPDET (the detective vignette hunt) ran on EVERY capmk, up to 14 extra
45 MB pairs per press; JJ's 00:16 capture came out partly empty, and an install left dinput8.dll missing (restored). CAPDET
now only by command "capdet <tag>"; old pictures MOVED (not deleted) to D:\Documents\Antigravity\AKN_capture_archive\akn.

### EYESHOT RESULTS 2026-10-10 00:31 (JJ: "rotated and shifted differently", skew / stretch with head moves) -> CARDPERSP
cards.txt x3 (LIVE): W 28 m away; eye forward axes 13.9 deg apart (TURNEDEYE +-7 deg) - VPs right. Centres L -0.311 / R -0.633
and corners: R card 25 % wider + stronger keystone = CORRECT perspective for a 13 m card (h 600-700 uu, card radius 0.275 NDC)
seen 16 vs 32 deg off-axis in turned eyes. eyes.bmp (desktop mirror, 874x476) crops: the "55m" differs per eye in size,
slant and place inside the card. Cause: the corners went to the GPU as NDC with w = 1, so the card's picture was spread
over its two triangles in SCREEN space (affine) - a different trapezoid per eye bends the content differently, and it
changes with every head move. CARDPERSP: corners as clip coordinates (x w, y w, 0.5 w, w), perspective-correct UVs.
Note: eyes.bmp is AKVR's g_hieDst = the DESKTOP MIRROR copy (small), not the headset swapchain - fine for placement.

## AFW PLAN 2026-10-10 - alternate frame warping as an overlay choice beside native stereo (JJ)
JJ: "include an option for AFW rendering ... in the VR overlay, choose between native and AFW" - "not just standard AER,
alternate frame warping".

Looked up: playbook @ b955f224 - 14 #velocity-not-temporal (motion blur off is required; TAA can be fine), 14 "temporal
trap" (history per eye, ONE shared exposure, results deferred by a frame), STR-002 (the pair carries each eye's pose/lens),
STR-012 / STR-014 + 17 #carry-eye-identity (carry the eye with the frame, one clock for parity), STR-013 (one eye per
frame culls from that eye only: the other eye's outer edge is never drawn), STR-015 (per-eye history bank), 10 "AFR stereo
coherence", 17 sync-sequential note, 19 #composed-reprojection (SnowRunner: depth shift of the fresh eye, holes filled from
the stale eye's warp = AFW's combined mode), 19 DLSS ("alternate-eye fights temporal accumulators" -> own history per eye),
coverage.md PureDark-UEVR (AFW branch registered, not reviewed). vrframework guide 08 §6 (the game must run at twice the
per-eye rate), guide 07 §2. Own: memory aer-v2-and-yaw-folding + ak-inter-eye-baseline-measured (plain AER on AK's boom:
22-36 cm stale-eye baseline in spins), NOTES M3b T3 (shared view state = eye separation read as motion), DLSS D1 / CAMMV
(per-eye depth, camera motion vectors, per-eye DLSS feature), Stage 6a (shared exposure). Source read 2026-10-10:
github PureDark/UEVR branch AFW - dependencies/pd-afwmod/include/PDAFWPlugin.h, D3D12Component.cpp, VR.cpp diff.

What AFW is (SOURCE): every game frame draws ONE eye (alternating by frame parity) and still submits a FULL pair. The
other eye is SYNTHESISED, not stale. Modes: AlternateEyeWarping = this frame's drawn eye moved into the other eye's view
with its depth; PreviousFrameWarping = the other eye's last real picture brought forward with motion vectors; Combined =
the first, its holes filled from the second. Inputs per eye: colour, depth, motion vectors, source/destination camera
matrices; motion blur forced off; claimed +60-80 % frame rate; known flaw: volumetrics / see-through effects differ per eye.
The plugin is a closed D3D12 DLL (no licence to ship, AK is D3D11): we write our own D3D11 warp.
Why it is not AER: in modes 1 and 3 both eyes show THIS frame's simulation, so the boom baseline of plain AER never appears.

Design (AKN, live switch):
- `afw 0|1`, `afwmode 1|2|3` (1 this-frame warp, 2 previous-frame warp, 3 combined = default), `afwwin N` (hole search
  width), `afwlum 0|1` (exposure shared across the alternating views, default 1), `afwdbg 0|1|2` (holes painted / warped eye only).
- stereo_calc: no clone; the game's family draws eye e = AFW frame parity (one counter on the game thread, carried in the
  renderer tag, STR-012). Right-eye frames swap the player's view state to the right eye's own (vs 4). Both eyes' views
  come from one camera through the same eye-build code; the undrawn eye's view + lens are kept as the warp destination.
- DLSS: each eye's own feature + CAMMV previous camera (already per eye, now 2 frames back); jitter index = frame / 2 so
  each eye walks the whole sequence.
- After the eye's render + DLSS: copy into its pair half, keep it (+ a depth copy + unjittered view x lens) as that eye's
  last real picture, warp into the other half, publish the pair every frame with this frame's stamp.
- Warp (compute): pass 1 splats every source pixel's world point into the destination (nearest wins, InterlockedMax on
  inverted depth); pass 2 gathers colour from the source through the splatted depth; holes (no splat: disocclusion, cracks,
  the destination's outer edge) take the farthest neighbouring depth in a horizontal window, then mode 3 samples the
  destination eye's own last real picture through that depth (its camera one frame back), mode 1 stretches the source.
- Order-dependent code made parity-safe: frame start/end work (HUD phase, RAINPAIR, XEYE, RTWAIT, eye0Done) keyed on the
  frame's only render instead of "eye 1 starts / eye 0 ends"; RAINONCE needs no skip (one render per frame); FRAMESYNC /
  RANDSYNC / ONCETIME only act with a clone (unchanged).
- AKVR: overlay section "Stereo rendering": native (both eyes drawn) / AFW (one eye drawn, the other warped) + warp mode;
  saved as `afw=` / `afwmode=` in akvr_settings.ini, sent to AKN at start and on change; status: eye renders/s, pairs/s,
  warp GPU ms, hole %.

Ranked risks (each with its check):
1. See-through effects (rain, smoke, glass) have no depth -> sit on the background in the warped eye; real/warped swap
   every frame -> shimmer. Check: RAIN TEST panel, AFW vs native.
2. Disocclusion holes around Batman (nearest, largest parallax). Check: hole %, `afwdbg 1`, mode 3 vs 1.
3. Real/warped alternation per eye (view-dependent gloss). Check: JJ, still camera, head still.
4. Frame rate: a pair per frame needs the game at the headset rate. Check: frames/s native vs AFW, warp GPU ms.
5. Eye swap (STR-012). Check: `eyemark 1` (left red, right blue) + log parity counters.
6. Per-view-state modes (detective, VSSPLIT) on alternating frames. Check: detective on/off in AFW.

## TEST PLAN AFW1 (JJ drives, engine-stereo launcher, DLSS as set; I trigger through command.txt)
| # | command / switch | what it proves | how to judge |
|---|---|---|---|
| T0 | `afw 0` (control), 30 s still | native baseline | 1 s lines: frames/s, GPU ms |
| T1 | `afw 1`, `afwmode 1` | AFW runs; parity 1:1 | log: L/R renders alternate, pairs/s = frames/s, 0 faults; JJ: 3D, nothing swapped |
| T2 | `eyemark 1` 3 s, `eyemark 0` | halves reach the right eyes | JJ: red left, blue right |
| T3 | `afwdbg 1`, `afwmode 3`, `afwmode 2`, `afwdbg 0` | holes and how each mode fills them | hole %, JJ: edges around Batman |
| T4 | head turn / camera spin / run / glide | warp under motion | JJ: no doubling or tearing |
| T5 | rain panel, detective mode, map, pause | parity-sensitive paths | JJ: rain, right eye after detective, map |
| T6 | 5 min soak in AFW | stability | 0 faults, pairs steady |
| T7 | overlay switch AFW -> native -> AFW (crash-prone, last) | live switching | picture back each time |
Follow-ups ready: parity drift -> `afwlog 1` prints the eye queue; big holes -> `afwwin 64`; eyes differ in brightness ->
`afwlum 0/1`; warped eye looks shifted -> `afwdbg 2` (warped eye shown in both eyes) to judge the warp alone.

### AFW1 BUILD 2026-10-10 (STATIC: both DLLs built + deployed, game closed; not yet run)
- AKN `src/afw.cpp` (new): warp passes (splat at the drawn area's size, gather at headset size), own private context state,
  hole counts every 30 warps (no stall), graphics-card ms per warp. Shaders compiled offline OK (d3dcompiler_47, cs_5_0).
- AKN `probe.cpp`: `eye_build()` = the per-eye camera + lens code moved out of the view ctor unchanged (native output identical);
  AFW branch in stereo_calc (no clone; `g_afwNext` alternates; right eye = its own view state); renderer tag carries
  `afw` + the other eye's view/lens; render-thread frame start/end keyed on "first / last render of the frame" (native: right
  starts, left ends, as before); after the drawn eye's DLSS the warp fills the other half and a full pair is published with
  this frame's stamp; exposure copied from the drawn eye into the other view state (`afwlum`); jitter index frame/2 in AFW.
  Status line: "AFW on mode N | warps | ms per warp | holes ... | frames/s drawing left/right | pairs/s warped / flat".
- AKN `dlss.cpp`: `akn_dlss_depth / _srv / _render_size` accessors; `akn_dlss_reset` also forgets the previous camera.
- AKVR `hooks.cpp`: overlay section STEREO RENDERING (native / AFW, warp from: this frame / previous frame / combined, DLSS
  warning, AKN's AFW status line); `afw=` / `afwmode=` saved in akvr_settings.ini (read in the separate-check block: the
  else-if chain is at MSVC's nesting limit, C1061 again).
- Needs DLSS on (depth comes from the DLSS path); without it AFW shows the drawn eye in both (flat, never a stale eye).
- Backup of the previous game-folder DLLs + settings: akvr/diagnostics/before-AFW-20261010.

### AFW1 RESULTS 2026-10-10 01:17 (JJ, HEADSET + LIVE log, capture c011732) - "it works really well"
- LIVE: AFW mode 3, 90 pairs/s warped, left 45 / right 45 frames/s drawn, 0 flat pairs, warp 0.52-0.56 ms on the graphics
  card; holes: behind objects ~12.5 %, outer edge ~22 %, filled from the previous picture ~34 % (every 16th pixel sampled).
  Exposure copied every frame (90/s).
- HEADSET (JJ): vertical lines at the very left and very right, "from dark to slightly brighter".
- Capture c011732 (left eye = the warped one): a brightness step 46 -> 40 at x 560 of 2624 = exactly where the drawn
  (right) eye's picture ends; outside it the left eye's own previous picture fills the outer edge. Whole-eye means 41.5 /
  43.1. So the two sources differ for the same direction at that join (likely the drawn eye's darkened picture edge -
  the game's edge falloff - moved inward by the warp; exposure sharing ruled out: copies ran every frame).
- Also seen (JJ did not mention): thin dark outlines around the crane where background holes take the previous picture.
- FIX FEATHER (built + deployed 01:3x): in mode 3, warped pixels whose source point lies within 12 % of the drawn eye's
  left / right edge blend smoothly (smoothstep) into the other eye's own previous picture. Command `afwfeather N`
  (thousandths of the eye width, 0 = the old hard cut, default 120). `afwdbg 2` tints what came from the previous picture.

## JJ 2026-10-10 ~01:30: lens flares still there (c012642 / c012647) + full-screen cuts show a black 16:9 window
Looked up: NOTES LENSFLAREOFF (config LensFlares=False - had no effect, "different effect, needs a hunt"); HANDOFF 4b;
Luma AK mod (references/luma-framework-2026-10: main.cpp option DISABLE_LENS_FLARE_AND_LENS_DIRT, Shaders/Batman Arkham
Knight/LensFlare_0x2CCEAEA7.ps_5_0.hlsl + BloomLensFlareLensDirt_0xB42A7F40.cs_5_0.hlsl); playbook 14 class 5 (optical
post: disable); AKVR xr.cpp BACKEXT / BLACKKEY / WHEELEYE / dim_layer (VR_HUD_GUIDE: flat-screen backdrops).
- Captures: red rings + ghosts around the crane lights, much stronger in the right eye. HUD sheet c012642 = the training
  result panel on an all-black sheet = the game's full-screen cover lives on the HUD sheet (shown as a panel in VR).
- FLAREOFF (AKN, built + deployed): the game's flare sprite PS 0x2CCEAEA7 and bloom/flare-ghost/lens-dirt CS 0xB42A7F40
  (Luma names = CRC32 of the bytecode; AKVR's akvr_shader_crc computes the same) are swapped at bind time (PSSetShader /
  CSSetShader hooks, immediate + deferred tables) for Luma's own copies compiled with DISABLE_LENS_FLARE_AND_LENS_DIRT=1
  (src/flare_shaders.h, generated from Luma's files, credit kept). Built when the hooks go in. Command `flareoff 0|1`
  (default 1). Log: "FLAREOFF: the game's ... found" once per shader; 1 s line "lens flare off: binds replaced/s".
  Offline: both compile with the switch (ps_5_0 / cs_5_0).
- FULLCUT (AKVR xr.cpp, built + deployed): each new HUD frame 64 border texels of the sheet are read back (3 staging
  copies, DO_NOT_WAIT); an even dark cover on all four edges (alpha min > 0.02, spread < 0.08, colour < 0.10) = a
  full-screen fade -> its alpha darkens the whole view: into both eyes under the HUD (hie_dim_alpha) or via the dim layer.
  Pause look: only a cover >= 0.9 counts. Command `fullcut 0|1` (AKN forwards); 1 s line "full-screen cut now A".
  Untested: whether the training-end black is on the sheet's edges (expected from c012642) and the 2-3 frame delay.

### CRASH 2026-10-10 01:42 at the first main menu (JJ) -> FULLCUT2
- Windows: BatmanAK.exe fault in virtualdesktop-openxr.dll (0xc0000409 fail-fast, +0xd4f69). VD OpenXR.log 01:42:07:
  "xrCreateSwapchain: ovrResult failure [-7000]" (LibOVR runtime exception) right before it. AKN's last 1 s line 01:42:07.8:
  FLAREOFF running fine (shaders ready, 28 bloom binds replaced/s), cut 0.00, main menu 0, HUD not in the eyes yet.
- Cause (most likely, not proven): FULLCUT read the start screen's black HUD sheet as a full-screen fade; outside the
  HUD-in-eyes path it went through dim_layer, whose 16x16 swapchain is created lazily - its first creation in this setup
  (engine stereo normally darkens inside the eyes, so the dim layer never ran) is the only new xrCreateSwapchain, and VD
  failed on it, then crashed in its own code.
- FULLCUT2 (AKVR, built + deployed): dim_layer untouched by FULLCUT again; cuts darken the eye pictures only (hud_in_eyes =
  gameplay + pause look, where JJ's training fade happens); no probe in the main menu. LESSON: never make a code path
  create a NEW OpenXR swapchain mid-session as a side effect of a detection - Virtual Desktop can crash on it.

### CRASH 2 2026-10-10 02:12 at boot / first main menu (JJ: "still crashes on boot, nothing in the headset")
- Same chain: VD OpenXR.log 02:12:46 "xrCreateSwapchain: ovrResult failure [-7000]", then fail-fast, this time in
  VirtualDesktop.LibOVRRT64_1.dll. AKN: native (afw=0), main menu 1, DLSS pair 5248x2860 made 02:12:45, cut 0, HUD not in
  the eyes -> FULLCUT was NOT involved: the FULLCUT explanation of crash 1 was wrong.
- Event log: every session tonight before 01:42 ended in a different crash (unknown module +0x7fff448e628f, at exit -
  00:16, 00:31, 00:47, 01:18, 01:27); the VD -7000 class starts at 01:42, the first launch with FLAREOFF + FULLCUT. No
  nvlddmkm / display events. VD files unchanged since 2026-10-01.
- Two candidates: (a) FLAREOFF (new PSSetShader / CSSetShader hooks; VD's in-process device passes through them, though
  its shaders never match the flare CRCs), (b) VD's PC side broken after many crashed sessions (cf. memory
  vr-session-refused-pc-side: only a restart clears it).
- Built + deployed: SCLOG (AKVR: every swapchain request + result to akvr_captures\akvr_swapchains.txt, flushed),
  FLAREOFF2 (swap only on the game's device; akvr_settings.ini flareoff=0 switches it off at start).
- Next: JJ restarts Virtual Desktop Streamer (or the PC) and launches. Crash again -> read akvr_swapchains.txt, set
  flareoff=0, launch again (one launch decides between a and b).

### FULLCUT3 2026-10-10 02:3x (JJ: entering / leaving training still shows a black 16:9 box, not full-view black)
- Run 02:19-02:23 (no crash after JJ restarted VD): cut probe fired 0 times; AKVR flat frames only at start-up / first
  load (02:19), the training transitions (02:20-02:23) were all 3D with the HUD in the eyes -> the box is on the HUD
  sheet, but its OUTER border was not an even cover. Most likely the game's fade is letterboxed 16:9 inside the
  near-square sheet (2624x2860) - JJ: "looks like a standard 16 by nine window".
- JJ's capture c022203 came after the box (normal compass only) - no shape data.
- FULLCUT3 (built; installs itself when the game closes): a second ring of 64 texels on the border of a centred 16:9 band
  (full width, 3 % in from its top / bottom); either ring evenly dark -> whole view darkened. AKN's 1 s line now prints
  "full-screen cut probe: sheet edges a T B L R rgb | 16:9 band edges a T B L R rgb | cut" (strongest reading of the
  second), so the next transition shows the box's real shape even if the cut still misses.

### CRASHES 3+4 2026-10-10 02:30 (boot) + 02:32 (entering training) -> FLAREOFF3 (swap at creation), FULLCUT4
- 02:30:45 boot: render thread, AV read 0x10 at BatmanAK+0x800f (FUN_140007ff0, RHI create on a null object) under the
  scene octree insert 0x8467a4 ... RenderViewFamily 0x81b4bc <- AKN hk_RenderVF (outer wrapper only; the AKN value
  +0x34598 seen deeper on the stack is .rdata, not code). Game's own dialog "Rendering thread exception".
- 02:32:31 training entry: worker thread (0xbf4140 -> task 0xfa592b -> 0x811c8e -> FUN_14080f210), null entry in a per-view
  primitive list (RBP = [R13], test [RBP+0x40]) at 0x80f264. No AKN code on that thread.
- Pattern: the 5 sessions before FLAREOFF (00:16-01:27) all fine (only the old exit crash); 4 of 5 since crashed (two VD
  -7000, these two). FLAREOFF's bind hooks (PSSetShader / CSSetShader on immediate + deferred tables, every bind, every
  device incl. VD's) are the common factor - cause not proven, the hooks REMOVED.
- FLAREOFF3 (AKVR hudsplit.cpp, built + deployed): Luma's way - at the game's CreatePixelShader / CreateComputeShader
  (AKVR's existing creation hooks, CRC32 already computed there) the two flare shaders are created from Luma's source
  with DISABLE_LENS_FLARE_AND_LENS_DIRT=1 instead. Once at load, nothing per frame. akvr_settings.ini flareoff=0|1
  (default 1, next start). AKN's 1 s line prints "lens flare off 1: flare sprite shader replaced N x, bloom ... N x".
- FULLCUT4 (AKVR xr.cpp, built + deployed): the 02:32 log showed the box: sheet edges T 0 B 0 L .37 R .37, 16:9 band
  T .67 B 0 L .62 R .62 = full width, middle part of the height. New probe: sheet ring + 3 columns (left, centre, right)
  x 32 rows; a cut = middle row covered, >= 8 rows evenly dark across all three columns. Log line "full-width rows
  covered N of 32 (rows a-b, middle yes/no, alpha ..) | cut".

### JJ 2026-10-10 ~03:00 (FULLCUT4 run): "it fades to black, but you can still see the black window" + "entering
### gameplay starts off in 2D flat for a split second"; JJ: "or you could just get rid of the black box entirely"
- FULLCUT5 (AKVR xr.cpp, built + deployed 03:10): the cut is measured on the GPU in the same frame (hiecut PS -> 1x1 R32F,
  same rules: sheet ring or >= 8 full-width rows with the middle covered; pause look >= 0.9) - no 2-3 frame readback lag.
  The in-eye darkening adds it (1 - (1-dim)(1-cut)); every sheet shader (plain / key / marker-cut) draws its alpha as
  1 - (1-a)/(1-cut): the game's box adds nothing on top of the darkening (was: darkened twice inside the box = a visible
  window mid-fade), HUD pieces above it keep their look = JJ's "get rid of the black box". CPU probe kept for the log only.
  All 8 HUD-in-eyes shaders compiled offline (scratchpad hie_hlsl_check.py) before the build.
- BLACKGAP (AKVR xr.cpp): in gameplay, frames without an AKN pair (AKN's 30-steady-frame gate after a load hitch) show
  black for up to 180 frames instead of the flat picture; then flat again. Status: "black (waiting for 3D) N" in the
  AKN-link diag.

## JJ 2026-10-10 ~03:20: start-up - long wait on the title screen after the click; Batman centred then pops right;
## steep menu shots move the wrong way with lean / head moves ("we had this fixed in the geo-11 version")
Looked up: memory ak-menu-camera-gimbal-roll, ak-menu-roll-is-geometry (LEVELFRAME), measure-head-vs-camera-1to1 (YAWAXIS,
FREEZE), ak-geo11-res-ceiling-and-pitch (PITCHUNLINK / rigid = mode 2), ak-main-menu-real-3d (MENU3D),
ak-startup-audio-hold (AUDIOSYNC); camera.cpp vs the geo-11 release (git HEAD 19d752d): composition unchanged.
- Title wait (STATIC, startup log 03:13): "7.8s no frame 5312 ms" = the game stops presenting while it loads the menu
  (sound held meanwhile). Oct 2 measured 0.8 + 1.7 s. Not changed yet - open question whether AKN-mode start-up work
  (shader hashing at creation, DLSS) lengthens it or the HDD cache was cold (JJ restarted VD / the PC).
- POP (SOURCE): MENUSIDE (camera slid -25 uu so Batman sits right of the menu text) applied only once "MAIN MENU on",
  = 3 s after the menu scene starts. MENUSIDE5 (built + deployed): from the menu scene's first live frame
  (akvr_xr_menu_scene: phase 0 with the camera live, or phase 1), eased ~0.3 s.
- STEEP SHOTS (SOURCE): with pitchunlink=2 (JJ's) the head ROTATION is rigid in the camera's pitched frame (TIPPED), the
  LEAN stayed level (forward = level heading, up = world up) -> on a 60 deg-down shot leaning forward slides the camera
  horizontally while you look down = the scene moves against the head. LEANTIP (built + deployed): with TIPPED, the lean
  uses the camera's own forward / right / up, blended by camera pitch 40 -> 60 deg (YAWAXIS rule). At pitch <= 40 deg
  bit-identical to before; at pitch 0 the frame equals the level one exactly (checked). Not measured in the headset yet:
  if JJ still sees it, run the 1:1 test (F1 trace in the menu, regress lean vs camera motion per axis).

### JJ 03:2x: "better but not quite perfect" - lean forward slides a little left (level + down shots), one angle shifts up;
### title still slow. -> LEANREF, LEANTIP2 (AKVR), HOOKBATCH (AKN)
- MEASURED (trace 03:20, 8192 rows, regression of the written offset in the base camera's fwd/right/up on the head lean):
  steep shots (-75..-45 deg) exactly 1:1 rigid after LEANTIP (102 uu/m on the diagonal, ~0 cross terms); level shots 1:1
  level; the -45..-30 shot: lean fwd -> camera up +54/m (the 40-60 blend left it level) = JJ's "shifts up".
  The game renders what we write (live vs written <= 3 deg); lean does not turn the camera.
- Root of the left slide: raw head yaw + head yaw = -12.5 deg in EVERY row = the recentre heading. Rotation strips it
  (delta = Ry(-refYaw) * cur), the lean did not (raw room axes) -> a straight lean came out sin(12.5) = 22 % sideways.
  Applies to gameplay too whenever the recentre heading is not 0.
- LEANREF (camera.cpp): lean turned by the same Ry(-refYaw). LEANTIP2: lean rigid at every pitch with TIPPED (the
  rotation already is). Built + deployed 03:24.
- TITLE WAIT (LIVE akn_log 03:18:55-59): AKN's context hooks took 4.5 s on the render thread at the first 3D frame
  (each MH_EnableHook freezes + resumes every game thread; ~40 hooks). Startup log: "7.0s no frame 5485 ms".
  HOOKBATCH: all queued (MH_QueueEnableHook), one MH_ApplyQueued; same for the 12 start-up hooks (~1.5 s during the
  logos). Log lines "context hooks switched on in one batch ... ms" / "start-up hooks switched on in one batch".
- Seen, not acted on: AKN's crash recorder logs two first-chance AVs in DINPUT8.dll+0x11EF0 (read 0x20007D0) on the game
  thread at start-up (03:18:54.9 / 59.7), handled (the game went on) - check what AKVR probes there.

## PERF1 2026-10-10 (JJ: "we've lost about five to ten frames since we first got native stereo working ... removing
## tests, optimizing code ... stuff that shouldn't be happening or could happen better")
Looked up: VR_ESSENTIALS §4 (measure: frames/s, GPU load, same test convention), playbook 19 "three ledgers"; own logs.
- LIVE (akn_log 03:2x-03:33 gameplay, 51 s): 60-70 pairs/s with the graphics card 89-97 % busy at ~2820 MHz = GPU-bound;
  DLSS 3.63 ms per eye evaluate = ~7.3 ms of a ~15 ms frame. Eye output 2624x2860 (engineres 2860) = 1.34x the pixels
  of VD's recommended 2284x2448 - everything after the render (DLSS, HUD, copies) pays for that (JJ's choice to make).
- STATIC audit (subagent, both DLLs): the HISTSWAP detector (test, off) scanned a 768-entry list on EVERY SRV / UAV / RT
  bind in the eye renders; 1-3 locked AKVR shader-name lookups per draw (gloss + rain depth default on) on every worker
  thread; MIPBIAS took a lock per sampler bind; capture-only bookkeeping (note_big, RT list, QueryInterface + GetDesc)
  per RT bind / dispatch; locked counters on every recorded draw; FULLCUT's CPU readback now telemetry only.
- PERF1 fixes (built + deployed, no visible change intended): detector only with histswap / its one log; per-thread
  shader-name + CRC caches (renewed on load hitches); per-thread sampler-twin cache; capture bookkeeping only while a
  capture is wanted; dispatch UAV walk only when needed (AFW exposure / test / capture); plain counters; AKVR FULLCUT
  readback off ("cutlog 1"), the darkening pass discards where nothing is darkened.
- NEW TEST: AKN command `perfsweep` - 8 phases x 8 s (everything on / gloss off / mip bias off / rain depth off / HUD in
  the eyes off / markers off / far rain off / everything on), 2 s settle + 6 s measured, pairs/s + GPU load, then a
  "PERFSWEEP RESULT" table with ms per frame per feature.

## TEST PLAN PERF1 (JJ drives, one launch, ~3 min)
| # | step | what it proves | judge |
|---|---|---|---|
| T0 | gameplay, stand still in the city, one fixed view, head still | baseline after PERF1 | pairs/s vs 60-70 before (same spot type) |
| T1 | I write `perfsweep`; JJ keeps still ~65 s | each feature's GPU / CPU cost | PERFSWEEP RESULT ms per feature |
| T2 | (if DLSS dominates) JJ picks: engineres 2448 / DLSS model / mip bias | resolution / model trade | pairs/s next launch |

### PERF1 RESULTS 2026-10-11 01:19 (LIVE perfsweep, JJ standing still in the city, PERF1 build)
| phase | pairs/s | GPU busy | cost of the feature |
|---|---|---|---|
| everything on (start / end) | 60.7 / 60.0 | 86 / 85 % | base 60.3 = 16.6 ms |
| suit gloss off | 62.7 | 83 % | 0.62 ms |
| texture sharpness off | 61.2 | 84 % | 0.23 ms |
| rain depth off | 62.5 | 88 % | 0.57 ms |
| HUD in the eyes off | 69.2 | 97 % | 2.12 ms |
| markers into the eyes off | 60.7 | 85 % | 0.09 ms (but akvr_hud_target_points still ran) |
| far rain off | 62.5 | 86 % | 0.57 ms |
- HUD in the eyes is the big one, and it is NOT graphics work: with it on the graphics card is idle ~12 % more -> the
  render thread waits (processor-side time in AKVR's Present path, or a sync). Base GPU 85 % = not fully GPU-bound.
- Gloss / rain depth / far rain each ~0.6 ms: per-draw constant-buffer copies + shader lookups (processor), plausible.
- PERFTIME (built + deployed): AKVR measures per frame its whole VR submit and the HUD step (marker part / drawing
  part), AKN's 1 s line prints "VR frame (processor): submit X ms | HUD in the eyes Y ms (markers part, drawing part)".
- Next launch: perfsweep again + the PERFTIME lines -> fix whatever part of the HUD step holds the render thread.

### PERF2 2026-10-11 01:24 (LIVE perfsweep + PERFTIME, JJ standing still; JJ: "you better not ask me to do another test")
- Base 64.7 pairs/s, GPU 93 %. Feature costs: gloss 0.83 ms, texture sharpness 0.50, rain depth 0.46, HUD in the eyes
  1.11, markers 0.24, far rain ~0.
- PERFTIME: AKVR's whole VR submit 0.9-1.7 ms of processor time; the HUD step = almost all of it, and of that the MARKER
  part (0.72 ms, growing to 1.50 ms within the minute); drawing 0.02 ms.
- PERFMARK (AKVR earlyres.cpp, built + deployed): page_ok remembered ONE memory region per thread -> a VirtualQuery system
  call on nearly every HUD node read; now 64 regions per thread, 1 s each. The move history (node_moved_recently /
  node_moved_within) was a linear scan of up to 256 records per call, filling up over the session (the growth); now a
  1024-slot hash.
- PERFRING (AKN): gloss + rain depth wrote ONE copy buffer per eye that the previous draw was still reading (the graphics
  card serialised every patched draw); now 32 copy + rows buffers per eye in turn, content per draw unchanged.
- Texture sharpness 0.5 ms = the sharper sampling itself: kept.

### MKGLOW2/3 + MKSTICKY 2026-10-11 (JJ: distance marker "pulsing in depth and kept flickering out to a duplicate above it,
### particularly the glow effect under the text, but also occasionally the text"; captures c013428 / 442 / 450 / 504)
- HUD sheets (all 4): the marker text routed, a soft striped glow patch left on the sheet = the duplicate.
- MKPIECE (800 lines from the captures): the glow paste c9b47e60 "other ... tag vs 0 -> hud sheet" 20 / 20 (never marked);
  marker text pieces sometimes unmarked (fd60f2d7 single at d 0.073 "tag 0 -> hud sheet"; 05154232 batch "tagged 0 (ps 4)")
  = the pulse animation rewrites the colour the mark rides on -> pieces flip sheet / marker = flicker + depth pulsing.
  Telemetry: ~105 not-routable draws/s; 9 % of seconds with no matched marker.
- MKGLOW2: a marked piece drawn into a scratch target puts it in a set; blur passes reading a set target add theirs; any
  other draw into a set target removes it; a HUD-sheet draw reading a set target = the glow paste -> marker sheet whole.
  MKGLOW3: binding a scratch target clears g_hudBound, so mk_route_begin never saw scratch draws - mk_scratch_track runs
  for every HUD-phase non-sheet draw while markers exist.
- MKSTICKY: a piece shader seen marked in the last 0.5 s stays with the marker while within 0.09 (g_mkReach) of its point.
- Built + deployed; judge from JJ's next normal play (status line "c_mkGlow" not printed yet; MKPIECE via capture).
- JJ 01:5x: "it was also the text sometimes ... and the border around it". MKPIECE: fd60f2d7 (text) marked 9 / 65, the
  unmarked ones at (-0.005, 0.178) = 0.07-0.08 above the marker point (JJ's "duplicate above"); batches 05154232 /
  54cd897e (text / border) marked on some frames only. MKSTICKY2: hold reach 0.10 (border corners ~0.09; 0.11 once pulled
  a neighbour); status line "markers: glow pastes into 3D/s N, unmarked pieces held with the marker/s N" so JJ's normal
  play shows whether both fire. Open: ~9 % of seconds with no matched marker point (whole marker flat then).

### MKDRAWN + MKGLOW4 + MKSTICKY3 2026-10-11 (JJ: "better, but it still flickers a little ... jitters, like tiny little
### jitters ... the text still pops out for random frames")
- LIVE (log 01:54-01:56): glow pastes into 3D only 1-4/s (of ~65 frames) - Flash caches the filter result and pastes it
  for many frames without redrawing the scratch, and MKGLOW2's set was cleared every HUD frame; held pieces 20-106/s on
  marker seconds (the hold fires); the text shader is marked ~1 frame in 7, so 0.5 s could lapse.
- SOURCE: the tag is re-applied after every SetCxform (PARTTAG2), but the pulse adds brightness on the child -> the
  composed add != the mark on those frames: holding is the right tool, not re-marking.
- Jitter cause (reasoned, not measured): akvr_hud_target_points reads the Scaleform display list at Present while the game
  thread may already have advanced it -> the card's window up to a frame off the drawn marker when it moves on screen;
  the same race makes the 0.06 world-point match miss on some frames -> whole marker flat ("pops out").
- MKDRAWN (AKN + AKVR): AKN records the bounding box of this HUD frame's routed marker pieces (sheet clip units) and exports
  akn_marker_drawn; AKVR's point = drawn centre + the anchor offset (EMA 0.05 of tree point - drawn centre).
- MKGLOW4: the scratch set persists across frames (a target leaves only when something else is drawn into it).
- MKSTICKY3: hold 5 s (reach still 0.10).
- JJ 02:0x: "Could the jitter be the head pose delay? We had issues in the past with the head pose delay on markers being
  different from the world" (geo-11 era: FRAMEPAIR / posedelay 2 for rain + markers vs 3 for the world). SOURCE: yes in
  kind - the marker card's shape used akvr_camera_axes_ago(0) = the NEWEST head-free camera, the shown pair is built
  from its own frame's camera (engine delay 1); the world points (akn_markers) were handed newest-frame first.
- PAIRAXES (AKN + AKVR): the head-free axes used to build each frame's views travel with the renderer tag and are
  published with the pair (akn_pair_axes); AKVR shapes the card from them. PAIRPTS: akn_markers hands the points whose
  frame stamp equals the shown pair's first. Rule (STR-002 applied to markers): every number a marker card uses must
  come from the shown pair's own frame - centre (MKDRAWN), axes (PAIRAXES), world point (PAIRPTS), eye VP (akn_pair_vp).
- HEADSET 02:09 JJ (build 02:04): "The distance marker looks even worse now. It is less stable and jittering even more, and
  also detaching from its position". Cause (SOURCE): MKDRAWN replaced the card's HUD point by the drawn pieces'
  bounding-box centre + an averaged offset - and that point is ALSO the window cut from the HUD sheet (c.hx/hy), not only
  the match point. The routed pieces differ frame to frame (text marked ~1 frame in 7, glow pastes, border, held
  pieces), so the centre jumped and the cut window slid under the card. MKDRAWN OFF (kMkDrawnOn=false in xr.cpp, export
  kept). PAIRAXES / PAIRPTS / MKGLOW4 / MKSTICKY3 stay (axes only tilt the card; points only pick the pair's frame).
  Lesson: never derive a card's window from a set of pieces whose membership changes per frame.
- HEADSET 02:13 JJ (MKDRAWN off): "Still jitters a little bit. You'll see the RB icon doing it in the capture I just did,
  also" (capture 20261011_021354_188: no HUD on screen in it). SOURCE: the card's HUD window centre (c.hx/hy) was the
  HUD-tree point, read at Present while the game thread may already hold the NEXT frame's layout -> marker pixels slide
  inside the card by one frame of screen motion (only while the camera / marker moves on screen).
  MKSAMEFRAME (AKVR xr.cpp, g_mkSameFrame, on): window centre = this pair's Canvas.Project point (PAIRPTS, same frame as
  the HUD sheet) mapped to sheet units + the layout anchor offset (tree - projected), learnt per world point only while
  the projected point moves < 0.002 per frame (then both frames agree), EMA 0.2; a new widget layout (offset jump > 0.06
  while still) re-initialises. Status: "same-frame centre N, learnt N, new N" in the HUD-in-the-eyes line. Deployed 02:16.
- HEADSET 02:19 JJ (MKSAMEFRAME build): "Now I've noticed the grappling hook reticle pop out randomly also just for single
  frames" (captures 021848 / 021900: the 3D picture only, HUD not in them). LIVE 02:18-02:19: same-frame centre ~73/s,
  learnt most frames, new 2 - MKSAMEFRAME runs. SOURCE: a frame whose HUD-tree read returns no marker point (game thread
  mid-update) builds no cards, so with the marker sheet that frame draws nothing / flat pieces (same mechanism as JJ's
  earlier "the text still pops out for random frames"). MKCARRY (AKVR xr.cpp, g_mkCarryOn): a MKSAMEFRAME slot matched by
  the tree within 250 ms and missed this frame is carried from this pair's projected point + its learnt offset. Status
  "carried through a missed frame N" (= how often the old code dropped a marker). Deployed 02:21.

## FIRSTPERSON 2026-10-11 (JJ: "Add a first person mode like Arkham City and put it at the top of the overlay,
## double-clicking the gamepad sticks should toggle it"; AFWNAMES: "leave the native and AFW in the stereo rendering tab
## and just rename them the way they're named in the Arkham City mod")
Looked up: batman_city/BATMAN_CITY_PLAN.md FIRSTPERSON + TEST PLAN HEADCAM (steady eye point + smoothed, capped head-bone
offset, 25 cm in front of the face, double-click both sticks; its Looked-up line covers playbook CAM-001 / CAM-013 /
FAIL-CAM-020 / FAIL-HAND-047, REFramework FirstPerson, MELE VR convo_fp + SetOwnerNoSee, BS Infinite bones/hide); memory
batman-city-first-person; AK notes/memory: nothing on the pawn (only the camera, akvr camera.cpp); akvr/
21473484_..._BatmanAKUTC.CT (freecam table, player pointer from another build - not used).
STATIC (re/ak.py, BatmanAK.exe Steam 54,632,960 bytes; native-name table entries {char* "Class" "execName", fn}):
- execGetBoneLocation 0x95CBC0 -> USkeletalMeshComponent::GetBoneLocation 0x959020 (rcx mesh, rdx FVector* out, r8 FName
  by value 8 bytes, r9d space: 1 = component, else world via LocalToWorld at +0xB0; SkeletalMesh +0x280; MatchRefBone
  0x9457C0; a missing bone writes a ZERO vector at 0x95913B).
- FName(out, const TCHAR*, number) 0xF38AE0 (-> FName::Init 0xF37BF0 with add); "Bip01_Head" wide at 0x141D6AAD8, used by
  0x412BB0 (so it is a live bone name). Natives also there: execHideBoneByName 0x950FD0, execSetOwnerNoSee 0xDD3A20,
  execGetBoneNames 0x95C510, execGetBoneMatrix 0x9579A0, execGetPawnViewLocation 0xB42A30 (APawn vtable +0xA60).
- ULocalPlayer +0x5C = PlayerController (CalcSceneView 0x9F6220: first test; +0x478 = PlayerCamera, FOV +0x304);
  GetPlayerViewPoint 0x9E9ED0; execGetViewTarget 0xB41EA0 -> PlayerController vtable +0xA58.
- USkeletalMeshComponent ctor 0x0E9F10 -> vtable 0x1BDCBA0 (slot 0 = 0xE2E600); one subclass 0x1BDD310 (ctor 0x0EB1D0).
  A scan of .rdata for every vtable sharing >= 60 % of the 68 slots the class overrides vs its parent (0x1B81E10) found
  only these two (script subclasses reuse the native vtable).
Design (akvr camera.cpp FIRSTPERSON, AKN exports akn_local_player from hk_Calc on the per-frame caller): in smooth_cb (the
camera finalize, game thread, the game's fresh position before our deltas) -> view target = PC->GetViewTarget(); the
followed actor's skeleton = the first pointer field (0x40..0x2000, step 4) whose vtable is one of the two and whose
Bip01_Head lies within 4 m of its origin (cached; re-searched every 0.5 s while missing) -> eye = mesh origin (LocalToWorld
row +0xE0) + smoothed head offset (EMA tau "head smoothing", default 0.08 s, capped 250 uu per axis; "follows his head"
off = straight above the origin, 0.5 s) + forward 25 cm along the game camera's own heading + up. The base position is
replaced and its difference goes into g_smoothApplied so CAMRESTORE gives the game its own camera back. Shoulder / up /
forward framing offsets, menu side slide and stick lift are 0 while it is active. Menus / screen mode / no head (Batmobile,
cutscene cameras) = third person. Panel: FIRST PERSON at the very top (Batman City's rows); both sticks clicked twice
quickly toggles it (gamepad.cpp, <0.35 s each, within 0.6 s). Settings first_person / fp_forward_cm / fp_up_cm / fp_head
/ fp_head_smooth. Status each second in akn_log: "first person on/off, active | ... | head frames, not on foot, skeleton
searches | smoothed head x y z". Not done (as in Batman City's stage 1): hiding the head (SetOwnerNoSee needs a
ProcessEvent call), body following the player's head.
AFWNAMES: the STEREO RENDERING drawer now reads "3D rendering" / "Native (both eyes drawn every frame)" / "AFW (one eye
drawn per frame, the other warped)" / warp from "this frame | previous frame | both (best)" (Batman City afw.cpp).
STATIC: both DLLs built clean 02:27; install queued for when the game closes (backups *.before-fp.dll). No test asked of
JJ (standing rule): the status line shows whether the head is found when he turns it on.

### MKGLOW5 2026-10-11 (JJ 02:28: "the HUD element labeled fear on the left hand side is misplaced. It's now lower than it
### should be and it's skewed"; capture 20261011_022718_028)
HEADSET capture: eyes.bmp - FEAR moves between the eyes by the same amount as the "48m" card (0.13 vs 0.14 of the eye
width) and not like the HUD (TARGET DETAIL 0.19) = FEAR was drawn INSIDE the marker card (card perspective = the skew,
card window offset = lower). AKN hud_rgb_c022718: FEAR on the HUD sheet at the TARGET DETAIL height (a frame where it was
not routed). Cause (SOURCE): MKGLOW4 kept filter targets marked across frames; Flash reuses its filter targets for other
glowing HUD, so FEAR's glow paste read a still-marked target and went WHOLE into the marker sheet (the glow route has no
position test). Fix: a mark carries the HUD frame of the marker drawing it came from (tagged draw = this frame, copies
inherit); a paste routes only within 30 HUD frames; ClearRenderTargetView of a target unmarks it; targets are tracked
also while no marker is on screen (their redraws unmark them). Not the cause: the HUD-part move history (PERFMARK) only
feeds target points of the marker parts; MKSTICKY needs 0.10 reach (FEAR was >= 0.22 away).
Deployed 03:18 (XINPUT9_1_0.dll). If the marker glow duplicate returns, the 30-frame window is the knob.

## RETHINK MARKERS 2 - the geo-11 model (JJ 2026-10-11: "This all feels very hacky. Are you sure you're making these
## fixes in the best way possible?" ... "look at how we did it in the Geo11 version because they were rock solid on that
## version" ... "They were at object distance, not at a fixed depth")
Looked up again: VR_WORLD_MARKERS_GUIDE (all; Batman City MARKEREYE / MARKERBASE / MARKERCARD - pieces moved per eye
INSIDE their own draw via cb0, never cut out); VR_HUD_GUIDE; playbook @ b955f224 04-ui-and-hud "#single-native-hud-draw"
(redirect one top-level target, Scaleform filter targets stay native), "Three ways to place a HUD in stereo" (never mix
silently), "#ui-not-one-class" (classify by geometry/ownership, not by shader), FAIL-HUD-006 (reticle must be at the ray
hit), FAIL-HUD-014 (redirect captures Scaleform's private filter passes); memory ak-markers-face-the-eye (TARGETANCHOR /
LISTMOVING / ANCHORLOST), ak-reticle-band-and-number-depth (TARGETDEPTH, TARGETSTOCK), ak-hud-depth-decision-all-13
(the fix's 13 HUD VS march scene depth at each piece's own origin), ak-one-frame-thread-lag-rain-hud (THREADSYNC:
OneFrameThreadLag=False made the reticle and distance marker "perfectly stable ... locked to their targets"; still False
in BmSystemSettings.ini), akn-marker-pieces-by-tag, root-cause-over-patches.

How the geo-11 version did it (SOURCE + memory): every HUD piece stayed IN THE EYE PICTURE, drawn by the game for the
camera it rendered with (head included), so its 2D place was the game's own and always the same frame as the world.
geo-11 drew each HUD draw once per eye; the fix's HUD vertex shaders gave each piece a per-eye shift from the scene depth
at the piece's own origin (object distance; TARGETDEPTH: pieces near a marker point share that point's depth). Only the
parts positively identified as room HUD (whole HUD-tree branches, ROOMALL / PARTTAG colour mark) went to the room layer.
Failure mode: a misjudged piece got a slightly wrong DEPTH, never a wrong PLACE.
How AKN does it now: the whole HUD goes to a room-fixed sheet; marker pieces are guessed out of it (tag, reach, sticky
hold, glow-scratch tracking) into a second sheet, cut out by windows centred on HUD-tree points read at Present (another
moment than the draw), and re-placed as cards. Failure mode: a misjudged piece (or a missed tree read) moves to a wrong
PLACE or vanishes = every symptom of the last two days (pop-outs, FEAR in the card, jitter, detaching). The patches
(MKGLOW2-5, MKSTICKY2-3, MKDRAWN, PAIRAXES/PTS, MKSAMEFRAME, MKCARRY, MKHOLD) all treat consequences of that design.

Plan (structural, replaces cards):
1. Polarity of the geo-11 version: HUD draws go into the EYE pictures by default; only branches the HUD tree marks as room
   HUD (the existing ROOMALL / PARTTAG mark, whole branches, inherited by every piece) go to the room sheet.
2. The game lays out the markers for the eye it renders (Canvas.Project with that eye's own view - MARKERBASE off), so in
   that eye every piece is already exactly where the game drew it, same frame (OneFrameThreadLag False).
3. The other eye: each eye-picture HUD draw is replayed once more into the other eye's picture with its pieces moved by
   the disparity of their object: the world point W of the nearest marker point (Canvas.Project, same HUD frame) when one
   is in reach, else the scene depth at the piece's origin (geo-11's own rule; Batman City AIMDEPTH style compute march).
   A piece judged wrongly only lands at a slightly wrong depth.
4. Glow / filter pastes: Scaleform's private filter passes stay native (playbook #single-native-hud-draw); the paste draw
   is an ordinary eye-picture draw and gets the same replay with the disparity of its own position.
5. Afterwards (separate step, JJ's rule): the card mapping (VR_WORLD_MARKERS_GUIDE §5) so markers turn only with the game
   camera - applied per piece in the same replay, not as a cut-out.
6. Remove: cards, cut windows, tree points at Present for placement, MKSAMEFRAME / MKCARRY / MKDRAWN / MKSTICKY / MKGLOW
   scratch tracking, MKHOLD. Keep the old path behind one switch until the new one is confirmed.
Open questions to settle from code / static data before building: the HUD sheet vs eye output sizes and the lens
(TURNEDEYE) mapping between the game's HUD frame and each eye picture; AFW (the rendered eye alternates); the paste
shader's placement rows (c9b47e60); which HUD-tree branches ROOMALL marks today (FEAR must be room, markers eye).
No test launch is needed for those; the first headset look is JJ's normal play.

### MKEYE BUILD 2026-10-11 03:41 (STATIC: both DLLs built clean and installed; backups *.before-mkeye.dll; not run)
AKN probe.cpp: g_mkEye (default 1, command "mkeye 0|1", panel checkbox in STEREO RENDERING). Two eye marker layers
(g_mkEyeTex[2], HUD-sheet size, cleared with it). mk_eye_frame() at each HUD frame start: the Canvas.Project points of
the pair's frame (stamp == g_pairStamp), the pair's eye view x lens (g_mcPubVP) and the head-free view x game lens the
HUD was laid out with (g_mcPubBVP, carried in the render tag beside PAIRAXES), AKVR's HUD-box map (akvr_hud_layout_map
= TARGETSCALE xf). Per on-screen marker and eye: F = game-screen point on the card through W (facing the game camera,
inverse head-free view x lens at W's depth) -> eye screen; J by finite differences (0.01); sheet->eye map
A = J diag(1+xf0, 1+xf1), t = F(R) + J(xf.zw - R) (VR_WORLD_MARKERS_GUIDE §5). g_mkC = the markers' sheet points (the
piece routing's "which marker", same frame). Routing decisions unchanged (MKTAG + sticky + glow provenance); in MKEYE
mode a routed draw returns 3 -> mk_eye_exec: one-piece rows 6/7 and batch pieces' X/Y rows (+ their w row) through the
map per eye into eye layer e (other batch pieces folded; the HUD-sheet copy keeps them); a whole draw (glow paste, or a
one-piece draw whose cb0 is only partly known) moved per eye by the viewport (a00, a11, t; no shear); scissor opened for
the moved copies. Status line "MKEYE: marker pieces drawn per eye/s, glow pastes per eye/s | HUD frames with markers
placed (markers), no layout map, no pair view".
AKVR xr.cpp: akvr_hud_layout_map export (last non-zero txf); in MKEYE mode no cards (nm = 0, nCuts = 0); in each eye's
pass after the room HUD sheet: AKN's eye layer e as a full-eye quad (g_hieVS / g_hiePS, ONE / INV_SRC_ALPHA). The HUD is
laid in before SVRSTRAIGHT's rewarp, so SteamVR gets it too. HUD diag: "markers per eye (MKEYE) on, layers drawn N,
layout map known".
Fallback kept: a marker with no world point this frame (or before the layout map is known) leaves its pieces on the HUD
sheet, as before. Old path: "mkeye 0" or the panel box (cards come back).
What to read in akn_log during JJ's normal play: the MKEYE line (pieces per eye > 0, markers placed, no-map / no-view 0);
"HUD in the eyes: ... markers per eye (MKEYE) on, layers drawn rising".

### MKEYE RESULTS 2026-10-11 ~03:55 (JJ, HEADSET) -> MKEYE2
JJ: "the distance markers box is attached to the object, but it's jittering a bit and it rotates on its own axes when you
move your head around ... The actual text for the distance marker is not inside the box. It floats around the area of the
box when you move the camera around, but it doesn't jitter".
LIVE: MKEYE ran at full rate (1050-1630 pieces per eye/s, glow pastes 20-70/s, 1 marker placed every HUD frame, no map /
view failures). Captures c035429 / c035436: the "52m" text is ON the HUD sheet, ~0.03 above the marker point; MKPIECE
shows the batch 05154232 (1024 B, 8 pieces, first at the text's place) TAGGED and routed - so its eye draw fell back.
Cause (SOURCE): mk_sub_buffer kept FOUR buffer sizes for the whole session and returned nothing for a fifth -> the
draw went to the HUD sheet unmoved = the text floating (room sheet: steady, no jitter). Second: the glow paste (VS
c9b47e60) was moved by the viewport (scale + shift only) = a sticker that turns with the head; its disassembly shows the
placement in cb0 rows 8/9 (w = 1).
MKEYE2 (AKN, 04:01, installed): buffers in 5 size buckets x 3 slots x a ring of 8 (never full); the glow paste through its
rows 8/9 with the full card map; status "MKEYE2: draws moved per eye/s - one piece, batch, by viewport, left unmoved |
first marker: layout point, head-free W, per-eye W and map" (F(R) vs W check for the jitter).
- MKLAG (AKN 04:05, installed with MKEYE2). JJ: "The cause of the jitter previously with the Geo11 version was the head pose
  delay which needed to be at 2 whereas the game was at three" (memory ak-one-frame-thread-lag-rain-hud). Order checked
  (SOURCE): the pair's eye views are published in after_eye_render BEFORE the HUD phase's first draw, so the eye side is
  the same frame. The open frame question is the LAYOUT: the game thread may run frame N+1 (Canvas.Project) while the
  render thread draws frame N's HUD. Now measured in play: per HUD frame the first marker-0 piece's sheet position against
  the marker point of the pair's kept frame and of the other kept frame; over 90 frames with the marker moving on screen,
  the steadier offset wins (auto, 30 % margin; "mklag 0|1|2", logged as "MKLAG: piece offset change per moving frame ...").
  Each eye's anchor is now that eye's own projection of W (exact); the layout offsets are taken about the chosen frame's
  point; J (card turn) from the head-free view about the pair's point.

### MKEYE2 + MKLAG RESULTS 2026-10-11 ~04:10 (JJ, HEADSET) -> MKANCHOR
JJ: "It's stable when holding completely still, but when rotating your head, both the text and the box it's in rotate
on their own axes to match the head movement, and when moving the camera, it jitters".
LIVE: text now routed (no unmoved draws; one-piece ~300/s, batch ~135/s). Capture 040941 eyes.bmp: "52m" exactly on
its object in BOTH eyes (left W 0.360/0.026, right 0.079/0.029 = drawn places). Layout point constant while the eye
points move (head-free base confirmed). MKLAG: offset change per moving frame 0.0058-0.0125 for BOTH candidate frames
(no candidate steady) -> the game's widget eases / lags by part of a frame, it does not follow the projected point
rigidly = the camera-move jitter (anchor exact at W, pieces carry the widget's easing).
Head-turn rotation: NOT explained yet - the math says world-fixed (J from the head-free card into each eye's own view, A
diag 1.9-2.3 varying with the eye point = perspective). Possibilities to settle with data: the game's layout itself
moves / reshapes with the head (its widget may use the head-included camera for tilt or easing).
MKANCHOR (AKN 04:17, installed): each marker's pieces placed about the frame's OWN layout anchor = first piece drawn
for it minus that piece's offset from the marker point, learnt while the point is still (EMA 0.1, relearn on a jump >
0.05, frame rejected if > 0.06 off); T.t = W's eye point - A G. Measures per second "while the marker point stands still:
the first piece moves X and reshapes Y per frame" (head turns with a still marker point -> non-zero = the game's layout
follows the head). Command "mkanchor 0|1".

### MKANCHOR RESULTS 2026-10-11 ~04:19 (JJ, HEADSET): "That's even worse ... this is a disgrace. We solved this issue with
### GeO11 a long time ago" -> MKEYE / MKANCHOR default OFF (04:24 build, installed): the old card path is back by default
LIVE (04:18): the game's layout does NOT move or reshape while the marker point stands still and the head moves (first
piece 0.00001 / 0.00000 per frame) - the layout is head-free. Captures 041837-041846: "52m" + box on the object in both
eyes, shape plausible. So every stage checks out on paper, yet JJ sees head-turn rotation and camera-move jitter =
something not visible in single captures or 1 s log samples. RULE from now on for markers: no build reaches JJ before a
fake-headset run that turns the simulated head (xrsim "head rot y p r") and moves the camera, measuring the marker
against world features in the eye pictures frame by frame (VR_WORLD_MARKERS_GUIDE §7). Suspects to measure there: the
per-frame J (card turn) under head yaw/pitch vs the world's change; the composite onto the pair (frame match under motion).

## TEST PLAN MKREC (JJ runs it himself, 2026-10-11 ~04:30; JJ: "No, I will do it")
Build 04:29 (installed): panel STEREO RENDERING -> "Record markers (20 s)": AKN writes akn\mkrec_<time>.csv (per HUD frame,
marker 0, per eye: W's eye point, the map A / t, picture angles of world up / head-free camera up + right through W and of
the marker's own up / right = A applied to the layout axes, the previous frame's first piece relative to W's point);
AKVR saves akvr_captures\mkrec_<time>\eyes_01..10.bmp (pair + HUD + marker layers, 2048 px) every 2 s.
| # | JJ does (marker in view, new method ticked) | proves | judge |
|---|---|---|---|
| 1 | 0-5 s hold still | baseline | angles steady, piece offset constant |
| 2 | 5-10 s turn head left / right slowly | head-turn rotation | L/R_mkUp vs L/R_camUp + worldUp track each other; piece offset constant |
| 3 | 10-15 s tilt head side to side | head roll | mkUp follows camUp/worldUp (world-upright) |
| 4 | 15-20 s right stick only | camera-move jitter | piece offset (pieceDx/Dy) frame to frame |
Then untick (old cards), optional second recording for pictures. Analysis: if mkUp tracks camUp/worldUp and piece offsets
are steady, the placement math is right and the fault is downstream (composite / display) -> compare eyes_XX pictures.

### MKREC RESULTS 2026-10-11 04:54 (JJ ran it; akn\mkrec_045423.csv, 2024 frames; pictures akvr_captures\mkrec_044649)
LIVE, per phase (left eye; right the same):
- marker up vs head-free camera up: within +-4 deg in every phase (the map did what it was built to do);
- head-free camera up vs WORLD up: up to 24-28 deg apart while JJ tilted his head = the "head-free" game camera
  INHERITS the head roll the mod writes into the game camera (CAMRESTORE gives back position only). The marker rolled
  with the head = JJ's "rotate on their own axes to match the head movement";
- head turns: marker up vs world up jumped 1.11 deg rms per frame while the camera's up moved 0.02 deg = the card map
  from that camera added rotation noise (the wobble / jitter);
- stick phase: smooth (piece offset drift, no frame-lag signature); MKLAG never found a steadier frame.
MKFACE (AKN 04:59, installed; MKEYE still default OFF until JJ ticks it, which also records): the geo-11 rule (TARGETFACE
+ TARGETUP): card at W facing the viewer's line of sight (from the mid-eye point, eye centres solved from each eye's view
x lens), up = WORLD up, right = the game view's right (sign only), size = the game's layout scale at W's base depth
(clip w / game lens P00, P11). No game-camera orientation in the map any more. Offline check (python, synthetic 20 deg
head roll with a rolled base camera): marker up 69.99/70.01 deg vs world up 70.00 in both eyes. MKLAG default 0.
Lesson (memory): the "head-free" game camera is NOT roll-free in AKVR - never orient anything from it; use world up.

### MKFACE RESULTS 2026-10-11 05:04 (JJ: "Much better. The only problem is they're still rotating with head rotations. They
### should only rotate with camera rotations"; akn\mkrec_050406.csv) -> MKFACE2
LIVE: marker up = world up within +-0.05 deg in all four phases (roll fixed). The layout point R stays constant while the
head turns (5.9-10 s) = the game camera's AIM is head-free; only its roll leaked. Remaining head-turn turning = the card
faced the viewer's EYES (moves with the head). MKFACE2 (AKN 05:06, installed): the card faces the game camera's
direction (-base forward), up = world up, right = the game view's right (sign). Offline check: head yaw 30 / roll 15 and
yaw -25 / roll -10 -> marker up = world up exactly in both eyes. Command "mkfacecam 0|1" (0 = face the eyes, MKFACE).

### MKFACE2 RESULTS 2026-10-11 13:12 (JJ: "still rotating ... when rotating my head" / "It's subtle but they are rotating";
### akn\mkrec_131221.csv) -> MKUPRIGHT + MKEYE default ON
LIVE: marker up vs world up 0.2-0.9 deg, piece offsets ~1 px/frame, R constant on head turns. Pictures: box parallel to the
door behind it. Also: after each recording JJ unticked the box (my instruction) = back to the OLD cards, which do turn with
the head -> MKEYE is the default again (13:15). Cause of the subtle turn (offline, python): the card faced the camera's FULL
direction, so it leaned back (camera looks down at Batman); a leaning card seen obliquely drifts up to 3.4 deg off the world
vertical over head turns of +-40 deg. MKUPRIGHT (13:16, installed): card normal = the camera's HEADING (horizontal), up =
exactly world up -> 0.0 deg in the same test. Falls back to the full direction only when the camera looks almost straight
up / down.

### MKUPRIGHT RESULT 2026-10-11 ~13:18 (JJ: "it's still subtly rotating on x and y axis when turning my head left and right or
### up and down") -> MKCARD
Cause (SOURCE): the per-eye move inside the piece draws is AFFINE - Scaleform's 2D vertex shaders write w = 1, so no
perspective (homography) is possible in their draws. A card off the view centre needs a taper (near edge taller); without
it the eye reads a card swinging on its x / y axis, growing as head turns move the marker off-centre (estimate: 30 deg off
axis, 5 deg card -> ~13 % edge-to-edge taper missing).
MKCARD (AKN + AKVR 13:21, installed; default on): pieces routed into the flat marker sheet in their own draws (MARKSHEET
routing, centres = this frame's marker points); AKN exports each marker as a perspective card per eye (akn_marker_cards:
upright at W, facing the camera's heading, world up, the game's layout scale, sheet window = this frame's point +- g_mkCardR,
x g_mkScale); AKVR draws them with the CARDPERSP path (clip w = perspective-correct texture = exact taper). Per-eye
layers off in this mode. "mkcard 0" = MKEYE moves (affine).

### MKCARD RESULT 2026-10-11 ~13:23 (JJ: "Although it's not rotating now, the elements have split off from each other and the
### size is changed ... I don't like this hacky method ... It should be fixed at the source"; capture 132345: text small, box
### corners spread) -> MKCARD off, MKGS
MKGS (AKN + AKVR 13:29, installed; default on): the geo-11 way - per-vertex 3D placement INSIDE the game's own HUD draw.
AKVR (hudsplit): every small VS the game creates keeps its bytecode; on first use a geometry shader is generated from its
output signature (DXBC OSGN, register order, no packing) and compiled (gs_5_0): each vertex's HUD position (x, y) becomes
K0 + x Kx + y Ky from cb13, z = 0.5 w. Offline (python, the game's dumped HUD VS): generated + compiled OK for 05154232,
54cd897e, 91e2b222, c9b47e60, 9938094a, fd60f2d7, 5c9618a3, 94f88871, 97657b85.
AKN: per marker and eye, K = the linear map sheet (x, y) -> layout (s x + b) -> point on the upright card at W (camera
heading, world up, the game's layout scale at W) -> that eye's clip (view x lens): exact perspective per vertex. A routed
piece / glow paste is drawn per eye into the eye layers with the GS bound (batches: per marker, other pieces folded); the
game's own constants are used (no row edits) - nothing cut, nothing resized. Status "MKGS: draws placed per vertex in 3D per
eye/s, without a geometry shader (row fallback)/s | built, failed". "mkgs 0" = the old affine rows.

### MKGS RESULT + MKGS2 2026-10-11 ~13:33 (JJ: markers "attached ... track very well ... no visible jitter, but they are still
### rotating on their own axes slightly when moving my head left and right, up and down")
LIVE (akn_log 13:31): "MKGS: ... placed per vertex in 3D per eye/s 0, without a geometry shader (row fallback)/s 361-503 |
geometry shaders built 0, failed 0" - MKGS NEVER RAN; JJ saw the affine MKEYE rows again (= MKUPRIGHT's known subtle swing).
Cause (SOURCE): AKVR's mkgs table had 2048 fixed slots and stopped storing once full; AK creates thousands of world vertex
shaders before the HUD's, so the HUD shaders were never stored and every lookup missed (counted nowhere).
MKGS2 (AKVR + AKN 13:36, installed; backups akvr_fix_backup\*.before-mkgs2.dll): unbounded map keyed by the shader object,
the GS source generated at creation (nothing kept for unsupported signatures); new export akvr_marker_gs_stats; the MKGS log
line adds "MKGS2: vertex shaders known N (unsupported N), lookups not found N | first error: ...".
Read on JJ's next play: "placed per vertex in 3D per eye/s" > 0 and "row fallback" ~0; "built" a handful (the HUD's VS),
"lookups not found" 0. If lookups are still not found: the HUD shaders are created before AKVR's hook (then the hook must go
in earlier). JJ judges: markers turn only with stick turns, same size and shape as now.

### SCREENNEAR 2026-10-11 13:39 (JJ: "move the title screens and the loading screens a bit closer to the camera" ... "not
### parallax, just a bit closer") - AKVR, installed with MKGS2
Start-up / title (SPLASHSIZE latch) and loading (LOADSIZE latch) screens: both eyes showed the same picture through the same
window = read as infinitely far, while their HUD quad (loading tips, g_hudOnScreen) hung at huddist 4 m. Now each eye's
window is shifted inward by tan = half eye distance / D (left eye to the right), so the screen reads D away at the same
angular size; the on-screen HUD quad uses the same D. Setting screendist (default 3.0 m, 1-20), panel slider "start-up /
loading screens distance (metres)" beside the loading-screen sliders. Pause / map / main menu unchanged. STATIC only.

### MKGS2 RESULT 2026-10-11 ~13:41 (JJ, HEADSET): "I think you did it with the HUD markers. They look solid and not rotating
### now. From what I can tell, they're perfect." - SOLVED
LIVE (akn_log 13:41): "placed per vertex in 3D per eye/s 224-950, row fallback 0 | geometry shaders built 7, failed 0 |
vertex shaders known 7590 (unsupported 150), lookups not found 0" (the game creates ~7,740 vertex shaders; the old table
held 2048). Method written up for every game: VR_WORLD_MARKERS_GUIDE.md §5a (the day's causes) + §5b (the recipe);
VR_ESSENTIALS.md §6.

### SCREENNEAR RESULT + SCREENROOM 2026-10-11 ~13:45 (JJ, HEADSET: "they appear closer but when I move left and right they
### seem to shift with me, making it feel more like they are still at infinity, which is weird")
Cause: SCREENNEAR gave stereo depth only (each eye's window turned inward); the layer was still a picture at infinity, so
the runtime reprojected rotation only and head moves dragged it along = depth cue and motion cue disagree. Lesson: never
fake a distance with disparity alone; a screen at D must be drawn from each eye's position.
SCREENROOM (AKVR 13:46, installed; engine stereo, SBSONE): the flat picture is drawn by us into each half of the
side-by-side image as a panel fixed in the room (g_fixedPose, screendist ahead, the screen's own window tangents and crop),
from each eye's place at the latest head pose (g_curPose +- half the eye distance) through the runtime's own straight lens;
black around it; the views loop declares exactly those views (pose + raw lens + full half). The disparity shift is gone.
The HUD quad on those screens hangs at the same distance. Uses last frame's screen kind / window (one frame late on entry).
mode_log "SCREENROOM: start-up / loading screen drawn as a room panel" on first use. STATIC only (built, installed).
- SCREENROOM RESULT 13:47 (JJ: "they don't feel close, they still feel far away"): NEVER RAN - no "SCREENROOM" line in
  akvr_startup_log; JJ's setup is "one swapchain per eye" (g_sbsActive false) and the build required the shared one. The
  removed SCREENNEAR shift left plain infinity. SCREENROOM2 (13:48, installed): works with one swapchain per eye too
  (each eye copies its own half on a room screen; views from offset 0). Check the startup log's mode line for
  "SCREENROOM: start-up / loading screen drawn as a room panel" after JJ's next look.
- SCREENROOM2 RESULT ~13:50 (JJ: "they are locked to 30 frames per second and jitter really badly when I move my head";
  log: "SCREENROOM ... drawn as a room panel" = it ran). Cause: the game presents ~30/s on these screens; the panel lived in
  the eye pictures, refreshed only per game picture; the runtime's rotation-only reprojection is exact for infinity (the
  old screen) but not for a panel 3 m away under head translation. Lesson: anything room-fixed and NEAR that must be smooth
  while the game is slow belongs in a compositor layer, not in the eye pictures.
  SCREENQUAD (AKVR 13:52, installed): the picture goes into its own headset image (2048 x 2048, format of the eye images,
  created ONCE in create_swapchain beside the first eye images - logged "screen panel" in akvr_swapchains.txt; sRGB image:
  the picture is decoded in the shader and the view encodes it back) and is submitted as a room-fixed quad (g_fixedPose,
  screendist ahead, the screen window's size) - per-eye pair under VD (hud_eye_pair = depth), single elsewhere - in the
  normal and the FPSLOCK repeat frames. Eye pictures black under it. Fallback (no image): SCREENROOM in-eye drawing.
  mode_log "SCREENQUAD: ... room-fixed headset panel". STATIC only.
- SCREENQUAD RESULT ~13:55 (JJ: "a bit too close ... double its distance"; loading screen "still 30 frames per second";
  title "looked better, but moving side to side was still weird. I didn't feel like it was at the distance"). LIVE: title
  used the panel ("SCREENQUAD ... room-fixed headset panel"), "screen panel" image created at start; the loading screen's
  path was not logged. Likely cause of "too close / not at the distance": the per-eye quad PAIR doubles the depth - VD
  already draws a quad per eye (PANELONE 2026-10-08: the per-eye panel pair read too close; single quad = right). That
  contradicts memory vd-quad-layers-no-parallax (2026-09-28): to be settled by JJ's next look.
  SCREENQUAD2 (AKVR 13:57, installed): ONE screen quad (and the on-screen HUD one quad too, main + repeat frames);
  screendist 6 m (default and JJ's ini, he asked to double it); on a start-up / loading screen the window picture wins over
  any AKN pair; mode_log on every change: "screen path: headset panel / in the eyes / black gap / FLAT (not placed) / none",
  "screen: 3D pairs arrive". Read akvr_startup_log's mode line after JJ's next loading screen. If the path is "headset
  panel" and it still looks 30 fps, the loading picture's own animation is 30 fps (the game's rate).

## TEST PLAN SCR30 2026-10-11 (JJ chose "fix it in the game": the title and loading screens run at ~30 frames/s, so a
## near screen judders; screens set back to far meanwhile = setting screendist 0, default 0)
Looked up: playbook @ b955f224 symptom-index / cross-project-index (nothing on capped menu or loading rates); failure-atlas
FAIL-XR-024 + 09 #runtime-owns-pacing (engine cap unlimited once a session exists); 19 / pattern-catalog:602 (Windows caps a
hidden or off-screen window's Present at ~30 - rule out); XR-003 / 09 "keep submitting during loading" (not the cap);
vrframework 07 (nothing), 14 (limiters desync counters, general). Our notes: Batman City FIX_CHANGES 2026-10-03 (the game
rewrote MaxSmoothedFrameRate from DefaultEngine.ini every launch, capped at 62 -> 90 in the source file); City
BATMAN_CITY_PLAN SVRPACE (slow loads = our own pacer, SteamVR only); City logos + loading go through GFullScreenMovie;
memory ak-comfort-settings-forced (regenerated BmSystemSettings reset MaxFPS to 30). AK RE notes: nothing on the limiter or
the movie player. Game files: every start-up / loading movie is authored at 30 fps (StartupMovie.swf 30, StartupLoopingLoad.usm
30/1 90 frames, nv_logo / Intro_BM3Logo_fire / Intro .usm 29.97); PC/PCEngine.ini [Engine.GameEngine] MaxSmoothedFrameRate=30
(-> generated BmEngine.ini 24/30, bSmoothFrameRate FALSE); DefaultEngine.ini [Engine.Engine] FrameRateLimitingSetting=7
("Limit MaxFPS to 30"); BmSystemSettings MaxFPS 120. Ghidra here holds a 32-bit Arkham game, not BatmanAK.exe.
Ranked causes:
1. The full-screen movie player (Scaleform start-up movie / CRI .usm loops) presents only when its 30 fps media has a new
   frame. Evidence: all these screens are movies at 30; gameplay and the 3D main menu are not capped. Needs RE of
   BatmanAK.exe (strings FullScreenMovie, StartupLoopingLoad, Startup.swf) if 2 and 3 are ruled out.
2. MaxSmoothedFrameRate=30 (PCEngine.ini) used outside gameplay. Test: 120 in the source file (installed, FIX_CHANGES).
3. FrameRateLimitingSetting=7 applied outside gameplay. Test: 0 in DefaultEngine.ini (installed, FIX_CHANGES).
Measurement built (AKVR 14:06, installed): mode log "start-up/title N frames/s (Ts)", "main menu", "loading", "gameplay"
per stretch (akvr_startup_log mode timeline; buffer 8 KB).
| # | JJ does (one launch, VR launcher as usual, window on the main monitor, in front) | proves | judge |
|---|---|---|---|
| 1 | start, watch the logos + title ~10 s, press a key | title rate with both settings changed | "start-up/title N frames/s" |
| 2 | main menu ~10 s, Continue | menu rate (3D, uncapped expected) | "main menu N" |
| 3 | loading screen to the end, 10 s of play, quit from the menu | loading + gameplay rate | "loading N", "gameplay N" |
Follow-ups: title / loading near 90 -> one of the two settings is the cap: next launch with one of them back to separate
them, keep the one that matters (FIX_CHANGES). Still ~30 -> restore both byte-exact, cause 1: RE plan for the movie player
(JJ opens BatmanAK.exe in Ghidra, or a static string/xref scan) - the fix is the player presenting every engine frame while
its media runs at 30. Either way the near screen (screendist) only comes back once these screens present at full rate.

### SCR30 RESULTS 2026-10-11 14:10 (JJ ran it; LIVE akvr_startup_log mode timeline)
"start-up/title 28 frames/s (13s)" with MaxSmoothedFrameRate 120 + FrameRateLimitingSetting 0 -> neither setting caps it;
"main menu" (3D) 64/s over its first 3 s. Loading not in the log (the startup log's mode timeline is written once, ~17 s
after start - later stretches never reach the file). Conclusion: the title / loading screens run at their movies' authored
30 fps (cause 1). JJ: "it's 30 frames a second because they are movies which were rendered at that frame rate, in which
case we probably shouldn't change it" - agreed: no game change. Both ini files restored byte-exact (FIX_CHANGES).
Decision: start-up / loading screens stay FAR (screendist 0 = default, smooth: rotation-only reprojection is exact at
infinity). The near-screen code (SCREENQUAD2 headset panel, one quad, image created at start) stays behind the panel slider
for anyone who accepts the 30 fps step. Lesson (VR_ESSENTIALS 6): a screen nearer than infinity needs the game to present
at the headset rate; 30 fps movies must stay at infinity.

## TEST PLAN FPFIX 2026-10-11 ~17:10 (JJ: "first person is not working in Arkham Knight ... it just does nothing"; "I want the
## first person function to be fixed first" - before the 0.3.0 release)
Looked up: memory ak-first-person / batman-city-first-person (City: pawn = controller + 0x1B0 found by back-pointer, head bone
Bip01_Head via GetBoneLocation), NOTES FIRSTPERSON (AK RE), VR_ESSENTIALS (nothing specific).
LIVE (akn_log 14:42, JJ toggled it on): "first person on, active 0 | third person for now ... actor F83CAC20 | head frames 0,
not on foot 1128, skeleton searches 33" = the view target was found but no skeleton with the head in it, so FP never applied.
STATIC: execGetViewTarget 0xB41EA0 really calls vtable +0xA58 (slot right). Causes, ranked: (1) the view target is not
Batman's pawn (a camera actor) or his mesh is not a direct field; (2) Batman's mesh is a native subclass with its own vtable
(the test only took two exact vtables); (3) the head bone is not named Bip01_Head in AK.
FPFIX build (AKVR 17:07, installed): searches the view target AND the controller's pawn (back-pointer, City's method); the
skeleton test also takes any vtable whose slot 0 = the family destructor 0xE2E600; head names Bip01_Head, Bip01-Head, Head,
head, b_Head, Bip01_Neck, Neck (first hit wins, shown in the status); one report per new actor in
akvr_captures\fp_probe.txt (pointer fields with vtable RVAs, skeleton candidates, origin, which names were FOUND).
FP back on the panel (top). JJ: on foot in the city, tick FIRST PERSON (or double-click both sticks), walk, Batmobile in/out,
quit. Judge: status "on: camera at Batman's head (<name>, ...)" + fp_probe.txt; if still none, the probe names the fields.
### FPFIX RESULT 17:11 (JJ: "Nothing is changing when you tick the first person checkbox") -> FPFIX2
LIVE fp_probe.txt: view target FDB99B40 (vtable 1CB5560) and the controller's "pawn" FDB70D60 (vtable 1BFF2C0) point at each
other (+0x478 / +0x5B0); no field held a skeleton of the two known vtables; every head name failed. FPFIX's "slot 0 =
0xE2E600" test was WRONG (UObject's shared destructor: every object matched, GetBoneLocation was called on random objects -
no crash, removed). STATIC: APawn::GetPawnViewLocation slot +0xA60 on vtable 1CB5560 = "ret" stub -> the view target is not a
pawn; 1BFF2C0 has no slot there. Objects are packed to 4 bytes (ULocalPlayer +0x5C) - the 8-byte steps missed fields.
Natives by joined name ("ClassexecName"): UObjectexecPathName 0xF90590 -> GetFullName 0xF3E160 (this, FString* out; returns
out; FString {wchar_t*, num, max}) and GetPathName 0xF38C00 (this, out, StopOuter).
FPFIX2 (AKVR 17:15, installed): objects named by the engine (GetFullName); a skeleton = class name ending in
"SkeletalMeshComponent" (or the two known vtables); per-frame check by the cached vtable; pawn search by class name containing
"Pawn" with a back-pointer to the controller, 4-byte steps; search every 2 s while missing; fp_probe.txt lists every object
field of the view target and the pawn with its full name, and every head-name result on each skeleton.
### FPFIX2 RESULT 17:20 (JJ: "it seemed to work while in normal gameplay, but as soon as I went into a training module, Batman
### just left the camera behind") -> FPTRAIN
LIVE: works in play - head Bip01_Head on BatEntry.RPawnPlayerBm_0 (found as the controller's pawn), skeleton at +0x38C; the
view target is the controller itself (BatEntry.RPlayerControllerCombat_1); main menu: frontend.RPawn_0 /
RCinematicCameraActor (menus stay third person). In training: still "active 1" on the same pawn, no new search (the view
target never changed) = the camera stayed with the parked body.
FPTRAIN (AKVR 17:24, installed): per frame, the controller's pawn pointer (offset remembered) must still be the skeleton's
pawn, else search again at once; the head must be within 15 m of the game's own camera (City's FPGATE), else third person
this frame and a new search (at most every 0.5 s); the search only accepts heads near the game camera.

## PANELTIDY5 2026-10-11 ~17:5x (JJ: "Put the first person option within the view drawer and remove the depth of field on off
## options and the skip unneeded steps and the game's own anti-aliasing ... Remove all the code that it uses as well and any
## leftover probes or tests ... remove ... any option to add extra anti-aliasing SMAA or soft - not required now we have DLSS")
JJ also chose to remove POST EFFECTS (the game's look / glow / grading only acted with depth of field off).
Removed (AKVR, builds clean, ~1,850 lines + files): panel rows; eyeaa.cpp (the mod's SMAA / soft pass + grading), src/shaders
(eyeaa*, tonefix*, toneglow*), vendor/smaa, tools/build_eyeaa_shaders.bat; hudsplit HDRCURVE / GAMELOOK / GLOWFIRST pass,
GAMEGRADE grab, GAMEAA (game SMAA skip), BLUROFF / SKIPSTEPS, DOFBLUR (a geo-11 fix shader); earlyres DOFSWITCH / DOFQUICK /
DOFLIVE (config forcing simplified: DepthOfField=True written in [SystemSettings] + buckets 2-5, so older installs' False is
undone); xr dim-layer DoF darkening, POSTOK; settings keys. ps_track() stays always on (as it effectively was).
Probes/tests (helper agent, then SHADERDUMP / SHCENSUS): test-tools switch and everything behind it, DRAWPROBE 1-4, RAINPARTS,
NORAIN, NEARRAIN modes, RAINSTRETCH / RAINSPLIT, RAINWRITER, TARGETTRACE, HUD hide test, HUD coverage read-back, MKREC,
SHADERFINDER, RADARREC, AIMSHOT, the render-target observer + scene TAP feed, shader dump at every start, F2 shader census.
Kept: FARRAIN (camera input = head-free game camera, as nearrainlag=0 gave), FLAREOFF, SUITGLOSS, MKGS, MKSINGLE/MKBATCH, HUD
split / layers / markers, F2 capture core, start-up / mode logs, AKN-command test switches (MARKSHEET, MARKERCUT, MAPFLAT ...),
first person (fp_report probe file kept until JJ confirms the training fix). Shipped settings file: 27 dead keys removed.

## JJ 2026-10-11 ~22:00: markers face straight out; hints missing ("A to rescue"); detective mode lacks lights in the LEFT eye; crash opening the panel in detective mode under SteamVR
Looked up: VR_WORLD_MARKERS_GUIDE §5a/§5b (AKN card = upright, facing the camera HEADING); batman_city BATMAN_CITY_PLAN
"MARKERFACE" (JJ asked City the same thing 17:4x; card faces the game camera's POSITION, right edge level; JJ now says it is
fixed in City); NOTES DETECTIVE / VSSPLIT / VSSYNC / DETVIG (detective mode was right in both eyes 2026-10-09 03:2x);
VR_ESSENTIALS §8 / memory vd-crash-new-swapchain-midsession, vr-session-refused-pc-side; no earlier note on missing prompts.
### Findings (LIVE / STATIC, 22:0x)
- Crashes, matched to launches (Application log + CrashDumps + akvr_swapchains.txt + xrclient_BatmanAK.txt):
  - 18:22-19:01:16 SteamVR session = JJ's crash: ACCESS VIOLATION (null read) in nvwgf2umx.dll+0x1b6d74 on an NVIDIA driver
    worker thread (only driver frames); at that moment a SteamVR (vrclient_x64) thread was in D3D11 / the driver (taking a
    submitted frame). AKN's log of that session was overwritten by the relaunches -> LOGKEEP below. Dump BatmanAK.exe.19880.dmp.
    (Stack scans without unwinding are noisy: the D3DCompile frames seen on AKN's status thread were stale - a false lead.)
  - 19:02:19 VD launch: "HUD layer" swapchain FAILED (-2), then fail-fast in virtualdesktop-openxr.dll = the known first-VD-
    session-after-SteamVR refusal (vr-session-refused-pc-side), not the mod.
  - 19:06:27 VD, and the same signature at 17:34 / 18:17: fail-fast (CFG, code 0xa) on a VirtualDesktop.LibOVRRT64_1 thread
    with ONE thread left in the process = a crash while the game is already exiting (after quitting). Not the play crash.
- Detective lights: c190600 (JJ, detective mode) - the green / warm lights on the pipes, tanks and the building at the right
  are in the right eye only; the left eye (the game's own family, rendered second) has them dark. Means L 13/19/23, R 15/22/25.
- Prompts: c184715 (JJ at the rescue spot, a person on the floor): no prompt in either eye nor on the HUD sheet (compass only).
  The per-eye marker layers were never saved by any capture, so a prompt routed as a world marker was invisible to checks.
### Built 22:14 (AKN only, installed; previous DLL in akvr\diagnostics\before-MKFACEPOS-20261011)
- MKFACEPOS (default on, "mkfacepos 0|1"): City's MARKERFACE in mk_eye_frame - the card faces the game camera's POSITION
  (solved from the head-free view x lens), right edge level, up = normal x right; size fitted so the game camera sees the
  game's layout at W (first order), in a ROLL-FREE frame (forward = camera aim, level right) because AKN's head-free camera
  carries head roll (MKREC 045423). Falls back to the upright card with the camera nearly above / below the marker. Status
  line: "markers N, faced to the camera N". Offline (scratchpad facepos_check.py): normal . (W->camera) = 1, right z = 0,
  size identical with a 25 deg rolled camera, layout reproduced (diagonal 0.998 / 1.000).
- LOGKEEP: akn_log.txt -> akn_log_prev.txt -> akn_log_prev2.txt at start (a crash + relaunch kept no evidence).
- PROMPTCAP: every panel capture / capmk also saves both eye marker layers (mk_left_<tag>.bmp, mk_right_<tag>.bmp).

## TEST PLAN JJ1011N (one launch; SteamVR launcher, so the crash repro can be the last step; JJ drives, I send switches)
| # | command / switch | what it proves | how to judge |
|---|---|---|---|
| 1 | none (mkfacepos 1 default); then mkfacepos 0 / 1 on JJ's word | markers tilt toward the camera, still no turn with the head | JJ: grapple point / objective above or to the side faces him; log "faced to the camera" > 0 |
| 2 | tools\Run-PROMPT.ps1 at a spot with a button prompt | where the prompt is lost (see the script header) | prompt in p0 mk layers / hud sheet / nowhere; which of p1-p4 makes it appear |
| 3 | tools\Run-DET2.ps1 in detective mode, standing still facing lit buildings (~70 s) | view-dependent (eyesame) vs per-eye state (gloss, vssync, vssplit, detvig, mipbias, framesync, rainonce) | left vs right brightness of the lit area per step; d0 = d11 (JJ held still) |
| 4 (last, crash-prone) | detective mode on, open the panel (both sticks), close, repeat 3x | reproduces the SteamVR crash with LOGKEEP on | crash: akn_log_prev.txt end + new dump; no crash: repeat once under VD |
Follow-ups: T3 eyesame 1 makes them equal -> culling / lens from the main camera in detective lighting (TURNEDEYE family): try a
left-eye light-cull fix next; a switch in d4-d10 restores the lights -> that feature skips / corrupts detective lighting in the
game's family: fix that path. T2 p1 shows it -> extend the HUD phase to that movie's draw; p2-p4 -> fix the routing / placement.
### JJ1011N T1 / T3 RESULTS 22:2x
- T1 HEADSET: JJ "the marker points are now facing the camera" - MKFACEPOS confirmed. T2 (prompts) set aside by JJ: he does
  not know a spot with a prompt yet.
- T3 run 1 (22:25:50-22:27:01, pair_det0..11): JJ was at Ace Chemicals facing the plant, NOT the 19:06 view (ledge looking
  down at the yard with the green-lit pipes / tanks). No lit structures in view; both eyes match in every step (whole-picture
  means L/R within 1.1 of 0-255; overall level drifts 8 -> 13 between steps = the detective look / camera, not the eyes).
  Inconclusive for the missing lights: re-run facing the 19:06 area.
### JJ1011N T3 run 2 + JJ report 22:29 (SteamVR, 22-43 frames/s) -> DETHOLD
- HEADSET (JJ): lighting "a lot more consistent" between the eyes in detective mode; no crash (panel opened). But after leaving
  detective mode the right eye was missing lighting, and in detective mode the building's OUTLINE (only that element) flicked
  on now and then - JJ thinks it should stay on all the time.
- LIVE (akn_log 22:21-22:28): (1) the DET2 step "detvig 0" (22:26:33) also switched detection OFF (ps_note_skip only ran while
  detvig hid the shader) -> VSSPLIT made the right eye's normal-play view state IN detective mode and used it for 5 s = that
  state left half in detective mode = the right eye wrong after the exit (test-induced, this launch only). (2) 22:27:44-
  22:28:37: the right eye swapped view states 12 times in 50 s while JJ was in detective mode: at 22 frames/s the 100 ms
  "composite seen" window dropped out.
- DETHOLD (AKN 22:31, installed): detection always runs; one hold (600 ms, "dethold <ms>") for the view-state choice, the
  exit and AKVR's vignette; per-second line "detective: composite seen/s, longest gap, right-eye view state swaps/s".
- Next check (JJ, next launch): detective on 30 s -> swaps/s 0 while on; outline steady? which eye flickered?; exit -> right
  eye lighting normal. Open: whether the outline flicker was the view-state swaps (right eye only) or the game's own.
- Side note: SteamVR ran at 22-43 frames/s this session (VD sessions ~88) - check pacing under SteamVR (VR_ESSENTIALS §11).
### DETHOLD RESULT + crash 2 (JJ, SteamVR, 22:35-22:37)
- LIVE: DETHOLD works - in detective mode 0 view-state swaps/s, longest composite gap 32 ms (was 12 swaps in 50 s).
- HEADSET (JJ): after the exit the right eye is "nearly perfect, just a little bit different" (capture c223654, 16 s after the
  22:36:38 exit, Ace Chemicals: the left eye shows a big searchlight shaft top-left, the right eye only a faint one at its
  left edge - may be just each eye's own turned view; needs a same-spot before / after pair to judge).
- CRASH 22:37:13 (JJ: looked down in detective mode): nvwgf2umx.dll+0x1b6d74 null read = IDENTICAL to 19:01. Both SteamVR,
  both in detective mode. No dump written this time; LOGKEEP log ends normally (detective on 11 s, 28 frames/s, nothing odd).
  Lead: graphics memory. NVML whole-card use: VD session 19:03-19:06 10.0-10.4 GB; SteamVR sessions 10.8-11.6 GB of 12 GB with
  GPU busy only 40-56 % at 22-28 frames/s (= paging); 6.0 GB used with the game CLOSED (SteamVR still up + browsers, Discord,
  Notion ...). Hypothesis: SteamVR + desktop apps push the card to its limit, detective mode / looking down (new textures)
  tips it over, the driver fails an allocation and reads null. Not proven.
- Next: (1) same-spot before/after capture for the right-eye remainder, under VD; (2) crash: run detective mode under VD
  (no crash there so far) and SteamVR with desktop apps closed; if SteamVR still crashes, cut AKN's own memory (render
  scale, HUD / marker layer textures) and log free memory per second.
### DETSHARE 2026-10-11 22:45-23:02 (JJ, VD and SteamVR) - the right eye's private view state IS the detective difference
- SteamVR crash 3 (22:46:5x): same driver null read; it came in the 3rd detective entry (22:46:48). Crash 2 (22:37:13) was in
  the 2nd entry. First entries never crashed. VD run 22:47-23:0x: toggling + looking down did NOT crash; VRAM 9.5 GB (SteamVR
  10.8). Lead: re-entry swaps the right eye back to its detective view state (VSSPLIT) + full graphics memory under SteamVR.
- JJ (VD): right-eye lighting differs from the START (not only after exit); after an exit the right eye misses lighting.
- Snapshots (akn\vs_norm1 / det1 / norm2 / det2 _0/_1.bin + pair_*.bmp; scratchpad vsdiff.py / vsstable.py / vsdet12.py):
  pair_detsame (eyesame 1 in detective) = identical eyes -> not view-dependent. det2 (JJ: "different"): the right eye lights
  the yard floor and the green pipes, the left (game) has the dark detective look; diff: ring buffers of frame numbers
  (+0x177c.., +0x1f18..) filled in the right eye and empty in the game's, +0x07c8 game 1 / right 0, +0x0724..0x073c.
- DECISIVE (pair_shareoff / pair_shareon, 23:01): "vs 0" (right eye uses the GAME's view state) -> both eyes the same dark
  detective look. JJ (HEADSET): "The two eyes are consistent now." The game's look is the right one.
- vs 0 left ON in JJ's session. Open: smearing / ghosting with a shared view state (the reason for own view states, AKN M3b
  T3 - before per-eye DLSS existed); the right eye after a detective exit with vs 0.
- JJ (HEADSET, vs 0): exiting detective mode "looks better now too" (right eye matches). No smearing reported yet - asked
  him to check normal play (fast turns, glide, cape edges) before making vs 0 the default.
### DETCALLOUT (open, JJ 23:02: "a marker that's supposed to be attached to a world object ... moves around when you move
### the camera and your head"; captures c230235 / 239 / 242 / 248)
- The detective scan callout ("VENTILATION SHAFT" text box + leader line ending at a small square on the object, and the
  green 4-corner scan brackets at the picture centre) is on the HUD SHEET (hud_rgb_c230248) = room-fixed, so it slides over
  the world. The distance marker (118 m) is correctly on the eye marker layers (mk_left / mk_right).
- No Canvas.Project point anchors it (aim-driven). Plan: world point from depth at the leader line's end (AIMDEPTH /
  TARGETDEPTH style ray-march, Batman City), route the callout's pieces as a marker card at that point (MKTAG needs a mark on
  the callout's HUD node - find its name in the HUD tree; or route by its shaders + text texture). Look up first: Batman
  City AIMDEPTH, VR_WORLD_MARKERS_GUIDE §6 "Aim reticles", geo-11 TARGETDEPTH, NOTES "2nd projection caller 5A2BB1".
- JJ 23:06: the callout = a line to a square made of four plus signs (one per corner), labelled (VENTILATION SHAFT, also on
  Rattler drones); the square must lock onto its object (drones move) - with head moves too.
- LIVE (capmk detmk 23:03:51, PROJ log): Canvas.Project (RA 0x86DC0D) projected only ONE world point, the 118 m objective
  (-35497 -118659 3315); the 2nd caller 0x5A2BB1 made no calls in that period (count unchanged at 151011). So the
  detective callouts are NOT placed through the projection calls we hook: their position comes from another path.
- Plan DETCALLOUT (next session), ranked: (A) depth anchor - find the four-plus square's pieces (shader + texture, as
  Batman City AIMDEPTH picks its sprites), take its centre in layout, march the HEAD-FREE camera's ray through that point
  into the left eye's depth (City AIMDEPTH compute pass), place the square + line + text as one MKGS card at that world
  point (follows moving drones because the game moves the square every frame); (B) RE where the game gets the scan
  target's screen point (detective HUD movie, native caller writing its position) for an exact world point.
### DETSHARE default (AKN 23:07, installed): g_vsMode 4 -> 0 (the right eye shares the game's view state; VSSPLIT / VSSYNC
then idle). "vs 4" / aknvs=4 restores the old route. JJ to check: fast turns / glide for smearing in the right eye; SteamVR
detective on/off toggles + looking down (crash with no view-state swaps?).
### DETSHARE RESULT 2026-10-12 (JJ, HEADSET): "No crashing in Steam VR now at that same place anymore. And everything looks
### clean getting in and out of detective mode." -> CONFIRMED. The SteamVR driver crash (nvwgf2umx+0x1b6d74) went with the
### right eye's view-state swaps on detective re-entry (mode 4 / VSSPLIT). Next: DETCALLOUT (JJ: "good to proceed").

## DETCALLOUT BUILD + TEST PLAN DC1 (2026-10-12; JJ: "good to proceed with attaching the scan boxes to their world space
## targets"; JJ AFK 10 h: "run any tests you wish on a fake vr headset")
Looked up: Batman City AIMDEPTH (markers.cpp: depth march of the head-free centre ray, 96 geometric samples, read back 3
passes later; BATMAN_CITY_PLAN 2026-10-08); VR_WORLD_MARKERS_GUIDE §5b/§5c/§6 (aim reticles by depth); AKN AFW (afw.cpp:
the DLSS-path depth matches each eye's own view x lens; akn_dlss_depth / akn_dlss_srv / akn_dlss_render_size); AKVR
TARGETANCHOR / point_of (a HUD part's sheet point from the live tree); MKTAG marks. REA (JJ) is not connected in this session.
Structure (F2 hudlayers of JJ's 23:02 captures, scratchpad layerdiff.py): scan HUD = K1/0.0.0.0.3.0; .9 = the box (moved
every frame; .9.1.0.0-.3 = the four plus signs), .4 / .10 (+ hidden .3 .5 .8) = label panels; .0 / .1 hexagon arcs and .2
(still, stage centre) stay in the room.
SOURCE (built 2026-10-12 ~00:2x, both DLLs, not yet installed):
- AKVR earleyres: scan_key(); those parts are "stays on its target" by default with their own mark kScanTag (-3/512); kept out
  of akvr_hud_target_points (the layout map comes from the first target part); export akvr_scan_callout(sheetW, sheetH, xy)
  = the box's sheet point (centre of the four plus signs), 0 when hidden; akvr_scan_tag_value, akvr_scan_set.
- AKN probe: mk_eye_frame turns the box point into the game's layout R (layout map), the head-free camera's ray through R,
  and - once a depth answer exists - a marker (W = camera + ray x distance) placed like every marker (MKGS card facing the
  camera, MKFACEPOS); mk_route_begin sends every piece carrying kScanTag (single, batched, whole draws) to that marker at any
  distance; scan_pass (end of file, original context functions) marches the ray through the LEFT eye's depth (DLSS depth,
  render size, left eye's view x lens), depth convention from the matrix itself, result read 3 passes later; distance kept and
  applied to the current ray (moving drones). Commands "scan 0|1", "scanmin <units>" (default 300 = past Batman). Per-second
  line "DETCALLOUT: scan box seen/s, placed/s, pieces routed/s | depth passes/s, answers (sky), distance, hit".
| # | step (Run-DC1.ps1, fake headset, into gameplay) | proves | judge |
|---|---|---|---|
| 0 | gameplay, capmk dc0 | baseline, no detective | DETCALLOUT line absent |
| 1 | padbtn 0001 (D-pad up) -> detective | detective on | "detective: composite seen/s" > 0 |
| 2 | look for a box: mouse sweeps, capmk per step | the box shows; AKVR finds it | "scan box seen/s" > 0 |
| 3 | capmk dc_on + headset shot | pieces leave the sheet, sit in mk layers at the object | hud_rgb has no box/label; mk_left/right have them; distance sane |
| 4 | head yaw +-15 / pitch -10 (xrsim) shots | locked to the object | box stays on the same object in the composited eyes |
| 5 | scan 0 -> capmk dc_off | control | box back on the sheet |
| 6 | detective off, quit | clean exit | no crash |
### DC1 RESULTS 2026-10-12 03:15-04:18 (fake headset, 8 runs, Run-DC1.ps1; JJ's settings backed up + restored each run)
- Run 1: detective mode never came on - injected pad buttons are ignored (known since 2026-10-09). AK's PC key is X
  (BmInput.ini kToggleDetectiveMode=Tap [X]); `akmenu.py hold 0x58 0.15` works ("detective: composite seen/s" ~130).
- Runs 2-3: sweeps too steep (camera at the roof). A shallow grid (3 downward steps x 24 yaw steps of 200 counts) finds a
  box in ~30-60 s: AKVR's akvr_scan_callout finds the box (LIVE "scan box seen/s" up to 78).
- Runs 3-6 = the DEPTH hunt: every answer "sky". Diag: the expected device depths along the ray were right (normal depth,
  near 10: 0.977 at 4.8 m) but the buffer read 1.0 - (a) at HUD time (cleared), (b) right after the left eye's render, (c) in
  the most-bound depth buffer (1520x1656 format 19), (d) copied at the SMAA edge pass. A 16x16 CPU dump of (d): only BATMAN has
  depth (0.88-0.93 = ~1 m), the world reads 1.0 = UE3 clears the depth mid-render for the foreground group. FIX: copy the depth
  just before each depth clear of the left eye's render except the first (immediate hk_ClrDSV + new deferred hkd_ClrDSV slot
  53); dump then: 203 of 256 grid points carry world depth. LEAD (not acted on): DLSS / CAMMV / AFW take their depth from the
  SMAA pass = foreground only - in detective mode at least; check normal play (camera motion vectors for the world may treat it
  as infinitely far) -> a merged depth (world copy + foreground) could help DLSS ghosting.
- Run 6: placed, pieces routed (~1,600/s), but nothing visible: the callout drew in the eye marker layers - captured
  (PROMPTCAP) box + line + "VENTILATION SHAFT" label per eye; the crops show the box around the same object (a figure at the
  billboard base) in BOTH eyes.
- Run 7: march moved to the GPU (geometric steps + 12 halvings), distance eased (35 %/frame, jumps > 15 % taken at once):
  distances steady (638 / 3,335 / 8,8xx units per target). Run 8: head yaw +-15 / pitch -12 with the box up (sighting 3,
  box seen 76/s throughout): the label keeps its place against the "123m" marker (world-locked) in every pose; distance
  8,330-8,346. Clean exits, no crash in 8 runs.
- STATIC: installed = build of 04:10 (XINPUT ad6bdac2fcad, dinput8 577d180b8e51); JJ's previous DLLs + settings in
  akvr\diagnostics\before-DETCALLOUT-20261012. Commands: "scan 0|1", "scanmin <units>".
- For JJ (HEADSET): detective mode at a scannable (vent, drones): the box of four plus signs on the object at its depth,
  the line and label with it, staying put when the head moves, following moving drones. Open questions: is the label's
  size / place at the object right for him (it is the flat layout carried to the object's depth); drones moving fast
  (the distance answer is 3 frames old; the direction is current).
### DEPTHPROBE DP1 2026-10-12 04:21 (fake headset, Run-DP1.ps1; command "depthprobe N")
LIVE, 32x32 grid of the left eye's depth: NORMAL PLAY - the SMAA-pass depth (DLSS / CAMMV / AFW input) holds the world (939 of
1024 points), 2 depth clears per render, the copy before the 2nd is empty. DETECTIVE MODE - 3 clears; the SMAA-pass depth holds
only the foreground (140 of 1024, Batman 0.889-0.92); the copy before the 3rd clear holds the world (939). So DLSS and camera
motion vectors lack world depth in detective mode only. Lead (not done): in detective mode give DLSS / CAMMV the merged depth
(world copy, foreground where the SMAA depth is < 1). DETCALLOUT's "last copy before a clear after the first" = the world in
detective mode (the only mode with callouts).

## JJ 2026-10-12 ~16:3x (HEADSET, DETCALLOUT build): "the markers were not connected to their world space objects ... All HUD
## elements which should have been square to my face were attached to a separate plane that was at a different angle to the
## headset and it would rotate with head movements ... even the distance target text would flip onto that plane"; also the
## menu's first sound "about one second too late"
LIVE (his log 16:30-16:32, captures c163157-c163217): callout distances jumped (67, 48, 31 m, then 3.1 m / 4.3 m = hits on Batman
or the ledge right after the 300-unit skip); the label + line were on the object's card (large, seen at an angle = "a separate
plane"); AKVR's box point matched the drawn box (fake run: reported 0.116 0.090, drawn 0.114 0.091 - not the cause).
DETCALLOUT2/3 (both DLLs, installed 17:08, fake-headset checked):
- only the BOX part goes to the object; the label panels stay on the HUD, square to the face. (".9.1" = the plus signs alone
  was tried: it is often MISSING from AKVR's 2048-part list, which sits at ~2040 - so ".9" = plus signs + the line.)
- whole draws carrying the callout's mark are no longer routed (a draw of unknown layout held other HUD pieces - the distance
  text "flipped onto that plane").
- depth: nearer than 600 units ignored (was 300); the median of the last 7 answers, then eased.
- AKVR diag "akvr_scan_diag" in AKN's per-second line while detective mode shows no box.
MENUSOUND (AKVR, installed): AUDIOSYNC held the menu's first sound from the menu scene's start until MAIN MENU on (3.0 s in every
run: no brightness and no UI signal exist in engine stereo - the eye pair and the window both read black, AKN draws no HUD in
start-up). In engine stereo the hold now ends 2 s after it began (fake run: released 8.9 s, MAIN MENU on 9.8 s). Also: AKN
export akn_hud_draws_last (HUD-sheet draws per frame), brightness probe fed in engine stereo (reads 0 there - kept for the diag).
Open: whether 2 s matches JJ's text appearing; the box's depth on near targets (a 6.6 m hit in the last fake run, unverified).

## JJ 2026-10-12 ~23:xx (HEADSET, DETCALLOUT3): "the line and the plus signed box ... is moving all around the place with your
## head movements and camera movements. The end of the line should connect with the text, and the other end that has the box
## attached to it should connect with whatever element it's attached to ... ventilation shaft locked ... rattler drones moving"
- Fake headset (Run-DC1 head-only test 18:17): xrsim "head rot" changed NEITHER the HUD box point NOR the eye views (left-eye
  projection of W constant to 0.001 over yaw +-15 / pitch -12) = the fake head turns do not reach the eye views in these runs;
  head-driven motion cannot be judged on the fake headset now.
- Root causes (SOURCE + his 16:3x log): (1) the box's world point came from a depth march (unstable, 3-67 m jumps); the game
  itself projects each scannable object through FSceneView::Project from RA 0x5A2BB1 (~1,000/s in detective mode only) - with
  the LEFT EYE's view (head included), like Canvas.Project before MARKERBASE; (2) the game's line ran from the box to the label
  in the flat layout: with the box on the object and the label on the shrunk HUD (hudscale 600/670) it could not meet both.
- SCANPROJ (AKN): RA 0x5A2BB1 answered head-free (scanbase 1) and kept (world + screen point) per game frame; the box's marker
  takes the world point whose screen point matches the box (0.06) - exact, per frame (moving drones); the depth march only
  when nothing matches (scanexact 0 = march). Export akn_scan_point (the box's world point this HUD frame).
- SCANLINE (AKVR): the game's line (.9.0, a shape drawn from the box to the label; its own transform never changes) is hidden
  every frame; hud_in_eyes draws a 1.3 px light-grey line per eye from where that eye sees the box's world point (AKN's pair
  view x lens) to where it sees the label's anchor (.10's origin on the sheet, through the sheet's corner clip positions).
  Anchor check pending: the fixed square by the label sits at sheet ~(0.25, -0.12) in JJ's and the fake captures; AKVR's diag
  "label anchor x y" in AKN's per-second "no box" line shows .10's point to compare.
- Installed 23:32 (before: akvr\diagnostics\before-SCANLINE-20261012). Fake run not done: Arkham Asylum was running (JJ or a
  peer); no smoke test of this build yet.

## JJ 2026-10-13 ~00:0x (HEADSET, SCANLINE build): "a little bit better. The squares seem to stay quite stable, but ... the drones
## seem to miss the mark a little bit most of the time ... the new lines are going from the HUD depth to the game depth. I think
## the lines and the box should all be at HUD depth for detective mode."
- LIVE: SCANPROJ never matched (0/s in JJ's run and the fake runs): RA 0x5A2BB1 projects ~6 points per frame 48-834 m away
  (detective markers for distant things), the nearest 0.22 NDC from the box - the box is NOT placed through that call. All
  boxes so far came from the depth march.
- Fake headset, head turned in 1 deg steps (HEADRAMP: a 15 deg jump is re-centred as a tracking glitch - why earlier fake
  head turns changed nothing): the eye views turned (shots) while the box's layout point stayed fixed to 0.001 over yaw
  +-20 = the game places the box from the HEAD-FREE camera (the head-free ray was right).
- SCANHUD (AKN + AKVR, installed 00:2x, fake-checked): the box is no world marker any more. AKN routes the callout's pieces
  unmoved to its marker sheet and exports the box's world point + sheet point (akn_scan_box); AKVR (hud_in_eyes) finds where
  the ray from the head's centre to the object (both eyes' directions to the world point) meets the HUD plane, draws the marker
  sheet on the HUD plane shifted so the box lands there, and draws the line from that point to the label's anchor on the same
  plane. Box + line + label all at HUD depth; the box's direction depends on the head's position only (not rotation), and
  since the world point lies on the head-free ray through the game's own box point, distance errors barely move it.
  Fake shots: box on the roof vent left of the "VENTILATION SHAFT" label, the line joining them, unchanged by head yaw.
- Open: JJ's check of drones (the game's per-frame box point is now what places the box); the 600-unit near limit (the roof
  vent is nearer; harmless at HUD depth).

## JJ 2026-10-13 ~04:0x (HEADSET, SCANHUD build?): "much more accurate, but for moving targets like the rattler drones the HUD
## boxes are not over them - offset by a fair margin. They are attached (they move with them), just not at the exact spot"
Looked up: NOTES DETCALLOUT/DC1/SCANLINE/SCANHUD (box direction = head centre -> W, W = head-free ray through the game's own box
point x marched distance; distance barely matters at HUD depth); memory ak-one-frame-thread-lag-rain-hud + NOTES MKLAG (Canvas.Project
markers showed NO frame lag in AKN, default 0 - but the scan box comes from another, unknown path); VR_WORLD_MARKERS_GUIDE §6.
No log newer than the 00:31 fake run (JJ's session not logged, or from before SCANHUD).
Ranked causes: (A) the GAME's box trails the drone (box point from last tick's location or a Scaleform ease toward the target) -
fits "attached but offset"; would show in the flat game too. (B) the scan box's HUD layout belongs to the neighbouring game frame
(MKLAG never measured this path). (C) head-centre parallax from a wrong marched distance (box ray misses the thin drone, hits the
background) - small unless the head is far from the recentre point.
## TEST PLAN SCANLAG (to build; JJ drives, detective mode at moving drones)
| # | command / switch | proves | judge |
|---|---|---|---|
| 1 | SCANREC (always on in detective): per HUD frame box layout point, its screen velocity, march hits | data for 2-3 | log |
| 2 | GPU blob search in the left eye's depth copy round the box: centroid of near pixels (the drone) vs the box | offset vs drone motion: along -velocity = lag (A/B), fixed direction = mapping, ~0 = (C)/HUD-depth perception | lag in frames = offset.v/|v|^2, steady -> A/B |
| 3 | "scanlag 0/1" (box from the other kept frame) | B | offset drops |
| 4 | fix "scansnap 1": add the measured blob offset (3 passes old, steady for steady motion) | A | box on drone in captures |
| 5 | fix "scanlead k": extrapolate the box by its own velocity k frames | A (fallback) | same |
| 6 | panel SCAN TEST button: off / scanlag / snap / lead 1 / lead 2, 3 s each, label + capture each | JJ judges after, not live | captures |
| 7 | (JJ, 1 min) flat game, detective mode on a drone: does the box trail it? | A is the game's own | yes/no |
### SCANLAG superseded -> SCANOWN (2026-10-13 17:0x). JJ: "the box is not behind. it's way off diagonally at a seemingly unrelated
### location"; no lag in the flat game. So NOT timing (plan SCANLAG above dropped).
- LIVE (JJ's log 16:53-16:54, captures c165345-c165418 + AKVR captures 20261013_1653xx): every second's "box layout" vs the
  nearest detective projection (RA 0x5A2BB1, world z ~1500 = the drones) fits box = 0.80 x point + (0.02, -0.194) to 0.003, e.g.
  point (0.188, -0.026) -> box (0.171, -0.215); (-0.128, 0.013) -> (-0.082, -0.183). HEADSET capture c165345: the line runs from
  the RATTLER DRONE label down-left to empty ground; the 5 drones float above it.
- Cause (SOURCE): akvr_scan_callout read the box point through the LIVE Scaleform matrices, which include JJ's HUD-layer
  adjustment on K1/0 (s 0.80 x 0.010 y -0.135, hudlayers.txt). AKN then cast the head-free ray through a shrunk, shifted point
  (so the depth march hit the ground) and the 0.06 projection match never fired. The game's own box point = the drone's
  projected point exactly.
- Fix SCANOWN (both DLLs, installed 17:01; before: akvr\diagnostics\before-SCANOWN-20261013): akvr_scan_callout gives AKN the
  box point through the GAME's own matrices (L.base for parts we moved, while the node still holds ours); the drawn point (with
  our shrink) is kept for AKVR's on-screen shift (export akvr_scan_box_drawn); the label anchor stays drawn. Expected: SCANPROJ
  matches every frame ("at the projected object point/s" ~= "placed/s", match < 0.01) = the drone's exact world point per frame.
  A/B: AKN command "scanown 0|1". Not run yet (built + installed only).
- Why static vents looked fine: unverified (the vent likely has no projected point and its box sat near the shrink's fixed point).
### SCANOWN RESULT 2026-10-13 17:04-17:05 (JJ, HEADSET): "That looks almost perfect now"
- LIVE: "at the projected object point/s" = "placed/s" every second, match 0.000-0.002 (box layout = the object's projected point);
  no depth march needed (passes 0). Drones (z ~1500) and the vent (-36288 -115332 4956) both matched. CONFIRMED.
- JJ: "a second line on one of the HUD elements that was attached to a box from an object, but then it went away". Capture
  20261013_170501_908: two lines from the vent box - ours to the label, and the GAME's line (.9.0 and its shape .9.0.0 both shown
  in hudlayers) routed with the box (a child of .9), its far end at the label's flat-layout place = partway.
- SCANLINE2 (AKVR + AKN diag, installed 17:1x; before: akvr\diagnostics\before-SCANLINE2-20261013): every frame hide every child
  of .9 except the plus-sign branch (one child holding 4 parts), and their children; count re-shows ("game line re-hidden N" in
  the AKVR box lookup diag, now logged every 5 s while a box shows too). If the count keeps climbing per frame, the game re-shows
  the line after our hide each frame -> next step: drop the line's pieces in AKN's routing instead.
### MENUSOUND2 2026-10-13 17:2x (JJ: "Remove the Audio effect two-second wait between the launch titles and the first menu")
AKVR audiosync.cpp: in engine stereo no menu-scene hold (it could only end on MENUSOUND's fixed 2 s timer: JJ's 17:03 timeline
"4.4s sound held (menu scene, no menu yet) ... 6.4s sound on after 2000 ms"), and any hold ends with the next frame. The 50 ms
frame-gap holds (game loading, no frames) stay. geo-11 path unchanged. Installed (dinput8 only; before: akvr\diagnostics\before-MENUSOUND2-20261013).

## JJ 2026-10-13 ~17:1x (HEADSET, SCANLINE2 + MENUSOUND2): (1) "still a second line appearing on one of the HUD elements attached
## to the object"; (2) main menu: "a black box overlay covering the menu items and Batman. Can we remove that?"; (3) "the bat
## transition that appears when you go from one menu to the next or into gameplay needs to be full field of view and closer to
## the camera, same as all the other mods"
Looked up: NOTES BACKEXT / BLACKKEY / HUDSKIP (pause backdrops: 50 % black sheet + 16:9 band; BLACKKEY keys out dark see-through
texels, pause look only - would also eat black bats); Batman City BATMAN_CITY_PLAN WIPEMAIN/HIDEWIPES/WIPEFLIP/WIPEPANEL (full-view
bats, a long series), Origins BUILD_PLAN O1D (bats full projection FOV, 0.8 m, enlargement 1.6, event-gated); memory
arkham-bat-sound-and-unpause (check unpause / map close / quit separately; prefer the engine's menu state).
- (1) LIVE 17:09-17:10: "game line re-hidden" +45/s while a box shows = the game shows its line again EVERY frame; hiding wins
  only some frames. SCANLINE3 (AKVR): the line branch's colour alpha (multiply + add, data+0x5C/+0x6C) set to 0 every frame and
  again right after the game sets that node's colour (hkObjSetCxform); diag "made transparent N".
- (2)+(3): no capture of either yet (AKVR's eyes picture is only filled in gameplay; MENUSTART grabs the geo-11 texture). HUDSIG
  (AKN, default on): every HUD-sheet draw kind (vs + ps + texture size) logged when new / back after 1.5 s / gone (duration,
  draws, sheet index range) - transitions = kinds living ~1-2 s; the menu box = kinds living through the main menu. Command
  "hudsig 0|1". Installed 17:2x (before: akvr\diagnostics\before-HUDSIG-20261013).
## TEST PLAN MENUBATS (one JJ-driven launch)
| # | JJ does | proves | judge |
|---|---|---|---|
| 1 | launch, title -> main menu; in the main menu: panel capture | box = which draws (capmk HUD dump + HUDSIG kinds at menu start) | hud_rgb shows the box; its draw kind |
| 2 | go into a sub-menu and back (bats between menus) | bat draw kinds + duration | HUDSIG new/gone pairs ~1-2 s |
| 3 | Continue -> gameplay (bats into gameplay) | same kinds? | HUDSIG back/gone |
| 4 | detective mode, scan drones + vent; capture if the 2nd line shows | SCANLINE3 | "made transparent" climbs, no 2nd line |
| 5 | pause -> quit to main menu (bats) ; then quit | exit wipe kinds | HUDSIG |
Next build from the result: drop the box draw kind in the main menu (HUDSKIP rule, menu-gated); while a bat kind is live, draw the
HUD sheet full-view close (City/Origins way), with a panel switch.
### MENUBATS RESULTS 2026-10-13 17:17-17:19 (JJ, HEADSET; captures 171758 (loading), 171819 (vent), 171835 + 171851 (drones))
- (1) Second line still there (171819: two dashed lines leave the vent box in the left eye - ours to the label, one partway; the
  HUD sheet holds neither = it is on the scan marker sheet). SCANLINE3's diag was cut (AKVR diag buffer 200 chars).
  Lead: apply_layers runs BEFORE the movie's own update (hkMovieSlot27 -> g_orig27), which shows the line again every frame.
  SCANLINE4: the hide + transparency also run right AFTER g_orig27 (box node kept, is_child_of checked); diag buffer 400,
  "after-update passes N".
- (2) Black box: 171758 was taken on the loading screen (AKVR timeline: main menu 7.7-24.8 s, capture ~40 s). Its HUD sheet: rgb
  black, alpha OPAQUE in a full-width 16:9 band = a solid black 16:9 rectangle on the sheet (candidate for the box). HUDSIG: too
  noisy to name it (text glyph kinds dominate; same shaders as the menu text).
- (4) NEW, JJ: "an orange box that's at an angle and it's not attached to where it should be attached, which is below it in the
  HUD". 171851: an orange diamond on the HUD SHEET at the sheet centre (0.50, 0.49); the eye marker layer mk_left holds two small
  orange dots at the blue hologram panel below it (pair: a world object). Reading: a target marker whose small pieces are routed
  to the object while its diamond stays on the sheet. Part not identified yet (no ON-TARGET part at the centre in hudlayers).
- HUDRING (AKN + AKVR, installed 17:3x; before: akvr\diagnostics\before-HUDRING-20261013): ring of 12 HUD sheets (every 3rd
  stereo frame, ~0.7 s, ~360 MB video memory); written at half size over grey (akn\ring<n>_<k>_<ms>_<why>.bmp) 0.3 s after each
  screen change (MAIN MENU / loading / screen / 3D / flat) and 0.9 s after an A or B press in menus or the pause screen; at most
  12 dumps per session; "hudring 0" turns it off. Turn OFF by default after this test.
### HUDRING RESULTS 2026-10-13 17:28-17:30 (JJ, HEADSET): orange box "fixed correctly" (nothing was changed for it - watch);
### extra line still there; NEW: duplicated compass briefly on first entering gameplay; a duplicate distance marker + its box
### flickering off to the side every 3-4 s
- LIVE: SCANLINE4 ran (after-update passes ~17,000, made transparent 3,458) - the extra line remains = it is NOT (only) .9.0.
  Captures 172944 / 173011 / 173023: some frames show one line per eye (the cyan dotted lines are the game's detective world
  outlines); 171819 showed two in the left eye. Neither the left eye render (pair) nor its marker layer holds it.
- HUDRING dumps (7, akn\ring01-07): THE BATS ARE ON THE HUD SHEET (Scaleform): black bat shapes sweep over the sheet and end
  fully black (dumps 6, 7: pad A in the pause/main menu, "screen"). THE BLACK BOX: dump 2 (A into "SELECT SAVED GAME") - an
  opaque black 16:9 band appears behind the menu text on the sheet (rgb 0, alpha 1), shown on the menu's HUD panel = a hard black
  box over Batman. HUDSIG cannot name either (same shaders / texture sizes as the menu text; ps d5b5473b also draws text).
- SCANCUT (AKVR, installed 17:4x; before: akvr\diagnostics\before-SCANCUT-20261013): the scan box's marker sheet is drawn only as a
  small square round the box (half-size 0.03 sheet units) - whatever else lands on that sheet (the game's line, a routed distance
  marker?) stays out. AKN "scancut 0|1 [r]".
- HUDRING2 (AKVR): an A / B press in a menu also writes the HUD tree 0.6 s later (akvr_captures\ringlayers_<n>_<time>.txt) =
  which part is the bat swarm / the black backdrop (engine-state signal for both fixes, City's lesson: prefer engine state).
- Next build from that: hide the backdrop part in menus; while the bat part is visible, draw the HUD sheet full view and close
  (Origins O1D: distance 0.8 m, enlargement 1.6), the sheet's bats keyed as they are.

## JJ 2026-10-13 ~17:45 (HEADSET, SCANCUT build): "all you've done is mask the incorrect HUD elements out instead of fixing it
## at a fundamental level." Line "a little bit shorter"; duplicate compass still at the start (lower down), then gone; the
## distance marker still flickers to a different location (captures 174302-174329)
Looked up again (rule: two failed fixes): playbook symptom-index "HUD or menus drawn twice" (doc 17: world twice, everything else
once); our NOTES MKTAG RESULTS (compass duplicate near the marker = leftover PS constants marking whole batches), MKGLOW2/3 +
MKSTICKY (distance marker "kept flickering out to a duplicate above it" = the pulse animation rewrites the colour the mark rides
on -> pieces flip between the marker layer and the HUD sheet), RETHINK MARKERS 2 (JJ 10-11 "this all feels very hacky"; plan:
geo-11 polarity - HUD pieces in the EYE pictures by default, room only for positively identified branches; a misjudged piece then
gets a wrong DEPTH, never a wrong PLACE). Only part of that plan was built (MKEYE moves marker pieces per eye); the default is
still the room sheet, so every misjudged piece lands in a wrong PLACE = today's symptoms. SCANLINE2-4 / SCANCUT were patches of
the same class (JJ is right).
Root causes (evidence):
1. Extra line = the GAME's dashed line (.9.0, a child of the box) goes wherever the box goes; hiding / transparency never held
   (the game re-shows it every frame, 17,000 after-update hides). Its draw identity is NOT known: the 216x4 dash-texture
   kinds start with gameplay and never go (checked 17:42:53 - not the callout; HUDSIG also hit its 800-line cap). Next:
   log the kinds of the draws routed to the scan sheet (box vs line) - then replace the game's line by identity.
2. Distance marker duplicate every 3-4 s = classification by colour mark flips on some frames (MKGLOW/MKSTICKY history; the
   marker's pulse). Not yet measured in AKN-MKEYE: FLIPREC = per frame, each piece of a marker shader within reach of a marker
   point: layer chosen + its add rows; written only when a piece changes layer between frames.
3. Duplicate compass at gameplay start, "lower down" = (lead) two HUD presentations overlapping while the HUD switches from
   the menu quad layer to the in-eye HUD (the quad still holding the last sheet), or the HUD-part adjustments not yet applied
   for the first seconds. PATHLOG: first 15 s of gameplay, per second: quad layer shown?, in-eye HUD drawn?, layers adjusted.
4. Bats = an unnamed menu movie ("K?" in ringlayers_06: a slanted part 0.0.0.1.0 with 11 scattered sprite children) - engine-
   state signal for the full-view bats.
5. Black box = the menus' opaque 16:9 backdrop (ring dump 2) - its part to be found in ringlayers_02/03/05 the same way.
- SOURCE finding (xr.cpp): hud_in_eyes draws INTO g_aknSbs, which is refreshed only on a fresh pair (aknSbsUpdate); on a frame
  without one, the old picture WITH its HUD is shown again, hieNow is false, and the HUD quad layer is added = two HUDs in two
  places for that frame (compass at start = many non-fresh frames while loading; marker flicker = a hitch). Structural lead.

## TEST PLAN REC1 (recording only - JJ: "I can do the recording for you, and then you can do the rebuild once you've identified
## what you need"). Installed 18:0x (before: akvr\diagnostics\before-REC1-20261013); SCANCUT default OFF, HUDRING OFF (its dumps stall).
| # | recorder | proves | read |
|---|---|---|---|
| 1 | PATHREC (AKVR, logged by AKN each second) | frames showing the old eye picture with its HUD again + the HUD layer (two HUDs) | "PATHREC: ... BOTH (two HUDs) N" at start-up and during play |
| 2 | FLIPREC (AKN) | pieces next to a marker point left on the HUD sheet, with their colour rows | "FLIPREC single/batch ..." + per-second counts |
| 3 | SCANREC (AKN) | the kinds of draws sent to the scan box's sheet (box vs the game's line) | "SCANREC kind N ..." |
| 4 | BLACKHUNT (AKN, capture in a menu) | the draw that makes the menus' black backdrop | "BLACKHUNT drop #k ... <== THIS DRAW" + HUDSKIPIDX line |
| 5 | BATREC (HUDSIG reset on A/B in menus) + ringlayers tree | the bat draws' kinds (live ~1 s) and their HUD part | HUDSIG new/gone within 1.5 s of a reset |
JJ: launch; main menu -> save select (black box) -> capture, keep still ~6 s (HUD flickers while each draw is dropped); a few
A / B menu moves; continue to gameplay (watch the compass); detective mode on drones + vent ~1 min (line, marker flicker); pause,
capture once on the pause screen; quit.

### REC1 RESULTS 2026-10-13 18:28-18:31 (JJ, HEADSET + LIVE log; captures 182851-182910 in the menus). JJ: second line still
### there; distance marker still flickers to a duplicate
1. PATHREC (LIVE): BOTH (two HUDs) = 0 in all 156 s; old picture with its HUD shown again only 22 frames, all at the start of
   gameplay, never with the HUD layer. The stale-frame theory is RULED OUT (negative result).
2. FLIPREC (LIVE): the marker duplicate = pieces of the marker that never carry the mark: 59 x fd60f2d7 single at (0.008,
   0.178) = 0.07-0.12 above the marker (the "duplicate above" of 10-11) + 2 x 9938094a at d 0.058 (inside reach, tag 0, not
   sticky). EVERY one of them carries a world point in cb0 rows 0-3 (r0-r2 axes, r3 = -35929.98 -117315.72 3263.27 / -54861.55
   -110354.06 173.61 = the marker's world position). The 245 batched 05154232 "near misses" carry the ROOM mark (-1/512 in
   their add row) = correctly room HUD near the marker, not flips. LEAD (unverified): world point in the piece's own draw data =
   the game's ownership of the piece; must check the shaders READ rows 0-3 (else it is leftover from the marker's previous draw).
3. SCANREC (LIVE): draws routed to the scan sheet: kind 2 = batch 54cd897e + 1024x1024 atlas, 5 pieces all callout (2,700+);
   kind 3 = single 9689d605 + 1024x1024 at (0.252, -0.094); kind 1 = single fd60f2d7 + 32x64 at the label (0.750, -0.262);
   kind 0 = batch 97657b85 + 2048x1984, 1 of 3 pieces. The second line is among these (SCANCUT shortened it = scan sheet), but
   .9.0 hide / transparency never touched it -> it is probably NOT .9.0. Next: per-piece placement of the scan-sheet draws.
4. BLACKHUNT: the sheet held no solid black when JJ captured in the menus (0-0.4 %). Captures: save select = menu panels only;
   182910 (after choosing the save) = a solid 16:9 black band on the sheet = the GAME'S FADE TO BLACK before loading. The "black
   box" is that fade shown on the menu's HUD panel instead of over the whole view. Same class as the bats.
5. BATREC: draw kinds cannot separate the bats (they share the 1024x1024 atlas with menu text); the "K? 11-sprite" group was not
   confirmed as bats. SOURCE (STATIC): BatmanAK.exe EXPORTS AK::SoundEngine::PostEvent (id / char / wchar overloads,
   RVA 0x1535d30 / 0x1535050 / 0x1534f80) and GetIDFromString - the Origins O1D route (Play_UI_Fader_Bats = FNV-1
   3904589100 in City/Origins) is open here.

## REBUILD PLAN RB1 (structural; JJ 18:0x: "you can do the rebuild once you've identified what you need")
Looked up again: Origins BUILD_PLAN O1D (event-gated full-view bats, 0.8 m, enlargement 1.6), City WIPEMAIN / BLACKFILL /
EXITFADE (fades + bats are screen effects: full view), VR_HUD_GUIDE §6/§7/§10, RETHINK MARKERS 2, memory arkham-bat-sound-and-
unpause (check unpause / map close / quit separately; prefer engine state).
A. Screen transitions (bats + fade to black): hook PostEvent (exported); the bat event opens a bounded window; while it is open,
   or while the HUD sheet is a full-screen cover (fade), show the HUD sheet over the WHOLE view, close (Origins 0.8 m). Log every
   event id in menus to confirm the bat id and find the fade's.
B. Marker pieces by the game's own world point (if 2's lead holds: the VS reads cb0 rows 0-3) instead of the colour mark; a
   piece carrying world point W is a marker piece and is placed by W. Else: mark ownership on the HUD tree branch that holds the
   marker's pieces and verify every piece of it routes.
C. Detective line: name the line piece by per-piece placement on the scan sheet, then route by identity; remove SCANCUT and
   SCANLINE2-4 (masks).
D. PANELWORDS for the AKVR / AKN panel (peer session's request; JJ "some of the labels ... are very confusing").
### RB1 BUILD 2026-10-13 ~19:0x (both DLLs, installed; before: akvr\diagnostics\before-RB1-20261013; not run)
- B DISPROVED (STATIC): the five marker / HUD vertex shaders found in GlobalShaderCache-PC-D3D-SM5.bin by AKVR's fnv64 hash and
  disassembled (scratchpad sh/*.asm): fd60f2d7 + 9938094a read ONLY cb0[6..9] (placement, uv) and [12..13] (colour mult / add);
  91e2b222 [6..9]; 05154232 / 54cd897e index rows from 14 by the vertex. Rows 0-3 (the world point) are NOT read = leftover from
  the draw before (a 3D marker part that writes a world matrix - itself a lead: some marker piece is drawn with the game's world
  point). FLIPREC single now logs r6, r7, r12 (mult), r13 (add) - the rows the shaders use.
- A TRANSFULL (AKN BATEVENT + HUDCOVER, AKVR tr_decide): Wwise PostEvent hooked (3 exported overloads, queued in HOOKBATCH),
  Play_UI_Fader_Bats id checked against Origins' 0xE8BB4D2C, every new event id / name logged once (AKEVENT) to find the fade's;
  HUDCOVER = 16 rows of the sheet's 16:9 band read back 2 frames late (alpha + darkness). Full view while bats < 2.5 s ago or the
  band is a dark cover (alpha > 0.6, brightness < 0.08), 0.2 s hold: the sheet drawn into both eyes on a head plane 0.8 m ahead,
  1.2 x 1.6 x distance half-width (Origins O1D), no cuts / marker layers / box / BLACKKEY; HUD layer off (also on repeated
  pictures). Mode timeline "full-view transition on (bats / fade / bats+fade)". Panel (MENUS AND SCREENS): "bat transitions and
  fades to black fill your whole view" (setting transfull, default on). AKN "transfull 0|1 [dist zoom]".
- C: SCANPIECE logs each callout piece (placement rows) against the box point -> name the line piece. SCANLINE2-4 still in the
  build (they act on .9.0, which is not the visible line) - remove with C.
- D: panel - new checkbox only; the default panel labels are already plain (peer session checked; jargon only behind advancedpanel).
## TEST PLAN RB1 (JJ drives; one launch)
| # | JJ does | proves | read |
|---|---|---|---|
| 1 | titles -> main menu, move between menus (bats), pick the save (fade) | TRANSFULL fires for bats + fade | BATEVENT lines, "full-view transition on (...)" in the timeline; headset: bats / black fill the view, close |
| 2 | pause -> resume once; pause -> quit to main menu | unpause must not go full view unless the game plays bats; quit bats | same lines |
| 3 | detective mode on a vent + drones ~30 s | SCANPIECE names the line piece | SCANPIECE lines |
| 4 | normal play ~1 min (distance marker in view) | FLIPREC rows 12-13 of the flicker pieces | FLIPREC single lines |
### RB1 RESULTS 2026-10-13 18:58-19:00 (JJ, HEADSET): "Lots of things are broken now": menu items jump close to the face and lock
### to it for a few seconds between menus; detective boxes no longer on their objects; the distance marker no longer fixed, text gone
- LIVE: BATEVENT never fired - AK does NOT post Play_UI_Fader_Bats (92 events, all posted BY ID, none = 0xE8BB4D2C; ids logged).
  The bats may be a Flash-side sound or another event id. Origins' route does not carry over as is.
- LIVE: the HUDCOVER "dark cover" rule fired on ordinary menus for 1-5 s at a time (6 times before gameplay) = the menu jumping to
  the face; in gameplay it fired in detective mode (dark detective HUD), and TRANSFULL switches off marker cuts / per-eye marker
  layers / the box = every gameplay symptom. Rule wrong: a dark HUD is not a transition.
- FIX (installed 19:01, AKVR only): TRANSFULL off by default, setting renamed transfull2 (JJ's saved transfull=1 ignored), panel
  label "TEST: ... (not ready)". AKN unchanged (recorders only).
- SCANPIECE: the 400-line cap was used up by an unrelated piece (97657b85, 2048x1984 atlas) before detective mode - no answer.
  Next time: only while a box shows, per kind.
- Lesson: a transition signal must come from the game's state (City: menu movie open/close via MENUSTATE; Origins: the sound
  event), never from how dark or covered the HUD looks - AK's menus and detective HUD are dark by design.

## RB2 (JJ 19:0x: "just fucking fix" ... "Stepping away now" -> fake headset allowed)
- SCANBOX (AKVR): the callout's box is drawn by AKVR - four plus signs on small quads round the four corner parts' DRAWN points
  (akvr_scan_corners), coloured from the game's plus sign at that point on the marker sheet (fallback green); the marker sheet
  itself is no longer shown, so the game's own line (whatever piece it is) cannot appear. Our line (SCANLINE) unchanged.
- PARTID (diagnostic, AKVR + AKN "partid 1|0"): room parts get kPartTag - (index + 1) * 4e-7 (clear of kMkTag), table in
  akvr_captures\partid.txt; FLIPREC prints r12 / r13 / batch rows at 8 decimals with the decoded part index.
- Runner tools/Run-RB2.ps1: gameplay, partid ~45 s of normal play with small camera moves, detective sweeps to a box, captures +
  headset shots, settings restored.
### RB2 / RB3 RESULTS 2026-10-13 19:09-19:22 (fake headset, JJ AFK; JJ's settings restored after each run)
- LIVE/fake HEADSET: the second line is GONE (rbf1/rbf2 shots: one line from the label to the vent). SCANBOX drew no plus signs:
  the corner parts were not found (list full at 2040; RB3: stale list entries) -> SCANBOX3 takes the corners from the box's
  CURRENT children always; diag "box drawn by AKVR N (corners found N)" in AKN's detective line. Not re-run yet.
- SCANLINE2-4 (hiding the game's line on the HUD tree after every movie update, ~17,000 x per session) switched OFF
  (g_scanKillOn 0): useless with SCANBOX, and JJ's marker flicker began with the build that added it.
- PARTID: no flips recorded - the distance marker was never in view from the rooftop start (both runs); no part found.
- MKCARRY2 (AKN): a HUD frame with no projected marker point keeps the last points for 250 ms (NOTES MKGLOW: ~9 % of seconds had
  no matched point = the whole marker flat on the HUD sheet = JJ's "duplicate ... off to the side every three or four seconds").
  Per-second "MKCARRY2: ..." line; command "mkcarry 0|1". Unverified (no marker in view in the fake runs).
- MENUSTATE (AKN): UGFxMoviePlayer Start / Close hooked from UE3's native table (RVA 5E7CD0 / 5E8080) - every menu movie start /
  close logged (12 in the menus); URGFxMovieUIexecStart / RGFxMovieFrontMostexecShouldFade not in the table by that spelling;
  the NAME TABLE was not found (AK's GNames is not a { data, num, max } TArray of entries with "None" at 0x4-0x20, checked live
  read-only too) -> movies by address only. Next for the bats / fade: AK's name storage (Ghidra / REA: FName::ToString callers).
- Installed 19:22 (both DLLs): SCANBOX3 + SCANLINE off + MKCARRY2 + MENUSTATE log; TRANSFULL off.
### RB4 2026-10-13 21:44-21:47 (fake headset; JJ HEADSET 21:3x: "no green plus boxes ... no orange diamond ... no distance marker,
### but there is the background glow"; lines correct, not doubled)
- CAUSE (LIVE, EYESKIP counters): the per-eye marker layers were skipped on EVERY frame since the RB1 build (18:45) - AKN drew
  ~900 marker pieces/s into them, AKVR drew 0 layers. TRANSFULL's `const bool tr` in hud_in_eyes_body was SHADOWED inside the
  per-eye loop by the existing `tr = tanf(f.angleRight)` (the lens tangent, never 0), so `!tr` there was always false. Renamed
  trFull (all uses). EYESKIP diag stays in "HUD in the eyes: ... eye layers skipped: no layer / cut / transition / pause key / mode".
- SCANBOX4: the box part (.9, AKVR's "box" key) has ONE child holding the four corners in this tree (box tree d0:1 d1:4 d2:1 x4
  d3:0 x4) - SCANBOX2/3 expected .9 -> (line, plus holder) -> 4. Breadth-first search for the first part with four resolvable
  children fixes it: "box drawn by AKVR 1918 (corners found 4)"; fake shot: four green plus signs on the vent, one line to the label.
- VERIFIED (fake headset, 21:48-21:50, installed build dinput8 SHA-256 63088405...): eye layers skipped 0 of every kind; headset
  shot rb4n: "123m" distance marker + its bracket box once per eye; detective shot rbf1: four green plus signs on the vent, one
  line to the label, the distance marker on screen. JJ's settings restored. Next: JJ's headset look, then HUD_REBUILD_PLAN HR1.
### HR1 start 2026-10-13 21:5x-22:11 (JJ HEADSET 21:5x: "looks really solid. The only thing I notice is the occasional flickering of
### the distance marker to another location for one frame")
- MKJUMP (LIVE, fake, 60 s normal play, marker in view): the game's projected marker list changed ONCE (0 -> 1 at gameplay start);
  W stable (-35497 -118659 3315) - the game's data does not jump. MKCARRY2 never fired (no empty lists in JJ's session either).
- MKMISS (LIVE, fake): no marked piece near the marker left on the flat sheet; the only misses = two marked text pieces at the right
  (0.75, -0.08) every frame (objective panel, correctly flat). FLIPREC near-marker: 0. "no layout map": only 1-8 frames at gameplay
  start (the map is cached in AKVR).
- => not reproduced with the fake headset's still head: the one-frame jump comes with JJ's real head motion / headset timing.
- HR5 quiet (AKN): HUDSIG, HUDCOVER readback, SCANREC / SCANPIECE / FLIPREC / MKMISS event lines OFF by default (counters stay);
  command "recorders 1". (The flicker was first reported with the 17:26 build that added per-frame recorders.)
- MKPLACE (AKN): per HUD frame the first marker's point in each eye (pair views); a frame that lies off the line between its two
  neighbours (out and straight back, > 0.03 NDC and > half the step) is counted ("MKPLACE: ... /s") and logged with HUD seq. Fake
  check: markers + box fine, layers skipped 0, settings restored; installed 22:11. JJ's next normal session decides: MKPLACE
  outliers = our placement; none while he sees the flicker = the compositing / timing side (AKVR hud_in_eyes, pair vs layer).
### HR2 / TAGFADE 2026-10-13 23:0x-23:38 (JJ HEADSET 23:0x: "the box for the distance marker was in the correct spot, but the text for
### it was in a different location now, and the occasional flicker would see the box very briefly flicker over to where the text is")
- LIVE (JJ's log): the marker placed every HUD frame (placed = HUD frames); marked draws ~150/s, MKSTICKY "held" 35-268/s = the number
  and its glow lived on the backup hold; when the hold slipped they fell to the flat sheet (number elsewhere; box one frame there).
- HELDREC (fake RB6, recorders 1): 300 / 300 held pieces = add-coloured pieces (multiply rgb 0) whose ALPHA ADD = mark x their own alpha
  multiply, exact (-0.00390625 x 0.23828125 = -0.00093079). The mark is never lost - it is scaled by the piece's fade.
- TAGFADE (AKN): a (row, row + 1) colour pair whose alpha add equals mark x alpha multiply counts as marked (single pieces: any pair
  outside rows 5-7; batched pieces: each colour row with the next). Fake RB6b: held 0 (was 300 lines), "TAGFADE: ... recognised by
  their faded mark/s" 16-73, marked draws ~325/s, settings restored. Command "tagfade 0|1". Installed 23:38 (XINPUT9_1_0.dll).
- MKSTICKY / MKCARRY2 stay as a fallback for now; HR5 removes them once JJ confirms (MKSTICKY hold counter should stay ~0).

### RB9 / RB10 RESULTS 2026-10-14 00:0x (JJ: "the distance marker always jumped to the other size/location once when entering and exiting detective mode")
- RINGEVERY / RINGBOTH (AKN HUDRING): the ring now keeps EVERY frame (was every 3rd - a one-frame jump fell between), 24 frames, middle
  60 % crop, HUD sheet + both eyes' marker layers (`ringNN_KK_<ms>_<why>_{hud,mkL,mkR}.bmp`), written 0.2 s after the detective flag
  changes. Note: the flag (DETHOLD) changes 600 ms AFTER the game's exit; the right eye's view state swaps at the flag change.
- STATIC (RB10, 5 entries + 5 exits, 240 frames per eye): both marker layers are steady in every frame (bounding box identical) EXCEPT
  one exit frame (dump 08 frame 14) where the corner brackets are missing from both layers - on that frame the brackets are on the FLAT
  HUD sheet around the flat number = the "jump to the other size / location" (flat sheet = HUD depth, our 0.80 shrink).
- STATIC: the distance NUMBER ("123m") is on the flat HUD sheet in EVERY frame; the marker layers hold brackets, glow and arrow only.
  Since MKSTICKY went off (RB8) nothing holds the number with the marker: TAGFADE does not recognise the number's draw.
- LIVE: no FLIPREC line (radius 0.12) - the flat number lies further from the marker point. Next (RB11, built): `fliprad 0.3` +
  `mkpieces 400` to read the number's draw (shader, kind, colour rows), A/B `mksticky 1`, two detective toggles with the ring.

### RB11-RB13 RESULTS 2026-10-14 00:2x-00:40 (JJ: "the entire fov seems to rescale briefly when entering and exiting detective mode"; "does the zoom matter?")
- EYERING (AKN, with HUDRING): per finished pair the left eye's picture + both eyes' lens. STATIC: lens identical (1.09169 / 1.00175, both
  eyes) through every switch - our lens never changes.
- TEST-TOOL ARTEFACT: the runners' focus click (akmenu click 0.98 0.97) moved the mouse = the game camera swung up to 80 deg in a few frames
  at each X press (camera.csv F2 00:35:32); RB12/RB13a "zoom" frames were partly this. Without the click (focus kept) the camera trace is
  still (0 changes in 8192 frames, F2 00:36:31). Lesson: press X without clicking once the window has focus.
- Clean entries (no click): ~13 frames (~0.2 s) before detective mode shows, near things (Batman) shift and grow while the far skyline
  stays put = the game's own camera pulls in toward Batman's shoulder for detective mode (not a lens zoom), with a radial streak blur.
- PSNEW (AKN, lean bind hooks slots 9 / 69 back in; "psnew 0|1", "psnew list", "psskip <crc>"): shaders first bound at the entry -
  02267B90 + 4B0F0E56 ~0.2 s before the composite, then the detective set. PSSKIP: 4B0F0E56 = the transition's radial streak blur (frames
  sharp without it, the pull-in stays); 61A7C04F = the outline sweep over Batman; C248EFCC / 02267B90: no visible change at entry.
- HUDRING now resettable ("hudring 1" resets the count) and triggered by the raw composite (no 600 ms hold) = the real exit (RINGRAW).
- Still open: the distance number / brackets falling onto the flat HUD sheet during the switch (RB10) - the main marker bug.

### MKCONT / MAPCONT / HELDDROP 2026-10-14 00:5x-01:15 (JJ: "the distance marker still flickers over to a copy")
- FLATREC (AKN, "recorders 1"): flat HUD-sheet draws for 0.7 s after each raw detective switch, one line per distinct piece, batch pieces
  with position + colour rows ("flatwin" = manual window). Showed at an exit: the marker point jumped (0.002 -0.082) -> (-0.166 0.053) for
  ONE HUD frame with the same world point = AKVR's HUD layout map (akvr_hud_layout_map) changed for that frame (0.667 / 0 -0.13 -> 0.28 -0.36).
- MAPCONT: a new layout map must hold 3 HUD frames before markers use it. MKCONT: a projected point with a new world position (300 u) must
  hold 3 frames. HELDDROP / HELDDROP2: on a held frame, marked marker pieces out of reach are LEFT OUT (single: draw skipped; batch: rows
  zeroed), never drawn flat. Command "mkcont 0|1" (default 1); per-second line "MKCONT: ... held back".
- LIVE (fake, 3 runs, 26 switches): map jumps held at about half the switches; marker layers steady in every frame (both eyes); flat corner
  copies gone. Left: 1 frame in 12 switches the distance NUMBER alone fell on the flat sheet (ring12 frame 07) - its text draw lost the
  mark that frame (different mechanism; next: the number's own draw identity, HR2).
- Not the game's lens: detective entry = the game's camera pull-in + streak blur (4B0F0E56), see RB11-RB13 RESULTS.

### HEADSET 2026-10-14 01:2x (JJ): MKCONT / MAPCONT / HELDDROP did NOT fix it - "It's still flickering to a different location"
- The fake headset never showed the flicker JJ sees in the headset (it shows other, rarer jumps); fixes "verified" there keep failing in
  the headset. Do not declare a marker fix from fake runs again; JJ will not do repeated test rounds for it.
- NEW LIST ITEM (JJ): RAIN brighter near bright lights (moon, volumetric beams) in the flat game; in the headset that brighter area is
  OFFSET from where it should be, and part of it looks masked by something large moving in and out. Leads to check first: the rain shader
  reading a screen-space light / shaft / bloom texture (per-eye lens = turned eyes, TURNEDEYE), RAINONCE (rain simulated once, re-anchored
  per eye - the light buffer of the first eye used in the second), AKN RAINDEPTH / near-rain block (memory ak-near-rain-camera-block).

### RAINLIGHT / RAINRELIGHT 2026-10-14 01:2x-01:4x (JJ: bright rain near the moon / beams offset, "masked by something large moving in and out")
- Shader (a96594b16ceb399b, simulation): each drop's colour = light picture t2 sampled at the drop's screen spot: uv = (pos - cb0[10]) x
  cb0[6..9] / w x cb1[19] + bias; the VS just draws the stored colour (offset 64). RAINCB 01:3x: cb0[6..9] = the turned EYE's view x lens;
  cb0[10] original vs head-free only ~3 units apart -> RAINLIGHT (row 9 corrected for C_hf - C_eye) is right but tiny, NOT the cause.
- Cause (fake, beam in view): RAINONCE runs the step in the right eye only, so the left eye shows the RIGHT eye's lighting (~14 deg off,
  the right eye's Batman silhouette in t2 cutting through it). rainonce 0 (each eye its own step) moved the left eye's bright rain into the
  beam. RAINRELIGHT (default 1, "rainrelight 0|1"): the left eye runs the step again with cb0[13].x (time step) = 0 - positions / age /
  wrap unchanged, streak length (cb0[13].y) kept, its own lighting. Deferred contexts only (immediate: skip as before). Counters in the
  per-second "RAINLIGHT ... RAINRELIGHT" line: relights 22-70/s, skipped 0.
- Same-camera check (eyesame 1, rain only): left vs right 0.038 with relight off, 0.48 with it on (full double step was 1.1-1.3) - the
  relit eye differs a little; cause unknown (light picture differs per render, or respawned drops). JJ's headset judges; stopped here (JJ:
  "stop all the tests. just ask me to do it").
- HEADSET 01:5x (JJ on RAINRELIGHT): "Left and right eyes are not fusing for the bright rain. Maybe for all the rain" -> RAINRELIGHT default
  OFF. New reading of the fake pictures: each eye's OWN step put its bright rain off its own beam in opposite directions = the light
  picture t2 is the PREVIOUS render's (the other eye's). RAINLIGHT2 (default 1, "rainlight2 0|1"): the one step (right eye's render) samples
  t2 with the last published LEFT eye's view x lens (g_mcPubVP[0]; rows 6..8 = its rows 0..2, row 9 = C x rows + row 3) -> one colour per
  drop for both eyes. Not tested (JJ tests). Panel: "TESTS (only when Claude asks)" drawer with rain as normal / show only the rain / hide.
  Installed 01:5x (XINPUT9_1_0.dll + dinput8.dll).
- HEADSET 02:0x (JJ on RAINLIGHT2): "better, yes" but rain vanishes from the bright area when the camera turns toward it ("connected to
  the camera"); fine while still. Reading: the view x lens was read when the step was RECORDED, not when it RUNS -> another frame than the
  light picture while the view moves. RAINLIGHT3: the left eye's view x lens is written to a GPU buffer on the render thread when its
  picture is done (immediate context), and a 1-thread pass builds rows 6..9 from it when the step runs. Installed 02:1x, not tested.
- Same report: in "show only the rain" the compass stayed, and its GREEN OBJECTIVE BOX flickers off like the distance marker. That box
  carries the marker mark (05154232 piece 2 at compass height, FLATREC) and joined the marker whenever the marker came within the marked
  reach (0.35). TAGREACH: g_mkTagReach 0.35 -> 0.15 (marker pieces lie within ~0.1 of their point; "mktagreach" live). Possibly also the
  distance marker's own flicker. Installed 02:1x, not tested.
- HEADSET 02:0x (JJ, RAINLIGHT3 + TAGREACH build loaded): compass green box still jumps (capture c020414), bright rain still fades when
  the camera turns toward the beam ("I'm wondering if it's a DLSS thing. it's not a massive deal").
- MAPROOT (AKVR earlyres.cpp akvr_hud_target_points, installed 02:2x, not tested): ROOT CAUSE of the one-frame map jumps (MAPCONT): the
  HUD layout map (akvr_hud_layout_map) came from the FIRST on-target part that had something drawn; JJ's tree has four on-target parts
  in three movies (K1/0.0.0.0.3.0.9, K2/0.0.0.0.1.0.0.0 + .1, K4/0.0.0.0.1.0.0.0), so on frames where the first was empty the map was
  another movie's (offset 0.28 / -0.36 instead of 0 / -0.13) -> every marker piece placed with the wrong offset, and the marker point
  landing near the compass box pulled the box in. The map now always comes from the first part's own movie root, drawn or not.
- HEADSET 02:1x (JJ): DLSS off FIXED the rain fading in the beam (DLSS, not RAINLIGHT3). Next for the rain: a bias-current-colour mask
  (pInBiasCurrentColorMask) over the rain draw f50d1365 per eye - our motion vectors are camera-only, so the falling drops reproject wrong
  and are averaged away where the background is bright. Not built yet (JJ: "not a massive deal").
- HUD flicker still there with DLSS off; JJ's 02:10 session had 0 MAPCONT holds (MAPROOT stopped the map jumps) - the flicker JJ sees is
  another cause. JJ: "if we can't fix it this time, then we're just going to leave it". HUDREC (installed 02:3x): every HUD frame's marker
  numbers (points, world point, eye points, map, routed / flat / miss / held counts, detective) in a ring of 400, written to
  akn\hudrec_<tag>.txt by the panel capture -> JJ captures right after a flicker.
- HEADSET 02:1x (JJ, TAGREACH build): "the distance marker box and glow have detached from the target again", compass box still flickers.
  HUDREC (3 captures, 400 frames each): one marker point, same world point, map constant in every frame (MAPROOT holds) - no jumps of the
  marker itself; missReach 1 on ~18 % of frames = a marker piece (glow / corner) beyond 0.15 drawn flat -> TAGREACH REVERTED to 0.35.
  At the capture moments no compass piece was routed; the compass box flicker is NOT pinned down. JJ: leave it if this round fails.
- RAINMASK (installed 02:4x, not tested): the rain draw (kRainVs, deferred + immediate DrawInstanced hooks) drawn again into a per-eye
  R8 mask (same target size, game's depth, PS = 1), cleared at each eye render start, passed as pInBiasCurrentColorMask to DLSS.
  Log line "RAINMASK: eye N mask WxH" once, per-second "RAINMASK: rain draws marked". "rainmask 0|1".
- HEADSET 02:3x (JJ): RAINMASK ran (mask 1520x1656 per eye, 50-127 marked draws/s) but the rain is still smoothed away facing the beam
  (fine in the corner of the view). Offered: try DLSS models live (K / J / M / L), else leave it, or the big job (rain drawn after DLSS).
- NEW (JJ 02:4x): chimney SMOKE sprites differ per eye and roll with head roll. Looked up: playbook FAIL-STR-023 / ch14
  #per-view-decisions (billboards oriented per view = rivalry; orient once for both eyes from one reference view). Our fleet: markers
  face the head-free game camera (MARKERCARD), SUITGLOSS3 gives flagged shaders the head-free camera's rows. Plan: give the sprite
  vertex shaders the head-free game camera's axes (no head roll, same for both eyes). SPRITEFIND (AKVR, installed): vertex shaders with
  sincos or vertex+instance ids are written to akvr_captures\sprite_vs\<hash>.txt at creation; the panel capture also writes
  akn\vs_inventory.txt (vertex shaders drawn in the next 3 frames) -> JJ captures while looking at the smoke.
- SPRITEFIND result (JJ capture 02:46, fresh launch): of 60 vertex shaders drawn, one sprite shader: 1d937df7248f2561 = UE3 particle
  sprite VS: corners = sin/cos(rotation) x cb0[23] / cb0[24] (camera right / up of the VIEW = per eye, head roll included); also a
  camera-position facing mode via cb1[8]. 32 of 398 saved sincos VS share the layout (rows 23/24, 25/26, 27/28, 26/27).
- SPRITEAXES (installed 02:5x, not tested): AKVR finds the two rows per VS at creation (sprite_rows: "sincos rS, rC" + "mul rX, rS.xxxx,
  cb0[A]" + "mad rX, -rC.xxxx, cb0[B]"; export akvr_vs_sprite_rows); AKN per draw turns those rows from this eye's camera frame into the
  head-free game camera's (Q = eye view x head-free view^T, 1-thread pass on the game's own values) in a cb0 copy -> one card orientation
  for both eyes, no head roll. "spriteaxes 0|1"; per-second line "SPRITEAXES: ... draws turned". Camera-position facing (cb1[8]) not
  handled yet. JJ thought the LEFT eye looked right.
- HEADSET 03:0x (JJ on SPRITEAXES): eyes agree and no roll now ("Okay, but") - cards still tilt back when looking up; wants "completely
  facing outwards, perfectly vertical". SPRITEUP (spriteaxes 2, default): target frame = world vertical up (UE3 +Z), the head-free camera's
  right made level, level forward; spriteaxes 1 = the head-free camera's own axes. Installed 03:0x, not tested.
- HEADSET 03:1x (JJ): the smoke brightens looking up, darkens looking down - only the smoke, also before today. Sprite VS 1d937df7:
  o8 = t1 (3D texture) sampled at THIS eye's screen uv (cb1[0..3] clip / w) + a depth slice from w = a screen-space light / fog grid
  (froxels); o7 = t0 world-space volume (fine). Suspect: the grid is filled for another camera (game's, other eye, previous frame).
  VOLWATCH (installed 03:2x): the panel capture arms it; the next sprite draw names its grid (size / format), and for 0.2 s every compute
  pass / draw that writes the grid is logged (shader, eye, recorded / immediate, render stamp) beside the eyes reading it.
- VOLWATCH result (JJ capture 03:00): grid 84x92x64 R8G8B8A8 (render res / 18 px cells, 64 depth slices), ONE texture written by an
  unnamed compute pass on the immediate context in EACH eye's render, read by the smoke in each eye's render (no cross-eye frame mixup
  seen at record time; execution order not proven). JJ 03:1x: in the flat 2D game the smoke stays dark looking up and down -> VR-only.
  Suspects: the grid is built for the head-free game camera (head pitch shifts the lookup) or with the game's 16:9 lens, while the smoke
  reads it through the eye's view x lens. VOLCB (installed 03:2x): at the grid pass (immediate, output = the grid) its cb0..cb3 are read
  back (2 dumps per capture) with the eye's view, lens, view x lens + inverse and the head-free camera x lens + inverse, to match rows.
  Fix plan: re-map the grid into the eye's frustum (1 pass over 84x92x64) and give the smoke the re-mapped copy.

### DIAGKIT 2026-10-14 03:3x (JJ: permanent test features in the overlay) - guide VR_DEBUG_TOOLKIT.md
- AKVR: every shader's code kept by name (g_shCode, sh_store); akvr_shader_save(hash, path) writes its disassembly; the panel capture
  saves the code of the vertex + pixel shaders drawn now into akvr_captures\<time>\shaders (2 s after, from AKN's inventories).
- AKN: capture also lists pixel shaders (ps_inventory), dumps the replay ring if on, the new-effect list if watching, and the hidden
  effect. FXSTEP: "fxcollect" (pixel shaders of 4 frames), "fxstep +1/-1/0" hides one at a time (g_skipPs), akn_diag_status for the panel.
  "psnewarm": 2 s baseline then new effects logged.
- Panel TESTS drawer: replay switch, detailed logs switch, watch for new effects, collect / previous / next / show all + status, rain view.
- Installed 03:4x (both DLLs), not tested. Cleared 9.6 GB of my old ring recordings from E: (33 GB free).
- VOLCB result (JJ capture 03:1x): the grid pass's cb1 = THIS eye's view buffer (rows 0-3 = eye view x lens exactly, 11-13 view rows,
  15-17 inverse, 24 = forward + depth offset, 34 / 36 = light directions); cb1[4..7] (previous view x lens) = the OTHER eye's (shared
  history alternates eyes - consistent with the one shared grid). The smoke VS reads it with the same eye view x lens -> no mismatch.
  Remaining suspect: scattering uses the camera forward (cb1[24]) = follows the head in VR. SMOKELIGHT (AKN, "smokelight 0|1", default 0)
  gives the grid pass (immediate dispatch whose output is a 3D R8G8B8A8 x64 texture) a cb1 copy with row 24.xyz = head-free forward.
  Panel A/B in the TESTS drawer ("Smoke lighting: follows your head / steady"). Installed 03:5x, not tested.
- HEADSET 03:2x (JJ): SMOKELIGHT (grid forward) made no difference; looking far down the smoke turns GREEN (his hunch: not lighting
  of the grid). DIAGKIT capture 03:16 saved 395 shaders: smoke PS 07ac19831f7c0c35 (inputs = sprite VS outputs): soft-particle depth
  fade (t0 / t3) and THREE lights whose directions arrive in VIEW space (cb0[25] / [27] / [29], colours cb0[26] / [28] / [30], one green)
  turned into the world with the view's inverse rows cb1[15..17] = the EYE's (head included) while the directions match the head-free
  camera -> the lights swing with head pitch. LIGHTDIR (AKVR flags PS with "mul rN.xyz, cb0[K].yyyy, cb1[16].xyz", akvr_shader_lightdir;
  AKN gloss_pre gives them cb1[15..17] = head-free inverse rows; smokelight 1 = default, panel "Smoke lighting" A/B). Installed 03:3x.
- NEW (JJ 03:2x): ground reflections flicker / differ per eye (capture 03:15:50: right eye's slabs shine, left dull). Known since 10-08
  (SteamVR mode-3 note: sheen in one eye only; City PERCAPTURE lead). Next: JJ uses the panel FX stepper to name the reflection shader.
- HEADSET 03:4x (JJ): LIGHTDIR fixed head moves ("The smoke is stable") but turning the camera with the gamepad still changes its colour.
  Reading: the light directions are made a frame or more before the draw (UE3 game -> render thread lag), so while the game camera turns
  the current rows are ahead of them. LIGHTLAG: a 4-deep history of the head-free view (mk_build), LIGHTDIR uses the one N frames back
  ("lightlag 0..3", default 1; panel "Smoke light delay" 0-3). Installed 03:5x, not tested. Reflections: JJ parked them (flicker in
  each eye, only seen here).
- HEADSET 03:5x (JJ): no light delay changes anything; stick pitch still changes the smoke colour (persistent, not lag). FXCB (DIAGKIT,
  installed 04:0x): a capture records the watched / selected effect's pixel-shader cb0..cb2 at its first draw in each eye (copied on the
  recording context, read back after the eye render) + eye view, the game's own view, game camera with head, head-free now / 1-3 back.
  "fxwatch <hash>" (default 07ac19831f7c0c35 = sprite smoke PS); the stepper's hidden effect takes priority. Plan: two captures (camera
  pitched up / down) -> which camera keeps the world light direction (cb0[25] / [27] / [29] through its inverse) constant.
- FXCB RESULT (JJ, 3 captures at 3 stick pitches, 03:40): smoke PS cb0[25] = (0, 0.5, 0, 1) every time (cb0[27] its opposite, cb0[29]
  sideways) = camera-RELATIVE fixed light directions, not world lights; through the head-free rows they swing with the stick (world
  (-0.67,0,0.74) / (0.63,0,0.78) / (-0.01,0,1.0)). Designed for a card that faces the camera (n.l constant in the flat game). SPRITEUP made
  the cards upright, so the camera-relative lights hit them differently per pitch = the colour change. LIGHTFRAME (installed 04:1x):
  LIGHTDIR now turns the lights with the CARD frame (sprite_frame, shared with SPRITEAXES) -> lit from world-up, same at every pitch.
  LIGHTLAG only matters for spriteaxes 1. Lesson: before changing an effect's geometry, check what its shading assumes about it.
- HEADSET 04:1x (JJ): LIGHTFRAME fixed the colour change ("Good, that's fixed"); asks the cards to tilt again to face the camera when
  looking up. SPRITEPITCH (spriteaxes 3, new default): level right + world-up frame tilted about the level right axis to the elevation of
  the eye's forward (stick + head pitch, both eyes share it), no roll; lighting follows (sprite_frame). Panel: "Smoke cards: upright /
  tilt with the stick / tilt to where you look"; the light delay choice removed. Installed 04:2x, not tested.
- HEADSET 04:3x (JJ): spriteaxes 3 -> colour changes with HEAD pitch (cause not found: lights turn with the card frame in every mode;
  suspects = per-vertex fog grid / ambient volume samples at the tilted card corners). JJ then picked the panel's "Smoke lighting:
  follows your head" = LIGHTDIR off -> per-eye differences + colour with head yaw (expected). That switch is REMOVED from the panel
  (known-bad, PANELWORDS rule); command "smokelight" stays. Installs when the game closes. Pending JJ: "Smoke cards: tilt with the stick".
- HEADSET 04:4x (JJ): upright = most stable but "very skewed" close up looking straight up; stick tilt looks better but the colours come
  and go a little with stick pitch. SPRITESTEEP (spriteaxes 4, new default): upright up to 40 deg of head-free camera pitch, then tilting
  in linearly to face fully at +-90. Panel "Smoke cards": upright, tilting when you look steeply / always upright / tilt with the stick /
  tilt to where you look. Installed 04:5x, not tested.
- HEADSET 04:5x (JJ): "always upright" = best compromise; every mode swivels the cards about their vertical axis when the camera yaws
  with the stick (cards copy the camera's DIRECTION, so ones off to the side are seen edge-on - "they should be turning to face" the
  viewer). All 64 AK sprite VS have the game's own "face the camera position, world-up" branch (weights row W = A + 9: x camera axes,
  y velocity, z axis lock, w camera position; AKVR now reads W, akvr_vs_sprite_rows out[2]). SPRITEFACE (spriteaxes 5, default): cb0
  row W = (0, 0, 0, 1) and VS cb1[8] (camera point, translated world, eye at 0) = head-free camera - this eye -> each card upright,
  facing the head-free camera position, same in both eyes. Lights: upright frame (sprite_frame mode 2 path). Panel "upright, each
  facing you" (default). Installed 05:0x, not tested.
- HEADSET 04:1x (JJ; my earlier times in this section ran ahead of the clock): SPRITEFACE ran 25 s (04:08:29-55) then JJ chose
  "always upright"; cards facing the camera POSITION still swing as the third-person camera orbits Batman - "they should keep facing my
  character instead". CHARFACE: AKVR camera.cpp fp_body (FIRSTPERSON's skeleton lookup split out of fp_eye) publishes Batman's skeleton
  origin + head every frame once asked (akvr_character_pos); SPRITEFACE's cb1[8] = Batman - this eye (head-free camera when he is not
  found). Per-second line "... facing Batman/s N". Panel "upright, facing Batman" (default). Installed 04:2x, not tested.
- HEADSET 04:1x (JJ): face-Batman ran only ~7 s of gameplay (log), then the panel went back to "always upright"; JJ: "Yes, I want
  always upright" -> default spriteaxes 2 (panel too). Trade-off told: upright cards turn with the camera heading; upright AND facing
  Batman per puff would need a rewritten sprite shader (per-particle position is only inside the shader) - offered for later.

## AIMHUD - Batmobile turret reticle follows the head (JJ 2026-10-18: "the Batmobile is aiming its turret when you have the left
## trigger pressed. It's aiming the turret with your head movements, but the HUD element is not moving with it. The HUD element for the
## Batmobile's weapons should be independent of the rest of the HUD and should move around with your head movements")
Looked up: akvr/CLAUDE_HANDOFF.md AIMHEAD / AIMTRIGGER / AIMTRIGGER2 / AIMRETICLE / AIMSHOT / RETICLETICK (2026-10-03, geo-11: the reticle =
part `.../0.0.0.0.1.0.0.0` with 96 children, centred at stage 512,384, shown only while aiming; fixed by ticking K13/... "on target" =
drawn in the game picture, head-locked because the game camera holds the head while aiming); VR_WORLD_MARKERS_GUIDE §6 (aim reticles,
markers placed without a projection call); NOTES DETCALLOUT / SCANHUD / SCANOWN (own colour mark -> pieces routed unmoved to their own
sheet -> AKVR draws that sheet on a moved HUD plane); memory jj-drives-i-record, panel-labels-plain-no-bad-switches. Playbook @ b955f224
04-ui-and-hud "Three ways to place a HUD in stereo" (head-locked layer is legitimate for an aim reticle that IS the head's aim). Nothing
else in the fleet on a head-aimed gun reticle.
STATIC (JJ's capture 20:54:56 today, akvr_captures\20261018_205456_148 + akn c205456): head aiming ON at capture, but the reticle part
(index 1404, `K?/0.0.0.0.1.0.0.0`, 96 kids, parent at 10240,7680 twips = stage centre) was HIDDEN - the panel took the pad, so the game
saw the trigger released (AIMSHOT lesson). Its container got no saved id ("K?"), so JJ's old `K13/...:...:1` tick never applies in the
native build: the reticle is on the room-fixed HUD sheet. Same path, saved ticked: `K4/0.0.0.0.1.0.0.0` = the 74-entry Batmobile world
marker list (2026-10-02) - must NOT be caught.
Fix AIMHUD (both DLLs):
- AKVR earlyres: candidate = key ends `/0.0.0.0.1.0.0.0` with >= 48 children; gets its own mark kAimTag (-4/512) instead of a room
  mark. Behaviour check: a candidate seen visible for 3 s with the left trigger released (> 0.4 s) is ruled out for the session (by
  node) - the marker list shows while driving, the reticle only while aiming. Kept out of the marker target points.
- AKN probe: pieces carrying kAimTag (single, batch, faded) go unmoved to an own AIM SHEET (cleared with the HUD sheet), before the
  marker rules (works with no marker on screen). Unknown-layout draws with the mark stay flat and are counted ("lost").
- AKVR xr hud_in_eyes: the aim sheet is drawn in each eye on a plane of the HUD sheet's own size at the HUD's distance, SQUARE TO THE
  FACE and centred on the head's forward direction while the head is in the game's aim (camera.cpp read-back flag = left trigger held
  + aim with head on); otherwise on the room HUD plane exactly where it was (nothing changes outside aiming).
## TEST PLAN AIMHUD (JJ drives, one launch, normal launcher)
| # | step | proves | judge |
|---|---|---|---|
| 1 | drive the Batmobile 10 s, no trigger | the marker list is ruled out, markers unchanged | log "AIMHUD: ... ruled out" names a K4-type part; distance markers still on objects |
| 2 | hold left trigger (battle mode), look around with the head | reticle follows the head, rest of HUD stays in the room | JJ; log "aim pieces routed/s" > 0, "head-locked frames" > 0 |
| 3 | still aiming, turn the head far left/right/up | reticle stays centred on the view, fuses (one image) | JJ |
| 4 | release trigger | reticle gone, nothing left behind | JJ; routed/s back to 0 |
| 5 | command `aimhud 0` while aiming (control) | old behaviour: reticle on the room HUD | JJ / log |
| 6 | if the reticle shows a glow copy left in the room | the glow paste is an unknown-layout draw | log "lost/s" > 0 -> next: route its paste like MKGLOW |
Follow-ups ready: reticle slightly above the aim point -> it is the game's own layout offset (part at -400 twips); if JJ wants it dead
centre, shift by the part's drawn point. Enemy lock brackets inside the same part (if any) would also hang on the head plane at HUD size,
not on the enemies -> next step would be sizing the plane to the game's lens.
### AIMHUD BUILD 2026-10-18 21:21 (STATIC: both DLLs built clean, exports present, installed; before: akvr\diagnostics\before-AIMHUD-20261018)
AKVR earlyres (aim_candidate / kAimTag / 3 s rule-out / akvr_aim_tag_value, akvr_aim_set, akvr_aim_diag), xr.cpp hud_in_eyes (aim sheet
drawn per eye after the eye marker layers; head plane while akvr_cam_aim_head_now() && trigger < 300 ms), AKN probe (aim sheet g_aimTex,
routing before the marker rules, split target g_splitRTV, akn_get_aim_hud, command "aimhud 0|1", per-second "AIMHUD:" line with AKVR's
diag). Not run yet - no fake-headset run (JJ 10-14: no fake tests, ask him). Waiting for JJ's TEST PLAN AIMHUD run.
### AIMHUD RESULT 1 + AIMHUD2 (2026-10-18 21:2x)
- HEADSET (JJ): "only partially moving with the head ... the inner reticle and the very outer horizontal lines at the top and the
  bottom are still static on the main HUD".
- LIVE (akn_log 21:23-21:25): AKVR "candidates 0, frames shown 0" all session = the reticle part was never in the list; yet draws were
  routed to the aim sheet already at 21:24:01 and while driving with "head in the aim now 0" -> the mark -4/512 (= -1/128) collided with
  the game's own HUD constants (vs fd60f2d764b91756 single, 05154232 batch). What moved with the head = those coincidental pieces.
- Cause of "candidates 0" (SOURCE): the HUD tree is read only 0.5 s and 3 s into a gameplay stretch (and on a capture); the reticle's
  ~96 children are made when battle mode first starts, so at those reads it had < 48 children. JJ's 20:54 capture re-read the tree.
- AIMHUD2 (installed 21:27): kAimTag = -0.0047131 (odd), AKN matches it exactly (1e-7; faded 1e-5 relative); AKVR re-reads the tree
  0.4 s into a left-trigger hold when no reticle part is listed (once per hold, >= 5 s apart; ~9 ms one-off). Diag "tree re-read at a
  trigger hold N". Same test plan AIMHUD.
### AIMHUD2 RESULT + AIMHUD3 (2026-10-18 21:3x)
- HEADSET (JJ): "still one element of the reticle, not the very central one, but the one just outside of that, which is still fixed to
  the rest of the HUD".
- LIVE (akn_log 21:30-21:32): the part is found now (candidates 1, K13/0.0.0.0.1.0.0.0, tree re-read at the hold 1, never ruled out),
  ~3,000 reticle draws/s to the aim sheet, on the head every frame while aiming. Marked draws of unknown layout left flat: 2/s steady,
  86-133/s at aim starts. AIMPIECE lines were used up in the first second - no shader list for the ring.
- Ranked causes: (A) the ring has a Flash filter (glow): drawn into a scratch target, the blurred copy pasted onto the HUD sheet, mostly a
  CACHED copy (MKGLOW4) whose paste does not carry the mark; (B) the ring is a SIBLING part under K13/0.0.0.0.1.0.0 (5 children), unmarked.
- AIMHUD3 (both DLLs, installed 21:36): (A) default - aim scratch chain like MKGLOW (targets written by aim-marked pieces or copied from
  one; cleared/other writes unmark; a HUD-sheet draw reading one goes whole to the aim sheet; counter "glow pastes"); (B) live switch
  AKN "aimgroup 1" = the reticle part's PARENT carries the mark. Diag AIMNEAR: while the reticle draws, flat HUD-sheet draws near the centre
  (one line per shader + kind, 80 max).
## TEST PLAN AIMHUD3 (JJ drives, one launch)
| # | step | proves | judge |
|---|---|---|---|
| 1 | Batmobile, hold left trigger ~1 s first (tree re-read), look around | (A) fixed? | JJ: ring follows; log "glow pastes" > 0 |
| 2 | if the ring still stays: I send `aimgroup 1` while JJ aims | (B) | JJ: ring follows; check nothing else (non-reticle) moved with it |
| 3 | release trigger | nothing left behind | JJ |
| 4 | log AIMNEAR lines | what is still flat near the centre | shader list for the next step if both fail |
### AIMHUD3 RESULT (JJ 2026-10-18 ~22:0x, HEADSET): "That's better" - the ring follows. LIVE: glow pastes 214-267/s to the aim sheet at
aim starts (cause A, the cached glow copy); aimgroup never used. CONFIRMED (JJ), aimgroup stays a command only.
### PITCHPANEL 2026-10-18 (JJ: "we need an option in the VR overlay to decouple the pitch from the camera")
The existing PITCHUNLINK mode 0 (view level, game tilt ignored, right stick up/down = camera height, STICKHEIGHT) was advanced-panel
only; now in VIEW: "decouple pitch: keep the view level ..." + its height slider. Default unchanged (pitchunlink=2, follows the game's
tilt). AKVR only, installed. Note: while head-aiming in the Batmobile with it on, the view ignores the game's pitch but the turret still
aims along game pitch + head - if the reticle and the shots part vertically, that is why.
### HEIGHTLIMIT 2026-10-18 (JJ with the decouple tick: "the camera is going up way too high and also going underneath the floor. It needs
### to be limited. I would suggest checking other mods for how they limit it")
Looked up: vrframework guide 09 s7 (decoupled pitch in Anvil / Valhalla and Starfield: strip the pitch, KEEP the engine camera's
translation - no added height); playbook pattern-catalog aim-mode entry (yaw / pitch ownership as settings); SKVR / DS3VR "tilt with camera
height" (a tilt strength, no height added); akvr PLAYBOOK_REVIEW HEIGHTPITCH / LAYERS 2026-09-26 (AK's boom already rises / drops with the
stick: -19.6 deg -> +236 uu, +57 deg -> -232 uu; STICKHEIGHT added +-2 m x sin(pitch) on top because JJ found plain mode 0 "dead").
Cause: stick up = the game's boom near the ground, then our -1.6 m = under the floor; stick down = boom rise + 1.6 m.
Fix (AKVR camera.cpp, hooks.cpp; installed): the extra lift is never negative (the game's own camera + its collision is the floor limit,
as in the other mods), default 0.5 m, max 1.5 m, eased (15 %/frame), none while head-aiming (g_rbHead). Setting key stickheight2 (JJ's
old stickheight=2.00 not carried over). Panel: "extra camera height when the game camera looks down, up to (m, 0 = like other VR mods)".
If it still rises too high at 0: that is AK's own boom - next option would be a scale on the game's own height change.
### TPPITCH + TPOFFSET 2026-10-18 23:59 (AKVR only, built + installed, JJ untested)
JJ: "Don't let the camera come completely over his head. Stop it, like well before that. Also, add some camera controls to third person
to allow the camera to be shifted left, right, back, forwards, up and down to taste".
- TPPITCH (camera.cpp tp_pitch_limited): AK's boom orbits with the game pitch (PLAYBOOK_REVIEW HEIGHTPITCH: -55 deg = over Batman). The
  CAMRESTORE hand-back now gives the game a pitch no steeper than -tppitchmax (default 30 deg; 60 = off), keeping the game's own wrap
  convention, so its boom stops rising there. Not in first person, menus, the frozen screen, or head-aiming (that branch keeps the head).
  ASSUMPTION to verify: the game's next camera update starts from the handed-back rotation (AIMHEAD proved it does for aim). If JJ still
  sees the camera swing over him, the boom reads another pitch (the controller's) -> clamp there instead.
- TPOFFSET: the "where you stand" offsets (shoulder / offsetup / offsetfwd, game units, +-300) always apply now (up / forward were tied
  to the old camfix switch, off) and move along the game camera's LEVEL heading + world up (were the head-rotated camera axes); none in
  menus / frozen screen / first person. Panel VIEW: pitch slider + "third-person camera position" (open by default). JJ's saved offsets 0.
### TPPITCH RESULT + TPPITCH2 + VIEWTIDY (2026-10-19 00:11, AKVR only, installed, JJ untested)
- HEADSET (JJ): "Stop the camera before it swings over Batman doesn't work at all. The slider does nothing." + "clean up these options
  in the view menu".
- Cause (inferred, SOURCE): the CAMRESTORE hand-back is read by the turret / aim (AIMHEAD), not by AK's boom - the boom builds from the
  controller's own rotation (UE3 ProcessViewRotation / control rotation, the UEVR route; playbook 17 / 11 ProcessViewRotation). Looked
  up: vrframework guide 09 s7, playbook 17-teardown-fc2vr ProcessViewRotation, uevr-afw IXRTrackingSystemHook control rotation.
- TPPITCH2 (camera.cpp tp_orbit_limit, called after smooth_cb's smoothing): position domain. Pivot = the point of the camera's view ray
  nearest Batman's head (CHARPOS, fp_body); past the limit the camera is put on the same orbit at the limit angle (same distance), added
  to g_smoothApplied (CAMRESTORE takes it off for the game). The view's tilt (b2pitchV) and the STICKHEIGHT lift use the limited pitch.
  Only with Batman found (not the Batmobile / cutscenes), not first person / menus / frozen screen / head-aiming. CAMRESTORE clamp removed.
  Panel shows "LIMITING" while active; status line "camera look-down limit (TPPITCH2): ... frames limited N, no Batman found M".
  Fallback if wrong: find the controller rotation (runtime scan of the PlayerController for the pitch / yaw pair) and limit there.
- VIEWTIDY (hooks.cpp): VIEW grouped under headings First person / Third-person camera / Batmobile / Size and picture; labels shortened
  ("keep the view level (decouple pitch)", "lowest the camera may look down at Batman", "ease the game's fast camera swings", "camera
  position", "aim with your head while holding the left trigger", "picture size per eye (restart)"); same settings.
