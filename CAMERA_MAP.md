# AKVR — Arkham Knight Camera Map (M2/M3 spec)

Source: codenamegamma "Batman Arkham Knight Freecam Table" (2018).
**Verified against the installed 2023 build** (`BatmanAK.exe`, 54,632,960 bytes)
on 2026-07-23 — all three camera signatures found and UNIQUE.

## Read hook — locating the camera object

AOB (unique): `0F 2E 83 70 05 00 00`  → instr `ucomiss xmm0,[rbx+0x570]`
At this instruction **`rbx` = camera object base**. Capture it here, or resolve
the base ourselves via AOB scan + hook, and read the fields below.

## Camera fields (offsets from camera base)

| Field   | Offset  | Type  | Notes |
|---------|---------|-------|-------|
| Pos X   | `0x574` | float | world units |
| Pos Y   | `0x578` | float | (table label "Y Axie") |
| Pos Z   | `0x57C` | float | |
| Yaw     | `0x580` | int32 | UE3 rotator: 65536 = 360 deg |
| Pitch   | `0x584` | int32 | UE3 rotator |
| Roll    | `0x588` | int32 | UE3 rotator (table label "Tilt") |
| FOV     | `0x58C` | float | degrees |

Nearby: `0x570` is a float compared in the read hook; the game copies the
`0x570` block into a `0x590` block each frame (desired->live). Watch during M2
to confirm which block our head-pose write must target.

## Write suppression — making our values stick (M3)

To stop the game overwriting the camera each frame, NOP these (verified unique):

- **Position + rotation writes** — AOB `89 83 74 05 00 00 8B 47`
  Original bytes (48): `89 83 74 05 00 00 8B 47 04 89 83 78 05 00 00 8B 47 08
  89 83 7C 05 00 00 8B 47 0C 89 83 80 05 00 00 8B 47 10 89 83 84 05 00 00 8B 47
  14 89 83 88 05 00 00`
  (These are `mov [rbx+0x574],eax` ... through `+0x588` — NOP the 6-byte movs,
  keep the `8B 47 xx` loads. See table's KillX db for exact NOP pattern.)
- **FOV write** — AOB `89 83 8C 05 00 00 48 8B 5C 24 30` → NOP first 6 bytes
  (`mov [rbx+0x58C],eax`).

## Implementation plan

- **M2 (read):** AOB-scan `0F 2E 83 70 05 00 00` in our DLL, mid-function hook to
  capture `rbx`, publish live pos/rot/FOV to the ImGui overlay. Verify values
  track as JJ flies the freecam.
- **M3 (write):** apply KillX + ModFOV NOP patches, then write our own
  pos/rot/FOV (head pose + gamepad) into the fields each frame.

Rotation is int32 rotator, not float — head-tracking math must convert
radians <-> rotator units (65536/360 per degree) when we reach M4.

## Field mapping CORRECTION (verified in-game 2026-07-23)

yaw/pitch are the REVERSE of the table labels:
`0x580 = PITCH (+up)`, `0x584 = YAW (+turn right)`, `0x588 = ROLL`.

## Camera-finalize function + M4 additive injection point

Disassembled (capstone, VA @ imagebase 0x140000000). The function that ends at
`0x140129801 ret` writes the final camera each frame:

```
0x140129744  ucomiss xmm0,[rbx+0x570]   <- M2 read-hook (rbx = cam base)
   ... conditional copy of 0x570-block -> 0x590-block (previous-frame snapshot)
0x1401297bb  mov [rbx+0x574],eax  (from [rdi])     X   <- KillX block start
   ... Y,Z,pitch,yaw,roll written from [rdi+..]
0x1401297f1  mov [rbx+0x58c],eax  (from [rdi+0x18]) FOV <- ModFOV
0x1401297f7  mov rbx,[rsp+0x30]   <- EPILOGUE: camera is FINAL, rbx still=cam base
0x1401297fc  add rsp,0x20
0x140129800  pop rdi
0x140129801  ret
```

- `rdi` = game's source camera (controller-driven base). Fields copied into
  `rbx+0x574..0x58C`. This is the LAST write per frame.
- **M4 additive hook = at the epilogue `0x1401297f7` (== ModFOV_addr + 6).**
  Replace the 5-byte `mov rbx,[rsp+0x30]` with a jmp to our cave; at entry
  rbx = cam base, all fields written. Add head-pose offset in place, run the
  original instruction, jmp back. NO write-suppression → controller keeps steering.
- Rotation add is pure int32 (add [rbx+0x584],eax etc.) — simple stub, no xmm.
  Positional lean (float adds to 0x574/578/57C) is a later pass.
- Anchor the hook off the unique ModFOV AOB (`89 83 8C 05 00 00 48 8B 5C 24 30`)
  + 6 bytes, so it stays version-robust.

## Projection jitter hook — `BuildProjectionMatrix` (from Luma, VERIFIED on our exe 2026-07-25)

Source: `Filoppi/Luma-Framework`, `Source/Games/Batman Arkham Knight/main.cpp`
(open source; the Nexus release was pulled but the code is public).

**RVA `0x104FF`** (Steam; GOG is `0x104BF`) → VA **`0x1400104FF`**, 12 stolen bytes.
Confirmed byte-for-byte against our installed `BatmanAK.exe` (54,632,960 bytes):

```
0x1400104FF  48 89 43 20                 mov [rbx+0x20], rax
0x140010503  48 C7 43 2C 00 00 80 3F     mov qword [rbx+0x2C], 1.0f
```

Luma patches this with a 12-byte jmp to a cave that does `mov rax,[&g_jitters]`
(loading TWO packed floats) immediately before the stolen `mov [rbx+0x20], rax`.
So in this struct:

| Offset | Meaning |
|---|---|
| `[rbx+0x20]` | projection jitter **X** (float, NDC units) |
| `[rbx+0x24]` | projection jitter **Y** (float, NDC units) |
| `[rbx+0x2C]` | 1.0f (scale, untouched) |

Luma feeds it `HaltonSequence(i,2) * 2 / renderWidth` (and `-…(i,3)*2/renderHeight`),
i.e. a **sub-pixel NDC offset applied to the projection matrix**.

**Why we care (independent of DLAA):** an NDC X-offset applied *per eye* is exactly a
**convergence** control — it slides the stereo window without moving the camera. We
currently only have *separation* (`[`/`]`, camera offset along the right vector); this
is the other half of the geo-11-style stereo pair and the knob M5's stereographer pass
wants. It needs no ReShade and no Luma at runtime — just a 12-byte patch in the same
hand-assembled style as our epilogue stub.

⚠️ Before using it: confirm what the game normally writes to `[rbx+0x20]`. AK's AA is
SMAA **1x** (spatial — shader hashes below), so the jitter is likely 0 in normal play,
but verify rather than assume.

AK's own AA passes (Luma's shader hashes, useful for finding them in RenderDoc):
`SMAAEdgeDetection` PS `0x067FF80F` · `SMAABlendingWeightCalculation` PS `0x2F55F98E` ·
`SMAANeighborhoodBlending` CS `0x42C0137E`. Motion vectors exist as the only
`DXGI_FORMAT_R16G16_FLOAT` render target, in [-1,1] range; depth is SRV1 of the SMAA
edge-detection pass.

### Convergence patch — implemented 2026-07-24

A 12-byte jmp at `0x1400104FF` routes to a hand-assembled cave that loads a packed
X/Y float pair from a writable slot and then runs the stolen `mov [rbx+0x20], rax` /
`mov qword [rbx+0x2C], 1.0f` instructions. The X value is `±convergence` per AER eye
(left = `+convergence`, right = `-convergence`), Y is 0. The slot is refreshed every
Present from `akvr_head_set_eye`, so the eye that is about to render gets the correct
offset. The value is exposed as a persisted slider in the in-headset settings menu
(range −0.1 to +0.1 NDC X). When VR head-tracking is off the jitter is forced to 0 so
the monitor view is not shifted.

## As-built (M4 shipped, calibrated in-game 2026-07-24)

Additive epilogue stub reads pre-computed deltas from code-cave slots and adds them
to the final camera fields (no suppression — controller still steers):

| Cave slot | Field added to | Type | Notes |
|---|---|---|---|
| `+0x80` | `[rbx+0x580]` pitch | int32 | rotator delta |
| `+0x84` | `[rbx+0x584]` yaw   | int32 | rotator delta |
| `+0x88` | `[rbx+0x588]` roll  | int32 | rotator delta |
| `+0x90/94/98` | `[rbx+0x574/578/57C]` pos X/Y/Z | float | lean, pre-rotated into world (movss/addss inside an xmm0 save/restore) |
| `+0x9C` | `[rbx+0x58C]` FOV | float | absolute-locked to the Quest per-eye FOV |

- **Head-pose signs (confirmed):** `YAW = -1` (OpenXR +yaw = left, game +yaw = right),
  `PITCH = -1`, `ROLL = -1`.
  `PITCH` moved `+1 -> -1` on 2026-07-25 when the compose-and-overwrite path replaced the
  additive one (in-game: looking up looked down). Reason: the composition pitches by rotating
  about `right0 = (-sin yaw, cos yaw, 0)`, which for `fwd=+X, up=+Z` actually points LEFT
  (`fwd x up = -right0`), so a positive rotation about it pitches DOWN while OpenXR's +pitch
  means looking UP. Roll is unaffected — rotating about `fwd` decomposes 1:1.
- **head-turn roll — FIX BUILT + DEPLOYED 2026-07-25, awaiting in-game test.** History: the
  rotation delta was originally extracted as yaw/pitch/roll and ADDED to the rotator fields, which
  is not rotation composition; the stub was then changed to save the pure base so C++ could
  compose base∘head and *overwrite* the rotator (cave slots `+0xA0/A4/A8`). That was necessary but
  not sufficient — two further bugs remained in `akvr_head_update`:
  1. **Head yaw was applied about the base camera's own `up` vector.** The third-person camera
     usually sits pitched down at Batman, which tilts its local up forward; rotating about a
     tilted axis tips the horizon. Head yaw now turns the basis about **world up (+Z)**; pitch
     still about the resulting right, roll about the resulting forward.
  2. **The recenter reference stored the full head orientation.** `conj(ref)*cur` expresses the
     delta in the reference's own frame, so any pitch you had at recenter time tilts the axes the
     Euler extraction reads. Recenter is now **yaw-only**: `delta = Ry(-refYaw) * cur`. OpenXR
     LOCAL space is gravity-aligned, so head pitch/roll are absolute and pass through untouched.
     This explains why swapping the quaternion multiply order never worked — `conj(ref)*cur` leaks
     a recentre *pitch* into yaw, `cur*conj(ref)` leaks a recentre *yaw* into roll. Neither plain
     order is correct; only removing the heading alone is.

  Verify with the "ROLL DIAG" overlay line (head-roll vs camera-final-roll): pitch the camera down
  with the right stick, then sweep your head left/right — camera-final roll should stay ~0.
- **Lean basis** (matches freecam): forward = `(cos yaw, sin yaw)`, right =
  `(-sin yaw, cos yaw)`, +Z up; room axes (right=+X, up=+Y, fwd=-Z) rotated by the
  game's finalized yaw, scaled by `posScale` (live-tunable, default 200; ~80
  units/metre is closer to true 1:1 per the AER depth data point).
- **FOV** is written as an absolute lock (recover game base FOV = `cur − dFov`, then
  set `dFov` so the result equals the headset's real horizontal FOV) — the render FOV
  must equal the display FOV or the world swims.

## HEAD ROLL in stereo — RESOLVED 2026-07-27

Rolling the head split the stereo (couldn't fuse), even held still. The `ROLL` diagnostic
overlay line (`head` / `game` / `display` roll) exposed the cause: at a tilt, `head` and
`game` read the same (e.g. +37.7) but `display` reads the **opposite sign** (−37.0) — the
OpenXR compositor and the game use opposite roll-sign conventions. The AER eye separation was
being offset along the *game's* roll (+37) while the compositor displays at −37, so the parallax
and the eyes ended up ~2×37° apart → vertical disparity → split.

- **Fix (stereo):** build the eye-offset right vector from the **negated** final roll
  (`build_basis(fyaw, fpitch, -froll)`), so the separation matches the display's roll sign.
  Zero at upright, so upright is unchanged. `akvr_head_update`, camera.cpp.
- **Fix (false roll):** the interim swing-twist roll caused a *false* roll when combining
  yaw+pitch (combined yaw+pitch has a nonzero twist about the forward). Reverted to the
  Tait-Bryan Euler roll (`quat_to_euler_ypr`), which is exactly 0 for pure yaw+pitch, with
  `ROLL_SIGN = -1`. So: composition roll = Euler (no false roll), eye-offset = −froll (fuses).
- Diagnostic line + `quat_rot` helper left in place; `akvr_xr_layer_roll()` exposes the display roll.

## THE SQUARE — projVR (projection-aspect), 2026-07-27

Not a resolution/viewport hack — we MinHook `BuildProjectionMatrix` (entry = kProjPat − 0xDF)
and, on the main 16:9 camera only (out `m00/m11` ratio ≈ 0.5625), overwrite `m[0]=1/tan(hH)`,
`m[5]=1/tan(hV)` with the Quest's per-eye half-FOV (`akvr_xr_eye_half_fov`, trimmed to match the
present layer). xr.cpp then presents the FULL frame at that FOV (`g_projvrOn` skips the crop) —
true square, all pixels used, no crop waste. Toggle "square view (PROJECTION)". Depth terms
untouched. Render resolution is still AK's fixed ~2423×1363 (unsolved — see the plan/memory).

