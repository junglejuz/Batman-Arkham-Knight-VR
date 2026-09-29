// AKVR — OpenXR bring-up + session + headset display (Milestones 4a / 4b)
// ------------------------------------------------------------------
// Instance + system: confirm the runtime sees the HMD.
//
// M4b change: the whole OpenXR frame loop now runs FROM the Present hook (game
// render thread), not a background thread. This gives us the smoothness
// architecture the plan requires:
//   * ONE predicted head pose per rendered frame (rule #1)
//   * that same pose feeds BOTH the camera injection AND the submitted layer
//     pose (rule #2) — otherwise the runtime's reprojection double-counts head
//     motion and the world judders violently.
//
// Per Present:
//   frame_begin()  -> pump events, create session/swapchain lazily, xrWaitFrame
//                     (paces us to the headset), xrBeginFrame, locate the head
//                     pose at predictedDisplayTime, publish it (seqlock).
//   frame_submit() -> copy the game's finished backbuffer into an OpenXR
//                     swapchain image and xrEndFrame() a stereo projection
//                     layer (same image + head pose to both eyes = mono; real
//                     per-eye stereo/AER is M5). The layer pose is the pose
//                     that ACTUALLY drew this frame (one Present ago, due to
//                     the render->present pipeline), so the compositor
//                     time-warps it to "now".
// ------------------------------------------------------------------
#define _CRT_SECURE_NO_WARNINGS
#include "xr.h"
#include "frameid.h"  // FRAMEID: engine frame identity for pose pairing
#include "hudsplit.h" // HUDLAYER: the HUD image
#include "geo11conv.h" // HUDLAYER: HUD distance
#include "camera.h" // akvr_camera_finalize_count() — the per-rendered-frame clock for AER eye sync

// earlyres.cpp — "is geo-11 installed next to us?", answered from d3dxdm.ini in
// DllMain. True in BOTH of geo-11's install modes, which is why the native-stereo
// default keys on this and not on who owns d3d11.dll (see akvr_xr_set_device).
bool akvr_early_geo11();
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>   // HUDLAYER4: ID3DBlob (D3DCompile loaded at run time)
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod for the SSW-mode frame lock
#include <vector>
#include <windows.h>

namespace {
XrInstance g_inst = XR_NULL_HANDLE;
XrSystemId g_sys = XR_NULL_SYSTEM_ID;
bool g_ok = false;
char g_status[256] = "OpenXR: not started";
char g_runtime[128] = "";

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_ctx =
    nullptr; // game's immediate context (cached from device)

// session/pose (now owned by the Present thread)
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_localSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;  // session in begun state
bool g_posValid = false; // has 6DOF position tracking ever been valid?
char g_sessStatus[320] = "session: not started";

// Overlay panel anchor — the panel is a screen FIXED in the room (world-locked),
// not glued to your head. We capture a pose in front of you the moment the panel
// appears and leave it parked there, so leaning toward it makes it bigger and
// leaning away makes it smaller, like a real monitor. Re-anchored each time the
// panel is re-opened (so it always spawns in front of wherever you're looking).
XrPosef g_ovAnchor = {};
bool    g_ovAnchored = false;

// Headset swapchains — ONE PER EYE (AER). Each eye keeps its last frame while
// the other eye's is refreshed, so alternating single renders read as stereo.
XrSwapchain g_swap[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
std::vector<ID3D11Texture2D *> g_swapImages[2];
bool g_swapInit[2] = {false, false}; // has this eye had a first frame?
// SBSONE 2026-09-27 — OFXR Bridge's flight log (JJ, 03:22) showed it syncing with the
// headset once PER SWAPCHAIN: per eye image a ~10 ms wait for a free private image and
// another ~10 ms to release it, so two eye swapchains held the game ~42 ms per frame
// (game's own work ~7 ms) and it ran at 18-22 fps. geo-11's shared surface already
// holds both eyes side by side, so in native mode ONE double-width swapchain carries
// both, each view reading its half through subImage (OFXR supports two non-overlapping
// eye viewports in one image, as UEVR Native Stereo submits). Eye swap and the flat
// floating screen become just which half a view reads. `sbsswapchain=0` = two again.
bool g_sbsOne = true;                // setting: native mode uses one SBS swapchain
bool g_sbsActive = false;            // the swapchains that exist were made that way
// The half-angles actually submitted last frame (rad) — for the render-vs-display
// comparison in the startup log. See the note where they are assigned.
float g_subHalfH = 0.0f, g_subHalfV = 0.0f;
uint32_t g_swW = 0, g_swH = 0; // FULL backbuffer size (both eyes in native/SBS)
uint32_t g_eyeW = 0;           // per-eye image width (= g_swW/2 in native mode)
DXGI_FORMAT g_swFmt = DXGI_FORMAT_UNKNOWN; // format the eye swapchains were created with
char g_tapDiag[192] = "tap: (off)";        // scene-tap copy diagnostic for the overlay

// Square presentation: show a CENTRED SQUARE crop of the game's native render in the
// headset (matched square FOV), instead of the full 16:9 mail-slot. This is the square
// that worked before the (failed) attempts to force the game's own resolution — it never
// touched the game's render at all, it just framed a square out of it on the VR side.
// Correct, undistorted, no black band. Live-toggleable (no restart).
bool g_squarePresent = true;

// Scene TAP: instead of copying the game's 16:9 backbuffer to the eyes, copy the game's
// own near-square high-res SCENE render target (2496x2688 BGRA8, display-ready) — the
// full image the game renders before it crops to 16:9 for a flat monitor. hooks.cpp finds
// it (OMSetRenderTargets) and hands us the pointer each frame; nullptr = use the backbuffer.
ID3D11Texture2D *g_sceneSrc = nullptr;

// NATIVE STEREO (geo-11 SBS): the geo-11 d3d11.dll proxy renders BOTH eyes
// every frame and composites them side-by-side into the backbuffer (left eye
// left half). We split that composite into the two eye swapchains — no AER
// alternation, no camera eye-offset (geo-11 applies the separation inside the
// projection).
bool g_native =
    false; // active mode (auto-set when the geo-11 proxy is detected)
bool g_nativeSwapEyes = false; // source-half routing only; never alters the eye poses

// OVERLAY QUAD LAYER: the UI panel gets its OWN swapchain + its own composition
// layer (a flat billboard the runtime draws on top of the game, in both eyes,
// at a fixed real-world size/distance). This frees the overlay from the game
// frame entirely — no FOV counter-scaling, no eye-seam, no per-eye scaling.
// Panel render resolution. Taller than wide since the settings menu grew past the
// bottom of the old 1024x1024 square (reported 2026-07-25). The quad's metre size
// below keeps the SAME pixels-per-metre, so text stays exactly as large as before —
// the panel just extends further down.
const uint32_t g_ovW = 1440, g_ovH = 1440;
XrSwapchain g_ovSwap = XR_NULL_HANDLE;
// HUDLAYER 2026-09-28: the HUD's own swapchain, copied from hudsplit.cpp's HUD image each frame and
// submitted as a quad in VIEW space, so the compositor keeps it head-locked at every refresh.
XrSwapchain g_hudSwap = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D *> g_hudImages;
uint32_t g_hudW = 0, g_hudH = 0;
int g_hudSrcFmt = 0;
int64_t g_hudFmt = 0;
int g_hudErr = 0;          // 1 no compatible format, 2 create failed
long g_hudSubmits = 0;
float g_hudDistUsed = 0.0f, g_hudSizeW = 0.0f, g_hudSizeH = 0.0f;
XrCompositionLayerQuad g_rpHud{XR_TYPE_COMPOSITION_LAYER_QUAD};
XrCompositionLayerQuad g_rpHudL{XR_TYPE_COMPOSITION_LAYER_QUAD}, g_rpHudR{XR_TYPE_COMPOSITION_LAYER_QUAD};
bool g_rpHasHud = false;
std::vector<ID3D11Texture2D *> g_ovImages; // swapchain images (copy targets)
// ImGui renders into this PRIVATE linear-store (UNORM) texture, then we
// raw-copy its bytes into the _SRGB swapchain. Rendering ImGui straight into an
// _SRGB target double-applies gamma → washed-out grey; this decouples the two.
ID3D11Texture2D *g_ovRenderTex = nullptr;
ID3D11RenderTargetView *g_ovRenderRTV = nullptr;
bool g_ovInit = false;          // swapchain created
bool g_ovDrewThisFrame = false; // UI rendered a fresh image this Present
bool g_ovEverDrew = false;      // at least one image exists to show

// Diagnostic: is the presented backbuffer a real side-by-side stereo pair
// (geo-11 SBS working) or a single mono image? Compares the centre of the left
// half with the matching point in the right half — in SBS those are the SAME
// view (tiny disparity → small diff); in mono they're different scene regions
// (big diff).
char g_probeStatus[96] = "backbuffer probe: (waiting)";

// KATANGA CAPTURE: geo-11 (katanga_vr mode) publishes its full-res stereo
// texture's shared handle through the "Local\KatangaMappedFile" mapping. We
// open that texture in-process (same adapter) and route it to the eyes — a
// passive consumer like the Katanga viewer, so no DXGI/Present interception
// fight with geo-11.
HANDLE g_katMap = nullptr;    // the file mapping
void *g_katView = nullptr;    // its mapped view (handle sits at offset 0)
uint32_t g_katLastHandle = 0; // last shared handle we opened
ID3D11Texture2D *g_katTex = nullptr; // geo-11's stereo texture (opened shared)
uint32_t g_katW = 0, g_katH = 0;     // its dimensions (SBS = double-wide)
char g_katStatus[160] = "katanga: idle";
// Send the shared surface to the eyes (ON) or keep taking the swapchain (OFF).
// The A/B switch for the whole geo-11 path: with it OFF we get the monitor's
// over/under preview in the headset, which is a useful "is geo-11 even drawing"
// answer when the surface is missing.
bool g_katFeed = true;
char g_dispStatus[128] = "headset: not displaying";
bool g_displaying = false;

// AER eye bookkeeping (1-frame render->present pipeline, same as the pose):
int g_injectEye =
    0; // eye the camera injection uses THIS Present (renders now, submits next)
int g_submitEye =
    0; // eye baked into the frame we submit THIS Present (= last inject eye)

// per-frame loop state (Present thread only)
bool g_frameBegun = false;
XrTime g_predicted = 0;
// Nominal headset frame period (ns) straight from xrWaitFrame. This is the rate the
// runtime is pacing us to, which is NOT necessarily the headset's maximum — Virtual
// Desktop's chosen refresh drives it. Reported in the panel so a low framerate can be
// told apart from a low refresh setting.
XrDuration g_displayPeriod = 0;
bool g_shouldRender = false;
XrPosef
    g_curPose{}; // pose sampled THIS Present (drives the NEXT rendered frame)
XrPosef g_layerPose{}; // pose that drew the backbuffer we're submitting NOW
// POSEDELAY 2026-09-26: how many Presents separate publishing a head pose from the
// image that was drawn with it. Was hard-coded to 1. JJ: "HUD elements appear a lot
// more stable than the 3D elements" — the HUD rides the submitted pose, the world
// rides the rendered one, so a wrong gap here jitters only the world. Unmeasured;
// this ring makes it a live comparison. 1 = previous behaviour.
XrPosef g_poseHist[64]{};
uint32_t g_poseHead = 0;
int g_poseDelay = 1;
// FRAMEID 2026-09-26: exact pose/picture pairing (see frameid.cpp). g_seqForFin maps
// a camera-finalize index to the pose sequence it consumed; g_poseAuto uses it when
// the engine's frame counters have been found, else the fixed g_poseDelay.
// OFF by default (2026-09-26): measured one frame LOW twice (picked 1 where 2-3 was
// steady, then 2 where only 3 was steady, JJ). The fixed delay is the trusted path.
bool g_poseAuto = false;
bool g_poseMatched = false;
int g_usedDelay = 1;
uint32_t g_seqForFin[256]{};
uint64_t g_finRecorded = 0;
uint32_t g_delayHist[16]{};
XrPosef g_fixedPose{}; // frozen world-fixed pose for 2D screens (menus/loading)
// PER-EYE render pose (AER fix): the head pose at which EACH eye's
// currently-held image was drawn. In AER the two eyes are rendered a frame
// apart, so submitting both with one shared pose makes the compositor reproject
// the older eye by the wrong amount during a turn → the eyes disagree →
// smear/ghost. Recording each eye's own render pose lets the runtime timewarp
// each independently to "now" (what Luke Ross / UEVR do). In native mode both
// eyes share a pose, so this is a no-op.
XrPosef g_eyePose[2] = {};
// YAW FOLDING (the spin-flicker fix). Alongside each eye's render-time head pose we
// remember the GAME CAMERA's own heading at that instant. Head turns are already
// corrected — we hand the runtime the head pose each eye was drawn at and its
// time-warp does the rest. A STICK turn is invisible to it: the head never moved, so
// it is told nothing changed and the stale eye keeps the old camera angle. Two eyes a
// big angle apart cannot be fused, and the visual system alternates between them —
// which is what reads as flicker while spinning. Recording the heading lets us declare
// the stale eye's pose rotated by however far the camera has turned since, so a stick
// spin looks to the runtime exactly like a head turn.
float g_eyeBaseYaw[2] = {0.0f, 0.0f};   // game-camera heading each eye was drawn at (deg)
bool g_eyeBaseOk[2] = {false, false};
// Separate ON/OFF from the gain deliberately (JJ, 2026-08-05): an A/B needs a switch
// you can flick back and forth without losing the value you had tuned to, and dragging
// a slider to 0 and back is not that.
// DEFAULT OFF from 2026-08-05. It is an untested experiment, and the shipped default
// has to be the behaviour we know is good — an experiment that is on until you find
// the switch is indistinguishable from a regression.
bool g_yawFoldOn = false;
float g_yawFold = 1.0f;                 // 1 = full correction, 0 = off, negative = flip
float g_foldApplied[2] = {0.0f, 0.0f};  // what we actually rotated each eye by (deg)
char g_foldDiag[128] = "spin correction: waiting for gameplay";
bool g_wasGameplay = false;
bool g_forceScreen = false; // manual override: always float world-fixed (Pause key)
float g_screenAspect = 0.0f;   // SCREENWIDE: floating-screen shape (see akvr_xr_screen_aspect_set)
float g_pauseAspect = 16.0f / 9.0f;   // PAUSE169: shape of pause/map/loading screens (0 = whole picture)
float g_floatScreenScale = 0.5f; // SCREENSIZE: floating-screen size (JJ), separate from the main menu

// Screen-mode (menu/loading) virtual-screen scale: fraction of the full headset
// FOV the flat image is presented within, so it reads like a normal-sized
// screen rather than wrapping around you edge-to-edge. Live-tuned
// Insert/Delete.
float g_screenFovScale = 0.5f;
// MENUS 2026-09-26 (JJ): "when you go into the map or press pause, the game reduces
// to a window in front of you instead of staying fully immersive", while the MAIN
// menu is "way too big ... and stuck to your face". Two different screens, so two
// sizes: g_screenFovScale for the main menu (and F6), g_pauseFovScale for screens the
// detector finds on its own (pause, map, loading) — default full size, world-fixed.
// SIZELATCH: 1.0 was "too big, you have to look around" (JJ); the main-menu size
// was "the correct size", so the default now matches it.
float g_pauseFovScale = 0.5f;
// The main menu runs a live 3D camera exactly like gameplay, so the detector calls it
// GAMEPLAY. But its place in the sequence is fixed: boot/splash (screen) -> main
// menu (camera live) -> loading screen -> game. So the FIRST camera-live stretch after
// launch is the main menu, and the first screen stretch after it (>1.5 s: a loading
// screen) ends it. 0 = before the menu, 1 = in the main menu, 2 = past it.
bool g_autoMainMenu = true;
bool g_menuFlat = true;   // MENUFLAT: floating screens show one eye to both eyes
// EYEVIEW 2026-09-27 (port of SKVR "NOSEWASTE", skvr/HANDOVER.md) — each Quest 3 eye sees
// ~49.3 deg outward but only ~35.4 inward, and FULLVIEW renders the OUTER edge on both sides,
// so ~20% of every row is never shown. geo-11 adds x += S(w - C) per eye: an off-axis term
// (S*w, NDC shift S) plus the eye translation (-S*C) — playbook A3.1/A3.3 as cited by SKVR.
// With a large S and a NEGATIVE C each eye's image turns outward by S; the game renders only
// the half-width tan (tO+tI)/2 and each eye is submitted off-centre by -/+ k*h. Here S and C
// are written LIVE into geo-11 (geo11conv.cpp, GEOLIVE); only the narrower render width needs
// a restart. g_eyeView = this launch's setting; g_eyeApplied = geo-11 confirmed at S/C.
bool  g_eyeView = false;       // set once at load (render width is fixed per launch)
bool  g_eyeApplied = false;    // geo-11 at the eye-view S/C for the picture being shown now
// EVGAME 2026-09-27 — JJ: "3D elements look fine, but 2D elements like menus and loading screens
// are doubled up and not in focus". Arkham's geo-11 fix leaves flat 2D draws unshifted
// (dm_hud_detection=0; F2 14:34: "PAUSE MENU" at the same pixel in both halves while the 3D
// turned), so under the turned frusta 2D lands in two directions. geo-11's values are live here,
// so the eye view now runs in GAMEPLAY only; menus, loading, pause and map get normal stereo
// and a centred frustum. g_eyeWantGameplay feeds hooks.cpp; the applied flag is delayed by the
// pose delay so frames still in flight keep the framing they were rendered for.
bool  g_eyeWantGameplay = false;
static bool     g_appliedHist[64] = {};
static uint32_t g_appliedHead = 0;
bool  g_eyeNarrowOk = true;    // the render really is the narrow shape (checked on size)
float g_eyeK = -1.0f;          // submission off-axis, fraction of h; <0 = the headset's own k
bool  g_eyeFlip = false;       // which way is "outward" relative to geo-11's eye halves
float g_eyeTanOut = 0.0f, g_eyeTanIn = 0.0f, g_eyeTanUp = 0.0f, g_eyeTanDown = 0.0f;
// Effective eye routing: a negative convergence flips geo-11's eye translation, so the
// SBS halves swap relative to normal while the eye view is applied (SKVR: swap off).
static bool swap_eff() { return g_nativeSwapEyes != g_eyeApplied; }
// MENU3D (experimental, off by default): the main menu stays a live head-tracked 3D
// view and earlyres.cpp slides the menu movies against the head so they read as
// pinned in the world. g_menu3dRef* = the applied head angles when it started.
bool g_menu3d = false;
bool g_menu3dWas = false;
// META-BUTTON RECENTER: set by the runtime's LOCAL-space change event (pump_events).
bool g_recenterPending = false;
XrTime g_recenterAt = 0;
bool g_refixScreen = false;
float g_menu3dRefYaw = 0.0f, g_menu3dRefPitch = 0.0f;
int g_menuPhase = 0;
ULONGLONG g_menuScreenSince = 0;
// HUDFIT 2026-09-26, first headset run of the above: the SPLASH suddenly grew and the
// main menu was still head-locked — a short gameplay-looking blip during startup
// opened the phase, and a later gap closed it before the real menu arrived. So the
// phase now needs 3 s of continuous gameplay to open and 5 s of screen to close, and
// every mode change is logged (g_modeLog) so the thresholds can be set from data.
ULONGLONG g_menuGameplaySince = 0;
// The 3D picture this frame was squeezed by projVR (pause and main menu as well as
// gameplay). Decides the presented SHAPE; hooks.cpp sets it from projection cadence.
bool g_anamorphic = false;
bool g_ofxr = false;   // OFXRBRIDGE: frame-generation layer loaded -> lock off (see fps_lock_eff)
// ENTRYHOLD 2026-09-26 — JJ likes menus/loading on the smaller floating screen (80%) but
// not the moment the game starts: the detector needs 10 perfect frames before it calls
// it gameplay, and those frames were shown on the small screen with black around them,
// then it popped out to full view. While a run of gameplay-looking frames is still
// being confirmed, the screen keeps its LAST picture (loading image / pause frame), so
// the view goes straight from the menu picture to full VR. Loads only produce runs of
// 1-2 such frames (19% measured), so a hold there is one or two frames of a still image.
bool g_holdScreen = false;
// The game camera's position (from hooks.cpp) and the main menu's remembered spots.
bool g_camOk = false;
float g_camPos[3] = {0, 0, 0};
float g_menuCam[16][3];
int g_menuCamN = 0;
char g_modeLog[1024] = "";
ULONGLONG g_modeLogStart = 0;
void mode_log(const char *what) {
  const ULONGLONG now = GetTickCount64();
  if (!g_modeLogStart) g_modeLogStart = now;
  char line[64];
  snprintf(line, sizeof(line), "%.1fs %s; ", (now - g_modeLogStart) / 1000.0, what);
  if (strlen(g_modeLog) + strlen(line) < sizeof(g_modeLog) - 1)
    strcat_s(g_modeLog, line);
}

// Published head euler + position. Written and read on the Present thread now,
// but we keep the seqlock (harmless, and akvr_head_update may read it too).
volatile LONG g_poseSeq = 0;
float g_pYaw = 0, g_pPitch = 0, g_pRoll = 0; // euler (deg) — display only now
float g_pX = 0, g_pY = 0, g_pZ = 0; // head position (meters, LOCAL space)
float g_pQx = 0, g_pQy = 0, g_pQz = 0,
      g_pQw = 1; // raw orientation quaternion — injection uses this

void publish_pose(float y, float p, float r, float px, float py, float pz,
                  float qx, float qy, float qz, float qw) {
  LONG s = g_poseSeq;
  g_poseSeq = s + 1;
  MemoryBarrier(); // mark write-in-progress
  g_pYaw = y;
  g_pPitch = p;
  g_pRoll = r;
  g_pX = px;
  g_pY = py;
  g_pZ = pz;
  g_pQx = qx;
  g_pQy = qy;
  g_pQz = qz;
  g_pQw = qw;
  MemoryBarrier();
  g_poseSeq = s + 2; // publish
}

const float R2D = 57.29578f;

bool has_d3d11_ext() {
  uint32_t n = 0;
  if (XR_FAILED(
          xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) ||
      n == 0)
    return false;
  std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
  if (XR_FAILED(
          xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data())))
    return false;
  for (auto &p : props)
    if (strcmp(p.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
      return true;
  return false;
}

void quat_to_euler(const XrQuaternionf &q, float &yaw, float &pitch,
                   float &roll) {
  // OpenXR: right-handed, +Y up, -Z forward. yaw=around Y, pitch=around X,
  // roll=around Z.
  float x = q.x, y = q.y, z = q.z, w = q.w;
  float sinp = 2.0f * (w * x - y * z);
  pitch = (fabsf(sinp) >= 1.0f) ? copysignf(90.0f, sinp) : asinf(sinp) * R2D;
  yaw = atan2f(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y)) * R2D;
  roll = atan2f(2.0f * (w * z + x * y), 1.0f - 2.0f * (z * z + x * x)) * R2D;
}

// Turn a pose about the tracking space's OWN vertical (+Y) by `rad`. Pre-multiplying
// is what makes it a world turn rather than a turn about the head's own tilted up —
// the same distinction that caused the head-yaw roll bug on the camera side, so it is
// deliberate here too: the game camera's heading is a world heading.
XrQuaternionf yaw_premul(const XrQuaternionf &q, float rad) {
  float s = sinf(rad * 0.5f), c = cosf(rad * 0.5f);
  XrQuaternionf r{c * q.x + s * q.z, c * q.y + s * q.w, c * q.z - s * q.x,
                  c * q.w - s * q.y};
  float n = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  if (n > 1e-6f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
  return r;
}

// Turn a pose about its OWN right axis (+X) by `rad` (+ = up): q * Rx(rad).
XrQuaternionf pitch_postmul(const XrQuaternionf &q, float rad) {
  float s = sinf(rad * 0.5f), c = cosf(rad * 0.5f);
  XrQuaternionf r{q.w * s + q.x * c, q.y * c + q.z * s, q.z * c - q.y * s,
                  q.w * c - q.x * s};
  float n = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  if (n > 1e-6f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
  return r;
}

// Rotate a vector by a quaternion (v' = q * v * q^-1). Used to place the head-
// locked panel: take an offset expressed in head-local axes and turn it into a
// world/local-space offset from the head position.
XrVector3f quat_rotate(const XrQuaternionf &q, const XrVector3f &v) {
  float x = q.x, y = q.y, z = q.z, w = q.w;
  // t = 2 * cross(q.xyz, v)
  float tx = 2.0f * (y * v.z - z * v.y);
  float ty = 2.0f * (z * v.x - x * v.z);
  float tz = 2.0f * (x * v.y - y * v.x);
  // v' = v + w*t + cross(q.xyz, t)
  return XrVector3f{
      v.x + w * tx + (y * tz - z * ty),
      v.y + w * ty + (z * tx - x * tz),
      v.z + w * tz + (x * ty - y * tx)};
}

// The sRGB sibling of a color format. OpenXR compositors sRGB-DECODE swapchain
// texels when the format is _SRGB; the game's backbuffer already holds
// sRGB-encoded (display-ready) pixels, so we must store them in an _SRGB
// swapchain — otherwise the compositor skips the decode and encodes a second
// time on output → washed-out / too-bright image. CopyResource still works
// (UNORM and its _SRGB sibling share one typeless family = a raw byte copy).
DXGI_FORMAT srgb_sibling(DXGI_FORMAT f) {
  switch (f) {
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

// ---- copy-format compatibility (CopyResource needs the same typeless family)
// ----
DXGI_FORMAT copy_family(DXGI_FORMAT f) {
  switch (f) {
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
    return DXGI_FORMAT_R8G8B8A8_TYPELESS;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    return DXGI_FORMAT_B8G8R8A8_TYPELESS;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
    return DXGI_FORMAT_R10G10B10A2_TYPELESS;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
    return DXGI_FORMAT_R16G16B16A16_TYPELESS;
  default:
    return f;
  }
}

bool create_session() {
  // Graphics requirements MUST be queried before session creation.
  PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(g_inst,
                                      "xrGetD3D11GraphicsRequirementsKHR",
                                      (PFN_xrVoidFunction *)&pfn)) ||
      !pfn) {
    snprintf(g_sessStatus, sizeof(g_sessStatus), "session: no D3D11 req fn");
    return false;
  }
  XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  pfn(g_inst, g_sys, &req); // we reuse the game's device; LUID assumed to match

  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  binding.device = g_device;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = g_sys;
  // LUIDCHECK 2026-09-28 — JJ: "not entering VR", xrCreateSession -2, window opening somewhere new; the PC
  // shows the RTX 4070 Ti as three DXGI adapters (virtual display drivers) and two monitors. The runtime can
  // only take a device on the adapter it names; we never compared. Report both.
  char luidNote[128] = "";
  {
    IDXGIDevice *dd = nullptr;
    IDXGIAdapter *ad = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) && dd &&
        SUCCEEDED(dd->GetAdapter(&ad)) && ad && SUCCEEDED(ad->GetDesc(&desc))) {
      const bool same = memcmp(&desc.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0;
      snprintf(luidNote, sizeof(luidNote), " | game GPU LUID %08lx:%08lx, headset wants %08lx:%08lx%s",
               (unsigned long)desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
               (unsigned long)req.adapterLuid.HighPart, req.adapterLuid.LowPart,
               same ? " (same)" : " - DIFFERENT ADAPTER");
    }
    if (ad) ad->Release();
    if (dd) dd->Release();
  }
  XrResult r = xrCreateSession(g_inst, &sci, &g_session);
  if (XR_FAILED(r)) {
    snprintf(g_sessStatus, sizeof(g_sessStatus),
             "session: xrCreateSession failed (%d)%s", (int)r, luidNote);
    return false;
  }

  XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rci.poseInReferenceSpace.orientation.w = 1.0f; // identity
  rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  xrCreateReferenceSpace(g_session, &rci, &g_localSpace);
  rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  xrCreateReferenceSpace(g_session, &rci, &g_viewSpace);

  // Identity poses so the very first submitted layer (before we've located a
  // real pose) has a valid unit quaternion, not a zero one.
  g_curPose = XrPosef{};
  g_curPose.orientation.w = 1.0f;
  g_layerPose = XrPosef{};
  g_layerPose.orientation.w = 1.0f;
  g_fixedPose = XrPosef{};
  g_fixedPose.orientation.w = 1.0f;
  for (auto &p : g_poseHist)
    p = g_layerPose;
  g_eyePose[0] = g_curPose;
  g_eyePose[1] = g_curPose;

  snprintf(g_sessStatus, sizeof(g_sessStatus),
           "session: created, waiting for READY");
  return true;
}

// Create the headset swapchain sized to the game's backbuffer so the game
// frame copies in 1:1 (no scaling). Called once, after the session is running
// and we know the backbuffer dimensions/format.
bool create_swapchain(uint32_t w, uint32_t h, DXGI_FORMAT bbFormat) {
  if (g_swap[0] != XR_NULL_HANDLE)
    return true;
  if (g_session == XR_NULL_HANDLE)
    return false;

  uint32_t fcount = 0;
  if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, 0, &fcount, nullptr)) ||
      fcount == 0)
    return false;
  std::vector<int64_t> formats(fcount);
  if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, fcount, &fcount,
                                            formats.data())))
    return false;

  // 1) Prefer the sRGB sibling of the backbuffer format (correct gamma — the
  //    game's pixels are already sRGB-encoded). 2) else exact backbuffer
  //    format. 3) else any format in the same copy family. CopyResource
  //    works across a typeless family (e.g. UNORM<->SRGB = a raw byte copy).
  DXGI_FORMAT srgb = srgb_sibling(bbFormat);
  int64_t chosen = 0;
  if (srgb != DXGI_FORMAT_UNKNOWN)
    for (int64_t f : formats)
      if ((DXGI_FORMAT)f == srgb) {
        chosen = f;
        break;
      }
  if (!chosen)
    for (int64_t f : formats)
      if ((DXGI_FORMAT)f == bbFormat) {
        chosen = f;
        break;
      }
  if (!chosen)
    for (int64_t f : formats)
      if (copy_family((DXGI_FORMAT)f) == copy_family(bbFormat)) {
        chosen = f;
        break;
      }
  if (!chosen) {
    snprintf(g_dispStatus, sizeof(g_dispStatus),
             "headset: no copy-compatible format");
    return false;
  }

  // Native/SBS: each eye's image is one HALF of the backbuffer (anamorphic 2:1
  // squeeze — the compositor stretches it back across the full horizontal FOV).
  // AER: each eye gets a full-size copy of the whole backbuffer.
  uint32_t eyeW = g_native ? (w / 2) : w;
  g_sbsActive = g_native && g_sbsOne;
  for (int e = 0; e < (g_sbsActive ? 1 : 2); ++e) {
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = chosen;
    sci.sampleCount = 1;
    sci.width = g_sbsActive ? w : eyeW;              // SBSONE: both eyes in one image
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g_session, &sci, &g_swap[e]))) {
      g_swap[e] = XR_NULL_HANDLE;
      return false;
    }

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(g_swap[e], 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(
        imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(g_swap[e], imgCount, &imgCount,
                               (XrSwapchainImageBaseHeader *)imgs.data());
    g_swapImages[e].clear();
    for (auto &i : imgs)
      g_swapImages[e].push_back(i.texture);
  }
  g_swW = w;
  g_swH = h;
  g_eyeW = eyeW;
  // EYEVIEW: the per-eye turn only makes sense on the narrow (taller-than-wide) render.
  if (g_native && eyeW && h) g_eyeNarrowOk = eyeW <= h;
  g_swFmt = (DXGI_FORMAT)chosen;
  return true;
}

// Tear down both eye swapchains (mode switch → per-eye size changes).
void destroy_swapchains() {
  for (int e = 0; e < 2; ++e) {
    if (g_swap[e] != XR_NULL_HANDLE) {
      xrDestroySwapchain(g_swap[e]);
      g_swap[e] = XR_NULL_HANDLE;
    }
    g_swapImages[e].clear();
    g_swapInit[e] = false;
  }
  g_swW = g_swH = g_eyeW = 0;
}

// Create the overlay panel's own swapchain (RGBA, sRGB, with alpha so the game
// shows through the transparent parts) + an RTV per image. Independent of the
// backbuffer size, so a resize never touches it.
bool create_overlay_swapchain() {
  if (g_ovInit)
    return true;
  if (g_session == XR_NULL_HANDLE)
    return false;

  uint32_t fcount = 0;
  if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, 0, &fcount, nullptr)) ||
      fcount == 0)
    return false;
  std::vector<int64_t> formats(fcount);
  if (XR_FAILED(xrEnumerateSwapchainFormats(g_session, fcount, &fcount,
                                            formats.data())))
    return false;
  int64_t chosen = 0;
  for (int64_t f : formats)
    if ((DXGI_FORMAT)f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
      chosen = f;
      break;
    }
  if (!chosen)
    for (int64_t f : formats)
      if ((DXGI_FORMAT)f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        chosen = f;
        break;
      }
  if (!chosen)
    chosen = formats[0];

  XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  sci.format = chosen;
  sci.sampleCount = 1;
  sci.width = g_ovW;
  sci.height = g_ovH;
  sci.faceCount = 1;
  sci.arraySize = 1;
  sci.mipCount = 1;
  if (XR_FAILED(xrCreateSwapchain(g_session, &sci, &g_ovSwap))) {
    g_ovSwap = XR_NULL_HANDLE;
    return false;
  }

  uint32_t imgCount = 0;
  xrEnumerateSwapchainImages(g_ovSwap, 0, &imgCount, nullptr);
  std::vector<XrSwapchainImageD3D11KHR> imgs(
      imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  xrEnumerateSwapchainImages(g_ovSwap, imgCount, &imgCount,
                             (XrSwapchainImageBaseHeader *)imgs.data());
  g_ovImages.clear();
  for (auto &i : imgs)
    g_ovImages.push_back(i.texture);

  // Private linear-store render target ImGui draws into (UNORM sibling of the
  // swapchain's _SRGB, so a raw CopyResource lands the bytes unchanged and the
  // compositor's single sRGB decode gives correct gamma).
  D3D11_TEXTURE2D_DESC td{};
  td.Width = g_ovW;
  td.Height = g_ovH;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_ovRenderTex))) {
    return false;
  }
  if (FAILED(g_device->CreateRenderTargetView(g_ovRenderTex, nullptr,
                                              &g_ovRenderRTV))) {
    return false;
  }

  g_ovInit = true;
  return true;
}

// Read 2 pixels off the backbuffer to classify MONO vs SBS. Cheap but does a
// GPU→CPU Map (a sync), so callers gate it to run only occasionally.
void probe_backbuffer(ID3D11Texture2D *bb, const D3D11_TEXTURE2D_DESC &bd) {
  static ID3D11Texture2D *staging = nullptr;
  static DXGI_FORMAT sfmt = DXGI_FORMAT_UNKNOWN;
  if (!g_ctx || !g_device || !bb)
    return;
  if (staging && (sfmt != bd.Format)) {
    staging->Release();
    staging = nullptr;
  }
  if (!staging) {
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = 2;
    sd.Height = 1;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = bd.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging))) {
      staging = nullptr;
      return;
    }
    sfmt = bd.Format;
  }
  UINT y = bd.Height / 2;
  UINT xL = bd.Width / 4;                // centre of the left half
  UINT xR = bd.Width / 4 + bd.Width / 2; // the matching point in the right half
  D3D11_BOX bl{xL, y, 0, xL + 1, y + 1, 1};
  D3D11_BOX br{xR, y, 0, xR + 1, y + 1, 1};
  g_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, bb, 0, &bl);
  g_ctx->CopySubresourceRegion(staging, 0, 1, 0, 0, bb, 0, &br);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    const uint8_t *pL = (const uint8_t *)m.pData;
    const uint8_t *pR = pL + 4; // second pixel, same row (32bpp)
    int d = 0;
    for (int c = 0; c < 3; ++c)
      d += abs((int)pL[c] - (int)pR[c]);
    g_ctx->Unmap(staging, 0);
    snprintf(g_probeStatus, sizeof(g_probeStatus),
             "backbuffer: %s  (L/R ctr diff %d)",
             d < 40 ? "SBS stereo pair" : "MONO - no SBS", d);
  }
}

// Open (or refresh) geo-11's shared stereo texture from the Katanga IPC
// mapping. The mapping holds the shared handle at offset 0; when it changes
// (geo-11 recreated the surface, e.g. on resize) we re-open. Runs each Present;
// cheap once opened.
void katanga_update() {
  if (!g_device) {
    snprintf(g_katStatus, sizeof(g_katStatus), "katanga: no device");
    return;
  }

  if (!g_katView) {
    if (!g_katMap) {
      g_katMap =
          OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\KatangaMappedFile");
      if (!g_katMap) {
        DWORD e = GetLastError();
        snprintf(g_katStatus, sizeof(g_katStatus),
                 "katanga: mapping not found, err=%lu (2=not created yet, "
                 "5=access denied)",
                 e);
        return;
      }
    }
    g_katView = MapViewOfFile(g_katMap, FILE_MAP_READ, 0, 0, 0);
    if (!g_katView) {
      DWORD e = GetLastError();
      snprintf(g_katStatus, sizeof(g_katStatus),
               "katanga: MapViewOfFile failed, err=%lu", e);
      return;
    }
  }

  uint32_t h = *(volatile uint32_t *)
                   g_katView; // legacy shared handle (32-bit) at offset 0
  if (h == 0) {
    snprintf(g_katStatus, sizeof(g_katStatus),
             "katanga: mapping open, handle still 0");
    return;
  }
  if (h == g_katLastHandle && g_katTex)
    return; // unchanged — nothing to do

  if (g_katTex) {
    g_katTex->Release();
    g_katTex = nullptr;
  }
  HRESULT hr = g_device->OpenSharedResource(
      (HANDLE)(uintptr_t)h, __uuidof(ID3D11Texture2D), (void **)&g_katTex);
  if (SUCCEEDED(hr) && g_katTex) {
    D3D11_TEXTURE2D_DESC d{};
    g_katTex->GetDesc(&d);
    g_katW = d.Width;
    g_katH = d.Height;
    g_katLastHandle = h;
    snprintf(g_katStatus, sizeof(g_katStatus),
             "katanga: OPEN  %ux%u  fmt=%d  arr=%u  (handle 0x%X)", d.Width,
             d.Height, (int)d.Format, d.ArraySize, h);
  } else {
    snprintf(g_katStatus, sizeof(g_katStatus),
             "katanga: OpenSharedResource failed 0x%lX (handle 0x%X)", hr, h);
    g_katLastHandle = 0;
  }
}

// What the RUNTIME itself wants per eye. We had never asked — which meant every
// judgement about "is the render big enough" was guesswork. This is the honest
// target: the recommended size already accounts for the lens-distortion resample,
// so it is normally well ABOVE the panel's own pixel count (Quest 3 panels are
// 2064x2208 each, and runtimes routinely recommend more). Rendering below it is
// what makes an image soft; rendering above it is ordinary supersampling and is
// exactly what the other mods let you do.
uint32_t g_recW = 0, g_recH = 0;   // recommended per-eye render size
uint32_t g_maxW = 0, g_maxH = 0;   // the runtime's hard per-eye ceiling

void query_recommended_size() {
  if (!g_inst || g_recW) return;
  uint32_t n = 0;
  if (XR_FAILED(xrEnumerateViewConfigurationViews(
          g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n,
          nullptr)) ||
      n == 0)
    return;
  std::vector<XrViewConfigurationView> vs(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  if (XR_FAILED(xrEnumerateViewConfigurationViews(
          g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
          vs.data())))
    return;
  g_recW = vs[0].recommendedImageRectWidth;
  g_recH = vs[0].recommendedImageRectHeight;
  g_maxW = vs[0].maxImageRectWidth;
  g_maxH = vs[0].maxImageRectHeight;
}

void pump_events() {
  query_recommended_size();   // cached after the first successful call
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      auto *s = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
      g_state = s->state;
      if (s->state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType =
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(g_session, &bi)))
          g_running = true;
      } else if (s->state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(g_session);
        g_running = false;
      }
    } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
      // META-BUTTON RECENTER 2026-09-26 — JJ: holding the Meta button "doesn't seem
      // to work once the mod launches you into VR". The runtime DOES recentre: it
      // moves LOCAL space and announces it here. We ignored it, so our own zero
      // (camera.cpp's recenter reference, and a floating screen's fixed pose) stayed
      // in the old space. Re-zero both once the change has taken effect.
      auto *r = reinterpret_cast<XrEventDataReferenceSpaceChangePending *>(&ev);
      if (r->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
        g_recenterAt = r->changeTime;
        g_recenterPending = true;
        mode_log("runtime recenter");
      }
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

// Headset per-eye FOV half-angles (radians), measured live from xrLocateViews.
// We display the game frame across the Quest 3's REAL FOV (fills the lens), and
// the game is forced to render at this same FOV → head motion stays 1:1.
float g_headHalfH = 0.86f; // ~49 deg default until measured
float g_headHalfV = 0.86f;
bool g_fullView = true;   // FULLVIEW: cover every eye's real frustum (see the measurement)
float g_vOffNdc = 0.0f;   // off-centre vertical shift of the shared projection, NDC
// TILTFILL 2026-09-26 — JJ: black band at the bottom with projVR off. The Quest 3
// sees ~39 deg up but ~50.5 down; a CENTRED picture of +-45.7 misses the bottom. The
// off-centre projection (FULLVIEW) broke geo-11's light/decal fixes, so instead the
// whole head pose is pitched down about its own right axis by half the difference
// (~5.6 deg) where it is sampled: the game draws a centred picture looking that much
// lower and the layer is submitted with the same tilted pose, so they cannot
// disagree and the picture covers ~40 up / ~51 down. Geo-11 native, projVR off only.
bool  g_tiltFill = true;
float g_tiltRad  = 0.0f;  // measured: (atan(down) - atan(up)) / 2 of the real frusta
float g_tiltUsed = 0.0f;  // what was applied to the last sampled pose (diagnostic)
// Raw per-eye frusta as the runtime reports them, before we symmetrise (diagnostic).
XrFovf g_rawFov[2]{};
bool   g_rawFovOk = false;
float g_lastGameFov = 90.0f; // retained for status only
// projVR: the game's projection has been rewritten (camera.cpp) to render the
// Quest's real per-eye FOV, so we present the FULL frame at that same FOV (no
// crop, no 16:9 aspect-fit). Set by camera.cpp when the projection hook is live.
bool g_projvrOn = false;

// HUDLAYER 2026-09-28 — (re)create the HUD swapchain for the HUD image's size and byte layout.
bool hud_swapchain(uint32_t w, uint32_t h, int srcFmt) {
  if (g_hudSwap != XR_NULL_HANDLE && g_hudW == w && g_hudH == h && g_hudSrcFmt == srcFmt)
    return true;
  if (g_hudSwap != XR_NULL_HANDLE) {
    xrDestroySwapchain(g_hudSwap);
    g_hudSwap = XR_NULL_HANDLE;
    g_hudImages.clear();
  }
  g_hudW = w; g_hudH = h; g_hudSrcFmt = srcFmt;
  uint32_t n = 0;
  xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
  std::vector<int64_t> formats(n);
  xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
  // The HUD bytes are display-ready (gamma-encoded), so the _SRGB sibling of the same byte
  // layout makes the compositor decode them exactly once (same trick as the panel overlay).
  const DXGI_FORMAT fam = copy_family((DXGI_FORMAT)srcFmt);
  DXGI_FORMAT want = DXGI_FORMAT_UNKNOWN;
  if (fam == copy_family(DXGI_FORMAT_R8G8B8A8_UNORM)) want = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  else if (fam == copy_family(DXGI_FORMAT_B8G8R8A8_UNORM)) want = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  int64_t chosen = 0;
  for (int64_t f : formats)
    if ((DXGI_FORMAT)f == want) { chosen = f; break; }
  if (!chosen)
    for (int64_t f : formats)
      if (copy_family((DXGI_FORMAT)f) == fam) { chosen = f; break; }
  if (!chosen) { g_hudErr = 1; return false; }
  g_hudFmt = chosen;
  XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  sci.format = chosen;
  sci.sampleCount = 1;
  sci.width = w;
  sci.height = h;
  sci.faceCount = 1;
  sci.arraySize = 1;
  sci.mipCount = 1;
  if (XR_FAILED(xrCreateSwapchain(g_session, &sci, &g_hudSwap))) {
    g_hudSwap = XR_NULL_HANDLE;
    g_hudErr = 2;
    return false;
  }
  uint32_t imgCount = 0;
  xrEnumerateSwapchainImages(g_hudSwap, 0, &imgCount, nullptr);
  std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  xrEnumerateSwapchainImages(g_hudSwap, imgCount, &imgCount, (XrSwapchainImageBaseHeader *)imgs.data());
  for (auto &i : imgs)
    g_hudImages.push_back(i.texture);
  g_hudErr = 0;
  return true;
}

// HUDLAYER4 2026-09-28 — JJ on HUDLAYER3: live, but not steadier, the distance was lost, and see-through
// pieces (the compass line) look dark and too solid.
// - Space: Virtual Desktop mishandled VIEW-space quads before (2025-07 panel note), and the lost distance
//   fits that. The quad is now placed in LOCAL space in front of the head pose predicted for each
//   xrEndFrame's display time - re-placed in every frame we end, including FPSLOCK's repeated frames, so
//   in "repeat the frame" mode it moves 90 times a second. (In "Virtual Desktop SSW" mode only 45 frames
//   are ended; VD's SSW warps the whole image in between, HUD included.) hudlayerspace=1 keeps VIEW space.
// - Colour: the HUD image is premultiplied in gamma space; the compositor blends in linear light, so raw
//   bytes read as sRGB darken every see-through pixel over bright backgrounds. Mode 1 (default) converts
//   each pixel: straight colour = c / a, to linear, times a (additive pixels with a ~ 0 kept as light).
XrQuaternionf hud_qmul(const XrQuaternionf &a, const XrQuaternionf &b) {
  return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
// HUDVIEW 2026-09-28 — JJ: lazy follow "locks the elements for a brief window to simulate stability"; Onimusha's
// locked-on bars feel far steadier. LOCAL placement puts the quad at the head pose PREDICTED for the frame, and
// with the frame lock off that happens only when the game draws (~45/s); every prediction error and every
// 45 Hz step shows as HUD motion, which lazy follow only masked. VIEW space: the compositor places the quad from
// the actual head pose at every refresh - nothing to predict. The HUDLAYER3 view-space test ("not steadier") ran
// with VD SSW on, which warps the whole picture, so it never measured this. Lazy follow is off for good.
// HUDWORLD 2026-09-29 — JJ: "the main HUD should be fixed to world space by default, not fixed to head
// movement; fixed to head movement should be an option, 'attach UI to head movement'" (UEVR's default).
// Mode 2 hangs the quad in the room: the head-relative pose placed once in front of the LEVEL head pose
// at the last recenter (F12, the Meta button, head tracking on) - the same heading camera.cpp takes as the
// game's straight ahead - and left there. Turning the head looks around it; the compositor keeps it put.
int g_hudSpace = 2;          // 0 LOCAL (re-placed per frame), 1 VIEW (attached to the head), 2 fixed in the room
bool g_hudWorldOk = false;   // mode 2: anchor taken since the last recenter
XrPosef g_hudWorldAnchor{};  // mode 2: level head pose at the recenter, LOCAL space
// HUDLAYER5 — JJ on HUDLAYER4: the HUD distance slider still changes nothing, though the log shows the
// quad moving (3.0 m). The quad keeps its angular size, so only the two eyes' difference shows depth:
// if the runtime composites a quad without that difference, depth never changes. Eye shift (option):
// one quad per eye, moved by +-half the eye distance, which puts back exactly the difference a HUD at
// the distance has when both eyes are drawn from the head centre. (Right runtime + shift = double.)
int g_hudEyes = 1;           // 0 one quad, 1 one per eye with the eye shift (JJ: this makes the slider work)
float g_hudHalfIpd = 0.0315f;
// Lazy follow (option): the HUD keeps its room direction until the head has turned more than g_hudLazyDeg
// away, then follows so it trails by exactly that angle - tremor does not move it, turns do.
int g_hudLazy = 0;           // HUDVIEW: off for good (room-space only; the HUD is in view space now)
float g_hudLazyDeg = 0.3f;
XrQuaternionf g_hudLazyOri{0, 0, 0, 1};
bool g_hudLazyOk = false;
int g_hudColour = 1;         // 0 raw copy, 1 converted to linear premultiplied
XrPosef g_hudViewPose{};     // the quad's pose relative to the head (view space)
long g_hudPlaceFails = 0;
char g_hudConvDiag[96] = "";
// Place the quad for display time t. LOCAL: head pose at t composed with the head-relative pose.
bool hud_place(XrCompositionLayerQuad &q, XrTime t) {
  if (g_hudSpace == 1) { q.space = g_viewSpace; q.pose = g_hudViewPose; return true; }
  if (g_hudSpace == 2) {
    if (!g_hudWorldOk) {
      XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
      if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, t, &hl)) ||
          (hl.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) !=
              (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ++g_hudPlaceFails; return false;
      }
      // heading only: the twist about +Y (as level_pose below), so a recenter while looking down still
      // hangs the HUD level in front of you
      g_hudWorldAnchor = hl.pose;
      const XrQuaternionf &o = hl.pose.orientation;
      const float n = sqrtf(o.w * o.w + o.y * o.y);
      g_hudWorldAnchor.orientation = n > 1e-6f ? XrQuaternionf{0.0f, o.y / n, 0.0f, o.w / n} : XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
      g_hudWorldOk = true;
    }
    const XrQuaternionf &a = g_hudWorldAnchor.orientation;
    const XrVector3f o = quat_rotate(a, g_hudViewPose.position);
    q.space = g_localSpace;
    q.pose.orientation = hud_qmul(a, g_hudViewPose.orientation);
    q.pose.position = {g_hudWorldAnchor.position.x + o.x, g_hudWorldAnchor.position.y + o.y,
                       g_hudWorldAnchor.position.z + o.z};
    return true;
  }
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, t, &loc)) ||
      !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) { ++g_hudPlaceFails; return false; }
  XrQuaternionf head = loc.pose.orientation;
  if (g_hudLazy) {
    // angle between the held direction and the head, then turn the held one just enough to keep it
    // within g_hudLazyDeg (slerp by the excess); a jump over 45 deg (recentre, cut) snaps.
    XrQuaternionf a = g_hudLazyOri, b = head;
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) { b = {-b.x, -b.y, -b.z, -b.w}; dot = -dot; }
    if (dot > 1.0f) dot = 1.0f;
    const float ang = 2.0f * acosf(dot);
    const float dead = g_hudLazyDeg * (3.14159265f / 180.0f);
    if (!g_hudLazyOk || ang > 0.785f) { g_hudLazyOri = head; g_hudLazyOk = true; }
    else if (ang > dead) {
      const float f = (ang - dead) / ang, half = ang * 0.5f, sn = sinf(half);
      const float wa = sinf((1.0f - f) * half) / sn, wb = sinf(f * half) / sn;
      XrQuaternionf r{wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
      const float n = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
      if (n > 1e-6f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
      g_hudLazyOri = r;
    }
    head = g_hudLazyOri;
  } else g_hudLazyOk = false;
  const XrVector3f o = quat_rotate(head, g_hudViewPose.position);
  q.space = g_localSpace;
  q.pose.orientation = hud_qmul(head, g_hudViewPose.orientation);
  q.pose.position = {loc.pose.position.x + o.x, loc.pose.position.y + o.y, loc.pose.position.z + o.z};
  return true;
}

// HUDLAYER5: the placed quad as a left/right pair, each moved along the quad's own right axis.
void hud_eye_pair(const XrCompositionLayerQuad &q, XrCompositionLayerQuad &l, XrCompositionLayerQuad &r) {
  l = q; r = q;
  l.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
  r.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
  const XrVector3f s = quat_rotate(q.pose.orientation, XrVector3f{g_hudHalfIpd, 0.0f, 0.0f});
  l.pose.position = {q.pose.position.x + s.x, q.pose.position.y + s.y, q.pose.position.z + s.z};
  r.pose.position = {q.pose.position.x - s.x, q.pose.position.y - s.y, q.pose.position.z - s.z};
}

// Colour conversion pass (real context, our own state saved and restored).
ID3D11VertexShader *g_hcVS = nullptr;
ID3D11PixelShader *g_hcPS = nullptr;
int g_hcState = 0;                       // 0 not built, 1 ok, -1 failed
ID3D11ShaderResourceView *g_hcSRV = nullptr;
ID3D11Texture2D *g_hcSRVTex = nullptr;
std::vector<ID3D11RenderTargetView *> g_hcRTV;
typedef HRESULT(WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob **, ID3DBlob **);
bool hud_conv_build() {
  if (g_hcState) return g_hcState == 1;
  g_hcState = -1;
  HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
  D3DCompileFn compile = m ? (D3DCompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
  if (!compile) { snprintf(g_hudConvDiag, sizeof(g_hudConvDiag), "no shader compiler"); return false; }
  static const char vs[] =
      "float4 main(uint id : SV_VertexID) : SV_Position {"
      " float2 uv = float2((id << 1) & 2, id & 2);"
      " return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
  static const char ps[] =
      "Texture2DArray<float4> t : register(t0);"
      "float3 lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }"
      "float4 main(float4 pos : SV_Position) : SV_Target {"
      " float4 p = t.Load(int4(pos.xy, 0, 0));"
      " float a = saturate(p.a);"
      " float3 o = a > 0.004 ? lin(saturate(p.rgb / a)) * a : lin(saturate(p.rgb));"
      " return float4(o, a); }";
  ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(compile(vs, sizeof(vs) - 1, "hudvs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, &err));
  if (err) { err->Release(); err = nullptr; }
  ok = ok && SUCCEEDED(compile(ps, sizeof(ps) - 1, "hudps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err));
  if (err) err->Release();
  ok = ok && SUCCEEDED(g_device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_hcVS));
  ok = ok && SUCCEEDED(g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_hcPS));
  if (vb) vb->Release();
  if (pb) pb->Release();
  snprintf(g_hudConvDiag, sizeof(g_hudConvDiag), ok ? "colour conversion ready" : "colour shaders failed");
  g_hcState = ok ? 1 : -1;
  return ok;
}
// Draw src slice 0 into swapchain image idx through the conversion shader. False = use a raw copy.
bool hud_conv_draw(ID3D11Texture2D *src, uint32_t idx) {
  if (!hud_conv_build()) return false;
  if (g_hcSRVTex != src) {
    if (g_hcSRV) { g_hcSRV->Release(); g_hcSRV = nullptr; }
    g_hcSRVTex = src;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = (DXGI_FORMAT)g_hudSrcFmt;
    if (vd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (vd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) vd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.MostDetailedMip = 0;
    vd.Texture2DArray.MipLevels = 1;
    vd.Texture2DArray.FirstArraySlice = 0;
    vd.Texture2DArray.ArraySize = 1;
    if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || FAILED(g_device->CreateShaderResourceView(src, &vd, &g_hcSRV))) {
      g_hcSRV = nullptr;
      snprintf(g_hudConvDiag, sizeof(g_hudConvDiag), "HUD image cannot be read by a shader (bind 0x%x)", sd.BindFlags);
    }
  }
  if (!g_hcSRV) return false;
  if (g_hcRTV.size() != g_hudImages.size()) {
    for (auto *r : g_hcRTV) if (r) r->Release();
    g_hcRTV.assign(g_hudImages.size(), nullptr);
  }
  if (!g_hcRTV[idx]) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = (DXGI_FORMAT)g_hudFmt;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_device->CreateRenderTargetView(g_hudImages[idx], &rd, &g_hcRTV[idx]))) {
      g_hcRTV[idx] = nullptr;
      snprintf(g_hudConvDiag, sizeof(g_hudConvDiag), "headset HUD image cannot be drawn into");
      return false;
    }
  }
  // save what we touch
  ID3D11RenderTargetView *oRTV = nullptr; ID3D11DepthStencilView *oDSV = nullptr;
  g_ctx->OMGetRenderTargets(1, &oRTV, &oDSV);
  UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_VIEWPORT ovp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  g_ctx->RSGetViewports(&nvp, ovp);
  ID3D11VertexShader *oVS = nullptr; ID3D11PixelShader *oPS = nullptr;
  g_ctx->VSGetShader(&oVS, nullptr, nullptr);
  g_ctx->PSGetShader(&oPS, nullptr, nullptr);
  ID3D11ShaderResourceView *oSRV = nullptr;
  g_ctx->PSGetShaderResources(0, 1, &oSRV);
  ID3D11InputLayout *oIL = nullptr; g_ctx->IAGetInputLayout(&oIL);
  D3D11_PRIMITIVE_TOPOLOGY oTop; g_ctx->IAGetPrimitiveTopology(&oTop);
  ID3D11BlendState *oBS = nullptr; FLOAT oBF[4]; UINT oBM = 0; g_ctx->OMGetBlendState(&oBS, oBF, &oBM);
  ID3D11DepthStencilState *oDS = nullptr; UINT oRef = 0; g_ctx->OMGetDepthStencilState(&oDS, &oRef);
  ID3D11RasterizerState *oRS = nullptr; g_ctx->RSGetState(&oRS);
  // draw
  D3D11_VIEWPORT vp{0.0f, 0.0f, (float)g_hudW, (float)g_hudH, 0.0f, 1.0f};
  g_ctx->OMSetRenderTargets(1, &g_hcRTV[idx], nullptr);
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->IASetInputLayout(nullptr);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_hcVS, nullptr, 0);
  g_ctx->PSSetShader(g_hcPS, nullptr, 0);
  g_ctx->PSSetShaderResources(0, 1, &g_hcSRV);
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  g_ctx->OMSetDepthStencilState(nullptr, 0);
  g_ctx->RSSetState(nullptr);
  g_ctx->Draw(3, 0);
  ID3D11ShaderResourceView *none = nullptr;
  g_ctx->PSSetShaderResources(0, 1, &none);
  // restore
  g_ctx->OMSetRenderTargets(1, &oRTV, oDSV);
  if (nvp) g_ctx->RSSetViewports(nvp, ovp);
  g_ctx->VSSetShader(oVS, nullptr, 0);
  g_ctx->PSSetShader(oPS, nullptr, 0);
  g_ctx->PSSetShaderResources(0, 1, &oSRV);
  g_ctx->IASetInputLayout(oIL);
  g_ctx->IASetPrimitiveTopology(oTop);
  g_ctx->OMSetBlendState(oBS, oBF, oBM);
  g_ctx->OMSetDepthStencilState(oDS, oRef);
  g_ctx->RSSetState(oRS);
  if (oRTV) oRTV->Release();
  if (oDSV) oDSV->Release();
  if (oVS) oVS->Release();
  if (oPS) oPS->Release();
  if (oSRV) oSRV->Release();
  if (oIL) oIL->Release();
  if (oBS) oBS->Release();
  if (oDS) oDS->Release();
  if (oRS) oRS->Release();
  snprintf(g_hudConvDiag, sizeof(g_hudConvDiag), "colour converted");
  return true;
}

// HUDLAYER: copy this frame's HUD image into the HUD swapchain and describe the quad, at the HUD
// distance, covering exactly the angles the game frame covers, turned down by the same TILTFILL
// pitch the frame was drawn with, so every HUD piece sits where it sat in the picture.
bool hud_layer_build(XrCompositionLayerQuad &q, const XrFovf &fov) {
  unsigned w = 0, h = 0;
  int fmt = 0;
  ID3D11Texture2D *src = akvr_hudsplit_layer_image(w, h, fmt);
  if (!src || !g_ctx || g_viewSpace == XR_NULL_HANDLE || !w || !h) return false;
  if (!hud_swapchain(w, h, fmt)) return false;
  uint32_t idx = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(g_hudSwap, &ai, &idx))) return false;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  const bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(g_hudSwap, &wi)) && idx < g_hudImages.size();
  // Subresource 0 = mip 0 of slice 0: geo-11's real copy may be a per-eye array (HUDLAYER3).
  if (ok && !(g_hudColour == 1 && hud_conv_draw(src, idx)))
    g_ctx->CopySubresourceRegion(g_hudImages[idx], 0, 0, 0, 0, src, 0, nullptr);
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(g_hudSwap, &ri);
  if (!ok) return false;

  float D = akvr_geo11_hud_dist();
  if (D < 0.3f) D = 10.0f;                       // "far away"
  const float tl = tanf(fov.angleLeft), tr = tanf(fov.angleRight);
  const float tu = tanf(fov.angleUp), td = tanf(fov.angleDown);
  const XrQuaternionf ident{0.0f, 0.0f, 0.0f, 1.0f};
  const XrQuaternionf ori = g_tiltUsed != 0.0f ? pitch_postmul(ident, -g_tiltUsed) : ident;
  g_hudViewPose.orientation = ori;
  g_hudViewPose.position = quat_rotate(ori, XrVector3f{D * (tr + tl) * 0.5f, D * (tu + td) * 0.5f, -D});
  q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied (HUD-004)
  q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  q.subImage.swapchain = g_hudSwap;
  q.subImage.imageArrayIndex = 0;
  q.subImage.imageRect.offset = {0, 0};
  q.subImage.imageRect.extent = {(int32_t)w, (int32_t)h};
  q.size = {D * (tr - tl), D * (tu - td)};
  if (!hud_place(q, g_predicted)) return false;
  g_hudDistUsed = D; g_hudSizeW = q.size.width; g_hudSizeH = q.size.height;
  ++g_hudSubmits;
  return true;
}

// ZOOMVIG 2026-09-29 — JJ: the right-stick-click zoom shows a 2D vignette overlay that "doesn't work very well"
// in VR (our FOV lock cancels the magnification, so only the overlay is left); wanted instead: "a strong
// vignette effect that covers all your field of vision, at the peripherals". A VIEW-space quad 1 m ahead,
// 5 m square (+-68 deg, past the headset's view), black with an alpha ramp: clear inside g_vigClearDeg, full
// strength g_vigRampDeg further out. On while the game's own FOV is below g_vigBelowDeg in gameplay (the zoom),
// or while previewing. The image is rewritten only when its look changes (a fade steps through 8 levels).
float g_vigStrength = 0.9f;    // 0 = off
float g_vigClearDeg = 22.0f;   // clear centre, half-angle
float g_vigRampDeg = 18.0f;    // from clear to full strength
float g_vigBelowDeg = 45.0f;   // zoom = the game's own FOV narrower than this (to be measured)
bool g_vigPreview = false;
float g_vigAmt = 0.0f;         // 0..1 fade
float g_gameFovMin = 0.0f, g_gameFovMax = 0.0f;
XrSwapchain g_vigSwap = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D *> g_vigImages;
float g_vigDrawn[3] = {-1.0f, -1.0f, -1.0f};   // strength, clear, ramp as last written
int g_vigErr = 0;
constexpr int kVigPx = 512;
constexpr float kVigDist = 1.0f, kVigSize = 5.0f;
bool vig_swapchain() {
  if (g_vigSwap != XR_NULL_HANDLE) return true;
  if (g_vigErr) return false;
  uint32_t n = 0;
  xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
  std::vector<int64_t> formats(n);
  xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
  int64_t chosen = 0;   // black: only the alpha byte (byte 3 in both orders) matters
  for (int64_t f : formats)
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
        f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { chosen = f; break; }
  if (!chosen) { g_vigErr = 1; return false; }
  XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  sci.format = chosen;
  sci.sampleCount = 1;
  sci.width = kVigPx;
  sci.height = kVigPx;
  sci.faceCount = 1;
  sci.arraySize = 1;
  sci.mipCount = 1;
  if (XR_FAILED(xrCreateSwapchain(g_session, &sci, &g_vigSwap))) { g_vigSwap = XR_NULL_HANDLE; g_vigErr = 2; return false; }
  uint32_t imgCount = 0;
  xrEnumerateSwapchainImages(g_vigSwap, 0, &imgCount, nullptr);
  std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  xrEnumerateSwapchainImages(g_vigSwap, imgCount, &imgCount, (XrSwapchainImageBaseHeader *)imgs.data());
  for (auto &i : imgs) g_vigImages.push_back(i.texture);
  return true;
}
// Write the ramp into the next swapchain image (only when the look changed); the runtime keeps showing
// the last released image, so frames in between submit the layer without touching the swapchain.
bool vig_fill(float strength) {
  if (!g_ctx || !vig_swapchain()) return false;
  if (g_vigDrawn[0] == strength && g_vigDrawn[1] == g_vigClearDeg && g_vigDrawn[2] == g_vigRampDeg) return true;
  uint32_t idx = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(g_vigSwap, &ai, &idx))) return false;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  const bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(g_vigSwap, &wi)) && idx < g_vigImages.size();
  if (ok) {
    static std::vector<uint32_t> px;
    px.resize((size_t)kVigPx * kVigPx);
    const float d2r = 3.14159265f / 180.0f;
    const float a0 = g_vigClearDeg * d2r, a1 = (g_vigClearDeg + (g_vigRampDeg < 1.0f ? 1.0f : g_vigRampDeg)) * d2r;
    for (int y = 0; y < kVigPx; ++y)
      for (int x = 0; x < kVigPx; ++x) {
        // tangent-space radius at 1 m: the quad spans +-kVigSize/2 metres
        const float u = ((x + 0.5f) / kVigPx - 0.5f) * kVigSize / kVigDist;
        const float v = ((y + 0.5f) / kVigPx - 0.5f) * kVigSize / kVigDist;
        const float ang = atanf(sqrtf(u * u + v * v));
        float t = (ang - a0) / (a1 - a0);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        t = t * t * (3.0f - 2.0f * t);
        const uint32_t a = (uint32_t)(strength * t * 255.0f + 0.5f);
        px[(size_t)y * kVigPx + x] = a << 24;   // black, premultiplied
      }
    g_ctx->UpdateSubresource(g_vigImages[idx], 0, nullptr, px.data(), kVigPx * 4, 0);
  }
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(g_vigSwap, &ri);
  if (!ok) return false;
  g_vigDrawn[0] = strength; g_vigDrawn[1] = g_vigClearDeg; g_vigDrawn[2] = g_vigRampDeg;
  return true;
}
// Per ended frame: fade toward on/off, and build the layer when any of it shows.
bool vig_layer(XrCompositionLayerQuad &q, bool gameplay) {
  const float fov = akvr_camera_game_fov();
  if (fov > 1.0f) {
    if (g_gameFovMin <= 0.0f || fov < g_gameFovMin) g_gameFovMin = fov;
    if (fov > g_gameFovMax) g_gameFovMax = fov;
  }
  const bool want = g_vigStrength > 0.0f && (g_vigPreview || (gameplay && fov > 1.0f && fov < g_vigBelowDeg));
  const float step = 1.0f / 8.0f;   // ~0.1 s at 90 Hz for the whole fade
  g_vigAmt = want ? (g_vigAmt + step > 1.0f ? 1.0f : g_vigAmt + step) : (g_vigAmt - step < 0.0f ? 0.0f : g_vigAmt - step);
  if (g_vigAmt <= 0.0f || g_viewSpace == XR_NULL_HANDLE) return false;
  if (!vig_fill(g_vigStrength * g_vigAmt)) return false;
  q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  q.space = g_viewSpace;
  q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  q.subImage.swapchain = g_vigSwap;
  q.subImage.imageArrayIndex = 0;
  q.subImage.imageRect.offset = {0, 0};
  q.subImage.imageRect.extent = {kVigPx, kVigPx};
  q.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  q.pose.position = {0.0f, 0.0f, -kVigDist};
  q.size = {kVigSize, kVigSize};
  return true;
}
XrCompositionLayerQuad g_vigQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
bool g_vigHas = false;
} // namespace

void  akvr_xr_set_projvr(bool on) { g_projvrOn = on; }
bool  akvr_xr_yawfold_on() { return g_yawFoldOn; }
void  akvr_xr_yawfold_on_set(bool on) { g_yawFoldOn = on; }
float akvr_xr_yawfold() { return g_yawFold; }
void  akvr_xr_yawfold_set(float gain)
{
    if (gain < -1.5f) gain = -1.5f;
    if (gain >  1.5f) gain =  1.5f;
    g_yawFold = gain;
}
const char* akvr_xr_yawfold_diag() { return g_foldDiag; }
void  akvr_xr_our_sizes(uint32_t& eyeW, uint32_t& eyeH, uint32_t& ovW, uint32_t& ovH)
{ eyeW = g_eyeW; eyeH = g_swH; ovW = g_ovW; ovH = g_ovH; }
// Roll (deg) of the pose the compositor DISPLAYS the current frame at (g_layerPose) —
// for the roll-stereo diagnostic: compare against the roll the game actually renders.
float akvr_xr_layer_roll() {
  float y, p, r; quat_to_euler(g_layerPose.orientation, y, p, r); return r;
}
// Diagnostic: the runtime's real per-eye frusta vs the single symmetric FOV we
// submit. A non-zero "centre" column is a lateral image offset, in radians.
void akvr_xr_fov_report(char* buf, int n) {
  if (!buf || n <= 0) return;
  if (!g_rawFovOk) { snprintf(buf, (size_t)n, "no views located yet"); return; }
  const float R2D = 57.2957795f;
  snprintf(buf, (size_t)n,
           "L raw  L%.2f R%.2f U%.2f D%.2f deg   centre h%+.2f v%+.2f\n"
           "   R raw  L%.2f R%.2f U%.2f D%.2f deg   centre h%+.2f v%+.2f\n"
           "   we submit SYMMETRIC +-%.2f x +-%.2f deg to BOTH eyes",
           g_rawFov[0].angleLeft * R2D, g_rawFov[0].angleRight * R2D,
           g_rawFov[0].angleUp * R2D, g_rawFov[0].angleDown * R2D,
           (g_rawFov[0].angleRight + g_rawFov[0].angleLeft) * 0.5f * R2D,
           (g_rawFov[0].angleUp + g_rawFov[0].angleDown) * 0.5f * R2D,
           g_rawFov[1].angleLeft * R2D, g_rawFov[1].angleRight * R2D,
           g_rawFov[1].angleUp * R2D, g_rawFov[1].angleDown * R2D,
           (g_rawFov[1].angleRight + g_rawFov[1].angleLeft) * 0.5f * R2D,
           (g_rawFov[1].angleUp + g_rawFov[1].angleDown) * 0.5f * R2D,
           g_subHalfH * R2D, g_subHalfV * R2D);
}

// True only once xrLocateViews has actually returned real frusta. Until then
// g_headHalfH/V still hold their 0.86/0.86 placeholders, whose ratio is exactly
// 1.0 — which reads as a perfectly plausible "square headset" and will silently
// poison anything derived from the shape. Callers deriving geometry MUST gate on
// this rather than on the values looking sane.
bool akvr_xr_fov_measured() { return g_rawFovOk; }

void  akvr_xr_eye_half_fov(float& h, float& v) {
  // MUST match the present-side half-angles (below) or render≠display → stretch/skew.
  // Same FOV trim the layer uses is added here so the game renders exactly what we show.
  float trimRad = akvr_head_fov_delta() * (3.14159265f / 180.0f) * 0.5f;
  h = g_headHalfH + trimRad;
  v = g_headHalfV + trimRad;
}
// Vertical centre shift for the projection (added to m[9]; 0 unless FULLVIEW).
float akvr_xr_eye_v_offset() { return g_vOffNdc; }
bool akvr_xr_full_view() { return g_fullView; }
bool akvr_xr_tilt_fill() { return g_tiltFill; }
void akvr_xr_tilt_fill_set(bool on) { g_tiltFill = on; }
float akvr_xr_tilt_used_deg() { return g_tiltUsed * (180.0f / 3.14159265f); }
void akvr_xr_full_view_set(bool on) { g_fullView = on; if (!on) g_vOffNdc = 0.0f; }

void akvr_xr_try_init() {
  if (g_ok)
    return;

  if (!has_d3d11_ext()) {
    snprintf(
        g_status, sizeof(g_status),
        "OpenXR: no runtime / no D3D11 (start SteamVR or Virtual Desktop)");
    return;
  }

  if (g_inst == XR_NULL_HANDLE) {
    const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "AKVR");
    ci.applicationInfo.apiVersion =
        XR_MAKE_VERSION(1, 0, 0); // 1.0 = universally supported
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r)) {
      snprintf(g_status, sizeof(g_status),
               "OpenXR: xrCreateInstance failed (%d)", (int)r);
      g_inst = XR_NULL_HANDLE;
      return;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_inst, &ip))) {
      std::strncpy(g_runtime, ip.runtimeName, 127);
      g_runtime[127] = 0;
      // ANYHEADSET 2026-09-29 — the per-eye HUD shift makes up for Virtual Desktop drawing quad layers
      // with no eye difference (memory vd-quad-layers-no-parallax). A runtime that draws them correctly
      // (SteamVR, Meta Quest Link, ...) would get the difference twice: HUD at the wrong depth. So only
      // under Virtual Desktop.
      g_hudEyes = std::strstr(g_runtime, "VirtualDesktop") ? 1 : 0;
    }
  }
  // OFXRBRIDGE 2026-09-27 — JJ wants OFXR Bridge (optical-flow frame generation, an
  // implicit OpenXR API layer: references/OFXR-Bridge) to work with AKVR. It makes the
  // app run a virtual half-rate loop and submits a synthetic + the real frame per app
  // frame. Our FPSLOCK fights that: its repeat submissions look like new real frames
  // (generation between identical pictures, an extra paced cycle each), and SSW mode's
  // sleep stacks on the bridge's own half rate. The loader loads layers at
  // xrCreateInstance, so the bridge's DLL being in the process means it is active: the
  // lock then stands down (see fps_lock_eff) and the panel says so.
  g_ofxr = GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll") != nullptr;

  XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
  si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrResult r = xrGetSystem(g_inst, &si, &g_sys);
  if (XR_FAILED(r)) {
    // RETRYING xrGetSystem ON THE SAME INSTANCE IS NOT ENOUGH — measured
    // 2026-08-06/07. With a headset genuinely connected to Virtual Desktop and
    // nothing else holding it, VDXR returned XR_ERROR_FORM_FACTOR_UNAVAILABLE
    // (-35) forever. Some runtimes settle their device list when the INSTANCE is
    // created, so an instance made before the headset was ready never sees it,
    // however many times you ask. The game's instance is created at the first
    // Present, which is almost always before the player has the headset on — so
    // this is the normal case, not an edge case.
    //
    // So: tear the instance down and build a fresh one every few seconds while
    // there is no system. Costs nothing when idle and self-heals whenever the
    // headset appears, in any order.
    static ULONGLONG lastRebuild = 0;
    static int rebuilds = 0;
    ULONGLONG now = GetTickCount64();
    if (!lastRebuild)
      lastRebuild = now;
    if (now - lastRebuild > 4000) {
      lastRebuild = now;
      ++rebuilds;
      xrDestroyInstance(g_inst);
      g_inst = XR_NULL_HANDLE;
      g_sys = XR_NULL_SYSTEM_ID;
    }
    snprintf(g_status, sizeof(g_status),
             "OpenXR: '%s' up, no HMD yet (put headset on) [%d, retry %d]",
             g_runtime, (int)r, rebuilds);
    return;
  }

  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  char sysname[128] = "HMD";
  if (XR_SUCCEEDED(xrGetSystemProperties(g_inst, g_sys, &sp))) {
    std::strncpy(sysname, sp.systemName, 127);
    sysname[127] = 0;
  }

  g_ok = true;
  snprintf(g_status, sizeof(g_status), "OpenXR LIVE:  %s  /  %s", g_runtime,
           sysname);
}

// ---- per-Present frame loop (M4b) ----
static double s_waitMs = 0.0;
double akvr_xr_wait_ms() { return s_waitMs; }

// FPSLOCK 2026-09-26 — JJ: the Batmobile "jumps back and forth" on spins. Measured
// (frames.csv 18:10): new frames stay on screen for 1 or 2 refreshes in an irregular
// ~70/30 mix while the game moves its camera by its own clock, so equal camera steps
// get unequal screen time. The lock holds EVERY game frame for exactly N refreshes.
// Done on the headset's clock, not the engine's MaxFPS (playbook ch09
// #runtime-owns-pacing / FAIL-XR-024: two schedulers fight; checkout b955f22), and
// as a no-op when the game is late (ch09 #wait-frame-is-not-a-pacer): at the START
// of a Present, if the new frame is early, the PREVIOUS layers are submitted again
// for the in-between refreshes (legal: a layer references the swapchain's last
// released image), then the new frame takes the slot N refreshes after the last.
// The game renders freely meanwhile; only an early frame is held back.
int    g_fpsLock = 0;          // 0 = off, else the target (45 / 40 / 30)
static inline int fps_lock_eff() { return g_ofxr ? 0 : g_fpsLock; }
int    g_lockDiv = 1;          // refreshes per game frame actually used
int    g_lockRepeats = 0;      // repeat submissions before the current frame
long   g_lockLate = 0;         // new frames that missed their slot (game too slow)
long   g_lockFrames = 0;       // new frames while the lock was on
XrTime g_lastEndDisplay = 0;   // display time of the last NEW frame submitted
XrCompositionLayerProjectionView g_rpViews[2]{};
XrCompositionLayerProjection g_rpLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
XrCompositionLayerQuad g_rpQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
bool   g_rpHasProj = false, g_rpHasQuad = false;
// FPSLOCK-SSW 2026-09-26 — JJ: choose between repeating the frame and letting Virtual
// Desktop's SSW synthesise the in-between refreshes. SSW only works if the runtime sees
// the app at half rate, so in this mode NOTHING is resubmitted: before xrWaitFrame the
// Present thread sleeps until a quarter period past the slot it wants to skip, so the
// wait returns the slot N refreshes after the last one. A late frame never sleeps.
bool   g_lockSkip = false;
double g_lastWaitMs = 0.0;       // QPC ms when the last xrWaitFrame returned
long   g_lockEarly = 0;          // SSW mode: frames that still came back too early
static double qpc_ms() {
  LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
  return 1000.0 * (double)q.QuadPart / (double)f.QuadPart;
}
static void sleep_until_ms(double target) {
  static bool s_period = false;
  if (!s_period) { timeBeginPeriod(1); s_period = true; }
  for (;;) {
    const double left = target - qpc_ms();
    if (left <= 0.0 || left > 100.0) return;    // done, or nonsense: never stall
    if (left > 1.5) Sleep((DWORD)(left - 1.0));
    else YieldProcessor();
  }
}
bool akvr_xr_fps_lock_ssw() { return g_lockSkip; }
void akvr_xr_fps_lock_ssw_set(bool on) { g_lockSkip = on; }
long akvr_xr_fps_lock_early() { return g_lockEarly; }
int  akvr_xr_fps_lock() { return g_fpsLock; }
void akvr_xr_fps_lock_set(int fps) { g_fpsLock = (fps == 45 || fps == 40 || fps == 30) ? fps : 0; }
void akvr_xr_fps_lock_info(int &div, double &hz, long &late, long &frames) {
  div = g_lockDiv; late = g_lockLate; frames = g_lockFrames;
  hz = g_displayPeriod > 0 ? 1e9 / (double)g_displayPeriod : 0.0;
}

void akvr_xr_frame_begin() {
  s_waitMs = 0.0;
  g_frameBegun = false;
  g_ovDrewThisFrame = false; // UI re-asserts this each Present it renders
  if (!g_ok || !g_device)
    return;

  if (g_session == XR_NULL_HANDLE) {
    // SESSBACKOFF 2026-09-28 — JJ: after Onimusha VR, Arkham "stumbles at half a frame a second" and never
    // enters VR. VD's OpenXR.log: every xrCreateSession fails (QueryInterface on VD's own submission device,
    // 80004002) and each attempt blocks ~1 s - we retried on EVERY Present. Back off 3, 6, 12, 24, then 30 s,
    // so the game stays playable and still enters VR once the runtime recovers.
    static ULONGLONG s_nextTry = 0;
    static int s_fails = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_nextTry)
      return;
    if (!create_session()) {
      ++s_fails;
      const ULONGLONG wait = s_fails >= 5 ? 30000ull : 3000ull << (s_fails - 1);
      s_nextTry = GetTickCount64() + wait;
      const size_t n = strlen(g_sessStatus);
      snprintf(g_sessStatus + n, sizeof(g_sessStatus) - n, " | try %d, next in %llus (restart VD Streamer or reboot)",
               s_fails, wait / 1000ull);
      return;
    }
    s_fails = 0;
  }

  pump_events();
  if (g_state == XR_SESSION_STATE_EXITING ||
      g_state == XR_SESSION_STATE_LOSS_PENDING) {
    g_running = false;
    return;
  }
  if (!g_running)
    return;

  XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState fs{XR_TYPE_FRAME_STATE};
  LARGE_INTEGER waitStart, waitEnd, waitFreq;
  QueryPerformanceFrequency(&waitFreq);
  QueryPerformanceCounter(&waitStart);
  // FPSLOCK-SSW: skip slots by waiting, never by resubmitting (see g_lockSkip).
  if (fps_lock_eff() > 0 && g_lockSkip && g_lastWaitMs > 0.0 && g_displayPeriod > 0) {
    const double perMs = (double)g_displayPeriod / 1e6;
    int div = (int)floor(1000.0 / perMs / (double)fps_lock_eff() + 0.5);
    div = div < 1 ? 1 : (div > 6 ? 6 : div);
    g_lockDiv = div;
    sleep_until_ms(g_lastWaitMs + ((double)(div - 1) + 0.25) * perMs);
  }
  XrResult waitResult = xrWaitFrame(g_session, &fwi, &fs);
  g_lastWaitMs = qpc_ms();
  if (fps_lock_eff() > 0 && g_lockSkip && XR_SUCCEEDED(waitResult) && g_lastEndDisplay != 0 &&
      fs.predictedDisplayPeriod > 0) {
    const XrDuration per = fs.predictedDisplayPeriod;
    ++g_lockFrames;
    if (fs.predictedDisplayTime > g_lastEndDisplay + (XrTime)g_lockDiv * per + per / 2) ++g_lockLate;
    if (fs.predictedDisplayTime < g_lastEndDisplay + (XrTime)g_lockDiv * per - per / 2) ++g_lockEarly;
  }
  // FPSLOCK (see fps_lock_eff()): hold an early frame by re-showing the previous one.
  g_lockRepeats = 0;
  if (XR_SUCCEEDED(waitResult) && fps_lock_eff() > 0 && !g_lockSkip && g_lastEndDisplay != 0 &&
      fs.predictedDisplayPeriod > 0) {
    const XrDuration per = fs.predictedDisplayPeriod;
    int div = (int)floor(1e9 / (double)per / (double)fps_lock_eff() + 0.5);
    div = div < 1 ? 1 : (div > 6 ? 6 : div);
    g_lockDiv = div;
    const XrTime due = g_lastEndDisplay + (XrTime)div * per - per / 2;
    while (XR_SUCCEEDED(waitResult) && fs.predictedDisplayTime < due &&
           g_lockRepeats < div) {
      XrFrameBeginInfo rb{XR_TYPE_FRAME_BEGIN_INFO};
      if (XR_FAILED(xrBeginFrame(g_session, &rb)))
        break;
      const XrCompositionLayerBaseHeader *rl[6];
      uint32_t rn = 0;
      if (g_rpHasProj) rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_rpLayer;
      // HUDLAYER4: the HUD is re-placed for THIS frame's display time (head-locked at 90 Hz).
      if (g_rpHasProj && g_rpHasHud && hud_place(g_rpHud, fs.predictedDisplayTime)) {
        if (g_hudEyes) {   // HUDLAYER5
          hud_eye_pair(g_rpHud, g_rpHudL, g_rpHudR);
          rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_rpHudL;
          rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_rpHudR;
        } else
          rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_rpHud;
      }
      if (g_rpHasProj && g_vigHas) rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_vigQuad;   // ZOOMVIG
      if (g_rpHasQuad) rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_rpQuad;
      XrFrameEndInfo re{XR_TYPE_FRAME_END_INFO};
      re.displayTime = fs.predictedDisplayTime;
      re.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
      re.layerCount = rn;
      re.layers = rn ? rl : nullptr;
      xrEndFrame(g_session, &re);
      ++g_lockRepeats;
      fs = XrFrameState{XR_TYPE_FRAME_STATE};
      waitResult = xrWaitFrame(g_session, &fwi, &fs);
    }
    ++g_lockFrames;
    if (XR_SUCCEEDED(waitResult) &&
        fs.predictedDisplayTime > g_lastEndDisplay + (XrTime)div * per + per / 2)
      ++g_lockLate;
  } else if (!(fps_lock_eff() > 0 && g_lockSkip)) {
    g_lockDiv = 1;
  }
  QueryPerformanceCounter(&waitEnd);
  s_waitMs = 1000.0 * double(waitEnd.QuadPart - waitStart.QuadPart) / double(waitFreq.QuadPart);
  if (XR_FAILED(waitResult))
    return; // paces us to the headset
  g_predicted = fs.predictedDisplayTime;
  g_displayPeriod = fs.predictedDisplayPeriod;
  g_shouldRender = fs.shouldRender;

  XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
  if (XR_FAILED(xrBeginFrame(g_session, &fbi)))
    return;
  g_frameBegun = true;

  // Sample the ONE predicted head pose for this frame. The pose that drew the
  // backbuffer we're about to submit is last Present's pose (g_curPose) — shift
  // it into g_layerPose, then take the fresh sample as the new g_curPose.
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_SUCCEEDED(
          xrLocateSpace(g_viewSpace, g_localSpace, g_predicted, &loc))) {
    // seed from the last published pose so an invalid bit just holds the prev
    // value
    float y, p, r, px, py, pz, qx, qy, qz, qw;
    akvr_xr_head_pose(y, p, r, px, py, pz);
    akvr_xr_head_quat(qx, qy, qz, qw);
    XrPosef newPose = g_curPose;
    if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
      // TILTFILL: pitch the sampled pose down about its own right axis, so the
      // camera AND the submitted layer both carry it (see g_tiltFill).
      // Not in the floating screen: that screen hangs level, so a tilted camera
      // would put the horizon low in the window.
      const bool tilt = g_tiltFill && g_fullView && !g_projvrOn && g_native && !g_forceScreen &&
                        g_tiltRad > 0.0f && g_tiltRad < 0.35f;
      g_tiltUsed = tilt ? g_tiltRad : 0.0f;
      if (tilt) loc.pose.orientation = pitch_postmul(loc.pose.orientation, -g_tiltRad);
      newPose.orientation = loc.pose.orientation;
      quat_to_euler(loc.pose.orientation, y, p, r); // display-only now
      qx = loc.pose.orientation.x;
      qy = loc.pose.orientation.y;
      qz = loc.pose.orientation.z;
      qw = loc.pose.orientation.w;
    }
    if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
      newPose.position = loc.pose.position;
      px = loc.pose.position.x;
      py = loc.pose.position.y;
      pz = loc.pose.position.z;
      g_posValid = true;
    }

    // Pose that rendered the current backbuffer: P_{N-delay} (delay 1 = P_{N-1},
    // the old fixed assumption). g_curPose is the pose the game renders next (P_N).
    // FRAMEID: every camera finalize since the last Present consumed the pose we
    // wrote LAST Present (sequence g_poseHead, before this increment). Record that,
    // so a finalize index can be turned back into the exact pose it drew with.
    {
      const uint64_t fc = akvr_camera_finalize_count();
      if (g_finRecorded == 0 || fc < g_finRecorded || fc - g_finRecorded > 200)
        g_finRecorded = fc;                        // first call, or a reset: no history
      while (g_finRecorded < fc) {
        ++g_finRecorded;
        g_seqForFin[g_finRecorded & 255u] = g_poseHead;
      }
    }
    g_poseHist[++g_poseHead & 63u] = newPose;
    // Which pose drew the picture being presented now? Exactly, when the engine's
    // own frame counters are found (frameid.cpp); otherwise the fixed delay JJ set.
    uint32_t delay = (uint32_t)g_poseDelay;
    g_poseMatched = false;
    uint64_t pf = 0;
    if (g_poseAuto && akvr_frameid_presented_finalize(pf) && pf <= g_finRecorded &&
        g_finRecorded - pf < 200) {
      const uint32_t d = g_poseHead - g_seqForFin[pf & 255u];
      if (d >= 1 && d <= 30) { delay = d; g_poseMatched = true; }
    }
    g_usedDelay = (int)delay;
    if (g_poseMatched && delay < 16) ++g_delayHist[delay];
    g_layerPose = g_poseHist[(g_poseHead - delay) & 63u];
    g_curPose = newPose;
    publish_pose(y, p, r, px, py, pz, qx, qy, qz,
                 qw); // camera injection uses P_N this Present
    // Runtime recentre (Meta button): once this frame's pose is in the new space,
    // take it as our zero too, and re-hang any floating screen in front of you.
    if (g_recenterPending && g_predicted >= g_recenterAt) {
      g_recenterPending = false;
      akvr_head_recenter();
      g_refixScreen = true;
      g_menu3dWas = false;   // re-take the pinned-menu reference as well
    }
  }

  // Measure the Quest 3's real per-eye FOV (drives both the display layer and
  // the game's render FOV). Average the two eyes' symmetric extents.
  {
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_predicted;
    vli.space = g_localSpace;
    XrViewState vst{XR_TYPE_VIEW_STATE};
    uint32_t nv = 0;
    XrView vv[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vst, 2, &nv, vv)) &&
        nv == 2) {
      {   // HUDLAYER5: half the eye distance, for the HUD's optional eye shift
        const float dx = vv[1].pose.position.x - vv[0].pose.position.x;
        const float dy = vv[1].pose.position.y - vv[0].pose.position.y;
        const float dz = vv[1].pose.position.z - vv[0].pose.position.z;
        const float ipd = sqrtf(dx * dx + dy * dy + dz * dz);
        if (ipd > 0.04f && ipd < 0.09f) g_hudHalfIpd = 0.5f * ipd;
      }
      float hH = ((vv[0].fov.angleRight - vv[0].fov.angleLeft) +
                  (vv[1].fov.angleRight - vv[1].fov.angleLeft)) *
                 0.25f;
      float hV = ((vv[0].fov.angleUp - vv[0].fov.angleDown) +
                  (vv[1].fov.angleUp - vv[1].fov.angleDown)) *
                 0.25f;
      // FULLVIEW 2026-09-26 — JJ: "black at the very left and right, and top and
      // bottom". The averaged symmetric box (+-42.35 x +-45) missed each eye's OUTER
      // 7 deg and the bottom 5.6 deg (Quest 3 via VDXR: L -49.34..35.36,
      // U 39.43 / D -50.57, mirrored for the right eye). One shared projection feeds
      // both geo-11 eyes, so: horizontally render the widest outer edge (spends the
      // 16:9 frame's surplus width, which was oversampled vs the vertical anyway);
      // vertically render exactly up..down with an OFF-CENTRE projection (same rows,
      // none wasted). g_vOffNdc is that centre shift, in NDC, for camera.cpp.
      if (g_fullView && hH > 0.1f && hV > 0.1f) {
        float outer = 0.0f, inner = 10.0f, tU = 0.0f, tD = 0.0f;
        for (int e = 0; e < 2; ++e) {
          outer = fmaxf(outer, fmaxf(-vv[e].fov.angleLeft, vv[e].fov.angleRight));
          inner = fminf(inner, fminf(-vv[e].fov.angleLeft, vv[e].fov.angleRight));
          tU = fmaxf(tU, tanf(vv[e].fov.angleUp));
          tD = fmaxf(tD, tanf(-vv[e].fov.angleDown));
        }
        if (outer > 0.1f && tU > 0.05f && tD > 0.05f) {
          hH = outer;
          g_eyeTanOut = tanf(outer);
          g_eyeTanIn = inner > 0.1f ? tanf(inner) : 0.0f;
          g_eyeTanUp = tU;
          g_eyeTanDown = tD;
          // EYEVIEW: render only the half-width each eye needs (drives the game's FOV lock
          // too); the geo-11 off-axis and the off-centre submission put it where the lens is.
          if (g_eyeView && g_eyeNarrowOk && g_eyeTanIn > 0.1f)
            hH = atanf((g_eyeTanOut + g_eyeTanIn) * 0.5f);
          hV = atanf((tU + tD) * 0.5f);
          g_vOffNdc = (tD - tU) / (tU + tD);
          g_tiltRad = (atanf(tD) - atanf(tU)) * 0.5f;
        }
      } else {
        g_vOffNdc = 0.0f;
        g_tiltRad = 0.0f;
      }
      if (hH > 0.1f && hV > 0.1f) {
        g_headHalfH = hH;
        g_headHalfV = hV;
      }
      // Keep the RAW per-eye frusta for diagnosis. We collapse them to one
      // symmetric FOV above; a headset whose real frusta are asymmetric (they
      // usually are) would then show a constant lateral offset, which is exactly
      // the residual JJ reports. Measure it instead of assuming it.
      for (int e = 0; e < 2; ++e) g_rawFov[e] = vv[e].fov;
      g_rawFovOk = true;
    }
  }

  // AER eye sync — derive BOTH eyes from the single per-rendered-frame counter
  // (increments exactly once per camera-finalize = once per rendered frame, the
  // cadence=1.000 clock). Both the render eye and the display eye come from the
  // SAME number, so they can't drift apart across the game/present thread
  // boundary (which was causing frames to land in the wrong eye). The frame we
  // submit now has counter parity fc&1; the frame the game renders next will be
  // fc+1.
  if (g_native) {
    // Native stereo: geo-11 renders both eyes every frame — no alternation.
    // The camera stays centered (-1); geo-11 applies the per-eye separation.
    g_submitEye = 0;
    g_injectEye = -1;
  } else {
    uint64_t fc = akvr_camera_finalize_count();
    g_submitEye = (int)(fc & 1ULL);
    g_injectEye = (int)((fc + 1ULL) & 1ULL);
  }

  katanga_update(); // open/refresh geo-11's shared stereo texture (native
                    // capture)

  snprintf(g_sessStatus, sizeof(g_sessStatus), "session: running (state %d)%s",
           (int)g_state, g_posValid ? ", 6DOF" : ", 3DOF");
}

void akvr_xr_frame_submit(IDXGISwapChain *swapChain, float gameFovDeg,
                          bool gameplay) {
  if (!g_frameBegun)
    return; // nothing was begun this Present
  g_frameBegun = false;
  if (gameFovDeg > 10.0f && gameFovDeg < 170.0f)
    g_lastGameFov = gameFovDeg;

  // Choose where the flat image lives. Gameplay: pin to the head (the game
  // camera counter-rotates the world → world-lock). 2D screens (menus/loading/
  // splash, no camera): freeze to a world-fixed pose the moment gameplay stops,
  // so the screen floats in place where you were looking instead of dragging
  // with your head. F6 forces the world-fixed path for menus that keep a live
  // 3D camera (main menu / loading) which auto-detection can't catch yet.
  // JJ 2026-08-05: "when first launching into the headset, the logo screens appear
  // TILTED as if my head is rolled, but it is not." Cause: we froze the head's FULL
  // orientation, so whatever roll and pitch you happened to have at that instant —
  // typically while putting the headset on, or with it still in your hands — became
  // the screen's permanent tilt. A screen hanging in the world should hang LEVEL.
  //
  // Keep the heading and throw away pitch and roll, by extracting the twist about the
  // tracking-space up axis (+Y in OpenXR). Done as a quaternion projection rather than
  // via Euler angles on purpose: this project has been bitten repeatedly by Euler
  // convention and multiply-order mistakes (see the roll-in-stereo history in
  // CAMERA_MAP.md), and the projection has no convention to get wrong.
  auto level_pose = [](const XrPosef &p) {
    XrPosef o = p;
    const float n = sqrtf(p.orientation.w * p.orientation.w +
                          p.orientation.y * p.orientation.y);
    if (n > 1e-6f) {
      o.orientation.x = 0.0f;
      o.orientation.y = p.orientation.y / n;
      o.orientation.z = 0.0f;
      o.orientation.w = p.orientation.w / n;
    } else {
      o.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return o;
  };

  {
    static int s_lastG = -1, s_lastA = -1;
    if ((int)gameplay != s_lastG) { mode_log(gameplay ? "game" : "screen"); s_lastG = gameplay; }
    if ((int)g_anamorphic != s_lastA) { mode_log(g_anamorphic ? "3D" : "flat"); s_lastA = g_anamorphic; }
  }
  if (g_autoMainMenu) {
    const ULONGLONG now = GetTickCount64();
    if (g_menuPhase == 0) {
      if (!gameplay) g_menuGameplaySince = 0;
      else if (!g_menuGameplaySince) g_menuGameplaySince = now;
      else if (now - g_menuGameplaySince > 3000) { g_menuPhase = 1; mode_log("MAIN MENU on"); }
    } else if (g_menuPhase == 1) {
      // Remember where the menu's cameras sit (JJ: the menu cross-fades between two
      // camera angles), one sample a second, merged within 2 m, at most 16.
      static ULONGLONG s_lastSample = 0;
      if (gameplay && g_camOk && now - s_lastSample > 1000) {
        s_lastSample = now;
        bool known = false;
        for (int i = 0; i < g_menuCamN; ++i) {
          const float dx = g_camPos[0] - g_menuCam[i][0], dy = g_camPos[1] - g_menuCam[i][1],
                      dz = g_camPos[2] - g_menuCam[i][2];
          if (dx * dx + dy * dy + dz * dz < 200.0f * 200.0f) { known = true; break; }
        }
        if (!known && g_menuCamN < 16) {
          for (int k = 0; k < 3; ++k) g_menuCam[g_menuCamN][k] = g_camPos[k];
          ++g_menuCamN;
        }
      }
      if (gameplay) {
        g_menuScreenSince = 0;
      } else {
        if (!g_menuScreenSince) g_menuScreenSince = now;
        else if (now - g_menuScreenSince > 5000) { g_menuPhase = 2; g_menuScreenSince = 0; mode_log("MAIN MENU off"); }
      }
    } else {
      // Back to the main menu later ("quit to main menu" left it stuck to the face):
      // a loading screen (5 s+ of screen) followed by a camera sitting at one of the
      // spots the main menu used. Checked for the first 2 s after the load only, so a
      // rooftop that happens to share coordinates cannot trigger it mid-game.
      static ULONGLONG s_afterLoad = 0;
      if (!gameplay) {
        if (!g_menuScreenSince) g_menuScreenSince = now;
        s_afterLoad = 0;
      } else {
        if (g_menuScreenSince && now - g_menuScreenSince > 5000) s_afterLoad = now;
        g_menuScreenSince = 0;
        if (s_afterLoad && now - s_afterLoad < 2000 && g_camOk) {
          for (int i = 0; i < g_menuCamN; ++i) {
            const float dx = g_camPos[0] - g_menuCam[i][0], dy = g_camPos[1] - g_menuCam[i][1],
                        dz = g_camPos[2] - g_menuCam[i][2];
            if (dx * dx + dy * dy + dz * dz < 500.0f * 500.0f) {
              g_menuPhase = 1; s_afterLoad = 0; mode_log("MAIN MENU again");
              break;
            }
          }
        }
      }
    }
  }
  {
    const bool m3 = akvr_xr_menu3d_active();
    if (m3 && !g_menu3dWas) {
      HeadTrackState ht = akvr_head_state();
      g_menu3dRefYaw = ht.yaw;
      g_menu3dRefPitch = ht.pitch;
    }
    g_menu3dWas = m3;
  }
  bool effGameplay = gameplay && !akvr_xr_screen_mode();
  // EVGAME: on only after 1.5 s of steady gameplay (JJ: the first menu "doubled up briefly, then
  // normal" - gameplay was reported for a moment before the main menu was recognised); off at once.
  {
    static ULONGLONG s_gpSince = 0;
    const bool gp = effGameplay && !akvr_xr_main_menu_detected();
    const ULONGLONG t = GetTickCount64();
    if (!gp) s_gpSince = 0; else if (!s_gpSince) s_gpSince = t;
    g_eyeWantGameplay = gp && t - s_gpSince > 1500;
    // HUDLAYER: redirect the HUD only in steady gameplay - and, since HUDWORLD (JJ 2026-09-29: "the main
    // menu is attached to the head as well"), on the live 3D main menu too, so its text hangs in the room.
    // MENUONE2 (JJ: the start screen's text "came in doubled, then the duplicate disappeared"): phase 0 -
    // before the main menu is confirmed - is splash / start screen too, so it counts as menu from the start.
    const bool menuLive = g_menu3d && g_autoMainMenu && g_menuPhase <= 1 && effGameplay;
    akvr_hudsplit_layer_gate(g_eyeWantGameplay && !menuLive, menuLive);
  }
  if (effGameplay) {
    g_wasGameplay = true;
  } else if (g_wasGameplay) {
    // PAUSEPOSE 2026-09-26 — JJ's F2 before/after pause: "the camera seems to have
    // rolled very slightly ... it doesn't match what you're looking at". The frozen
    // 3D picture (pause, map) was drawn with the head's full orientation, but it was
    // hung LEVEL (pitch and roll stripped). Hang it exactly as drawn: the pose the
    // LAST camera finalize consumed (the camera stops with the game). Flat 2D screens
    // (loading) still hang level.
    XrPosef drawn = g_layerPose;
    if (g_finRecorded) {
      const uint32_t seq = g_seqForFin[g_finRecorded & 255u];
      if (g_poseHead - seq < 60u) drawn = g_poseHist[seq & 63u];
    }
    g_fixedPose = g_anamorphic ? drawn : level_pose(g_curPose);
    g_wasGameplay = false;
  }
  if (g_refixScreen) {
    g_fixedPose = level_pose(g_curPose);
    g_refixScreen = false;
  }
  // At startup (before any gameplay), g_fixedPose is still identity at origin.
  // Without a real head pose the screen sits at the tracking-space floor —
  // invisible, and the compositor treats it as stale → head-locked drag.
  // Capture it from the live head pose on the first non-gameplay frame.
  if (!effGameplay && g_fixedPose.orientation.w == 1.0f &&
      g_fixedPose.position.x == 0.0f && g_fixedPose.position.y == 0.0f &&
      g_fixedPose.position.z == 0.0f)
    g_fixedPose = level_pose(g_curPose);   // the splash screens land here — keep them level
  XrPosef submitPose = effGameplay ? g_layerPose : g_fixedPose;

  // Copy the finished game frame into ONLY the current eye's swapchain (AER);
  // the other eye keeps its previous frame. g_submitEye is the eye this
  // backbuffer was rendered for (its camera was offset that way last Present).
  const bool holdScreen = g_holdScreen && !effGameplay && g_swapInit[0] && g_swapInit[1];
  if (g_shouldRender && swapChain && g_ctx && !holdScreen) {
    ID3D11Texture2D *backBuffer = nullptr;
    bool ownBackBuffer = false;
    bool fromKatanga = false;
    // GEO-11 NATIVE STEREO: the stereo pair does NOT live in the swapchain.
    // geo-11's katanga_vr mode publishes a separate shared surface ("DoubleTex",
    // the window doubled in WIDTH = side-by-side) and puts an OVER/UNDER preview
    // on the monitor. Judging the layout by the screen is the trap that cost us
    // a session — always take the shared surface. See geo11-works-on-ak-fresh-install.
    if (g_native && g_katFeed && g_katTex) {
      backBuffer = g_katTex;     // 5120x1440 SBS, one eye per half
      fromKatanga = true;
    } else if (g_sceneSrc) {
      backBuffer = g_sceneSrc;   // TAP: the game's near-square high-res scene buffer
    } else if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                              (void **)&backBuffer))) {
      ownBackBuffer = true;      // we own this ref → must Release it below
    }
    if (backBuffer) {
      D3D11_TEXTURE2D_DESC bd{};
      backBuffer->GetDesc(&bd);
      // The game can resize the backbuffer (graphics-settings menu, resolution
      // change). Our eye swapchains are sized to the OLD backbuffer, so the
      // size guard below would fail forever → black headset. Detect the change
      // and rebuild the eye swapchains at the new size.
      if (g_swap[0] != XR_NULL_HANDLE &&
          (bd.Width != g_swW || bd.Height != g_swH || g_sbsActive != (g_native && g_sbsOne)))
        destroy_swapchains();
      if (g_swap[0] == XR_NULL_HANDLE)
        create_swapchain(bd.Width, bd.Height, bd.Format);

      static int probeTick =
          0; // classify MONO vs SBS every ~45 frames (Map is a GPU sync)
      if (fromKatanga) {
        // The shared surface is SBS by construction and geo-11 is writing to it
        // live; a staging Map here would stall on another renderer's texture for
        // an answer we already know.
        snprintf(g_probeStatus, sizeof(g_probeStatus),
                 "source: geo-11 katanga surface %ux%u (SBS)", bd.Width,
                 bd.Height);
      } else if ((probeTick++ % 45) == 0) {
        probe_backbuffer(backBuffer, bd);
      }

      if (g_swap[0] != XR_NULL_HANDLE && bd.Width == g_swW &&
          bd.Height == g_swH) {
        // Native: split the SBS composite — left half -> left eye, right half
        // -> right eye, BOTH every frame.
        // AER gameplay: whole frame -> ONLY the submit eye (alternating).
        // AER non-gameplay (splash/menu/loading): whole frame -> BOTH eyes,
        //   since the camera isn't moving — both swapchains need the same
        //   image or the stale eye is black/wrong (e.g. splash left-eye
        //   black, menu text only right eye). Same pose for both.
        if (g_sbsActive) {
          uint32_t idx = 0;
          XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
          if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_swap[0], &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swap[0], &wi)) &&
                idx < g_swapImages[0].size()) {
              D3D11_BOX box{0, 0, 0, g_swW, g_swH, 1};
              g_ctx->CopySubresourceRegion(g_swapImages[0][idx], 0, 0, 0, 0, backBuffer, 0, &box);
              float by = 0.0f;
              const bool bok = akvr_camera_base_yaw_deg(by);
              for (int e = 0; e < 2; ++e) {
                g_swapInit[e] = true;
                g_eyePose[e] = submitPose;
                g_eyeBaseOk[e] = bok;
                g_eyeBaseYaw[e] = by;
              }
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(g_swap[0], &ri);
          }
        } else {
        int eFirst, eLast;
        if (g_native) {
          eFirst = 0;
          eLast = 1;
        } else if (effGameplay) {
          eFirst = g_submitEye;
          eLast = g_submitEye;
        } else {
          eFirst = 0;
          eLast = 1;
        }
        for (int e = eFirst; e <= eLast; ++e) {
          uint32_t idx = 0;
          XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
          if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_swap[e], &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swap[e], &wi)) &&
                idx < g_swapImages[e].size()) {
              if (g_native) {
                D3D11_BOX box{};
                // MENUFLAT 2026-09-26 — JJ: in the floating main menu "Batman is fixed
                // and kind of warps when your head moves". A stereo picture frozen on a
                // screen in the world does exactly that (the 3D-TV effect). While a
                // screen floats, give both eyes the same half: a flat picture.
                int sourceEye = swap_eff() ? (1 - e) : e;
                if (g_menuFlat && !effGameplay) sourceEye = swap_eff() ? 1 : 0;
                box.left = (UINT)sourceEye * g_eyeW;
                box.right = box.left + g_eyeW;
                box.top = 0;
                box.bottom = g_swH;
                box.front = 0;
                box.back = 1;
                g_ctx->CopySubresourceRegion(g_swapImages[e][idx], 0, 0, 0, 0,
                                             backBuffer, 0,
                                             &box); // this eye's half
              } else if (bd.SampleDesc.Count > 1) {
                // Tapped scene buffer is multisampled — CopyResource can't read MSAA;
                // resolve it into the (single-sample) eye image instead.
                g_ctx->ResolveSubresource(g_swapImages[e][idx], 0, backBuffer, 0, bd.Format);
              } else if (g_sceneSrc) {
                // TAP path: source (scene buffer) and dest (eye image) may differ in
                // size and/or format family, which makes a bare CopyResource a silent
                // no-op → frozen image. Read both descs, report them, and fall back to
                // a region copy (overlapping box) when a whole-resource copy is illegal.
                D3D11_TEXTURE2D_DESC ed{};
                g_swapImages[e][idx]->GetDesc(&ed);
                bool sameFamily = copy_family(bd.Format) == copy_family(ed.Format);
                bool sameSize = (bd.Width == ed.Width && bd.Height == ed.Height);
                if (e == eFirst)
                  snprintf(g_tapDiag, sizeof(g_tapDiag),
                           "src %ux%u f%d  eye %ux%u f%d  %s%s", bd.Width, bd.Height,
                           (int)bd.Format, ed.Width, ed.Height, (int)ed.Format,
                           sameFamily ? "" : "FMT-MISMATCH ",
                           sameSize ? "size-ok" : "SIZE-MISMATCH");
                if (sameFamily && sameSize) {
                  g_ctx->CopyResource(g_swapImages[e][idx], backBuffer);
                } else if (sameFamily) {
                  // Same byte layout, different size: copy the overlapping top-left box
                  // so the eye at least updates live (proves the source is fresh).
                  UINT cw = bd.Width < ed.Width ? bd.Width : ed.Width;
                  UINT ch = bd.Height < ed.Height ? bd.Height : ed.Height;
                  D3D11_BOX box{0, 0, 0, cw, ch, 1};
                  g_ctx->CopySubresourceRegion(g_swapImages[e][idx], 0, 0, 0, 0,
                                               backBuffer, 0, &box);
                }
                // FMT-MISMATCH: leave as-is; diag reports it and we handle it next build.
              } else
                g_ctx->CopyResource(g_swapImages[e][idx],
                                    backBuffer); // whole frame -> this eye
              g_swapInit[e] = true;
              g_eyePose[e] =
                  submitPose; // remember the pose this eye's image was drawn at
              // ...and the game camera's own heading at the same instant. The last
              // camera-finalize before this Present IS the frame we are copying, so
              // the two are from the same moment by construction.
              {
                float by = 0.0f;
                g_eyeBaseOk[e] = akvr_camera_base_yaw_deg(by);
                g_eyeBaseYaw[e] = by;
              }
            }
            XrSwapchainImageReleaseInfo ri{
                XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(g_swap[e], &ri);
          }
        }
        }   // SBSONE else
      }
      if (ownBackBuffer) backBuffer->Release();   // don't release the tapped scene buffer (not ours)
    }
  }

  bool submitted =
      g_swapInit[0] ||
      g_swapInit[1]; // something to show once either eye has a frame

  // Stereo projection layer: each eye reads its OWN swapchain (its latest
  // render) AND its OWN render-time pose (g_eyePose), so the compositor
  // timewarps each eye independently to display time — this is what removes the
  // AER turn-smear.
  XrCompositionLayerProjectionView views[2]{
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  // Match the layer frustum to what the game ACTUALLY rendered, at any
  // resolution: horizontal = the headset FOV (render is forced to it); vertical
  // = derived from the real backbuffer aspect (UE3 MaintainXFOV:
  // tan(halfV)=tan(halfH)*H/W). This kills the vertical stretch on non-square
  // frames AND the shear/warp on pitch (layer vertical-FOV now equals render
  // vertical-FOV → 1:1 in both axes). The Home/End trim (akvr_head_fov_add)
  // widens/narrows what the GAME renders — but that's useless unless the
  // HEADSET DISPLAY frame widens/narrows by the same amount too, since the two
  // must always match (mismatch = borders, or the world swimming when you turn
  // your head). This was a real bug: the trim only touched the render side.
  // Apply the same half-of-full-degrees trim here so they move together.
  // (fovDelta is a full-angle degrees value; half of it, in radians, is added
  // to the half-angle used everywhere below.)
  float trimRad = akvr_head_fov_delta() * (3.14159265f / 180.0f) * 0.5f;
  float halfH = g_headHalfH + trimRad;
  float halfV = halfH;
  if (g_projvrOn && (effGameplay || g_anamorphic)) {
    // projVR: the game rendered the Quest's true per-eye FOV (projection hook),
    // anamorphically into its 16:9 buffer. Present the WHOLE frame at that same
    // FOV so it un-squishes — full square view, no crop, no wasted render.
    halfV = g_headHalfV + trimRad;
  } else if (akvr_head_wide_render()) {
    // RE02: display the headset's actual VFOV and crop the centre headset HFOV
    // out of a wider 16:9 render. This preserves the full vertical FOV and gets
    // rid of the mail-slot, at the cost of softer horizontal resolution.
    halfV = g_headHalfV + trimRad;
  } else if (g_eyeW && g_swH) {
    // ⚠ PER-EYE width, not the source width. These are the same number in AER
    // (one eye IS the whole backbuffer) and differ by 2x in geo-11 native/SBS,
    // where the source is the 7680x2160 shared surface and each eye is one
    // 3840x2160 half. Using g_swW here submitted a frustum of aspect 7680:2160 =
    // 3.56:1 — a thin letterbox slot, and *exactly* the shape of one half of
    // geo-11's over/under monitor preview, which is what it looked like in the
    // headset on 2026-08-08. The picture itself was correct stereo the whole
    // time; only the angular window it was poured into was wrong.
    halfV = atanf(tanf(halfH) * (float)g_swH / (float)g_eyeW);
  }
  // Square present: the centred square crop (side = min dim) spans the SAME angle
  // vertically and horizontally, so the layer FOV is square. For a landscape (16:9)
  // render side = height, and the square's horizontal half-angle equals halfV.
  if (g_squarePresent && !akvr_head_wide_render() && !g_projvrOn)
    halfH = halfV;

  // RE02 centre-crop: how much of the wider render horizontally corresponds to
  // the headset's real HFOV. Use only that centre strip; the rest is wasted
  // render (outside the headset's physical field of view).
  float cropWidthRatio = 1.0f;
  if (akvr_head_wide_render()) {
    float renderHfov = akvr_head_render_hfov();
    float displayHfov = akvr_xr_headset_hfov_deg();
    if (renderHfov > displayHfov && displayHfov > 10.0f) {
      float rh = renderHfov * 0.5f * (3.14159265f / 180.0f);
      float dh = displayHfov * 0.5f * (3.14159265f / 180.0f);
      cropWidthRatio = tanf(dh) / tanf(rh);
    }
  }
  if (cropWidthRatio < 0.1f) cropWidthRatio = 0.1f;
  if (cropWidthRatio > 1.0f) cropWidthRatio = 1.0f;
  // Screen-mode (menu/loading, !effGameplay): presenting the flat 2D content at
  // the FULL gameplay FOV wraps it edge-to-edge around you — too big to see all
  // at once, like standing too close to a huge screen. Cropping the SOURCE
  // image doesn't fix this (confirmed: no change even with zero crop) because
  // the problem is angular size, not framing. Shrink the LAYER's FOV instead —
  // same full image, presented in a smaller angular window, like a normal-sized
  // virtual screen. Live-tunable (Insert/Delete).
  // SIZELATCH 2026-09-26 — JJ: the loading screen "started correctly then suddenly
  // scaled up before entering the game": it began in the main-menu phase (menu size)
  // and the phase ended 5 s into it (pause size). Which size a screen gets is now
  // decided once, when the screen appears, and held until gameplay returns.
  static bool s_wasEff = true;
  static bool s_latchedMenuSize = false;
  if (!effGameplay && s_wasEff) s_latchedMenuSize = akvr_xr_screen_mode();
  if (akvr_xr_screen_mode()) s_latchedMenuSize = true;   // F6 / menu mid-screen: menu size
  s_wasEff = effGameplay;
  float fovScale = effGameplay ? 1.0f
                   : (g_forceScreen ? g_floatScreenScale
                      : (s_latchedMenuSize ? g_screenFovScale : g_pauseFovScale));
  // Publish what we ACTUALLY submit, so the startup log can put it beside what
  // projVR left in the render matrix. Those two must agree or the picture is
  // stretched by exactly the ratio of their tangents.
  g_subHalfH = halfH * fovScale;
  g_subHalfV = halfV * fovScale;
  XrFovf fov;
  fov.angleRight = halfH * fovScale;
  fov.angleLeft = -halfH * fovScale;
  fov.angleUp = halfV * fovScale;
  fov.angleDown = -halfV * fovScale;
  // FULLVIEW: the projection was shifted by g_vOffNdc (camera.cpp adds it to m[9]),
  // so its top edge is at tan = (1 - o) * tan(halfV) and its bottom at (1 + o) *
  // tan(halfV). Submit exactly those edges or the world swims when you look up/down.
  if (g_vOffNdc != 0.0f && g_projvrOn && (effGameplay || g_anamorphic)) {
    const float t = tanf(halfV);
    fov.angleUp = atanf((1.0f - g_vOffNdc) * t) * fovScale;
    fov.angleDown = -atanf((1.0f + g_vOffNdc) * t) * fovScale;
  }
  // SCREENWIDE: crop the floating screen to a wider shape (see g_screenAspect).
  float vFrac = 1.0f;
  // PAUSE169 2026-09-27 — JJ: "the pause screen and the map screen are too high, they should be
  // restricted to 16 by 9 because the mask that's over them is restricted to that". The render is
  // taller than 16:9 (3560x3120, or 2864x3120 with the eye view), so every non-gameplay screen is
  // cropped to 16:9, centred; a floating F6 screen keeps its own shape setting.
  const float scrAspect = g_forceScreen ? g_screenAspect : (!effGameplay ? g_pauseAspect : 0.0f);
  if (scrAspect > 0.5f) {
    const float tH = tanf(halfH), tV = tanf(halfV);
    if (tH > 0.0f && tV > 0.0f && tH / scrAspect < tV)
      vFrac = (tH / scrAspect) / tV;
  }
  if (vFrac < 0.999f) {
    fov.angleUp = atanf(tanf(halfV) * vFrac) * fovScale;
    fov.angleDown = -fov.angleUp;
  }
  int fallback =
      g_swapInit[0] ? 0
                    : 1; // before both eyes exist, show the one we have to both

  // ---- YAW FOLDING: fold the GAME camera's turn into the pose we submit ------
  // Both eyes are re-pinned to the heading the camera has RIGHT NOW. The eye that
  // was just drawn is already at that heading, so its correction is zero by
  // construction and only the stale eye moves — no new latency, no cost, we are
  // only feeding better information to a correction the runtime already runs.
  // Expect a PARTIAL win: Arkham is third person, so a spin is a pivot PLUS a
  // slide along the camera boom, and time-warp is rotation-only. The rotational
  // part (the dominant, most nauseating half) is what this removes.
  float baseNow = 0.0f;
  bool baseOk = akvr_camera_base_yaw_deg(baseNow);
  bool foldOn = g_yawFoldOn && baseOk && !g_native && effGameplay &&
                fabsf(g_yawFold) > 0.001f;

  for (int e = 0; e < 2; ++e) {
    int src = g_swapInit[e] ? e : fallback;
    XrPosef eyePose = g_swapInit[e]
                          ? g_eyePose[e]
                          : g_eyePose[fallback]; // per-eye render pose (AER fix)
    float fold = 0.0f;
    if (foldOn && g_eyeBaseOk[src]) {
      float d = baseNow - g_eyeBaseYaw[src];
      while (d > 180.0f) d -= 360.0f;
      while (d < -180.0f) d += 360.0f;
      // A cut, a teleport or a long-held stale eye can leave a huge delta. Swinging
      // the image by that much is far worse than the flicker it is meant to cure, so
      // past a fast spin's worth of angle we simply do nothing.
      if (fabsf(d) <= 25.0f) fold = d * g_yawFold;
    }
    g_foldApplied[e] = fold;
    // Game yaw and tracking yaw run OPPOSITE ways (game +yaw turns right, OpenXR
    // +yaw turns left — the same YAW_SIGN=-1 relation the camera injection uses),
    // and the correction is itself the negative of the camera's turn. The two
    // negatives cancel, so the tracking-space turn is +fold. If a spin ever looks
    // WORSE rather than better, that is this line: set the panel slider to -1.00.
    if (fold != 0.0f)
      eyePose.orientation =
          yaw_premul(eyePose.orientation, fold * (3.14159265f / 180.0f));
    views[e].pose = eyePose;
    views[e].fov = fov;
    // EYEVIEW: this eye's image is turned outward by k (geo-11 off-axis); submit exactly that
    // frustum. OpenXR eye 0 = left, whose outward side is -x. SKVR MENUTURN: flat menus and
    // floating screens are shifted by geo-11 too, so they get the same turn sized to their own
    // half-width; only a flat screen showing ONE half to both eyes stays symmetric.
    if (g_eyeApplied && (effGameplay || !g_menuFlat)) {
      const float h = tanf(halfH * fovScale);
      const float c = (e == 0 ? -1.0f : 1.0f) * (g_eyeFlip ? -1.0f : 1.0f) * akvr_xr_eye_k() * h;
      views[e].fov.angleLeft = atanf(c - h);
      views[e].fov.angleRight = atanf(c + h);
    }
    views[e].subImage.swapchain = g_sbsActive ? g_swap[0] : g_swap[src];
    views[e].subImage.imageArrayIndex = 0;
    // SBSONE: this view's half of the side-by-side image (same choice the two-image
    // copy made: swapped eyes, and one half for both eyes on a flat floating screen).
    int32_t halfX = 0;
    if (g_sbsActive) {
      int sourceEye = swap_eff() ? (1 - e) : e;
      if (g_menuFlat && !effGameplay) sourceEye = swap_eff() ? 1 : 0;
      halfX = sourceEye * (int32_t)g_eyeW;
    }
    int32_t fullW = (int32_t)g_eyeW;
    int32_t fullH = (int32_t)g_swH;
    if (g_squarePresent && !g_native && !akvr_head_wide_render() && !g_projvrOn) {
      // Centred SQUARE crop of this eye's image — the actual "square frame".
      int32_t side = (fullW < fullH) ? fullW : fullH;
      views[e].subImage.imageRect.offset = {halfX + (fullW - side) / 2, (fullH - side) / 2};
      views[e].subImage.imageRect.extent.width = side;
      views[e].subImage.imageRect.extent.height = side;
    } else {
      int32_t cropW = (int32_t)(fullW * cropWidthRatio);
      int32_t cropX = (fullW - cropW) / 2;
      int32_t cropH = (int32_t)(fullH * vFrac);   // SCREENWIDE
      views[e].subImage.imageRect.offset = {halfX + cropX, (fullH - cropH) / 2};
      views[e].subImage.imageRect.extent.width = cropW;
      views[e].subImage.imageRect.extent.height = cropH;
    }
  }
  // Peak-hold the correction so a spin leaves a readable number behind instead of
  // snapping back to 0 the moment you stop — you cannot read a live figure while
  // spinning, and the whole point is to check it moves at all.
  {
    static float peak = 0.0f;
    float now = fabsf(g_foldApplied[0]) > fabsf(g_foldApplied[1])
                    ? fabsf(g_foldApplied[0]) : fabsf(g_foldApplied[1]);
    if (now > peak) peak = now;
    else peak *= 0.995f;
    if (!g_yawFoldOn)
      snprintf(g_foldDiag, sizeof(g_foldDiag),
               "spin correction: OFF (comparison mode)");
    else if (!baseOk)
      snprintf(g_foldDiag, sizeof(g_foldDiag),
               "spin correction: no camera heading yet (VR head-track off?)");
    else if (!foldOn)
      snprintf(g_foldDiag, sizeof(g_foldDiag), "spin correction: idle (menu/screen)");
    else
      snprintf(g_foldDiag, sizeof(g_foldDiag),
               "spin correction: L %+.2f  R %+.2f deg   peak %.2f",
               g_foldApplied[0], g_foldApplied[1], peak);
  }
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer.space = g_localSpace;
  layer.viewCount = 2;
  layer.views = views;

  // Overlay QUAD layer — a flat screen FIXED in the room (world-locked), composited
  // ON TOP of the game in both eyes. Only shown when the UI drew a fresh image this
  // frame (F8-hidden → not drawn → quad vanishes). Alpha blend so the game shows
  // through the transparent margins of the panel.
  //
  // World-locked, NOT head-locked: we capture a pose 1.30 m in front of you the first
  // frame the panel appears and PARK it there. Leaning toward it makes it bigger,
  // leaning away smaller — natural perspective, like a monitor floating in the room
  // (requested 2026-07-25). It re-anchors each time you re-open the panel so it always
  // spawns in front of wherever you're looking. (Earlier it was submitted VIEW-space;
  // Virtual Desktop mishandled that and scaled it backwards as you leaned.)
  if (!g_ovAnchored) {
    const XrVector3f localOff{0.0f, -0.10f, -1.30f};
    XrVector3f off = quat_rotate(g_curPose.orientation, localOff);
    g_ovAnchor.orientation = g_curPose.orientation;
    g_ovAnchor.position = {g_curPose.position.x + off.x,
                           g_curPose.position.y + off.y,
                           g_curPose.position.z + off.z};
    g_ovAnchored = true;
  }
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  quad.space = g_localSpace; // world space; parked at the captured room anchor
  quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  quad.subImage.swapchain = g_ovSwap;
  quad.subImage.imageArrayIndex = 0;
  quad.subImage.imageRect.offset = {0, 0};
  quad.subImage.imageRect.extent.width = (int32_t)g_ovW;
  quad.subImage.imageRect.extent.height = (int32_t)g_ovH;
  quad.pose = g_ovAnchor;
  // Metres, at the original 1024px/0.70m density (1440 px -> 0.984 m) so the panel
  // grows without shrinking the text. Widened as well as heightened on 2026-07-25 —
  // the settings labels and the resolution diagnostic were being clipped at 1024.
  quad.size = {0.984f, 0.984f};

  // HUDLAYER: the HUD image (hudsplit.cpp), head-locked by the compositor at every refresh.
  XrCompositionLayerQuad hudq{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const bool hasHud = submitted && hud_layer_build(hudq, fov);
  const XrCompositionLayerBaseHeader *layers[6];
  uint32_t nLayers = 0;
  if (submitted)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&layer;
  XrCompositionLayerQuad hudL{XR_TYPE_COMPOSITION_LAYER_QUAD}, hudR{XR_TYPE_COMPOSITION_LAYER_QUAD};
  if (hasHud && g_hudEyes) {   // HUDLAYER5: eye shift
    hud_eye_pair(hudq, hudL, hudR);
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&hudL;
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&hudR;
  } else if (hasHud)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&hudq;
  g_vigHas = submitted && vig_layer(g_vigQuad, g_eyeWantGameplay);   // ZOOMVIG: over the game and HUD, under the panel
  if (g_vigHas)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&g_vigQuad;
  if (g_ovDrewThisFrame)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&quad;
  else
    g_ovAnchored = false; // panel hidden → re-anchor in front of you when it reopens

  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
  fei.displayTime = g_predicted;
  fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  fei.layerCount =
      nLayers; // always end the frame we began (0 layers if nothing to show)
  fei.layers = nLayers ? layers : nullptr;
  xrEndFrame(g_session, &fei);
  // FPSLOCK: keep what was just shown, to show again while the next frame is early.
  g_rpHasProj = submitted;
  if (submitted) {
    g_rpViews[0] = views[0];
    g_rpViews[1] = views[1];
    g_rpLayer = layer;
    g_rpLayer.views = g_rpViews;
  }
  g_rpHasQuad = g_ovDrewThisFrame;
  g_rpHasHud = hasHud;
  if (hasHud) g_rpHud = hudq;
  if (g_rpHasQuad) g_rpQuad = quad;
  g_lastEndDisplay = g_predicted;

  g_displaying = submitted;
  if (submitted)
    snprintf(g_dispStatus, sizeof(g_dispStatus),
             "headset: DISPLAYING %s%s %ux%u/eye mode=%s FOV %.0fx%.0f cropX=%d cropW=%d",
             g_native ? (g_katFeed && g_katTex ? "GEO11(katanga SBS)"
                                               : "NATIVE(SBS)")
                      : "AER",
             akvr_head_wide_render() ? "/WIDE" : "", g_eyeW, g_swH,
             effGameplay ? "GAMEPLAY" : (g_forceScreen ? "FORCED_SCREEN" : (akvr_xr_main_menu_detected() ? "MAIN_MENU" : "SCREEN")),
             g_subHalfH * 2.0f * 57.29578f, g_subHalfV * 2.0f * 57.29578f,
             views[0].subImage.imageRect.offset.x, views[0].subImage.imageRect.extent.width);
  else
    snprintf(g_dispStatus, sizeof(g_dispStatus),
             "headset: session running, not displaying");
}

bool akvr_xr_ok() { return g_ok; }
const char *akvr_xr_status() { return g_status; }
void akvr_xr_set_device(ID3D11Device *dev) {
  if (!g_device && dev) {
    g_device = dev;
    g_device->GetImmediateContext(&g_ctx);

    // Is the geo-11 stereo renderer active? If so, default to native SBS mode.
    // (\ key still overrides.)
    //
    // ⚠ 2026-08-08: this used to ask "is the module answering to `d3d11.dll`
    // loaded from the GAME folder rather than System32?" — which was a fine
    // proxy for "geo-11 is here" right up until we made geo-11 stop being
    // d3d11.dll. In HOOK mode the genuine System32 copy is loaded on purpose
    // (see `geo11-hook-mode-rename`), so the old test answered NO, g_native
    // stayed false, and the headset showed geo-11's flat OVER/UNDER monitor
    // preview from the swapchain while the real side-by-side pair sat unread on
    // the Katanga surface. Everything else was working; only this said no.
    //
    // Ask the question we actually mean instead. `akvr_early_geo11()` keys on
    // `d3dxdm.ini` next to us, which is true in BOTH modes and is already the
    // flag the rest of the mod stands down on.
    if (akvr_early_geo11()) {
      g_native = true;
    } else {
      HMODULE m = GetModuleHandleW(L"d3d11.dll");
      if (m) {
        wchar_t mod[MAX_PATH] = L"", sys[MAX_PATH] = L"";
        GetModuleFileNameW(m, mod, MAX_PATH);
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n && _wcsnicmp(mod, sys, n) != 0)
          g_native = true;
      }
    }
  }
}
bool akvr_xr_session_running() { return g_running; }
bool akvr_xr_head_pos_valid() { return g_posValid; }
const char *akvr_xr_session_status() { return g_sessStatus; }
bool akvr_xr_displaying() { return g_displaying; }
const char *akvr_xr_display_status() { return g_dispStatus; }
// F6 / the panel box flip what you SEE: during the automatic main-menu phase that
// means leaving it (the detector guessed wrong), otherwise the manual override.
void akvr_xr_screen_mode_toggle() {
  if (g_autoMainMenu && g_menuPhase == 1) { g_menuPhase = 2; return; }
  g_forceScreen = !g_forceScreen;
}
// Phase 0 floats too (MENUFIX 2026-09-26): the 3 s confirmation window used to show
// the main menu head-locked and full size before it switched ("initially started too
// big and stuck to the face and quickly switched"). Before the menu is confirmed
// everything is splash or menu anyway, so nothing is lost by floating it.
bool akvr_xr_screen_mode() {
  return g_forceScreen || (g_autoMainMenu && g_menuPhase <= 1 && !g_menu3d);
}
// SCREENTRACK 2026-09-26 — JJ: the floating screen (Pause key) keeps the higher
// resolution of a smaller window but should still be head-tracked, "as if you're in
// the world, restricted to the window". The screen stays fixed in the room; head
// rotation and lean still drive the game camera. Only the screen you switch on
// yourself; pause, map, loading and menus still freeze the camera.
bool g_screenTrack = true;
// SCREENWIDE 2026-09-26 — JJ: a wider floating screen. 0 = the picture's own shape;
// otherwise width/height (1.778 = 16:9, 2.333 = 21:9): the top and bottom of the
// picture are cropped and the layer's vertical angle shrinks to match.
float akvr_xr_float_screen_scale() { return g_floatScreenScale; }
void akvr_xr_float_screen_scale_set(float s) { g_floatScreenScale = s < 0.1f ? 0.1f : (s > 1.0f ? 1.0f : s); }
float akvr_xr_screen_aspect() { return g_screenAspect; }
float akvr_xr_pause_aspect() { return g_pauseAspect; }
void  akvr_xr_pause_aspect_set(float a) { g_pauseAspect = (a > 0.5f && a < 4.0f) ? a : 0.0f; }
void akvr_xr_screen_aspect_set(float a) { g_screenAspect = (a > 0.5f && a < 4.0f) ? a : 0.0f; }
bool akvr_xr_screen_track() { return g_screenTrack; }
void akvr_xr_screen_track_set(bool on) { g_screenTrack = on; }
bool akvr_xr_screen_frozen() {
  if (g_forceScreen && g_screenTrack) return false;
  return akvr_xr_screen_mode();
}
// MENU3D: the main-menu phase, shown live and head-tracked instead of floating.
bool akvr_xr_menu3d_active() {
  return !g_forceScreen && g_autoMainMenu && g_menuPhase <= 1 && g_menu3d;
}
bool akvr_xr_menu3d() { return g_menu3d; }
// MENULIVE 2026-09-26: back, WITHOUT the text pinning. The F1 trace from the crash
// session proved head tracking does reach the main-menu camera in live mode (head
// -9 deg -> final yaw = base + head, and the game's camera reads back the same). What
// crashed was the per-frame cross-thread viewport push that pinned the text; that is
// gone (akvr_xr_menu3d_offset below never reports a shift). The text now behaves like
// the HUD: in view, at the main-menu size.
void akvr_xr_menu3d_set(bool on) { g_menu3d = on; g_menu3dWas = false; }
bool akvr_xr_menu_flat() { return g_menuFlat; }
void akvr_xr_menu_flat_set(bool on) { g_menuFlat = on; }
// Head rotation since the pinned menu appeared, in OpenXR's sense (+yaw = left,
// +pitch = up), from the angles the camera actually applied (camera.cpp's YAW/PITCH
// signs are both -1), so the text moves with the same frame the 3D moves with.
bool akvr_xr_menu3d_offset(float &dyawDeg, float &dpitchDeg) {
  dyawDeg = dpitchDeg = 0.0f;
  if (true) return false;   // MENULIVE: pinning removed (see akvr_xr_menu3d_set)
  if (!akvr_xr_menu3d_active() || !g_menu3dWas) return false;
  HeadTrackState ht = akvr_head_state();
  dyawDeg = -(ht.yaw - g_menu3dRefYaw);
  dpitchDeg = -(ht.pitch - g_menu3dRefPitch);
  return true;
}
bool akvr_xr_main_menu_detected() { return g_autoMainMenu && g_menuPhase == 1; }
void akvr_xr_set_anamorphic(bool on) { g_anamorphic = on; }
void akvr_xr_set_screen_hold(bool on) { g_holdScreen = on; }
void akvr_xr_set_camera_pos(bool ok, float x, float y, float z) {
  g_camOk = ok; g_camPos[0] = x; g_camPos[1] = y; g_camPos[2] = z;
}
const char *akvr_xr_mode_log() { return g_modeLog; }
bool akvr_xr_auto_main_menu() { return g_autoMainMenu; }
void akvr_xr_auto_main_menu_set(bool on) { g_autoMainMenu = on; }
float akvr_xr_pause_zoom() { return g_pauseFovScale; }
void akvr_xr_pause_zoom_set(float v) {
  g_pauseFovScale = v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v);
}
void akvr_xr_menu_zoom_mul(float factor) {
  g_screenFovScale *= factor;
  if (g_screenFovScale < 0.1f)
    g_screenFovScale = 0.1f;
  if (g_screenFovScale > 1.0f)
    g_screenFovScale = 1.0f;
}
float akvr_xr_menu_zoom() { return g_screenFovScale; }
void akvr_xr_menu_zoom_set(float v) {
  g_screenFovScale = v;
  if (g_screenFovScale < 0.1f)
    g_screenFovScale = 0.1f;
  if (g_screenFovScale > 1.0f)
    g_screenFovScale = 1.0f;
}
float akvr_xr_headset_hfov_deg() { return g_headHalfH * 2.0f * 57.29578f; }
float akvr_xr_headset_vfov_deg() { return g_headHalfV * 2.0f * 57.29578f; }
int akvr_xr_inject_eye() {
  return g_injectEye;
} // eye the camera injection should use this Present (-1 = centered, native
  // mode)
const char *akvr_xr_probe_status() {
  return g_probeStatus;
} // MONO vs SBS backbuffer classification
const char *akvr_xr_katanga_status() {
  return g_katStatus;
} // geo-11 shared-texture capture status
bool akvr_xr_square_present() { return g_squarePresent; }
void akvr_xr_square_present_set(bool on) { g_squarePresent = on; }

void akvr_xr_set_scene_source(ID3D11Texture2D *tex) { g_sceneSrc = tex; }
const char *akvr_xr_tap_status() { return g_tapDiag; }

bool akvr_xr_kat_feed() { return g_katFeed; }
void akvr_xr_kat_feed_set(bool on) {
  if (g_katFeed == on)
    return;
  g_katFeed = on;
  destroy_swapchains(); // the source changes size (5120x1440 vs the window)
}

bool akvr_xr_native() { return g_native; }
bool akvr_xr_ofxr_active() { return g_ofxr; }
bool akvr_xr_sbs_one() { return g_sbsOne; }
void akvr_xr_sbs_one_set(bool on) { g_sbsOne = on; }
bool akvr_xr_sbs_active() { return g_sbsActive; }
bool akvr_xr_native_swap_eyes() { return g_nativeSwapEyes; }
// EYEVIEW API
bool  akvr_xr_eye_view() { return g_eyeView; }
void  akvr_xr_eye_view_set(bool on) { g_eyeView = on; }
void  akvr_xr_eye_applied_set(bool on) {
  // once per Present: remember geo-11's state per frame, show the one that drew this picture
  g_appliedHist[++g_appliedHead & 63u] = on && g_eyeView && g_eyeNarrowOk;
  int d = g_usedDelay; d = d < 0 ? 0 : (d > 30 ? 30 : d);
  g_eyeApplied = g_appliedHist[(g_appliedHead - (uint32_t)d) & 63u];
}
bool  akvr_xr_eye_gameplay() { return g_eyeWantGameplay; }
bool  akvr_xr_eye_applied() { return g_eyeApplied; }
float akvr_xr_eye_k_head() {
  return (g_eyeTanOut > 0.1f && g_eyeTanIn > 0.1f)
             ? (g_eyeTanOut - g_eyeTanIn) / (g_eyeTanOut + g_eyeTanIn) : 0.2426f;
}
float akvr_xr_eye_k() { return g_eyeK >= 0.0f ? g_eyeK : akvr_xr_eye_k_head(); }
float akvr_xr_eye_k_setting() { return g_eyeK; }
void  akvr_xr_eye_k_set(float k) { g_eyeK = k < 0.0f ? -1.0f : (k > 0.6f ? 0.6f : k); }
bool  akvr_xr_eye_flip() { return g_eyeFlip; }
void  akvr_xr_eye_flip_set(bool on) { g_eyeFlip = on; }
bool  akvr_xr_eye_narrow_ok() { return g_eyeNarrowOk; }
bool  akvr_xr_eye_tangents(float& o, float& i, float& u, float& d) {
  o = g_eyeTanOut; i = g_eyeTanIn; u = g_eyeTanUp; d = g_eyeTanDown;
  return o > 0.1f && i > 0.1f && u > 0.05f && d > 0.05f;
}
void akvr_xr_native_swap_eyes_set(bool on) { g_nativeSwapEyes = on; }
int akvr_xr_pose_delay() { return g_poseDelay; }
int akvr_xr_pose_delay_used() { return g_usedDelay; }
bool akvr_xr_pose_auto() { return g_poseAuto; }
void akvr_xr_pose_auto_set(bool on) { g_poseAuto = on; }
bool akvr_xr_pose_matched() { return g_poseMatched; }
// "delay:count" for every exactly matched Present so far.
const char *akvr_xr_pose_delay_hist() {
  static char s[160];
  int n = 0; s[0] = 0;
  for (int d = 0; d < 16; ++d)
    if (g_delayHist[d] && n < (int)sizeof(s) - 16)
      n += snprintf(s + n, sizeof(s) - n, "%d:%u ", d, g_delayHist[d]);
  return s[0] ? s : "none yet";
}
void akvr_xr_pose_delay_set(int frames) {
  g_poseDelay = frames < 0 ? 0 : (frames > 3 ? 3 : frames);
}
void akvr_xr_native_toggle() {
  // Mode switch changes the per-eye swapchain width — rebuild from scratch.
  g_native = !g_native;
  destroy_swapchains();
}
void akvr_xr_backbuffer_size(uint32_t &w, uint32_t &h) {
  w = g_swW;
  h = g_swH;
}
// The size of ONE EYE's image — the same thing in AER, half the width in geo-11
// native/SBS. Anything reasoning about the SHAPE of a rendered view wants this
// one; g_swW spans both eyes and gives an aspect twice too wide (the letterbox
// bug of 2026-08-08, see the halfV derivation in frame_submit).
// geo-11's shared stereo surface, for the F2 frame grab. Each half is a FULL-height
// eye, unlike the swapchain preview, so it is the only picture that shows the real
// shape of what was rendered.
ID3D11Texture2D *akvr_xr_katanga_texture() { return g_katTex; }
void akvr_xr_submitted_fov_deg(float &h, float &v) {
  h = g_subHalfH * 2.0f * 57.29578f;
  v = g_subHalfV * 2.0f * 57.29578f;
}
void akvr_xr_eye_image_size(uint32_t &w, uint32_t &h) {
  w = g_eyeW ? g_eyeW : g_swW;
  h = g_swH;
}
void akvr_xr_recommended_size(uint32_t &w, uint32_t &h, uint32_t &mw,
                              uint32_t &mh) {
  w = g_recW;
  h = g_recH;
  mw = g_maxW;
  mh = g_maxH;
}
void akvr_xr_notify_resize() {
  // The game resized its backbuffer (e.g. graphics-settings menu). Drop the eye
  // swapchains so frame_submit rebuilds them at the new size next frame —
  // without this the headset goes black after the menu (stale-size copy is
  // skipped).
  destroy_swapchains();
}

// ---- overlay quad layer: hooks.cpp renders ImGui into this each Present ----
void akvr_xr_overlay_size(uint32_t &w, uint32_t &h) {
  w = g_ovW;
  h = g_ovH;
}
bool akvr_xr_overlay_ready() {
  return g_running && (g_ovInit || create_overlay_swapchain());
}

// Hand back the PRIVATE linear render target. Caller clears it to transparent,
// renders the UI, then calls overlay_end() (which copies it into the
// swapchain).
bool akvr_xr_overlay_begin(ID3D11RenderTargetView **rtvOut) {
  if (!rtvOut)
    return false;
  if (!g_running)
    return false;
  if (!g_ovInit && !create_overlay_swapchain())
    return false;
  if (!g_ovRenderRTV)
    return false;
  *rtvOut = g_ovRenderRTV;
  return true;
}
void akvr_xr_overlay_end() {
  if (!g_ovInit)
    return;
  // Copy the freshly-rendered UI (linear-store bytes) into the acquired
  // swapchain image (_SRGB) — a raw byte copy across the shared typeless
  // family, so the compositor's sRGB decode yields correct gamma (no
  // double-encode / grey wash).
  uint32_t idx = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(g_ovSwap, &ai, &idx)))
    return;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  if (XR_SUCCEEDED(xrWaitSwapchainImage(g_ovSwap, &wi)) &&
      idx < g_ovImages.size()) {
    g_ctx->CopyResource(g_ovImages[idx], g_ovRenderTex);
    g_ovDrewThisFrame =
        true; // include the quad layer in this frame's xrEndFrame
    g_ovEverDrew = true;
  }
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(g_ovSwap, &ri);
}

void akvr_xr_head_pose(float &yaw, float &pitch, float &roll, float &px,
                       float &py, float &pz) {
  for (;;) // seqlock read: retry if a write straddled us
  {
    LONG s0 = g_poseSeq;
    MemoryBarrier();
    yaw = g_pYaw;
    pitch = g_pPitch;
    roll = g_pRoll;
    px = g_pX;
    py = g_pY;
    pz = g_pZ;
    MemoryBarrier();
    LONG s1 = g_poseSeq;
    if (!(s0 & 1) && s0 == s1)
      return;
  }
}
void akvr_xr_head_deg(float &yaw, float &pitch, float &roll) {
  float px, py, pz;
  akvr_xr_head_pose(yaw, pitch, roll, px, py, pz);
}
float akvr_xr_headset_hz() {
  return (g_displayPeriod > 0) ? (1000000000.0f / (float)g_displayPeriod) : 0.0f;
}

void akvr_xr_head_quat(float &qx, float &qy, float &qz, float &qw) {
  for (;;) // seqlock read: retry if a write straddled us
  {
    LONG s0 = g_poseSeq;
    MemoryBarrier();
    qx = g_pQx;
    qy = g_pQy;
    qz = g_pQz;
    qw = g_pQw;
    MemoryBarrier();
    LONG s1 = g_poseSeq;
    if (!(s0 & 1) && s0 == s1)
      return;
  }
}

const char *akvr_xr_hud_layer_diag() {
  static char d[400];
  if (g_hudErr == 1) snprintf(d, sizeof(d), "headset HUD layer: no swapchain format matches the HUD image");
  else if (g_hudErr == 2) snprintf(d, sizeof(d), "headset HUD layer: could not create its swapchain");
  else if (g_hudSwap == XR_NULL_HANDLE) snprintf(d, sizeof(d), "headset HUD layer: not started");
  else snprintf(d, sizeof(d), "headset HUD layer: %ld frames sent, %ux%u fmt %d, at %.1f m, %.2f x %.2f m, %s, %s, place fails %ld%s",
                g_hudSubmits, g_hudW, g_hudH, (int)g_hudFmt, g_hudDistUsed, g_hudSizeW, g_hudSizeH,
                g_hudSpace == 1 ? "attached to the head" : (g_hudSpace == 2 ? "fixed in the room" : "room space, re-placed each frame"),
                g_hudColour == 1 ? g_hudConvDiag : "raw colour copy", g_hudPlaceFails,
                g_rpHasHud ? "" : " (not in the last frame)");
  return d;
}
int  akvr_xr_hud_space() { return g_hudSpace; }
void akvr_xr_hud_space_set(int v) { g_hudSpace = (v == 1 || v == 2) ? v : 0; g_hudWorldOk = false; }
void akvr_xr_hud_reanchor() { g_hudWorldOk = false; }
int  akvr_xr_hud_colour() { return g_hudColour; }
void akvr_xr_hud_colour_set(int v) { g_hudColour = v == 0 ? 0 : 1; }
int  akvr_xr_hud_eyes() { return g_hudEyes; }
void akvr_xr_hud_eyes_set(int v) { g_hudEyes = v ? 1 : 0; }
int  akvr_xr_hud_lazy() { return g_hudLazy; }
void akvr_xr_hud_lazy_set(int v) { g_hudLazy = v ? 1 : 0; }
float akvr_xr_hud_lazy_deg() { return g_hudLazyDeg; }
void akvr_xr_hud_lazy_deg_set(float d) { g_hudLazyDeg = d < 0.1f ? 0.1f : (d > 6.0f ? 6.0f : d); }
float akvr_xr_hud_half_ipd() { return g_hudHalfIpd; }
// RETSQUASH: tan of the game frame's half-angles (what the shader needs to undo the edge squash).
void akvr_xr_game_tan(float &th, float &tv) {
  th = g_subHalfH > 0.01f ? tanf(g_subHalfH) : 0.0f;
  tv = g_subHalfV > 0.01f ? tanf(g_subHalfV) : 0.0f;
}

// ZOOMVIG: panel + settings
float akvr_xr_vig_strength() { return g_vigStrength; }
void  akvr_xr_vig_strength_set(float v) { g_vigStrength = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float akvr_xr_vig_clear() { return g_vigClearDeg; }
void  akvr_xr_vig_clear_set(float d) { g_vigClearDeg = d < 5.0f ? 5.0f : (d > 60.0f ? 60.0f : d); }
float akvr_xr_vig_below() { return g_vigBelowDeg; }
void  akvr_xr_vig_below_set(float d) { g_vigBelowDeg = d < 0.0f ? 0.0f : (d > 120.0f ? 120.0f : d); }
void  akvr_xr_vig_preview(bool on) { g_vigPreview = on; }
const char *akvr_xr_vig_diag() {
  static char d[200];
  snprintf(d, sizeof(d), "game's own view now %.1f deg (lowest %.1f, widest %.1f this session) | vignette %s%s",
           akvr_camera_game_fov(), g_gameFovMin, g_gameFovMax, g_vigAmt > 0.0f ? "ON" : "off",
           g_vigErr ? (g_vigErr == 1 ? " - no image format" : " - could not make its image") : "");
  return d;
}
