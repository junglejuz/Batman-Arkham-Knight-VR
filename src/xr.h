// AKVR — OpenXR (Milestone 4)
#pragma once
#include <cstdint>
struct ID3D11Device;
struct IDXGISwapChain;

// Idempotent: safe to call repeatedly until it connects (runtime may not be up
// at game launch). Creates an OpenXR instance + HMD system. Session creation and
// the whole frame loop now live in the Present hook (see akvr_xr_frame_*).
void        akvr_xr_try_init();
bool        akvr_xr_ok();               // instance + HMD system acquired
const char* akvr_xr_status();           // instance-level status for the overlay

void        akvr_xr_set_device(ID3D11Device* dev);  // hooks.cpp hands us the game's device

// Session/pose (Milestone 4a-1): live head orientation, degrees.
bool        akvr_xr_session_running();
bool        akvr_xr_head_pos_valid();    // true once 6DOF position tracking has been valid
const char* akvr_xr_session_status();
void        akvr_xr_head_deg(float& yaw, float& pitch, float& roll);  // absolute head euler
// Full head pose: euler (deg) + position (meters, OpenXR LOCAL space, +X right/+Y up/-Z fwd).
void        akvr_xr_head_pose(float& yaw, float& pitch, float& roll, float& px, float& py, float& pz);
// Raw orientation quaternion (OpenXR convention: +Y up, -Z fwd, right-handed). The
// injection removes only the recenter HEADING from this (delta = Ry(-refYaw)*cur),
// NOT by subtracting separately-extracted Euler angles and not by a full-orientation
// quaternion delta — both leak axes into each other (e.g. a pure yaw turn reading as
// roll once pitch is non-level, which is normal head posture). LOCAL space is
// gravity-aligned, so head pitch/roll are absolute and pass through untouched.
void        akvr_xr_head_quat(float& qx, float& qy, float& qz, float& qw);

// ---- Milestone 4b: headset display (frame loop, driven from the Present hook) ----
// Call ONCE per Present, in this order:
//   akvr_xr_frame_begin();   // pump events, lazy session/swapchain, xrWaitFrame+
//                            // xrBeginFrame, sample the ONE predicted head pose,
//                            // publish it for the camera injection.
//   ... camera injection (akvr_head_update) reads the published pose ...
//   akvr_xr_frame_submit(swapChain, gameFovDeg);  // copy the game frame into the
//                            // headset swapchain and xrEndFrame() a projection
//                            // layer. Balances every begin (0 layers if nothing
//                            // to show), so the two are always called as a pair.
void        akvr_xr_frame_begin();
// gameplay = is the 3D game camera live this frame? When true the layer is pinned
// to the head (world-lock). When false (splash / loading / menus — no camera to
// counter-rotate) the layer is frozen to a world-fixed pose so the 2D screen
// floats in place instead of being stuck to your face.
void        akvr_xr_frame_submit(IDXGISwapChain* swapChain, float gameFovDeg, bool gameplay);
bool        akvr_xr_displaying();       // true once we're submitting frames to the HMD
const char* akvr_xr_display_status();   // headset-display line for the overlay
void        akvr_xr_screen_mode_toggle(); // F6: force the image to float world-fixed (menus)
bool        akvr_xr_screen_mode();
bool        akvr_xr_screen_frozen();               // screen mode AND the camera ignores the head
bool        akvr_xr_pause_live();                  // PAUSELOOK: paused, shown in full view, head drives the camera
bool        akvr_xr_pause_look();                  // PAUSELOOK: the setting
void        akvr_xr_pause_look_set(bool on);
bool        akvr_xr_hud_eye_follow();              // HUDEYES: HUD quads shifted by the real eyes' offsets
void        akvr_xr_hud_eye_follow_set(bool on);
unsigned long long akvr_xr_menu_start_tick();      // MENUSTART: GetTickCount64 when the 3D main menu first came up (0 = not yet)
float       akvr_xr_pause_menu_size();             // PAUSESIZE: the pause menu's size on the room layer (0.2..1)
void        akvr_xr_pause_menu_size_set(float v);
bool        akvr_xr_pause_no_back();               // PAUSENOBACK: the pause menu's dark backing left out of the layer
void        akvr_xr_pause_no_back_set(bool on);
float       akvr_xr_pause_dim();                   // PAUSEDIM: how much darker the paused world is (0..0.9)
void        akvr_xr_pause_dim_set(float v);
bool        akvr_xr_screen_track();                // SCREENTRACK: head tracking in the floating screen
void        akvr_xr_screen_track_set(bool on);
float       akvr_xr_screen_aspect();               // SCREENWIDE: 0 = picture shape, else width/height
float       akvr_xr_float_screen_scale();          // SCREENSIZE: floating-screen size (fraction of full view)
void        akvr_xr_float_screen_scale_set(float s);
void        akvr_xr_screen_aspect_set(float a);

// Menu/loading screens (screen-mode) presented at the full gameplay FOV wrap
// edge-to-edge around you — too big to see at once. This shrinks the LAYER's
// angular FOV in screen-mode (fraction of the full headset FOV, 0.1-1.0) so the
// flat image reads like a normal-sized virtual screen instead. Doesn't affect
// gameplay's 1:1 FOV. Live-tunable (Insert = bigger, Delete = smaller).
void        akvr_xr_menu_zoom_mul(float factor);   // multiply the scale (e.g. 1.1, 1/1.1)
float       akvr_xr_menu_zoom();
void        akvr_xr_menu_zoom_set(float v);        // set the screen-mode scale directly (slider)
float       akvr_xr_pause_zoom();                  // size of auto-detected screens (pause/map/loading)
void        akvr_xr_pause_zoom_set(float v);
bool        akvr_xr_auto_main_menu();              // treat the first camera-live stretch after launch as the main menu
void        akvr_xr_auto_main_menu_set(bool on);
bool        akvr_xr_main_menu_detected();          // currently in that main-menu phase
void        akvr_xr_set_screen_hold(bool on);      // ENTRYHOLD: floating screen keeps its last picture
void        akvr_xr_set_anamorphic(bool on);       // this frame's 3D went through projVR (present at its shape)
const char* akvr_xr_mode_log();                   // timeline of mode changes since launch
void        akvr_xr_set_camera_pos(bool ok, float x, float y, float z);  // game camera, for main-menu recognition
float       akvr_xr_eye_v_offset();                // FULLVIEW off-centre vertical shift (NDC) for the projection
bool        akvr_xr_full_view();                   // cover each eye's real frustum (wider H, off-centre V)
void        akvr_xr_full_view_set(bool on);
bool        akvr_xr_tilt_fill();                   // TILTFILL: pitch the pose down to fill the bottom
void        akvr_xr_tilt_fill_set(bool on);
float       akvr_xr_tilt_used_deg();               // tilt applied to the last sampled pose
int         akvr_xr_fps_lock();                    // FPSLOCK: 0 off, 45 / 40 / 30
void        akvr_xr_fps_lock_set(int fps);
void        akvr_xr_fps_lock_info(int &div, double &hz, long &late, long &frames);
bool        akvr_xr_sbs_one();                     // SBSONE: native mode = one side-by-side swapchain
void        akvr_xr_sbs_one_set(bool on);
bool        akvr_xr_sbs_active();
bool        akvr_xr_ofxr_active();                 // OFXRBRIDGE: frame-generation layer loaded (lock stands down)
bool        akvr_xr_fps_lock_ssw();                // FPSLOCK-SSW: skip slots, VD fills in
void        akvr_xr_fps_lock_ssw_set(bool on);
long        akvr_xr_fps_lock_early();
bool        akvr_xr_menu3d_active();               // MENU3D: main menu live + head-tracked, text pinned
bool        akvr_xr_menu3d();
void        akvr_xr_menu3d_set(bool on);
bool        akvr_xr_menu_flat();                   // MENUFLAT: floating screens show one eye to both
void        akvr_xr_menu_flat_set(bool on);
bool        akvr_xr_menu3d_offset(float& dyawDeg, float& dpitchDeg);   // head turn since the pinned menu appeared

// Quest 3's real per-eye FOV (degrees), measured from the runtime. The game is
// forced to render at the horizontal value so the world stays 1:1.
// The refresh rate the OpenXR runtime is pacing us to (from xrWaitFrame's nominal
// frame period). This is Virtual Desktop's CHOSEN refresh, not the headset's maximum
// — so a low value here means a VD setting, not a slow GPU. 0 = not running yet.
float       akvr_xr_headset_hz();

float       akvr_xr_headset_hfov_deg();
float       akvr_xr_headset_vfov_deg();

// AER: which eye (0=left, 1=right) the camera injection should offset for this
// Present; -1 = centered (native stereo — geo-11 owns the separation).
// hooks.cpp bridges this to akvr_head_set_eye before akvr_head_update.
int         akvr_xr_inject_eye();

// ---- Milestone 5: native stereo (geo-11 SBS) ----
// When the geo-11 d3d11.dll proxy is detected in the game folder, we default to
// native mode: geo-11 dual-renders every frame and composites side-by-side; we
// split the halves into the two eye swapchains. AER remains the fallback.
bool        akvr_xr_native();
void        akvr_xr_native_toggle();   // \ key: switch NATIVE(SBS) <-> AER (rebuilds swapchains)

// Square presentation: frame a centred SQUARE out of the game's render in the headset
// (matched square FOV). Presentation-side only — never touches the game's resolution.
bool        akvr_xr_square_present();
void        akvr_xr_square_present_set(bool on);

// Scene TAP: give xr the game's own near-square high-res scene render target to send to
// the eyes instead of the 16:9 backbuffer (nullptr = back to the backbuffer). hooks.cpp
// finds this texture via OMSetRenderTargets and calls this each frame.
struct ID3D11Texture2D;
void        akvr_xr_set_scene_source(ID3D11Texture2D *tex);
const char* akvr_xr_tap_status();      // scene-tap copy diagnostic (sizes/formats/verdict)
void        akvr_xr_set_projvr(bool on);           // camera.cpp: projection rewritten to Quest FOV → present full frame
float       akvr_xr_layer_roll();                  // roll (deg) of the DISPLAY layer pose (roll-stereo diag)

// ---- Yaw folding: the SPIN-FLICKER fix -------------------------------------
// Under AER one eye is always a frame behind. The runtime time-warps that stale eye
// for HEAD rotation (we submit each eye's render-time pose) but knows nothing about a
// STICK turn, so during a spin the two eyes sit a large angle apart, cannot be fused,
// and the visual system alternates between them — the flicker. This folds the game
// camera's own turn into the pose we submit, so a stick spin looks to the runtime
// exactly like a head turn and its existing correction handles it.
// The ON/OFF switch is kept separate from the gain on purpose: an A/B needs a switch you
// can flick back and forth without losing the value you tuned to.
// Gain: 1.0 = full correction, negative = flip the direction (a sign A/B that needs no
// rebuild). Partial by nature in a third-person game — see akvr/README.md.
bool        akvr_xr_yawfold_on();
void        akvr_xr_yawfold_on_set(bool on);
float       akvr_xr_yawfold();
void        akvr_xr_yawfold_set(float gain);
const char* akvr_xr_yawfold_diag();
void        akvr_xr_eye_half_fov(float& h, float& v); // Quest per-eye half-angles (rad) for the projection hook
void        akvr_xr_fov_report(char* buf, int n);     // raw per-eye frusta vs the symmetric FOV we submit
bool        akvr_xr_fov_measured();                   // false until xrLocateViews has given us REAL frusta
void        akvr_xr_notify_resize();   // hooks.cpp calls on ResizeBuffers so the eye swapchains rebuild
// Our OWN surfaces, so the viewport/RT observers in hooks.cpp can tell the GAME's
// rectangles apart from ours. Without this, a size we created reads exactly like a
// size the engine chose, and a whole night gets spent matching against our own draws.
void        akvr_xr_our_sizes(uint32_t& eyeW, uint32_t& eyeH, uint32_t& ovW, uint32_t& ovH);
void        akvr_xr_backbuffer_size(uint32_t& w, uint32_t& h);  // the game's live render resolution
const char* akvr_xr_hud_layer_diag();   // HUDLAYER: the head-locked HUD quad
void        akvr_xr_game_tan(float& th, float& tv);   // RETSQUASH: tan half-angles of the game frame
int         akvr_xr_hud_space();        // HUDLAYER4: 0 room space re-placed per frame, 1 view space (attached
void        akvr_xr_hud_space_set(int v);  //   to the head), 2 fixed in the room (HUDWORLD, default)
void        akvr_xr_hud_reanchor();     // HUDWORLD: hang the room-fixed HUD in front of the head again
int         akvr_xr_menu_pose_delay();  // MENUDELAY: head-pose delay on the live 3D main menu (JJ: 2)
float       akvr_xr_splash_zoom();      // SPLASHSIZE: size of the screens before the main menu (0.1..1)
void        akvr_xr_splash_zoom_set(float v);
float       akvr_xr_load_zoom();        // LOADSIZE: the loading screen into the game (after the main menu)
void        akvr_xr_load_zoom_set(float v);
float       akvr_xr_load_up();          // LOADUP: loading screens up (+) / down (-), degrees
void        akvr_xr_load_up_set(float v);
void        akvr_xr_menu_pose_delay_set(int frames);
// ZOOMVIG: our own peripheral vignette while the game's zoom narrows its FOV (or while previewing)
float       akvr_xr_vig_strength();     // 0 = off
void        akvr_xr_vig_strength_set(float v);
float       akvr_xr_vig_clear();        // clear centre, half-angle in degrees
void        akvr_xr_vig_clear_set(float d);
float       akvr_xr_vig_below();        // zoom = the game's own FOV narrower than this (deg)
void        akvr_xr_vig_below_set(float d);
void        akvr_xr_vig_preview(bool on);
bool        akvr_xr_vig_active();       // ZOOMHIDE: the zoom vignette is on (not a preview)
const char *akvr_xr_vig_diag();
int         akvr_xr_hud_colour();       // HUDLAYER4: 0 raw copy, 1 converted to linear premultiplied
void        akvr_xr_hud_colour_set(int v);
int         akvr_xr_hud_eyes();         // HUDLAYER5: 1 = one quad per eye with the eye shift
void        akvr_xr_hud_eyes_set(int v);
int         akvr_xr_hud_lazy();         // HUDLAYER5: 1 = lazy follow (small head wobble ignored)
void        akvr_xr_hud_lazy_set(int v);
float       akvr_xr_hud_lazy_deg();
void        akvr_xr_hud_lazy_deg_set(float d);
float       akvr_xr_hud_half_ipd();
void        akvr_xr_eye_image_size(uint32_t& w, uint32_t& h);   // ONE eye's image (half the width in geo-11 SBS)
void        akvr_xr_submitted_fov_deg(float& h, float& v);      // FULL angles we actually submitted last frame
ID3D11Texture2D* akvr_xr_katanga_texture();                     // geo-11's shared surface (F2 grab), may be null
// What the OpenXR runtime asks for per eye, and its hard ceiling. The recommended
// size already allows for the lens-distortion resample, so it is normally larger
// than the physical panel — rendering below it is what looks soft.
void        akvr_xr_recommended_size(uint32_t& w, uint32_t& h, uint32_t& mw, uint32_t& mh);

// ---- overlay quad layer (the floating UI panel) ----
// The UI is rendered into its OWN swapchain and submitted as a separate quad layer,
// so it's a floating panel independent of the game frame (no FOV/seam/per-eye hacks).
struct ID3D11RenderTargetView;
void        akvr_xr_overlay_size(uint32_t& w, uint32_t& h);   // panel render resolution
bool        akvr_xr_overlay_ready();                          // session up + swapchain creatable
bool        akvr_xr_overlay_begin(ID3D11RenderTargetView** rtvOut);  // acquire → RTV to draw into
void        akvr_xr_overlay_end();                            // release → include quad this frame
const char* akvr_xr_probe_status();   // "backbuffer: MONO/SBS ..." diagnostic for the overlay
const char* akvr_xr_katanga_status(); // geo-11 shared-texture capture status ("katanga: OPEN WxH ...")
bool        akvr_xr_kat_feed();               // ON = the eyes come from geo-11's shared surface
bool        akvr_xr_native_swap_eyes();       // route opposite SBS half to each eye
// EYEVIEW 2026-09-27 (SKVR port): each eye's own view, ~20% fewer pixels per row.
bool        akvr_xr_eye_view();                // this launch's setting
void        akvr_xr_eye_view_set(bool on);
void        akvr_xr_eye_applied_set(bool on);  // geo-11 confirmed at the eye-view S/C
bool        akvr_xr_eye_applied();
bool        akvr_xr_eye_gameplay();            // EVGAME: eye view wanted this frame (gameplay only)
float       akvr_xr_pause_aspect();            // PAUSE169: pause/map/loading screen shape, 0 = whole
void        akvr_xr_pause_aspect_set(float a);
float       akvr_xr_eye_k_head();              // (tO - tI) / (tO + tI) from the headset
float       akvr_xr_eye_k();                   // effective submission off-axis
float       akvr_xr_eye_k_setting();           // <0 = use the headset's k
void        akvr_xr_eye_k_set(float k);
bool        akvr_xr_eye_flip();
void        akvr_xr_eye_flip_set(bool on);
bool        akvr_xr_eye_narrow_ok();
bool        akvr_xr_eye_tangents(float& o, float& i, float& u, float& d);
void        akvr_xr_native_swap_eyes_set(bool on);
int         akvr_xr_pose_delay();              // Presents between pose publish and its image (0-3)
void        akvr_xr_pose_delay_set(int frames);
int         akvr_xr_pose_delay_used();         // the delay actually applied this Present
bool        akvr_xr_pose_auto();               // FRAMEID: pair poses exactly when possible
void        akvr_xr_pose_auto_set(bool on);
bool        akvr_xr_pose_matched();            // this Present used the exact match
const char* akvr_xr_pose_delay_hist();         // matched delays so far, delay:count
void        akvr_xr_kat_feed_set(bool on);    // A/B switch for the whole geo-11 path

// Measured CPU duration of this frame's xrWaitFrame; zero if not called.
double akvr_xr_wait_ms();
