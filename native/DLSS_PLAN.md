# DLSS in Arkham Knight VR (engine stereo, AKN) — plan

Written 2026-10-08. Goal (JJ): use NVIDIA DLSS in the no-geo-11 version, for clean anti-aliasing and, if
possible, more frame rate. Read with `NOTES.md` (engine stereo record) and `VR_ESSENTIALS.md`.

## 1. What already exists: Luma's Arkham Knight mod

**We do not run Luma itself.** Luma loads through ReShade (one more layer hooking the game's graphics, next to
AKVR, AKN and Virtual Desktop's in-game runtime) and drives one DLSS for one picture. We need two DLSS (one per eye)
inside our own eye renders. So: we copy Luma's method into AKN and call NVIDIA's DLSS library directly (the same
nvngx_dlss.dll Luma ships), possibly reusing Luma's small DLSS wrapper (`Source/Core/dlss/DLSS.cpp`) with credit.

- **Luma Framework** (Filippo Tarpini and contributors), open source, "Custom MIT" licence: free to reuse and
  modify, **credit the authors / project**, commercial use only with permission. Our use is fine with a credit
  line. Source: `references/luma-framework-2026-10/` (cloned 2026-10-08, commit 5d7ee54, 2 Oct 2026).
- **Arkham Knight mod** by Garamond, released 2 Oct 2026, "DLSS only, no HDR yet". Whole game code is one file:
  `Source/Games/Batman Arkham Knight/main.cpp` (340 lines). NVIDIA library calls: `Source/Core/dlss/DLSS.cpp`.
- How it works (all four parts are small):
  1. **Jitter:** patches the game's projection builder (`BatmanAK.exe+0x104FF`, Steam) to add a sub-pixel
     Halton(2,3) offset per frame (8-frame cycle). AK has no jitter of its own.
  2. **Motion vectors:** the game already renders them (for motion blur) into the only `R16G16_FLOAT` render
     target; Luma grabs it when the game clears it. Range [-1, 1], scaled by half the render size for DLSS.
  3. **Depth:** the depth texture bound to the game's SMAA edge-detection pass (pixel shader slot 1).
  4. **The swap:** SMAA's first two passes are skipped; SMAA's last pass (a compute shader) is replaced by a DLSS
     call: input = the linear (pre-tonemap, HDR) scene picture, output = the same target SMAA would write.
     Exposure: DLSS auto exposure. Preset selectable.
- **It is DLAA, not upscaling:** render size = output size (`core.hpp` sets render_resolution = output). Better
  anti-aliasing, no speed gain. It runs through ReShade's addon system - we would NOT load ReShade or Luma in VR;
  we port the four parts into AKN (our engine-stereo DLL), with credit.

## 2. What is different in VR (and why we can do it, where geo-11 could not)

- Each eye is its own engine render with its own view state, so each eye gets its **own DLSS instance and own
  history** (two NGX features). Native stereo is exactly what DLSS needs (playbook / guide 10: per-eye history).
- **AK skips its SMAA when depth of field is off** (EYEAA note, `akvr/src/eyeaa.cpp`): the Luma swap point only
  exists with the DoF block on. **The DoF block stays ON** - turning it off also removes exposure, colour grading,
  glows and SMAA (memory `ak-dof-is-a-whole-block`; JJ 2026-10-08: keep all of that). It is on now
  (BmSystemSettings `DepthOfField=True`). Only the blur steps inside it may be dropped, as a panel choice
  (geo-11 version's BLUROFF/SKIPSTEPS live in hudsplit, which engine mode does not install - port if wanted).
- **Jitter is easier for us:** AKN already writes every eye's lens (projection) itself in the view constructor,
  so the jitter goes in there (no code patch in the game). Both eyes use the same jitter index each frame.
- **Our recorded-work finding:** AK records most drawing on deferred contexts. DLSS must run on the immediate
  context: check where the SMAA last pass runs in our setup (Luma asserts it is the immediate context).
- **Rain and other effects without motion vectors** may smear or ghost under DLSS. Watch for it; options exist
  (draw rain after DLSS, or a mask) if it shows.
- **The HUD is already separate** (our HUD layer), so DLSS never touches the HUD.

## 3. Stages (each: one launch on the fake headset, measured pass/fail, then JJ in the headset)

### Stage D0 - groundwork (no picture change)
- Find the three SMAA shaders by name in engine mode (Luma's names are CRC32 of the bytecode: 0x067FF80F edge PS,
  0x2F55F98E weights PS, 0x42C0137E neighbourhood CS; add CRC32 naming next to our FNV names in AKVR's creation
  hook). Log per eye: do they run, on which context, render size; find the motion-vector target and depth.
- Port the DoF-blur skip to engine mode so the SMAA block runs without the blur.
- Also measure where the frame time goes now (game thread, render thread per eye, GPU ms per eye, GPU load %)
  at today's native size, and at a forced small eye size without DLSS (the ceiling DLSS can reach).
- PASS: all three SMAA passes seen once per eye per frame, motion vectors and depth found per eye, the last pass on
  the immediate context. FAIL -> if the SMAA block is missing: DoF block off in the VR profile -> turn it on;
  if recorded on a deferred context: run DLSS at the playback point instead.

**Goal set by JJ (2026-10-08): real DLSS upscaling - each eye RENDERED SMALLER and upscaled to the headset size.
DLAA alone is not the goal; it is only the 100 % end of the same render-scale slider. Frame-rate target: JJ is at
almost 90 now; aim for 120 Hz (Quest 3 at 120) at the faster presets.**
**Limit to check first (D0):** DLSS only removes graphics-card work. 120 Hz = 8.3 ms per frame for the game thread,
the render thread (two eye renders) AND the graphics card. If the processor side is already near 8-11 ms, DLSS
alone cannot reach 120 - then the next lever is the render thread (the second eye's draw work).

### Stage D1 - DLSS upscaling per eye, first version (upscale where the eye picture is finished)
- Each eye renders smaller: AKN/AKVR's eye render size x a **render scale** (100 % / 77 % / 67 % / 58 % / 50 % per
  side = DLAA / Ultra quality / Quality / Balanced / Performance; 67 % draws ~2.2x fewer pixels).
- Load NVIDIA's NGX with the **newest DLSS 4.x Super Resolution DLL - NOT DLSS 5** (JJ 2026-10-08: version 5 has
  no upscaling), one DLSS feature per eye (own history).
- Per eye: jitter in AKN's lens (in render-size pixels); motion vectors + depth from that eye's render (small size).
- **Upscale point: the end of each eye's render**, where AKN already copies the finished eye picture into the
  headset image. DLSS reads the small finished picture and writes the full-size eye straight into the headset
  image. The whole game chain (grading, glows, exposure, DoF block) runs untouched at the small size.
  DLSS input is then tonemapped (DLSS supports this; slightly lower quality than Luma's earlier placement).
- The game's SMAA is skipped (DLSS does the anti-aliasing); texture sharpness: a negative mip bias for the smaller
  render (Luma does the same, -1 at the output size).
- Panel: DLSS off / on, render scale, preset, sharpness.
- Tests in one launch: (a) frames/s and GPU ms at 100 / 67 / 58 / 50 % against today's native render (the reason
  for all this); (b) sharpness: crops of the same spot, native vs each scale; (c) head-turn trails: fake head sweep;
  (d) no leak between eyes (move one eye's camera, the other must not change); (e) rain smear check.
- PASS: a clear frame-rate gain over native (JJ ~90 now; target 120 at Balanced / Performance if the processor side
  allows - D0 says) and no trails above native.
  Then JJ in the headset. FAIL (trails): check each eye's motion vectors (camera part from its own previous view)
  and the jitter sign; FAIL (too soft): D2.

### Stage D2 - better quality: upscale earlier (only if D1 looks too soft)
- Move the upscale to the SMAA point (before tonemapping, like Luma) so bloom, grading and grain are drawn at full
  size on top of a sharp upscaled scene. Needs every pass after that point at full size while the scene ran small:
  test the game's own ScreenPercentage + UpscaleScreenPercentage (BmSystemSettings.ini, now 100 / True) - if the
  scene renders into a corner of full-size targets and the game stretches it late, DLSS replaces that stretch.

### Stage D3 - JJ's headset pass and settings
- Panel: DLSS off / on, render scale (DLAA ... Performance), preset, sharpness; saved.
- Ship notes: credit Luma (Filippo Tarpini, Garamond) in the readme; DLSS DLL licence (NVIDIA RTX SDK, redistribution
  allowed with the notice).

## 4. Risks we know about

| Risk | What we would see | Answer |
|---|---|---|
| SMAA block not running in VR settings | no swap point | turn the DoF block on, drop the blur (D0) |
| Motion vectors wrong for the eye cameras | trails on head turns | each eye has its own view state; verify, else write our own camera motion vectors |
| Rain / particles without motion vectors | smearing rain | draw rain after DLSS, or accept |
| DLSS on deferred context | crash / nothing | run it at the immediate playback point |
| Cost of two DLSS instances | about 1-2 ms per eye | paid back many times by the smaller render |
| Turned / asymmetric eye lenses | none expected | DLSS works on any projection; the jitter is in pixels |

## 5. Order and time

D0 one launch; D1 two to three launches + JJ (upscaling from the start); D2 only if D1 looks too soft.
