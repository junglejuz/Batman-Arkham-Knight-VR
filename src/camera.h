// AKVR — camera capture (Milestone 2)
#pragma once
#include <cstdint>

struct CameraView
{
    bool     valid;                 // true once the hook has fired at least once
    float    x, y, z;               // world position (offsets 0x574/578/57C)
    int32_t  yaw, pitch, roll;      // UE3 rotator units, 65536 = 360deg (0x580/584/588)
    float    fov;                   // degrees (0x58C)
};

// Scan BatmanAK.exe for the camera injection point and install the capture
// hook. Idempotent + thread-safe to call again (e.g. F9 retry). Returns true
// once the hook is in place.
bool         akvr_camera_install();
bool         akvr_camera_installed();
uintptr_t    akvr_camera_target();   // resolved injection address (0 if not found)
uint64_t     akvr_camera_finalize_count(); // # times the camera-finalize fn ran (M4a-0 cadence)
CameraView   akvr_camera_read();     // snapshot of the live camera fields
void         akvr_camera_shutdown(); // restore all hand-assembled patches (DLL detach / exit)

// ---- Milestone 3: take control of the camera ----
struct FreecamState
{
    bool  on;
    bool  suppressed;               // are the game's camera writes NOP'd?
    float x, y, z;                  // our commanded position
    float yaw, pitch, roll;         // our commanded rotation (degrees)
    float fov;                      // our commanded FOV
    float speed;                    // units/sec
};
void          akvr_freecam_toggle();   // seed-from-live + apply write-suppression / restore
bool          akvr_freecam_on();
void          akvr_freecam_update();    // call each rendered frame: input -> write fields
FreecamState  akvr_freecam_state();

// ---- Milestone 4a-3: additive head-tracking (VR) ----
// Unlike freecam, this does NOT suppress the game's camera writes. It installs
// a second hook at the camera-finalize EPILOGUE (ModFOV_addr + 6, the
// 'mov rbx,[rsp+0x30]' at VA 0x1401297f7) that ADDS our head-pose delta to the
// rotation fields after the game has written them — so the controller still
// steers and the head only adds a look offset on top (Luke Ross model).
struct HeadTrackState
{
    bool  on;
    bool  installed;                // epilogue additive stub built
    float yaw, pitch, roll;         // current applied rotation delta from recenter (degrees)
    float leanX, leanY, leanZ;      // current head lean from recenter (meters)
    float posScale;                 // world-units-per-meter lean gain (live-tunable)
    float fovDelta;                 // degrees added to the game FOV in VR (live-tunable)
};
bool           akvr_head_install();    // build the epilogue additive stub (lazy, idempotent)
void           akvr_head_toggle();     // on/off; auto-recenters on enable, disables freecam
void           akvr_head_recenter();   // set the current head pose as the zero reference
void           akvr_head_update();     // call each Present: pose -> delta -> cave int32 slots
HeadTrackState akvr_head_state();
void           akvr_head_pos_scale_mul(float f);  // multiply the lean gain (live dial)
float          akvr_head_pos_scale();
void           akvr_head_pos_scale_set(float v);  // set lean gain directly (slider)
void           akvr_head_fov_add(float d);        // add to the VR FOV offset (live dial)
float          akvr_head_fov_delta();
void           akvr_head_fov_set(float v);        // set the VR FOV offset directly (slider)
void           akvr_head_set_eye(int e);          // AER: 0=left,1=right (xr sets it each Present)

// ---- Yaw folding (the spin-flicker fix): publish the GAME camera's own heading ----
// The base (controller-driven) camera yaw in degrees, as saved by the epilogue stub
// BEFORE our head rotation is composed in. The display side needs exactly this: the
// runtime already corrects head turns (we hand it each eye's render-time head pose),
// but a STICK turn is invisible to it, so the stale AER eye keeps the old heading.
// Head rotation must NOT be included here or the stale eye would be corrected twice.
// Returns false when there is nothing meaningful to report (stub idle / VR off).
bool           akvr_camera_base_yaw_deg(float& deg);
void           akvr_head_stereo_add(float d);     // dial stereo separation / depth strength
float          akvr_head_stereo();                // current half eye-separation (world units)
void           akvr_head_stereo_set(float v);     // set depth strength directly (slider)

// ---- Shoulder cancel: slide the viewpoint sideways out of AK's over-the-shoulder framing ----
void           akvr_head_shoulder_set(float v);   // world units; + = viewpoint moves RIGHT
float          akvr_head_shoulder();
void           akvr_head_shoulder_add(float d);
// ---- Decoupled pitch: how much of the game camera's own tilt survives in gameplay ----
void           akvr_head_pitch_keep_set(float v); // 0 = level horizon, 1 = game's full tilt
float          akvr_head_pitch_keep();
void           akvr_head_pitch_unlink_set(int mode); // 0 level, 1 tilt + level horizon, 2 tilt, world tipped
int            akvr_head_pitch_unlink();
void           akvr_head_stick_height_set(float m); // STICKHEIGHT: extra lift, metres at full stick
float          akvr_head_stick_height();
// The other two axes of the same offset. All three ride the FINALIZED camera basis,
// so they stay glued to the camera however it is pitched or rolled.
void           akvr_head_offset_up_set(float v);  // + = viewpoint moves UP
float          akvr_head_offset_up();
void           akvr_head_offset_fwd_set(float v); // + = viewpoint moves FORWARD
float          akvr_head_offset_fwd();
// Master switch for every camera change made on 2026-08-05 (the 3-axis offset here and
// the spin correction in xr.cpp), so ONE tick box reverts the lot for an A/B. OFF = the
// exact pre-2026-08-05 camera arithmetic.
void           akvr_head_camfix_set(bool on);
bool           akvr_head_camfix();
// Camera trace: every finalize is recorded into a ring buffer; this writes the last
// ~48 s to CSV. Returns the number of rows written (0 = nothing recorded yet).
int            akvr_camera_trace_dump(const wchar_t* path);

// ---- RE02: make the game's own viewport SQUARE instead of 16:9 ----
// The game paints a centred 16:9 rect inside its client area (that's the mail-slot;
// see camera.cpp for the located function and the verified arithmetic). This patches
// that computation to emit a centred SQUARE instead, so the game genuinely renders
// square with a square projection. Applied at startup, before the first frame.
// Returns false if the pattern isn't found (then nothing is changed).
bool      akvr_vp_square_install();
bool      akvr_vp_square_on();
uintptr_t akvr_vp_square_addr();

// ---- RE02: wide-render / centre-crop to escape the 16:9 mail-slot ----
bool  akvr_head_wide_render();      // true = render wider than headset HFOV, then crop to it
void  akvr_head_wide_render_set(bool on);
float akvr_head_render_hfov();    // HFOV the game is currently being forced to render at

// ---- RE04 Win 1: projection convergence (NDC X offset per eye) ----
void           akvr_head_convergence_add(float d); // dial convergence
float          akvr_head_convergence();            // current NDC X convergence offset
void           akvr_head_convergence_set(float v); // set convergence directly (slider)

// ---- projVR: rewrite the game projection to the Quest FOV (engine-level square) ----
bool           akvr_projvr_install();   // MinHook BuildProjectionMatrix (call once, at install)
bool           akvr_projvr_ok();        // hook installed?
uint64_t       akvr_projection_observation_count(); // matched builds, even with projVR off
const char*    akvr_projection_seen();              // every distinct projection shape built (diagnostic)
bool           akvr_projvr();           // rewrite currently ON?
void           akvr_projvr_set(bool on);// toggle the projection rewrite (+ present side)
void           akvr_projvr_diag(float& ratio, int& hits, float& farp);  // diagnostics
// Both projection axes before/after the rewrite, plus the ones we rejected.
void           akvr_projvr_diag2(float& origH, float& origV, float& postH, float& postV,
                                 int& skips, float& skipFov, float& skipRatio);
