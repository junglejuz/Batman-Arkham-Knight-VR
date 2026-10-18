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
#include "akn_link.h" // AKN M5: engine stereo (the game draws both eyes)
#include "geo11conv.h" // HUDLAYER: HUD distance
#include "camera.h" // akvr_camera_finalize_count() — the per-rendered-frame clock for AER eye sync

// earlyres.cpp — "is geo-11 installed next to us?", answered from d3dxdm.ini in
// DllMain. True in BOTH of geo-11's install modes, which is why the native-stereo
// default keys on this and not on who owns d3d11.dll (see akvr_xr_set_device).
bool akvr_early_geo11();
float akvr_head_pos_scale();   // camera.cpp: game units per metre (world scale)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3d11_1.h>   // DLSSPAIR: SwapDeviceContextState
#include <d3dcompiler.h>   // HUDLAYER4: ID3DBlob (D3DCompile loaded at run time)
#include <dxgi.h>
#include <dxgi1_2.h>   // SHAREPROBE: IDXGIFactory2
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod for the SSW-mode frame lock
#include <vector>
#include <windows.h>

int akvr_hud_target_points(float* xy, int max, int rtW, int rtH, float* off, float* xf);   // earlyres.cpp: TARGETANCHOR (MARKERCUT)
bool akvr_cam_aim_head_now();                        // camera.cpp AIMTRIGGER2: the head is in the game's aim now (AIMHUD)
unsigned long long akvr_gamepad_left_trigger_ms();   // gamepad.cpp AIMTRIGGER (AIMHUD)
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
// SCLOG 2026-10-10 (two game crashes in Virtual Desktop right after "xrCreateSwapchain: ovrResult failure [-7000]" in
// its OpenXR.log, at the first main menu; AKVR never said which of its headset images that was): every swapchain request
// is written to akvr_captures\akvr_swapchains.txt before and after, flushed at once (VD fail-fasts the process).
XrResult sc_create(const char *what, const XrSwapchainCreateInfo *sci, XrSwapchain *out) {
  static FILE *f = nullptr; static bool tried = false;
  if (!tried) {
    tried = true;
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (wchar_t *sl = wcsrchr(p, L'\\')) { sl[1] = 0; wcscat_s(p, L"akvr_captures\\akvr_swapchains.txt"); _wfopen_s(&f, p, L"a"); }
    if (f) { SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "--- %04d-%02d-%02d %02d:%02d:%02d game start\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); fflush(f); }
  }
  SYSTEMTIME t; GetLocalTime(&t);
  if (f) { fprintf(f, "%02d:%02d:%02d.%03d %s %ux%u format %lld: asking\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, what, sci->width, sci->height, (long long)sci->format); fflush(f); }
  const XrResult r = xrCreateSwapchain(g_session, sci, out);
  if (f) { fprintf(f, "    -> %s (%d)\n", XR_SUCCEEDED(r) ? "ok" : "FAILED", (int)r); fflush(f); }
  return r;
}
XrSpace g_localSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
// SVRSPACE 2026-10-09 - JJ in SteamVR: title screens "behind me ... upside down", the 3D menu upside down and
// unfusable. SteamVR's xrclient log: "xrLocateViews: Switching to TrackingUniverseSeated because
// XR_REFERENCE_SPACE_TYPE_LOCAL continued to be requested for more than 3 seconds" - LOCAL changes under us with NO
// reference-space-change event, after the first screen was hung and the head zero taken. Both spaces are made;
// g_localSpace (every locate + every layer) points at the chosen one. xrspace: 0 auto (STAGE under SteamVR, LOCAL
// otherwise), 1 LOCAL, 2 STAGE; live via AKN command "xrspace N" (re-zeroes like a runtime recenter).
XrSpace g_spaceLocal = XR_NULL_HANDLE, g_spaceStage = XR_NULL_HANDLE;
int g_xrSpaceMode = 0;
bool g_spaceIsStage = false;
long g_poseJumps = 0;   // POSEJUMP: re-zeroes after a one-frame pose jump
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
int g_poseDelay = 2;   // THREADSYNC 2026-10-02: one delay for everything with OneFrameThreadLag=False (JJ: 2)
// MENUDELAY 2026-09-29 — JJ: on the live 3D main menu Batman "jiggles" when the head moves (gameplay is fine);
// head-pose delay 2 fixes it there, 3 stays right for gameplay. Used while the live main menu is up.
int g_menuPoseDelay = 2;
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
// SPLASHSIZE 2026-09-30 — JJ: "the splash screens at the start ... including the big Batman logo ... are a bit too
// big". Screens before the main menu is found (menu phase 0) had the pause/map size; now their own, smaller.
float g_splashFovScale = 0.55f;
// LOADSIZE 2026-09-30 — JJ: "reduce the size of the loading screens a bit too", without touching gameplay. The loading
// screen into the game (after the main menu, before gameplay) gets its own size; gameplay is always full size.
float g_loadFovScale = 0.6f;
// LOADUP 2026-09-30 — JJ: "a slider to move the loading screen up and down". Degrees, + = up; an off-axis shift of
// the loading screen's frustum (tangent space), so the picture moves without changing shape.
float g_loadUpDeg = 0.0f;
// SCREENNEAR 2026-10-11 - JJ: "move the title screens and the loading screens a bit closer to the camera" (then: "not
// parallax, just a bit closer"). Both eyes showed the same picture through the same window = the screen read as infinitely
// far. Each eye's window is now turned inward by atan(half eye distance / D), so the screen reads D metres away at the same
// angular size; the HUD on those screens (loading tips) hangs at the same D. Start-up / title and loading screens only.
float g_nearScreenM = 0.0f;     // setting screendist (metres); 0 = far (at infinity, smooth). JJ 2026-10-11: far until the
                                // game draws these screens at full rate (it drops to ~30/s there; near screens judder)
bool g_nearScreenNow = false;   // this frame shows a start-up / title or loading screen
// SCREENROOM 2026-10-11 - JJ after SCREENNEAR: "they appear closer but when I move left and right they seem to shift with
// me, making it feel more like they are still at infinity". The inward turn gave stereo depth only; the layer itself was
// still a picture at infinity, so head moves dragged it along. Now (engine stereo) the screen is a panel fixed in the
// room, D metres in front of the screen pose: we draw it ourselves into each eye's full view from that eye's own
// position (runtime lens, no turn) and declare exactly those views (SKVR SCREENPAD / Batman City SCREENROOM: never a
// narrow projection layer or a lone quad under Virtual Desktop).
bool g_roomScrNow = false;      // the eye images hold a room screen (the views loop declares the eyes it was drawn for)
XrPosef g_roomScrHead{};        // the head pose it was drawn from
float g_roomScrT[4] = {};       // the screen window's tangents l r u d (last frame's screen fov)
bool g_roomScrTOk = false;
// SCREENQUAD 2026-10-11 - JJ after SCREENROOM2: "they are locked to 30 frames per second and jitter really badly when I
// move my head". The game presents ~30 pictures/s on these screens and the panel was drawn into the eye pictures only per
// game picture; a picture at infinity hid that (rotation-only reprojection is exact there), a panel at 3 m cannot. Now the
// screen picture goes into its own headset image (created once with the eye images - never mid-session, VD crash lesson)
// and is submitted as a room-fixed quad pair (the HUD's per-eye pair = depth under VD): the runtime composites it from the
// current head pose every refresh. The eye pictures are black under it.
XrSwapchain g_scrSwap = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D *> g_scrImages;
std::vector<ID3D11RenderTargetView *> g_scrRTV;
int64_t g_scrFmt = 0; bool g_scrSrgb = false; int g_scrErr = 0;
constexpr uint32_t kScrPx = 2048;
bool g_scrQuadNow = false;      // this frame's screen is the quad (eye pictures black)
XrCompositionLayerQuad g_scrQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
XrCompositionLayerQuad g_scrQuadL{XR_TYPE_COMPOSITION_LAYER_QUAD}, g_scrQuadR{XR_TYPE_COMPOSITION_LAYER_QUAD};
ID3D11PixelShader *g_scrPSdec = nullptr;   // display values -> linear, for an sRGB headset image (it encodes them back)
// The main menu runs a live 3D camera exactly like gameplay, so the detector calls it
// GAMEPLAY. But its place in the sequence is fixed: boot/splash (screen) -> main
// menu (camera live) -> loading screen -> game. So the FIRST camera-live stretch after
// launch is the main menu, and the first screen stretch after it (>1.5 s: a loading
// screen) ends it. 0 = before the menu, 1 = in the main menu, 2 = past it.
bool g_autoMainMenu = true;
bool g_menuFlat = true;   // MENUFLAT: floating screens show one eye to both eyes
// MENU3D 2026-10-08 (JJ: in the main menu "Batman looks great for about a second then it switches and the depth and scale
// look noticeably worse"): the first second is true eye separation, then MENUCLOSE detects the main menu and renders
// both eyes from one spot. With the turned lens and pose stamp the close Batman now fuses - real 3D is the default;
// the one-spot view stays as an option (setting menuonespot).
bool g_menuOneSpot = false;
static bool menu_one_spot() { return g_menuOneSpot && akvr_xr_main_menu_detected(); }
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
static bool swap_eff() { return akvr_engine_stereo() ? false : (g_nativeSwapEyes != g_eyeApplied); }   // GEO11OFF: engine stereo never swaps (swapeyes=1 sent each eye the other's picture)
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
char g_modeLog[8192] = "";   // SCR30: room for the per-screen frame rates
ULONGLONG g_modeLogStart = 0;
void mode_log(const char *what) {
  const ULONGLONG now = GetTickCount64();
  if (!g_modeLogStart) g_modeLogStart = now;
  char line[64];
  snprintf(line, sizeof(line), "%.1fs %s; ", (now - g_modeLogStart) / 1000.0, what);
  if (strlen(g_modeLog) + strlen(line) < sizeof(g_modeLog) - 1)
    strcat_s(g_modeLog, line);
  // (HUDRING on screen changes removed 2026-10-14 - release tidy; the replay is written by the panel capture only)
  if (false) {
    using DumpFn = void (*)(const char *, int); static DumpFn f = nullptr; static bool t = false;
    if (!t) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); f = m ? (DumpFn)GetProcAddress(m, "akn_hudring_dump") : nullptr; t = m != nullptr; }
    if (f) f(what, 300);
  }
}

// SVRSPACE: point g_localSpace at the chosen space; a live change re-zeroes (head heading, floating screens, HUD)
void space_choose(bool live) {
  if (g_spaceLocal == XR_NULL_HANDLE) return;
  const bool svr = std::strstr(g_runtime, "SteamVR") != nullptr;
  const bool stage = (g_xrSpaceMode == 2 || (g_xrSpaceMode == 0 && svr)) && g_spaceStage != XR_NULL_HANDLE;
  const XrSpace want = stage ? g_spaceStage : g_spaceLocal;
  if (want == g_localSpace) return;
  g_localSpace = want; g_spaceIsStage = stage;
  if (live) { g_recenterPending = true; g_recenterAt = 0; }
  mode_log(stage ? "space STAGE" : "space LOCAL");
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

// FPS72: does the runtime advertise this extension? (playbook PERF-004: enable an optional
// lever only when it is advertised, and still check every call.)
bool g_fbRefresh = false;   // XR_FB_display_refresh_rate advertised AND enabled on our instance
bool runtime_has_ext(const char *name) {
  uint32_t n = 0;
  if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) || n == 0)
    return false;
  std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
  if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data())))
    return false;
  for (auto &p : props)
    if (strcmp(p.extensionName, name) == 0)
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

// SHAREPROBE 2026-10-07 - SteamVR still "Failed to create sync texture" after DXGI11, which answered 0 calls (the game
// never uses CreateDXGIFactory). vrclient makes a keyed-mutex shared texture on OUR device (the game's, with geo-11 on
// it). Discriminate: can the game's device make one at all, what made the device, is CreateTexture2D detoured, and can
// a fresh device on the same GPU make one. Reported in the F2 / startup status.
char g_shareProbe[480] = "not run";
static const char *mod_of(void *fn, char *buf, size_t n) {
  HMODULE m = nullptr; buf[0] = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m) && m) {
    char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
    const char *sl = strrchr(path, '\\'); strncpy_s(buf, n, sl ? sl + 1 : path, _TRUNCATE);
  } else strncpy_s(buf, n, "?", _TRUNCATE);
  return buf;
}
static HRESULT keyed_try(ID3D11Device *d, UINT misc) {
  D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 16; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = misc;
  ID3D11Texture2D *t = nullptr;
  HRESULT hr = E_FAIL;
  __try { hr = d->CreateTexture2D(&td, nullptr, &t); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = (HRESULT)0xC0000005; }
  if (t) t->Release();
  return hr;
}
static void share_probe(ID3D11Device *d) {
  if (!d) return;
  void **vt = *(void ***)d;
  char m1[64], m2[64];
  const unsigned char *code = (const unsigned char *)vt[5];   // ID3D11Device::CreateTexture2D
  const bool detour = code && (code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25) || (code[0] == 0x48 && code[1] == 0xB8));
  bool f1 = false, f2 = false;
  IDXGIDevice *dd = nullptr; IDXGIAdapter *ad = nullptr;
  if (SUCCEEDED(d->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) && dd && SUCCEEDED(dd->GetAdapter(&ad)) && ad) {
    IDXGIFactory1 *fa1 = nullptr; IDXGIFactory2 *fa2 = nullptr;
    f1 = SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory1), (void **)&fa1)) && fa1; if (fa1) fa1->Release();
    f2 = SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory2), (void **)&fa2)) && fa2; if (fa2) fa2->Release();
  }
  const HRESULT kG = keyed_try(d, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
  const HRESULT kN = keyed_try(d, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
  const HRESULT sh = keyed_try(d, D3D11_RESOURCE_MISC_SHARED);
  // SHAREPROBE2 2026-10-07: the fresh-device test is GONE. geo-11 attached to that throw-away device (its
  // CreateTexture2D came back "other") instead of the game's: no katanga surface, and the eyes were the flat frame cut
  // in half (JJ: "way too close to my face, and the eyes were too separated"). Never create a D3D11 device in a
  // geo-11 process.
  if (ad) ad->Release();
  if (dd) dd->Release();
  snprintf(g_shareProbe, sizeof(g_shareProbe),
           "game device: flags 0x%X, level 0x%X, factory1 %s, factory2 %s, CreateTexture2D in %s%s | keyed-mutex texture 0x%08lX, "
           "keyed+NT 0x%08lX, plain shared 0x%08lX",
           d->GetCreationFlags(), (unsigned)d->GetFeatureLevel(), f1 ? "yes" : "NO", f2 ? "yes" : "NO", mod_of(vt[5], m1, sizeof(m1)),
           detour ? " (DETOURED)" : "", (unsigned long)kG, (unsigned long)kN, (unsigned long)sh);
  (void)m2;
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
  share_probe(g_device);   // SHAREPROBE
  XrResult r = xrCreateSession(g_inst, &sci, &g_session);
  if (XR_FAILED(r)) {
    snprintf(g_sessStatus, sizeof(g_sessStatus),
             "session: xrCreateSession failed (%d)%s", (int)r, luidNote);
    return false;
  }

  XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rci.poseInReferenceSpace.orientation.w = 1.0f; // identity
  rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  xrCreateReferenceSpace(g_session, &rci, &g_spaceLocal);
  rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;   // SVRSPACE
  if (XR_FAILED(xrCreateReferenceSpace(g_session, &rci, &g_spaceStage))) g_spaceStage = XR_NULL_HANDLE;
  g_localSpace = g_spaceLocal; g_spaceIsStage = false;
  space_choose(false);
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
    if (XR_FAILED(sc_create("eye image", &sci, &g_swap[e]))) {
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
  if (g_scrSwap == XR_NULL_HANDLE && !g_scrErr) {   // SCREENQUAD: once, beside the first eye images
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = chosen; sci.sampleCount = 1; sci.width = kScrPx; sci.height = kScrPx;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
    if (XR_FAILED(sc_create("screen panel", &sci, &g_scrSwap))) { g_scrSwap = XR_NULL_HANDLE; g_scrErr = 1; }
    else {
      uint32_t n = 0;
      xrEnumerateSwapchainImages(g_scrSwap, 0, &n, nullptr);
      std::vector<XrSwapchainImageD3D11KHR> si(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
      xrEnumerateSwapchainImages(g_scrSwap, n, &n, (XrSwapchainImageBaseHeader *)si.data());
      for (auto &i : si) g_scrImages.push_back(i.texture);
      g_scrRTV.assign(g_scrImages.size(), nullptr);
      g_scrFmt = chosen;
      g_scrSrgb = chosen == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }
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
  if (XR_FAILED(sc_create("overlay panel", &sci, &g_ovSwap))) {
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

extern float g_eyeLocDistMm;   // HUDEYES (below): the measured eye distance
int g_aknLensMode = 2;   // TURNEDEYE (JJ's 5th Quest test): AK's light culling needs a centred lens; 1 = the old off-centre lens
                         // (2026-10-08 "way too costly"), option only
// MAPFLAT 2026-10-08 (JJ: "on the map, the HUD appears to be completely out of alignment and the incorrect size for the
// actual map beneath it"; captures c223528/c224357: the left eye's map drawn through the headset's turned 85x90 deg eye
// lens, the map's icons laid out by the game for its own 75 deg lens). A floating screen (map, plain pause) shows ONE
// picture to both eyes: AKN draws it the flat game's way - the game's own lens, both eyes at the head - so the game's
// HUD lies on it exactly; the HUD sheet gets the screen's crop too (g_scrVFrac / g_scrCropW).
bool g_aknFlatScreen = false; int g_mapFlat = 1;   // setting-free switch: command / export akvr_set_map_flat
float g_scrVFrac = 1.0f, g_scrCropW = 1.0f;
bool g_hudOnScreen = false;   // engine mode: this frame's HUD layer belongs on the floating screen (g_fixedPose)
// AKN M5: the measured eye distance (metres), 64 mm until the runtime has given one.
float akn_ipd_m() { return (g_eyeLocDistMm > 40.0f && g_eyeLocDistMm < 90.0f) ? g_eyeLocDistMm * 0.001f : 0.064f; }
// AKN M5: our side-by-side source. A fresh AKN pair (both eyes drawn by the game this frame) is copied in whole;
// without one (logos, loading, anything with no 3D) the window picture goes into both halves, like a flat screen.
ID3D11Texture2D *g_aknSbs = nullptr;
static void bright_probe(ID3D11Texture2D *src, UINT eyeW, UINT h);   // STARTBRIGHT (below); MENUSOUND feeds it the AKN pair
long g_aknSeq = -1;
bool g_aknFresh = false;
// TRANSFULL 2026-10-13 (JJ: the bat transition "needs to be full field of view and closer to the camera, same as all the other mods";
// the menus' "black box" = the game's fade to black on the HUD sheet): while the bats play (the game's Wwise Play_UI_Fader_Bats, AKN
// BATEVENT) or the HUD sheet's 16:9 band is a dark cover (AKN HUDCOVER), the sheet is drawn into both eyes on a plane g_trDist in
// front of the head, enlarged g_trZoom past the view (Origins O1D: 0.8 m, 1.6), and the HUD layer is off.
int g_trOn = 0; float g_trDist = 0.8f, g_trZoom = 1.6f; bool g_trFullNow = false; long c_trFrames = 0; char g_trWhy[16] = "";
bool g_sbsHasHud = false; bool g_prSbsShown = false; long c_prFrames = 0, c_prFresh = 0, c_prHie = 0, c_prQuad = 0, c_prBoth = 0, c_prStale = 0;   // PATHREC (see g_hieThisFrame)
long g_aknNoFresh = 1000;   // LOADFIT: Presents since the last fresh AKN pair
long g_aknPairs = 0, g_aknFlat = 0;
char g_aknDiag[200] = "";
// DLSSPAIR 2026-10-08 (AKN DLSS D1): with DLSS the game draws each eye SMALLER and AKN's pair is the full headset size,
// bigger than the window. The side-by-side source takes the pair's size; flat frames (menus, loading) are stretched
// into it (a linear-filtered copy, drawn in a private context state so the game's state is untouched).
typedef HRESULT(WINAPI *StrCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
ID3D11Texture2D *g_flatIn = nullptr; ID3D11ShaderResourceView *g_flatSRV = nullptr; ID3D11RenderTargetView *g_sbsRTV = nullptr;
ID3D11VertexShader *g_strVS = nullptr; ID3D11PixelShader *g_strPS = nullptr; ID3D11SamplerState *g_strSamp = nullptr;
ID3DDeviceContextState *g_strState = nullptr; int g_strOk = 0;
ID3D11PixelShader *g_strPSL = nullptr;   // DLSSMIRROR: left half only
ID3D11ShaderResourceView *g_sbsSRV = nullptr; ID3D11RenderTargetView *g_bbRTV = nullptr; ID3D11Texture2D *g_bbRTVTex = nullptr;
bool stretch_build() {
  if (g_strOk) return g_strOk == 1;
  g_strOk = -1;
  HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
  StrCompileFn compile = m ? (StrCompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
  if (!compile) return false;
  static const char vs[] =
      "void main(uint id : SV_VertexID, out float4 p : SV_Position, out float2 uv : TEXCOORD0) {"
      " uv = float2((id << 1) & 2, id & 2); p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
  static const char ps[] =
      "Texture2D t : register(t0); SamplerState s : register(s0);"
      "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }";
  ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(compile(vs, sizeof(vs) - 1, "strvs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, &err));
  if (err) { err->Release(); err = nullptr; }
  ok = ok && SUCCEEDED(compile(ps, sizeof(ps) - 1, "strps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err));
  if (err) err->Release();
  ok = ok && SUCCEEDED(g_device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_strVS));
  ok = ok && SUCCEEDED(g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_strPS));
  if (vb) vb->Release();
  if (pb) pb->Release();
  static const char psl[] =
      "Texture2D t : register(t0); SamplerState s : register(s0);"
      "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv * float2(0.5, 1)); }";
  ID3DBlob *plb = nullptr; err = nullptr;
  if (ok && SUCCEEDED(compile(psl, sizeof(psl) - 1, "strpsl", nullptr, nullptr, "main", "ps_5_0", 0, 0, &plb, &err)) && plb)
    g_device->CreatePixelShader(plb->GetBufferPointer(), plb->GetBufferSize(), nullptr, &g_strPSL);
  if (err) err->Release();
  if (plb) plb->Release();
  D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
  ok = ok && SUCCEEDED(g_device->CreateSamplerState(&sd, &g_strSamp));
  ID3D11Device1 *d1 = nullptr;
  if (ok && SUCCEEDED(g_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)) && d1) {
    const D3D_FEATURE_LEVEL fl = g_device->GetFeatureLevel();
    ok = SUCCEEDED(d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_strState));
    d1->Release();
  } else ok = false;
  g_strOk = ok ? 1 : -1;
  return ok;
}
// stretch the window picture into both halves of the side-by-side source
bool flat_stretch(ID3D11Texture2D *bb, const D3D11_TEXTURE2D_DESC &d, UINT halfW, UINT h) {
  if (!stretch_build()) return false;
  if (g_flatIn) { D3D11_TEXTURE2D_DESC fd{}; g_flatIn->GetDesc(&fd); if (fd.Width != d.Width || fd.Height != d.Height || fd.Format != d.Format) { if (g_flatSRV) g_flatSRV->Release(); g_flatSRV = nullptr; g_flatIn->Release(); g_flatIn = nullptr; } }
  if (!g_flatIn) {
    D3D11_TEXTURE2D_DESC fd = d; fd.MipLevels = 1; fd.ArraySize = 1; fd.SampleDesc = {1, 0}; fd.Usage = D3D11_USAGE_DEFAULT;
    fd.BindFlags = D3D11_BIND_SHADER_RESOURCE; fd.CPUAccessFlags = 0; fd.MiscFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&fd, nullptr, &g_flatIn)) || FAILED(g_device->CreateShaderResourceView(g_flatIn, nullptr, &g_flatSRV))) return false;
  }
  if (!g_sbsRTV && FAILED(g_device->CreateRenderTargetView(g_aknSbs, nullptr, &g_sbsRTV))) return false;
  g_ctx->CopyResource(g_flatIn, bb);
  ID3D11DeviceContext1 *c1 = nullptr; ID3DDeviceContextState *prev = nullptr;
  if (FAILED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&c1)) || !c1) return false;
  c1->SwapDeviceContextState(g_strState, &prev);
  // STRBLEND 2026-10-09: g_strState is shared and KEEPS what was set in it - MIRRORHUD leaves the HUD blend
  // (ONE / INV_SRC_ALPHA) set, so every later draw in this state blended onto the old picture: the SVRSTRAIGHT output
  // piled up frame on frame (JJ: "blown out to white ... blurring when moving your head"). Opaque, no depth, default raster.
  { const FLOAT bf0[4] = {0, 0, 0, 0}; c1->OMSetBlendState(nullptr, bf0, 0xffffffff); c1->OMSetDepthStencilState(nullptr, 0); c1->RSSetState(nullptr); }
  c1->OMSetRenderTargets(1, &g_sbsRTV, nullptr);
  c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c1->VSSetShader(g_strVS, nullptr, 0); c1->PSSetShader(g_strPS, nullptr, 0);
  c1->PSSetShaderResources(0, 1, &g_flatSRV); c1->PSSetSamplers(0, 1, &g_strSamp);
  for (int e = 0; e < 2; ++e) {
    D3D11_VIEWPORT vp{ (float)(e * halfW), 0.0f, (float)halfW, (float)h, 0.0f, 1.0f };
    c1->RSSetViewports(1, &vp);
    c1->Draw(3, 0);
  }
  ID3D11ShaderResourceView *none = nullptr; c1->PSSetShaderResources(0, 1, &none);
  ID3D11RenderTargetView *nrt = nullptr; c1->OMSetRenderTargets(1, &nrt, nullptr);
  if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); }
  c1->Release();
  return true;
}
// DLSSMIRROR 2026-10-08 (JJ: "Am i supposed to be seeing anything in the desktop window? AA looks bad still"): with
// DLSS the window holds the game's own small picture WITHOUT anti-aliasing (SMAA is replaced). Show the DLSS left eye
// there instead (scaled down to the window), after the headset took its copy.
void mirror_hud(ID3D11DeviceContext *c, ID3D11RenderTargetView *rtv, UINT w, UINT h);   // MIRRORHUD (below hud_in_eyes)
// MIRRORLATE 2026-10-09 (JJ in SteamVR: "a 2D flat copy of all 3D elements overlaid ... moves in the opposite direction"
// - his capture c184453: the HUD sheet held the whole left-eye picture): AKN takes every window-buffer bind outside the
// eye renders as HUD until akn_present, so this mirror draw landed on the HUD sheet (since STRBLEND opaque; before, the
// leftover HUD blend hid it). It now runs after akn_send_present, when the frame's HUD is complete.
bool g_mirrorPending = false;
bool g_mirrorHasHud = false;   // MIRRORHUD2: the eye picture being mirrored already holds the HUD
void dlss_mirror(ID3D11Texture2D *bb, const D3D11_TEXTURE2D_DESC &d) {
  if (!g_strPSL || !stretch_build()) return;
  if (!g_sbsSRV && FAILED(g_device->CreateShaderResourceView(g_aknSbs, nullptr, &g_sbsSRV))) return;
  // the window's buffer is borrowed for this one draw only (a kept view would block the game's ResizeBuffers)
  if (FAILED(g_device->CreateRenderTargetView(bb, nullptr, &g_bbRTV)) || !g_bbRTV) { g_bbRTV = nullptr; return; }
  ID3D11DeviceContext1 *c1 = nullptr; ID3DDeviceContextState *prev = nullptr;
  if (FAILED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&c1)) || !c1) return;
  c1->SwapDeviceContextState(g_strState, &prev);
  // STRBLEND 2026-10-09: g_strState is shared and KEEPS what was set in it - MIRRORHUD leaves the HUD blend
  // (ONE / INV_SRC_ALPHA) set, so every later draw in this state blended onto the old picture: the SVRSTRAIGHT output
  // piled up frame on frame (JJ: "blown out to white ... blurring when moving your head"). Opaque, no depth, default raster.
  { const FLOAT bf0[4] = {0, 0, 0, 0}; c1->OMSetBlendState(nullptr, bf0, 0xffffffff); c1->OMSetDepthStencilState(nullptr, 0); c1->RSSetState(nullptr); }
  c1->OMSetRenderTargets(1, &g_bbRTV, nullptr);
  c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c1->VSSetShader(g_strVS, nullptr, 0); c1->PSSetShader(g_strPSL, nullptr, 0);
  c1->PSSetShaderResources(0, 1, &g_sbsSRV); c1->PSSetSamplers(0, 1, &g_strSamp);
  D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)d.Width, (float)d.Height, 0.0f, 1.0f };
  c1->RSSetViewports(1, &vp);
  c1->Draw(3, 0);
  // MIRRORHUD2 2026-10-09 (JJ: "doubled-up HUD elements ... on the desktop mirror"): since MIRRORLATE the left eye is copied
  // AFTER HUDINEYE drew the HUD into it - draw the sheet again only when the eyes went without it this frame
  if (!g_mirrorHasHud) mirror_hud(c1, g_bbRTV, d.Width, d.Height);   // MIRRORHUD: the HUD on the desktop window too
  ID3D11ShaderResourceView *none = nullptr; c1->PSSetShaderResources(0, 1, &none);
  ID3D11RenderTargetView *nrt = nullptr; c1->OMSetRenderTargets(1, &nrt, nullptr);
  if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); }
  c1->Release();
  g_bbRTV->Release(); g_bbRTV = nullptr;
}
// BLACKGAP 2026-10-10 (JJ: "when entering gameplay it starts off in 2D flat for a split second before it corrects into 3D
// stereo"): after a load AKN waits for 30 steady frames before it draws both eyes (its M5c crash gate), and those frames
// showed the flat game picture in both eyes. In gameplay, up to 2 s without a pair are now BLACK (a fade-in into 3D);
// after that the flat picture again, so a real stall still shows something.
long g_aknBlack = 0;
// SCREENROOM: the window picture as a panel fixed in the room (g_fixedPose, g_nearScreenM ahead, the screen's own window),
// drawn into each half of the side-by-side image from that eye's own place at the latest head pose, black around it
bool hie_build();
extern ID3D11VertexShader *g_mcVS; extern ID3D11Buffer *g_mcVCB; extern ID3D11RasterizerState *g_hieRS; extern ID3D11DepthStencilState *g_hieDS;
extern XrFovf g_rawFov[2]; extern bool g_rawFovOk;
long g_roomScrFrames = 0;
bool room_screen_draw(ID3D11Texture2D *bb, const D3D11_TEXTURE2D_DESC &d, UINT halfW, UINT h) {
  if (!g_roomScrTOk || !g_rawFovOk || !stretch_build() || !hie_build() || !g_mcVS || !g_mcVCB) return false;
  if (g_flatIn) { D3D11_TEXTURE2D_DESC fd{}; g_flatIn->GetDesc(&fd); if (fd.Width != d.Width || fd.Height != d.Height || fd.Format != d.Format) { if (g_flatSRV) g_flatSRV->Release(); g_flatSRV = nullptr; g_flatIn->Release(); g_flatIn = nullptr; } }
  if (!g_flatIn) {
    D3D11_TEXTURE2D_DESC fd = d; fd.MipLevels = 1; fd.ArraySize = 1; fd.SampleDesc = {1, 0}; fd.Usage = D3D11_USAGE_DEFAULT;
    fd.BindFlags = D3D11_BIND_SHADER_RESOURCE; fd.CPUAccessFlags = 0; fd.MiscFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&fd, nullptr, &g_flatIn)) || FAILED(g_device->CreateShaderResourceView(g_flatIn, nullptr, &g_flatSRV))) return false;
  }
  if (!g_sbsRTV && FAILED(g_device->CreateRenderTargetView(g_aknSbs, nullptr, &g_sbsRTV))) return false;
  // the panel's corners in the room: top-left, top-right, bottom-left, bottom-right (the vertex order of g_mcVS)
  const float D = g_nearScreenM, *T = g_roomScrT;
  const XrVector3f loc[4] = {{D * T[0], D * T[2], -D}, {D * T[1], D * T[2], -D}, {D * T[0], D * T[3], -D}, {D * T[1], D * T[3], -D}};
  XrVector3f corner[4];
  for (int k = 0; k < 4; ++k) {
    const XrVector3f o = quat_rotate(g_fixedPose.orientation, loc[k]);
    corner[k] = {g_fixedPose.position.x + o.x, g_fixedPose.position.y + o.y, g_fixedPose.position.z + o.z};
  }
  const XrPosef head = g_curPose;
  float q[2][20]; bool front[2] = {true, true};
  for (int e = 0; e < 2; ++e) {
    const XrFovf &f = g_rawFov[e];   // the runtime's own lens, straight: right for every runtime (SteamVR too)
    const float tl = tanf(f.angleLeft), tr = tanf(f.angleRight), tu = tanf(f.angleUp), td = tanf(f.angleDown);
    const XrVector3f r = quat_rotate(head.orientation, XrVector3f{(e == 0 ? -0.5f : 0.5f) * akn_ipd_m(), 0.0f, 0.0f});
    const XrVector3f ep{head.position.x + r.x, head.position.y + r.y, head.position.z + r.z};
    const XrQuaternionf inv{-head.orientation.x, -head.orientation.y, -head.orientation.z, head.orientation.w};
    for (int k = 0; k < 4; ++k) {
      const XrVector3f v = quat_rotate(inv, XrVector3f{corner[k].x - ep.x, corner[k].y - ep.y, corner[k].z - ep.z});
      const float w = -v.z;
      if (w < 0.05f) front[e] = false;
      q[e][k * 4 + 0] = (2.0f * v.x + (tr + tl) * v.z) / (tr - tl);
      q[e][k * 4 + 1] = (2.0f * v.y + (tu + td) * v.z) / (tu - td);
      q[e][k * 4 + 2] = 0.5f * w; q[e][k * 4 + 3] = w;
    }
    // the picture's shown part: the middle slice the old floating screen showed (SCREENWIDE / PAUSE169 crop)
    const float cw = g_scrCropW, vf = g_scrVFrac;
    q[e][16] = 0.5f * (1.0f - cw); q[e][17] = 0.5f * (1.0f - vf); q[e][18] = cw; q[e][19] = vf;
  }
  // SCREENQUAD: the picture into the screen panel's own headset image (sRGB image: decoded first, the view encodes back)
  static int s_decOk = 0;
  if (!s_decOk) {
    s_decOk = -1;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    StrCompileFn compile = m ? (StrCompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    static const char ps[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { float3 c = t.Sample(s, uv).rgb;"
        " c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); return float4(c, 1); }";
    ID3DBlob *pb = nullptr, *err = nullptr;
    if (compile && SUCCEEDED(compile(ps, sizeof(ps) - 1, "scrdec", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err)) && pb &&
        SUCCEEDED(g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_scrPSdec)))
      s_decOk = 1;
    if (pb) pb->Release();
    if (err) err->Release();
  }
  const bool quadPath = g_scrSwap != XR_NULL_HANDLE && (!g_scrSrgb || s_decOk == 1);
  uint32_t sidx = 0; bool sAcq = false;
  if (quadPath) {
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_scrSwap, &ai, &sidx))) {
      sAcq = true;
      XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
      if (XR_FAILED(xrWaitSwapchainImage(g_scrSwap, &wi)) || sidx >= g_scrImages.size()) sidx = 0xffffffffu;
      else if (!g_scrRTV[sidx]) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = (DXGI_FORMAT)g_scrFmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_device->CreateRenderTargetView(g_scrImages[sidx], &rd, &g_scrRTV[sidx]))) { g_scrRTV[sidx] = nullptr; sidx = 0xffffffffu; }
      }
    }
  }
  const bool toQuad = sAcq && sidx != 0xffffffffu;
  g_ctx->CopyResource(g_flatIn, bb);
  ID3D11DeviceContext1 *c1 = nullptr; ID3DDeviceContextState *prev = nullptr;
  if (FAILED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&c1)) || !c1) {
    if (sAcq) { XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(g_scrSwap, &ri); }
    return false;
  }
  c1->SwapDeviceContextState(g_strState, &prev);
  { const FLOAT bf0[4] = {0, 0, 0, 0}; c1->OMSetBlendState(nullptr, bf0, 0xffffffff); }   // STRBLEND: opaque
  c1->OMSetDepthStencilState(g_hieDS, 0); c1->RSSetState(g_hieRS);
  const FLOAT black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  c1->ClearRenderTargetView(g_sbsRTV, black);
  c1->IASetInputLayout(nullptr); c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c1->VSSetShader(g_mcVS, nullptr, 0); c1->VSSetConstantBuffers(0, 1, &g_mcVCB);
  c1->PSSetShaderResources(0, 1, &g_flatSRV); c1->PSSetSamplers(0, 1, &g_strSamp);
  if (toQuad) {
    // the whole panel image = the screen's shown part of the picture
    float fq[20] = {-1, 1, 0.5f, 1,  1, 1, 0.5f, 1,  -1, -1, 0.5f, 1,  1, -1, 0.5f, 1,  q[0][16], q[0][17], q[0][18], q[0][19]};
    c1->PSSetShader(g_scrSrgb ? g_scrPSdec : g_strPS, nullptr, 0);
    c1->OMSetRenderTargets(1, &g_scrRTV[sidx], nullptr);
    c1->UpdateSubresource(g_mcVCB, 0, nullptr, fq, 0, 0);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, (float)kScrPx, (float)kScrPx, 0.0f, 1.0f};
    c1->RSSetViewports(1, &vp);
    c1->Draw(4, 0);
  } else {
    c1->PSSetShader(g_strPS, nullptr, 0);
    c1->OMSetRenderTargets(1, &g_sbsRTV, nullptr);
    for (int e = 0; e < 2; ++e) {
      if (!front[e]) continue;   // the panel behind this eye: black
      c1->UpdateSubresource(g_mcVCB, 0, nullptr, q[e], 0, 0);
      D3D11_VIEWPORT vp{(float)(e * halfW), 0.0f, (float)halfW, (float)h, 0.0f, 1.0f};
      c1->RSSetViewports(1, &vp);
      c1->Draw(4, 0);
    }
  }
  ID3D11ShaderResourceView *none = nullptr; c1->PSSetShaderResources(0, 1, &none);
  ID3D11RenderTargetView *nrt = nullptr; c1->OMSetRenderTargets(1, &nrt, nullptr);
  if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); }
  c1->Release();
  if (sAcq) { XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(g_scrSwap, &ri); }
  if (toQuad) {   // the room-fixed quad: the screen pose, D ahead, the screen's own window
    XrCompositionLayerQuad &sq = g_scrQuad;
    sq = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    sq.layerFlags = 0;   // opaque
    sq.space = g_localSpace;
    sq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    sq.subImage.swapchain = g_scrSwap;
    sq.subImage.imageRect.offset = {0, 0};
    sq.subImage.imageRect.extent = {(int32_t)kScrPx, (int32_t)kScrPx};
    sq.pose.orientation = g_fixedPose.orientation;
    const XrVector3f c = quat_rotate(g_fixedPose.orientation, XrVector3f{0.5f * D * (T[0] + T[1]), 0.5f * D * (T[2] + T[3]), -D});
    sq.pose.position = {g_fixedPose.position.x + c.x, g_fixedPose.position.y + c.y, g_fixedPose.position.z + c.z};
    sq.size = {D * (T[1] - T[0]), D * (T[2] - T[3])};
  }
  g_scrQuadNow = toQuad;
  g_roomScrHead = head;
  if (!g_roomScrFrames++) mode_log(toQuad ? "SCREENQUAD: start-up / loading screen as a room-fixed headset panel" : "SCREENROOM: start-up / loading screen drawn as a room panel");
  return true;
}
// TRANSFULL: the bats (the game's sound, AKN BATEVENT) or a dark cover over the sheet's 16:9 band (AKN HUDCOVER: the fade to black)
bool tr_decide() {
  using AgeFn = long (*)(); using CovFn = int (*)(float *, float *);
  static AgeFn p_age = nullptr; static CovFn p_cov = nullptr; static bool s_t = false;
  if (!s_t) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); p_age = m ? (AgeFn)GetProcAddress(m, "akn_bat_age_ms") : nullptr; p_cov = m ? (CovFn)GetProcAddress(m, "akn_hud_cover") : nullptr; s_t = m != nullptr; }
  const long age = p_age ? p_age() : -1;
  float a = 0.0f, b = 1.0f; const bool cov = p_cov && p_cov(&a, &b);
  const bool bats = age >= 0 && age < 2500, fade = cov && a > 0.6f && b < 0.08f;
  static ULONGLONG s_until = 0; static bool s_was = false;
  const ULONGLONG now = GetTickCount64();
  if (g_trOn && (bats || fade)) { s_until = now + 200; strcpy_s(g_trWhy, bats ? (fade ? "bats+fade" : "bats") : "fade"); }
  const bool on = g_trOn && now < s_until;
  if (on != s_was) { char t[48]; snprintf(t, sizeof(t), on ? "full-view transition on (%s)" : "full-view transition off", g_trWhy); mode_log(t); s_was = on; }
  return on;
}
bool aknSbsUpdate(IDXGISwapChain *sc, bool blackGap) {
  const bool roomWant = g_nearScreenNow;   // SCREENROOM (this frame's screen kind is decided later: last frame's)
  g_roomScrNow = false; g_scrQuadNow = false;
  ID3D11Texture2D *bb = nullptr;
  if (!sc || !g_ctx || FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) || !bb) return false;
  D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);
  long seq = 0;
  ID3D11Texture2D *pair = akn_get_pair(&seq);
  // DLSSPAIR: the side-by-side size = the pair's when AKN upscales (pair taller than the window), else the window x2
  UINT sbsW = d.Width * 2, sbsH = d.Height;
  if (pair) { D3D11_TEXTURE2D_DESC pd{}; pair->GetDesc(&pd); if (pd.Height > d.Height && pd.Format == d.Format) { sbsW = pd.Width; sbsH = pd.Height; } }
  if (g_aknSbs) {
    D3D11_TEXTURE2D_DESC sd{}; g_aknSbs->GetDesc(&sd);
    if (sd.Width != sbsW || sd.Height != sbsH || sd.Format != d.Format) { if (g_sbsRTV) { g_sbsRTV->Release(); g_sbsRTV = nullptr; } if (g_sbsSRV) { g_sbsSRV->Release(); g_sbsSRV = nullptr; } g_aknSbs->Release(); g_aknSbs = nullptr; }
  }
  if (!g_aknSbs) {
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Width = sbsW; sd.Height = sbsH; sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; sd.CPUAccessFlags = 0; sd.MiscFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &g_aknSbs))) { g_aknSbs = nullptr; bb->Release(); return false; }
  }
  bool fresh = false;
  if (pair && seq != g_aknSeq && !roomWant) {   // SCREENQUAD2: on a start-up / loading screen the window picture wins
    D3D11_TEXTURE2D_DESC pd{}; pair->GetDesc(&pd);
    if (pd.Width == sbsW && pd.Height == sbsH && pd.Format == d.Format) fresh = true;
  }
  // MENUSOUND 2026-10-12 (JJ: the menu's first sound comes ~1 s after its text; the start-up brightness was never measured in
  // engine stereo - "picture -1" - so the sound hold waited for MAIN MENU on, 3 s after the menu scene began; run 2: the eye
  // pair stays black while the menu TEXT is already up - AKN draws no HUD in start-up, the text is only in the game's own
  // window picture): the window picture
  bright_probe(bb, d.Width, d.Height);
  if (fresh) { g_ctx->CopyResource(g_aknSbs, pair); g_sbsHasHud = false; g_aknSeq = seq; ++g_aknPairs; if (sbsH != d.Height) g_mirrorPending = true; }   // MIRRORLATE
  else if (blackGap && (g_sbsRTV || SUCCEEDED(g_device->CreateRenderTargetView(g_aknSbs, nullptr, &g_sbsRTV)))) {   // BLACKGAP
    const FLOAT black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_ctx->ClearRenderTargetView(g_sbsRTV, black);
    ++g_aknBlack;
  }
  else if (roomWant && room_screen_draw(bb, d, sbsW / 2, sbsH)) {   // SCREENROOM
    g_roomScrNow = true;
    ++g_aknFlat;
  }
  else if (sbsH != d.Height) {
    flat_stretch(bb, d, sbsW / 2, sbsH);   // DLSSPAIR: menus / loading at the small render size, stretched
    ++g_aknFlat;
  }
  else {
    g_ctx->CopySubresourceRegion(g_aknSbs, 0, 0, 0, 0, bb, 0, nullptr);
    g_ctx->CopySubresourceRegion(g_aknSbs, 0, d.Width, 0, 0, bb, 0, nullptr);
    ++g_aknFlat;
  }
  g_aknFresh = fresh;
  {   // SCREENQUAD2: which way a start-up / loading screen reached the headset - logged on every change
    const int path = !roomWant ? 0 : g_scrQuadNow ? 1 : g_roomScrNow ? 2 : blackGap ? 3 : 4;
    static int s_path = -1; static bool s_pairSeen = false;
    if (roomWant && pair && seq != g_aknSeq && !s_pairSeen) { s_pairSeen = true; mode_log("screen: 3D pairs arrive (window picture used)"); }
    if (!roomWant) s_pairSeen = false;
    if (path != s_path) {
      static const char *nm[5] = {"screen path: none", "screen path: headset panel", "screen path: in the eyes", "screen path: black gap", "screen path: FLAT (not placed)"};
      s_path = path; mode_log(nm[path]);
    }
  }
  g_aknNoFresh = fresh ? 0 : (g_aknNoFresh < 1000 ? g_aknNoFresh + 1 : 1000);   // LOADFIT
  akn_send_report(g_aknPairs, g_aknFlat);   // AKN logs it every second (judge T8: stereo share in gameplay)
  if (pair && !fresh && seq != g_aknSeq && !roomWant) {   // a pair exists but does not fit this window: say why, once per change
    D3D11_TEXTURE2D_DESC pd{}; pair->GetDesc(&pd);
    static UINT s_w = 0, s_h = 0; static int s_f = 0;
    if (pd.Width != s_w || pd.Height != s_h || (int)pd.Format != s_f) {
      s_w = pd.Width; s_h = pd.Height; s_f = (int)pd.Format;
      char m[160]; snprintf(m, sizeof(m), "AKN pair %ux%u f%d does not fit the side-by-side %ux%u f%d - flat", pd.Width, pd.Height, (int)pd.Format, sbsW, sbsH, (int)d.Format);
      mode_log(m);
    }
  }
  snprintf(g_aknDiag, sizeof(g_aknDiag), "%s | eye %ux%u | stereo frames %ld, flat frames %ld, black (waiting for 3D) %ld | eyes %.1f mm, world scale %.1f",
           akn_link_diag(), d.Width, d.Height, g_aknPairs, g_aknFlat, g_aknBlack, akn_ipd_m() * 1000.0f, akvr_head_pos_scale());
  bb->Release();
  return true;
}
const char *akvr_xr_akn_diag() { return g_aknDiag; }

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
      if (r->referenceSpaceType == (g_spaceIsStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL)) {   // SVRSPACE
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
  if (XR_FAILED(sc_create("HUD layer", &sci, &g_hudSwap))) {
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
bool g_hudMenuNow = false;   // MENUSIZE: the layer carries the live main menu (xr.cpp's menuLive)
bool g_pauseLook = true;     // PAUSELOOK setting: the pause in full view (off: the old floating window)
bool g_pauseLive = false;    // PAUSELOOK: live now
bool g_pauseCand = false;    // PAUSEEDGE: a pause could start now (the pause look fits, gameplay within 1 s)
bool g_pauseWaiting = false;   // MAPFLY: the pause test holds but has not held 250 ms yet
int  g_stillPresents = 0;    // PAUSEEDGE: Presents in a row without a camera finalize
float g_pauseDim = 0.5f;     // PAUSEDIM: 0 = no darkening, 1 = black (JJ: "just darker")
float g_pauseMenuScale = 0.5f;   // PAUSESIZE: the pause menu on the room layer
ULONGLONG g_menuStartTick = 0;   // MENUSTART: when the 3D main menu first came up (0 = not yet)
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

// PAUSELOOK: the pause menu hangs in the room straight ahead of where you look when the pause starts (a new anchor
// from the next placement); the recentre anchor comes back when it ends (SKVR MENUAIM).
bool g_pauseAnchorSaved = false, g_savedWorldOk = false;
XrPosef g_savedWorldAnchor{};
void hud_pause_anchor(bool on) {
  if (on) {
    g_savedWorldAnchor = g_hudWorldAnchor; g_savedWorldOk = g_hudWorldOk; g_pauseAnchorSaved = true;
    g_hudWorldOk = false;
  } else if (g_pauseAnchorSaved) {
    g_hudWorldAnchor = g_savedWorldAnchor; g_hudWorldOk = g_savedWorldOk; g_pauseAnchorSaved = false;
  }
}

// HUDEYES 2026-10-01 (VR_HUD_GUIDE.md section 1, "HUD depth: per-eye quads"; SKVR HUDEYES3, run 108): the HUD layer
// is fixed in the room, so the old shift along the quad's own right axis is fixed to the direction faced at the last
// recentre - with the head turned and tilted towards an edge of the HUD it gains an up / down part between the eyes,
// and edge pieces stop converging (Sekiro). Now each eye's quad moves by minus that eye's offset from the eye midpoint,
// with the eyes as xrLocateViews reports them in LOCAL space at display time (the projection views carry the head
// pose for both eyes: SKVR HUDEYES2's shift from them was zero). The HUD picture itself carries no depth (geo-11 HUD
// shift 0 in the layer, checked in JJ's F2s of 2026-10-01). Panel switch "HUD depth follows your eyes" (hudeyefollow);
// off = the old axis shift. Only for a room-placed quad: attached to the head (view space) the old shift is exact.
XrVector3f g_eyeLocPos[2] = {};
bool g_eyeLocOk = false;
bool g_hudEyeFollow = true;
float g_eyeLocDistMm = 0.0f;
// HUDLAYER5: the placed quad as a left/right pair, each moved along the quad's own right axis.
void hud_eye_pair(const XrCompositionLayerQuad &q, XrCompositionLayerQuad &l, XrCompositionLayerQuad &r) {
  l = q; r = q;
  l.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
  r.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
  if (g_hudEyeFollow && g_eyeLocOk && q.space == g_localSpace) {   // HUDEYES
    const XrVector3f &p0 = g_eyeLocPos[0], &p1 = g_eyeLocPos[1];
    const XrVector3f mid{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f, (p0.z + p1.z) * 0.5f};
    l.pose.position = {q.pose.position.x - (p0.x - mid.x), q.pose.position.y - (p0.y - mid.y), q.pose.position.z - (p0.z - mid.z)};
    r.pose.position = {q.pose.position.x - (p1.x - mid.x), q.pose.position.y - (p1.y - mid.y), q.pose.position.z - (p1.z - mid.z)};
    return;
  }
  const XrVector3f s = quat_rotate(q.pose.orientation, XrVector3f{g_hudHalfIpd, 0.0f, 0.0f});
  l.pose.position = {q.pose.position.x + s.x, q.pose.position.y + s.y, q.pose.position.z + s.z};
  r.pose.position = {q.pose.position.x - s.x, q.pose.position.y - s.y, q.pose.position.z - s.z};
}

// Colour conversion pass (real context, our own state saved and restored).
ID3D11VertexShader *g_hcVS = nullptr;
ID3D11PixelShader *g_hcPS = nullptr;
ID3D11PixelShader *g_hcPSold = nullptr;   // HUDGLOW: the pre-2026-10-08 clipping conversion (hud colour mode 2)
ID3D11PixelShader *g_hcPS2 = nullptr;    // PAUSENOBACK: the same, dropping dark see-through pixels
ID3D11PixelShader *g_hcPS3 = nullptr;    // MENUBACK: the same, dark pixels faded by cb0.x
ID3D11Buffer *g_hcCB = nullptr;          // MENUBACK: cb0 = (opacity of the dark backing, 0, 0, 0)
bool g_pauseNoBack = true;               // PAUSENOBACK setting
// MENUNOBACK 2026-10-02 — JJ: "can the black background in the menu be removed? It's like a 2D element where I can see
// the square shape of it when I look around": the live main menu's dark backing, on the HUD layer like the pause's.
bool g_menuNoBack = true;
// MENUBACK 2026-10-02 — JJ: with the backing gone "the menu items look a bit weird now ... maybe it was needed"; he
// wants it back with "a slider for opacity". The live main menu's dark pixels are drawn at this opacity (1 = the game's
// own look, 0 = gone); text and highlights untouched. Replaces MENUNOBACK's on/off.
float g_menuBackOpacity = 1.0f;
float g_menuBackFeather = 0.5f;   // MENUFEATHER: 0..1 -> radius 0..6% of the layer image's width
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
  // HUDGLOW 2026-10-08 — JJ: weapon wheels look "posterised", "in the wrong blending mode" - in the geo-11 version too,
  // so not AKN's route: THIS conversion is shared. saturate(rgb / a) clips every pixel whose colour is above its own
  // coverage, i.e. light ADDED on top of a see-through backing (glows, additive strokes): rgb / a > 1 -> all such
  // pixels become one flat top value = bands. Now each pixel is split: the part its coverage carries (min(rgb, a))
  // is converted as before, the light above it (rgb - a) is added on in linear light, nothing is clipped.
  // The old shader stays for an A/B (hud colour mode 2).
  static const char ps[] =
      "Texture2DArray<float4> t : register(t0);"
      "float3 lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }"
      "float4 main(float4 pos : SV_Position) : SV_Target {"
      " float4 p = t.Load(int4(pos.xy, 0, 0));"
      " float a = saturate(p.a);"
      " float3 b = min(p.rgb, a);"
      " float3 e = max(p.rgb - a, 0);"
      " float3 o = (a > 0.004 ? lin(b / a) * a : 0) + lin(saturate(e));"
      " return float4(o, a); }";
  static const char psOld[] =
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
  {   // HUDGLOW: the old clipping conversion, for the A/B
    ID3DBlob *pbo = nullptr, *erro = nullptr;
    if (ok && SUCCEEDED(compile(psOld, sizeof(psOld) - 1, "hudpsold", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pbo, &erro)) && pbo)
      if (FAILED(g_device->CreatePixelShader(pbo->GetBufferPointer(), pbo->GetBufferSize(), nullptr, &g_hcPSold))) g_hcPSold = nullptr;
    if (erro) erro->Release();
    if (pbo) pbo->Release();
  }
  if (vb) vb->Release();
  if (pb) pb->Release();
  // PAUSENOBACK 2026-10-01 — JJ: "for the pause screen, remove the gradient dark HUD layer and darken the actual world
  // instead". His pause F2: the layer was 47% covered, almost all of it see-through black (the pause menu's dark
  // gradient backing, on the room layer at the pause size). While the pause view is live, this variant drops pixels
  // that are partly see-through AND dark (brightest channel under 0.12 once the coverage is divided out): the backing
  // and the text's soft shadows go; text, the highlight bar and button icons stay. PAUSEDIM darkens the world instead.
  // PAUSENOBACK2 (JJ: the black still "appears briefly", "some sort of corner element there on the top left, still
  // present", "the text that says pause menu is not very readable"): dark pixels go whether see-through or solid (the
  // corner piece), and anything dim but not dark is lifted so its brightest channel reaches 0.6 (the dark-grey title).
  static const char ps2[] =
      "Texture2DArray<float4> t : register(t0);"
      "float3 lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }"
      "float4 main(float4 pos : SV_Position) : SV_Target {"
      " float4 p = t.Load(int4(pos.xy, 0, 0));"
      " float a = saturate(p.a);"
      " float3 e = a > 0.004 ? max(p.rgb - a, 0) : 0;"
      " float3 c = a > 0.004 ? min(p.rgb, a) / a : saturate(p.rgb);"
      " float m = max(c.r, max(c.g, c.b));"
      " if (m < 0.12 && max(e.r, max(e.g, e.b)) < 0.02) return float4(0, 0, 0, 0);"
      " if (m < 0.6 && m > 0.0001) c *= 0.6 / m;"
      " float3 o = a > 0.004 ? lin(saturate(c)) * a + lin(saturate(e)) : lin(c);"
      " return float4(o, a); }";
  ID3DBlob *pb2 = nullptr, *err2 = nullptr;
  if (ok && SUCCEEDED(compile(ps2, sizeof(ps2) - 1, "hudps2", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb2, &err2)) && pb2)
    if (FAILED(g_device->CreatePixelShader(pb2->GetBufferPointer(), pb2->GetBufferSize(), nullptr, &g_hcPS2))) g_hcPS2 = nullptr;
  if (err2) err2->Release();
  if (pb2) pb2->Release();
  // MENUBACK: the plain conversion, with dark pixels (brightest channel under ~0.12, eased over 0.08-0.16 so the text's
  // soft edges do not step) scaled by cb0.x. At 1 it is exactly the plain shader.
  // MENUFEATHER 2026-10-02 — JJ: "The black element, I think, is needed, but can we feather the edges of it so it's not as
  // noticeable when looking around?" cb0.y = a radius in pixels: a dark pixel fades with its distance to the nearest
  // empty pixel (or the picture's edge), looked for in 8 directions at 6 distances up to that radius. Compiled offline
  // with d3dcompiler_47 before it went in (ps_5_0, no errors).
  static const char ps3[] =
      "Texture2DArray<float4> t : register(t0);"
      "cbuffer K : register(b0) { float4 k; };"
      "float3 lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }"
      "float empty(int2 q, int2 sz) {"
      " if (q.x < 0 || q.y < 0 || q.x >= sz.x || q.y >= sz.y) return 1.0;"
      " return t.Load(int4(q, 0, 0)).a < 0.02 ? 1.0 : 0.0; }"
      "float4 main(float4 pos : SV_Position) : SV_Target {"
      " float4 p = t.Load(int4(pos.xy, 0, 0));"
      " float a = saturate(p.a);"
      " float3 e = a > 0.004 ? max(p.rgb - a, 0) : 0;"
      " float3 c = a > 0.004 ? min(p.rgb, a) / a : saturate(p.rgb);"
      " float m = max(c.r, max(c.g, c.b));"
      " float3 o = a > 0.004 ? lin(c) * a + lin(saturate(e)) : lin(c);"
      " float dark = saturate(k.x);"
      " if (k.y > 0.5 && m < 0.16 && a > 0.004) {"
      "  uint w, h, e; t.GetDimensions(w, h, e);"
      "  int2 sz = int2(w, h); int2 P = int2(pos.xy);"
      "  float dmin = k.y;"
      "  [loop] for (int i = 1; i <= 6; ++i) {"
      "   float r = k.y * i / 6.0;"
      "   [unroll] for (int j = 0; j < 8; ++j) {"
      "    float an = j * 0.7853982;"
      "    int2 q = P + int2(round(cos(an) * r), round(sin(an) * r));"
      "    if (empty(q, sz) > 0.5) dmin = min(dmin, r); } }"
      "  dark *= smoothstep(0.0, 1.0, dmin / k.y); }"
      " float f = lerp(dark, 1.0, smoothstep(0.08, 0.16, m));"
      " return float4(o, a) * f; }";
  ID3DBlob *pb3 = nullptr, *err3 = nullptr;
  if (ok && SUCCEEDED(compile(ps3, sizeof(ps3) - 1, "hudps3", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb3, &err3)) && pb3)
    if (FAILED(g_device->CreatePixelShader(pb3->GetBufferPointer(), pb3->GetBufferSize(), nullptr, &g_hcPS3))) g_hcPS3 = nullptr;
  if (err3) err3->Release();
  if (pb3) pb3->Release();
  if (g_hcPS3) {
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const float init[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = init;
    if (FAILED(g_device->CreateBuffer(&bd, &sd, &g_hcCB))) g_hcCB = nullptr;
  }
  snprintf(g_hudConvDiag, sizeof(g_hudConvDiag), ok ? "colour conversion ready" : "colour shaders failed");
  g_hcState = ok ? 1 : -1;
  return ok;
}
// STARTBRIGHT 2026-10-02 — JJ: with AUDIOSYNC3 "the audio started after the main menu had appeared. ... We're syncing
// it up with the appearance of the first menu", not "an arbitrary value". During start-up only (menu phase 0), five
// 64x64 patches of the left eye's picture are copied to a small CPU-readable texture (3 in flight, read when the GPU
// is done) and their mean brightness kept: the menu has appeared when the picture is no longer black.
ID3D11Texture2D *g_brStage[3] = {};
bool g_brPend[3] = {};
int g_brNext = 0;
DXGI_FORMAT g_brFmt = DXGI_FORMAT_UNKNOWN;
volatile float g_startBright = -1.0f;
volatile ULONGLONG g_startBrightAt = 0;
static float half_to_float(uint16_t h) {
  const uint32_t e = (h >> 10) & 31, m = h & 1023;
  float v = e == 0 ? m / 1024.0f / 16384.0f : (e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * powf(2.0f, (float)e - 15.0f));
  return (h & 0x8000) ? -v : v;
}
static void bright_read(ID3D11Texture2D *t) {
  D3D11_MAPPED_SUBRESOURCE m{};
  HRESULT hr = g_ctx->Map(t, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
  if (FAILED(hr)) return;
  double sum = 0.0; int n = 0;
  for (int y = 0; y < 64; y += 2) {
    const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
    for (int x = 0; x < 320; x += 2) {
      float v = -1.0f;
      if (g_brFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const uint16_t *p = (const uint16_t *)(row + x * 8);
        v = fmaxf(half_to_float(p[0]), fmaxf(half_to_float(p[1]), half_to_float(p[2])));
      } else if (g_brFmt == DXGI_FORMAT_R10G10B10A2_UNORM || g_brFmt == DXGI_FORMAT_R10G10B10A2_TYPELESS) {
        const uint32_t q = *(const uint32_t *)(row + x * 4);
        v = (float)max(q & 1023u, max((q >> 10) & 1023u, (q >> 20) & 1023u)) / 1023.0f;
      } else {
        const uint8_t *p = row + x * 4;
        v = (float)max(p[0], max(p[1], p[2])) / 255.0f;
      }
      sum += v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); ++n;
    }
  }
  g_ctx->Unmap(t, 0);
  if (n) { g_startBright = (float)(sum / n); g_startBrightAt = GetTickCount64(); }
}
static void bright_probe(ID3D11Texture2D *src, UINT eyeW, UINT h) {
  if (!src || !g_ctx || !g_device || !(g_autoMainMenu && g_menuPhase == 0) || eyeW < 256 || h < 256) return;
  D3D11_TEXTURE2D_DESC sd{};
  src->GetDesc(&sd);
  if (sd.SampleDesc.Count != 1) return;
  const bool known = sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || sd.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                     sd.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
                     (sd.Format >= DXGI_FORMAT_R8G8B8A8_TYPELESS && sd.Format <= DXGI_FORMAT_R8G8B8A8_SINT) ||
                     (sd.Format >= DXGI_FORMAT_B8G8R8A8_UNORM && sd.Format <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB);
  if (!known) return;
  if (g_brFmt != sd.Format) {
    for (auto &t : g_brStage) if (t) { t->Release(); t = nullptr; }
    for (auto &p : g_brPend) p = false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 320; d.Height = 64; d.MipLevels = 1; d.ArraySize = 1; d.Format = sd.Format;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &t : g_brStage) if (FAILED(g_device->CreateTexture2D(&d, nullptr, &t))) t = nullptr;
    g_brFmt = sd.Format;
  }
  for (int i = 0; i < 3; ++i)
    if (g_brPend[i] && g_brStage[i]) { bright_read(g_brStage[i]); g_brPend[i] = false; break; }
  const int k = g_brNext;
  if (!g_brStage[k] || g_brPend[k]) return;
  static const float fx[5] = {0.2f, 0.5f, 0.8f, 0.3f, 0.7f}, fy[5] = {0.3f, 0.3f, 0.3f, 0.7f, 0.7f};
  for (int i = 0; i < 5; ++i) {
    const UINT x = (UINT)(fx[i] * eyeW) - 32, y = (UINT)(fy[i] * h) - 32;
    D3D11_BOX b{x, y, 0, x + 64, y + 64, 1};
    g_ctx->CopySubresourceRegion(g_brStage[k], 0, (UINT)i * 64, 0, 0, src, 0, &b);
  }
  g_brPend[k] = true;
  g_brNext = (k + 1) % 3;
}
// the picture's mean brightness in start-up, 0..1, measured in the last 300 ms; -1 = unknown
float startup_bright_impl() {
  return (g_startBrightAt && GetTickCount64() - g_startBrightAt < 300) ? g_startBright : -1.0f;
}

// MENUCRASH 2026-10-06: the graphics menu crashed the game in CreateShaderResourceView on the HUD layer's source (a
// texture that lives inside geo-11; it was rebuilt under us). hudsplit now follows it every frame; this is the
// second line: a fault here drops the HUD layer for the frame and has hudsplit search for the texture again.
volatile long g_hudSrcFaults = 0;
static bool hud_srv_guarded(ID3D11Texture2D *src, const D3D11_SHADER_RESOURCE_VIEW_DESC *vd, ID3D11ShaderResourceView **out) {
  __try { return SUCCEEDED(g_device->CreateShaderResourceView(src, vd, out)); }
  __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; ++g_hudSrcFaults; akvr_hudsplit_source_lost(); return false; }
}
static bool hud_copy_guarded(ID3D11Texture2D *dst, ID3D11Texture2D *src) {
  __try { g_ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, nullptr); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { ++g_hudSrcFaults; akvr_hudsplit_source_lost(); return false; }
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
    if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !hud_srv_guarded(src, &vd, &g_hcSRV)) {
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
  // PAUSENOBACK for the pause; MENUBACK (opacity) for the live main menu
  const bool noBack = g_pauseLive && g_pauseNoBack;
  const bool menuBack = g_hudMenuNow && !g_pauseLive && g_hcPS3 && g_hcCB &&
                        (g_menuBackOpacity < 0.999f || g_menuBackFeather > 0.001f);   // MENUFEATHER
  ID3D11Buffer *oCB = nullptr;
  if (menuBack) {
    g_ctx->PSGetConstantBuffers(0, 1, &oCB);
    const float kk[4] = { g_menuBackOpacity, g_menuBackFeather * 0.06f * (float)g_hudW, 0.0f, 0.0f };
    g_ctx->UpdateSubresource(g_hcCB, 0, nullptr, kk, 0, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_hcCB);
  }
  g_ctx->PSSetShader(menuBack ? g_hcPS3 : (noBack && g_hcPS2 ? g_hcPS2 : (g_hudColour == 2 && g_hcPSold ? g_hcPSold : g_hcPS)), nullptr, 0);   // HUDGLOW: mode 2 = old   // PAUSENOBACK / MENUBACK
  g_ctx->PSSetShaderResources(0, 1, &g_hcSRV);
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  g_ctx->OMSetDepthStencilState(nullptr, 0);
  g_ctx->RSSetState(nullptr);
  g_ctx->Draw(3, 0);
  if (menuBack) { g_ctx->PSSetConstantBuffers(0, 1, &oCB); if (oCB) oCB->Release(); }   // MENUBACK: put the game's cb0 back
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
extern XrCompositionLayerQuad g_hudLastQ; extern bool g_hudLastQOk;   // HUDINEYE (below)
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
  const long faults0 = g_hudSrcFaults;
  if (ok && !(g_hudColour == 1 && hud_conv_draw(src, idx)) && g_hudSrcFaults == faults0)
    hud_copy_guarded(g_hudImages[idx], src);   // MENUCRASH: not again on a source that just faulted
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(g_hudSwap, &ri);
  if (!ok) return false;

  float D = akvr_geo11_hud_dist();
  if (D < 0.3f) D = 10.0f;                       // "far away"
  if (g_hudOnScreen && g_roomScrNow) D = g_nearScreenM;   // SCREENROOM: the HUD on the screen, at the screen's distance
  const float tl = tanf(fov.angleLeft), tr = tanf(fov.angleRight);
  const float tu = tanf(fov.angleUp), td = tanf(fov.angleDown);
  const XrQuaternionf ident{0.0f, 0.0f, 0.0f, 1.0f};
  const XrQuaternionf ori = g_tiltUsed != 0.0f ? pitch_postmul(ident, -g_tiltUsed) : ident;
  g_hudViewPose.orientation = ori;
  // MENUSIZE 2026-09-30 — JJ: with the whole main menu on the layer "the menu text elements are too big". On the
  // menu the quad follows the panel's "main menu size" (the floating menu screen's own size), about its centre.
  // PAUSELOOK: the pause menu at its own size. PAUSESIZE 2026-10-01 — JJ: "make the whole pause screen HUD element
  // scaled down so you don't have to turn your head to see it" - its own setting (the map keeps the pause/map size).
  const float msz = g_pauseLive ? g_pauseMenuScale : g_screenFovScale;
  const float ms = g_hudMenuNow ? (msz < 0.1f ? 0.1f : (msz > 1.0f ? 1.0f : msz)) : 1.0f;
  g_hudViewPose.position = quat_rotate(ori, XrVector3f{ms * D * (tr + tl) * 0.5f, ms * D * (tu + td) * 0.5f, -D});
  q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied (HUD-004)
  q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  q.subImage.swapchain = g_hudSwap;
  q.subImage.imageArrayIndex = 0;
  q.subImage.imageRect.offset = {0, 0};
  q.subImage.imageRect.extent = {(int32_t)w, (int32_t)h};
  if (!g_hudMenuNow && (g_scrVFrac < 0.999f || g_scrCropW < 0.999f)) {
    // MAPFLAT: a floating screen shows only the middle slice of its picture (PAUSE169 / SCREENWIDE) - the HUD sheet is
    // laid out over the whole picture, so it shows the same slice (it was squeezed whole into the slice: map icons off)
    const int32_t cw = (int32_t)(w * g_scrCropW), ch = (int32_t)(h * g_scrVFrac);
    q.subImage.imageRect.offset = {((int32_t)w - cw) / 2, ((int32_t)h - ch) / 2};
    q.subImage.imageRect.extent = {cw, ch};
  }
  q.size = {ms * D * (tr - tl), ms * D * (tu - td)};
  if (g_hudOnScreen && !g_hudMenuNow) {
    // MAPFLAT: on a floating screen the HUD hangs ON that screen (the screen's own room pose), not at the gameplay HUD's
    // room anchor - opened while looking elsewhere, the map's icons sat off to one side of the map
    q.space = g_localSpace;
    q.pose.orientation = hud_qmul(g_fixedPose.orientation, g_hudViewPose.orientation);
    const XrVector3f o = quat_rotate(g_fixedPose.orientation, g_hudViewPose.position);
    q.pose.position = {g_fixedPose.position.x + o.x, g_fixedPose.position.y + o.y, g_fixedPose.position.z + o.z};
  } else if (!hud_place(q, g_predicted)) return false;
  g_hudLastQ = q; g_hudLastQOk = true;   // HUDINEYE: where the HUD hangs, for drawing it into the eye pictures
  g_hudDistUsed = D; g_hudSizeW = q.size.width; g_hudSizeH = q.size.height;
  ++g_hudSubmits;
  return true;
}

// HUDINEYE 2026-10-08 - JJ (engine stereo): the weapon wheels "in a weird blending mode that looks posterized" - also
// under geo-11. His captures (akvr_native/captures_jj_20261008, c194443 / c195922): the HUD sheet itself is right (the
// wheel composited the game's way looks exactly like the flat game), but the headset blends the HUD QUAD in LINEAR
// light while the game blends in display (gamma) values: the wheel's dark see-through backing comes out milky, the
// world shows through it far too much, and the dark gradient bands in Virtual Desktop's stream. Fix: in gameplay
// the HUD is no longer a layer; it is drawn into each eye picture (our side-by-side copy, 8-bit display values) with
// the game's own blend (premultiplied ONE / INV_SRC_ALPHA) - at exactly the quad's place in the room, through each
// eye's own pose and lens, so it keeps its distance and depth. Menus / pause / flat frames keep the layer.
int g_hudInEye = 1;           // setting hudineye: 1 = HUD drawn into the eyes in gameplay, 0 = the old quad layer
XrCompositionLayerQuad g_hudLastQ{XR_TYPE_COMPOSITION_LAYER_QUAD};
bool g_hudLastQOk = false;
bool g_hieThisFrame = false;  // the eyes carry the HUD this frame: no HUD layer
// PATHREC 2026-10-13 (JJ: a duplicate compass at gameplay start, the distance marker flickering to another place every few s):
// hud_in_eyes draws INTO g_aknSbs, which is refreshed only on a fresh pair; on a frame without one the old picture (HUD drawn in)
// is shown again AND the HUD quad layer is added = two HUDs. Counted per second (AKN logs akvr_path_diag).
// (PATHREC flags: with g_aknFresh)
long g_hieDraws = 0, g_hieSkips = 0;
int g_hieState = 0;           // 0 not built, 1 ready, -1 failed
ID3D11VertexShader *g_hieVS = nullptr; ID3D11PixelShader *g_hiePS = nullptr;
// MARKERCUT: VS with a uv rectangle, PS with up to 8 marker circles (cut out of the sheet / kept on the cards)
ID3D11VertexShader *g_mcVS = nullptr; ID3D11PixelShader *g_mcPS = nullptr; ID3D11Buffer *g_mcVCB = nullptr, *g_mcPCB = nullptr;
int g_markerCut = 1;          // setting markercut: markers drawn into each eye at their object
float g_mcRadius = 0.08f;   // MKREACH 2026-10-09: 0.06 lost the bracket corners (MARKERRING still guards nearby HUD) -     // NDC (0.12 = the geo-11 / Arkham City reach took big holes out of nearby HUD - JJ 2026-10-09)
long g_mcCards = 0, g_mcNoWorld = 0;
// MARKSHEET 2026-10-09 (JJ: "do that one instead" - the structural fix for the HUD holes): AKN draws the marker pieces
// into their own sheet (routed by the marker centres exported here, last frame's); the HUD sheet is then drawn whole and
// the marker sheet as per-eye cards (radius g_mkCardR, no ring test). Falls back to MARKERCUT + MARKERRING when AKN routed
// nothing last frame. Test switch akvr_set_marksheet.
int g_markSheetOn = 1; float g_mkCardR = 0.22f; long g_mkSheetFrames = 0;   // MKREACH 2026-10-09: card 0.15 clipped the bracket corners / distance text
// MARKERWORLD 2026-10-09 (JJ: the distance marker "a little bit too small" and "rotating when my head rolls" - it should
// turn only with the camera): each card is a panel in the world at the marker's point, sides along the head-free game
// camera's right / up (akvr_camera_axes_ago base axes), its size the card's on-screen size x g_mkScale, projected per eye.
float g_mkScale = 1.25f;   // setting markersize
volatile LONG g_mkWorldOn = 1;   // EYESHOT A/B: 0 = the old flat per-eye square (AKN command "mkworld 0|1")
extern "C" __declspec(dllexport) void akvr_marker_world_set(int on) { InterlockedExchange(&g_mkWorldOn, on ? 1 : 0); }
float g_mkExp[16] = {}; volatile long g_mkExpN = 0;
ID3D11ShaderResourceView *g_mkSRV = nullptr; ID3D11Texture2D *g_mkSrc = nullptr;
using AknMarkerHudFn = ID3D11Texture2D *(*)(long *);
AknMarkerHudFn p_aknMarkerHud = nullptr; bool p_aknMarkerHudTried = false;
extern "C" __declspec(dllexport) int akvr_marker_centres(float *xy, int max) {   // MARKSHEET (AKN): last frame's cut centres, sheet NDC
  const int n = (int)g_mkExpN < max ? (int)g_mkExpN : max;
  for (int i = 0; i < n * 2; ++i) xy[i] = g_mkExp[i];
  return g_markSheetOn ? n : 0;
}
extern "C" __declspec(dllexport) void akvr_set_marksheet(int v) { g_markSheetOn = v ? 1 : 0; }   // MARKSHEET test switch
using AknMarkersFn = int (*)(float *, int); using AknPairVpFn = int (*)(float *);
AknMarkersFn p_aknMarkers = nullptr; AknPairVpFn p_aknPairVp = nullptr;
ID3D11Buffer *g_hieCB = nullptr; ID3D11BlendState *g_hieBS = nullptr; ID3D11SamplerState *g_hieSS = nullptr;
ID3D11RasterizerState *g_hieRS = nullptr; ID3D11DepthStencilState *g_hieDS = nullptr;
ID3D11ShaderResourceView *g_hieSRV = nullptr; ID3D11Texture2D *g_hieSrc = nullptr;
ID3D11RenderTargetView *g_hieRTV = nullptr; ID3D11Texture2D *g_hieDst = nullptr;
char g_hieDiag[512] = "HUD in the eyes: not used yet";   // 512: MKSAMEFRAME / MKCARRY counters
char g_mkCardDiag[2048] = "marker cards: none yet";   // EYESHOT: the first card's numbers, written into each capture (cards.txt)
int g_hieNPts = 0; float g_hiePts[16] = {}, g_hieXf[4] = {};   // MARKERCUT
bool g_mkSameFrame = true; unsigned g_mkSfUsed = 0, g_mkSfLearn = 0, g_mkSfNew = 0;   // MKSAMEFRAME (below)
bool g_mkCarryOn = true; unsigned g_mkCarry = 0;   // MKCARRY (below)
// MKEYE 2026-10-11 (akvr_native/NOTES.md RETHINK MARKERS 2): AKN draws each marker piece into a left- and a right-eye
// layer at its world point (inside the game's own draw); here each layer goes over its eye picture 1:1 and no cards are cut
bool g_hieEyeMode = false; float g_hieXfLast[4] = {}; bool g_hieXfOk = false; unsigned g_hieEyeDraws = 0;
unsigned g_eyeSkipNoSrv = 0, g_eyeSkipCut = 0, g_eyeSkipTr = 0, g_eyeSkipKey = 0, g_eyeSkipMode = 0;   // EYESKIP 2026-10-13: why the per-eye marker layers are not drawn
extern "C" __declspec(dllexport) int akvr_hud_layout_map(float *xf)   // AKN: HUD-sheet clip -> the movie's own layout
{
  if (!g_hieXfOk || !xf) return 0;
  for (int i = 0; i < 4; ++i) xf[i] = g_hieXfLast[i];
  return 1;
}
// WHEELEYE: the darkening (pause / depth-of-field-off exposure) drawn into the eyes UNDER the HUD when the HUD is in
// the eyes (the dim layer would sit over the in-eye HUD); dim_layer then sends no layer that frame
ID3D11PixelShader *g_hieDimPS = nullptr; ID3D11Buffer *g_hieDimCB = nullptr; bool g_hieDimDone = false;
// BACKEXT 2026-10-09 (JJ: the weapon wheel / pause "sitting in a dark window"; capture c000911: the game's backdrop =
// 50 % black over the whole sheet + a darker 16:9 band, made for a flat screen, so it ends at the sheet's edges): on the
// pause look the sheet's border is carried on outward over the whole view (clamped edge texels), so the backdrop covers
// the view as it covers the flat screen. Setting-free switch: g_backExt / export akvr_set_backext.
ID3D11PixelShader *g_hieExtPS = nullptr; int g_backExt = 0;   // off since BLACKKEY (it carried the black edges outward)
// BLACKKEY 2026-10-09 (JJ: every screen that pauses the game - pause, weapon wheel, the other wheels, mission dialogs -
// "still have the black background with hard straight edges at the top and bottom, and the sides ... we need to remove
// those black backgrounds"): their backdrops are PURE black with coverage (c000911: rgb 0, alpha 127-222), made for a
// flat screen; every real menu piece has colour (the wheels' insides are dark blue). On the pause look the sheet is drawn
// with pure-black see-through texels removed; AKVR's own pause darkening covers the whole view evenly instead.
// Command / export akvr_set_blackkey.
ID3D11PixelShader *g_hieKeyPS = nullptr; int g_blackKey = 1;
// FULLCUT 2026-10-10 (JJ: "when finishing a training section where it goes to a black screen, and then back to the start.
// The black screen is sixteen by nine. Window ... make it cover the entire field of view ... just black in the headset.
// This should happen anytime one of these full screen cuts happens"). The game fades to black by covering its HUD sheet
// (made for a flat screen), so the headset showed a black panel with the world around it. Each new HUD frame, 64 texels
// along the sheet's border are copied out (read back a few frames later, never waiting): an EVEN dark cover on all four
// edges = a full-screen fade, and its alpha darkens the WHOLE view in both eyes (and the dim layer when the HUD is not in
// the eyes), so fades in and out keep their own pace. On the pause look only a near-full cover counts (its 50 % backdrop
// is already AKVR's own pause darkening). Switch: export akvr_set_fullcut / AKN command "fullcut 0|1".
// FULLCUT5: the cut measured on the graphics card each frame (1x1 float, read by the darkening and the sheet shaders)
ID3D11PixelShader *g_cutPS = nullptr; ID3D11Texture2D *g_cutTex = nullptr; ID3D11RenderTargetView *g_cutRTV = nullptr;
ID3D11ShaderResourceView *g_cutSRV = nullptr; ID3D11Buffer *g_cutCB = nullptr;
int g_cutLog = 0;   // PERF1: the CPU border readback is telemetry only since FULLCUT5 - off unless asked ("cutlog 1")
int g_fullCut = 1; float g_cutA = 0.0f; long g_cutSeq = -1, g_cutFrames = 0; char g_cutDiag[256] = "full-screen cut probe: no reading yet";
ID3D11Texture2D *g_cutStage[3] = {}; bool g_cutPend[3] = {}; int g_cutI = 0; DXGI_FORMAT g_cutFmt = DXGI_FORMAT_UNKNOWN;
void cut_probe() {
  // FULLCUT4 2026-10-10 (the 02:32 log of a training transition: sheet edges a T 0 B 0 L .37 R .37, 16:9 band T .67 B 0
  // L .62 R .62 = a FULL-WIDTH dark box covering only the middle part of the sheet's height). Ring 0 (64 texels) = the
  // sheet's border; then 3 columns (1.5 %, 50 %, 98.5 % across) x 32 rows. A row counts when all three texels are dark
  // with the same coverage; a cut needs the middle row covered (cutscene letterbox bars never are) and >= 8 such rows
  // that agree; its alpha darkens the whole view.
  if (!g_fullCut || !akvr_engine_stereo() || !g_ctx || !g_device || akvr_xr_main_menu_detected()) { g_cutA = 0.0f; return; }
  constexpr int kRing = 64, kRows = 32, kN = kRing + 3 * kRows;
  for (int k = 0; k < 3; ++k) {   // the oldest copies first
    const int i = (g_cutI + k) % 3;
    if (!g_cutPend[i] || !g_cutStage[i]) continue;
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (g_ctx->Map(g_cutStage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) != S_OK) continue;
    const uint8_t *px = (const uint8_t *)ms.pData;
    auto A = [&](int n) { return px[n * 4 + 3] / 255.0f; };
    auto C = [&](int n) { return (float)max(px[n * 4], max(px[n * 4 + 1], px[n * 4 + 2])) / 255.0f; };
    // ring 0: the whole sheet covered evenly
    float aMin = 1.0f, aMax = 0.0f, aSum = 0.0f, rgbMax = 0.0f, edgeA[4] = {};
    for (int n = 0; n < kRing; ++n) { const float a = A(n); aMin = min(aMin, a); aMax = max(aMax, a); aSum += a; rgbMax = max(rgbMax, C(n)); edgeA[n / 16] += a / 16.0f; }
    const float ringCut = (aMin > 0.02f && aMax - aMin < 0.08f && rgbMax < 0.10f) ? aSum / kRing : 0.0f;
    // columns: rows covered evenly across the full width
    int covered = 0, first = -1, last = -1; float rowMin = 1.0f, rowMax = 0.0f, rowSum = 0.0f; bool mid = false;
    for (int r = 0; r < kRows; ++r) {
      const int n0 = kRing + r, n1 = kRing + kRows + r, n2 = kRing + 2 * kRows + r;
      const float a0 = A(n0), a1 = A(n1), a2 = A(n2);
      const float lo = min(a0, min(a1, a2)), hi = max(a0, max(a1, a2));
      const bool dark = max(C(n0), max(C(n1), C(n2))) < 0.10f;
      if (lo > 0.02f && hi - lo < 0.08f && dark) {
        ++covered; if (first < 0) first = r; last = r;
        const float m = (a0 + a1 + a2) / 3.0f; rowMin = min(rowMin, m); rowMax = max(rowMax, m); rowSum += m;
        if (r == kRows / 2 || r == kRows / 2 - 1) mid = true;
      }
    }
    const float boxCut = (mid && covered >= 8 && rowMax - rowMin < 0.10f) ? rowSum / covered : 0.0f;
    g_ctx->Unmap(g_cutStage[i], 0);
    g_cutPend[i] = false;
    float cut = max(ringCut, boxCut);
    if (g_pauseLive && cut < 0.9f) cut = 0.0f;
    {   // the strongest reading of the last second, for AKN's log
      const float strength = max(max(edgeA[2], edgeA[3]), covered / (float)kRows);
      static ULONGLONG s_t = 0; static float s_best = -1.0f; const ULONGLONG now = GetTickCount64();
      if (now - s_t > 1000) { s_t = now; s_best = -1.0f; }
      if (strength > s_best) {
        s_best = strength;
        snprintf(g_cutDiag, sizeof(g_cutDiag), "sheet edges a %.2f %.2f %.2f %.2f rgb %.2f | full-width rows covered %d of %d (rows %d-%d, middle %s, alpha %.2f-%.2f) | cut %.2f%s",
                 edgeA[0], edgeA[1], edgeA[2], edgeA[3], rgbMax, covered, kRows, first, last, mid ? "yes" : "no", covered ? rowMin : 0.0f, covered ? rowMax : 0.0f, cut, g_pauseLive ? " (pause look)" : "");
      }
    }
    if (cut > 0.0f && g_cutA <= 0.0f) mode_log("full-screen cut");
    if (cut > 0.0f) ++g_cutFrames;
    g_cutA = cut;
  }
  long seq = 0;
  ID3D11Texture2D *hud = akn_get_hud(&seq);
  if (!hud) { g_cutA = 0.0f; return; }
  if (seq == g_cutSeq) return;   // one probe per HUD frame
  g_cutSeq = seq;
  D3D11_TEXTURE2D_DESC hd{}; hud->GetDesc(&hd);
  const DXGI_FORMAT f = hd.Format;
  const bool fourByte = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                        f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
  if (!fourByte || hd.Width < 64 || hd.Height < 64 || hd.SampleDesc.Count != 1) { g_cutA = 0.0f; return; }
  if (g_cutFmt != f) {
    for (auto &t : g_cutStage) { if (t) t->Release(); t = nullptr; }
    for (bool &b : g_cutPend) b = false;
    D3D11_TEXTURE2D_DESC d{}; d.Width = kN; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &t : g_cutStage) if (FAILED(g_device->CreateTexture2D(&d, nullptr, &t))) t = nullptr;
    g_cutFmt = f;
  }
  const int i = g_cutI;
  if (g_cutPend[i] || !g_cutStage[i]) return;   // still waiting for the graphics card
  const float inX = hd.Width * 0.015f, inY = hd.Height * 0.015f;
  for (int n = 0; n < kN; ++n) {
    float fx, fy;
    if (n < kRing) {   // ring 0: 16 texels along each edge, 1.5 % in from it
      const int e = n / 16, k = n % 16;
      fx = (e < 2) ? (k + 0.5f) / 16.0f * hd.Width : (e == 2 ? inX : hd.Width - 1 - inX);
      fy = (e < 2) ? (e == 0 ? inY : hd.Height - 1 - inY) : (k + 0.5f) / 16.0f * hd.Height;
    } else {           // columns: left edge, centre, right edge; 32 rows top to bottom
      const int c = (n - kRing) / kRows, r = (n - kRing) % kRows;
      fx = c == 0 ? inX : c == 1 ? 0.5f * hd.Width : hd.Width - 1 - inX;
      fy = (r + 0.5f) / kRows * hd.Height;
    }
    const UINT x = (UINT)min(max(fx, 0.0f), (float)(hd.Width - 1)), y = (UINT)min(max(fy, 0.0f), (float)(hd.Height - 1));
    const D3D11_BOX bx{x, y, 0, x + 1, y + 1, 1};
    g_ctx->CopySubresourceRegion(g_cutStage[i], 0, (UINT)n, 0, 0, hud, 0, &bx);
  }
  g_cutPend[i] = true; g_cutI = (i + 1) % 3;
}
float hie_dim_alpha();   // below dim_layer
// SCANLINE (hud_in_eyes_body): the detective scan line drawn per eye - a solid, slightly see-through light grey like the game's
extern "C" int akvr_scan_anchor(float *out2);   // earlyres.cpp: the label's anchor on the HUD sheet
extern "C" int akvr_scan_box_drawn(float *out2);   // earlyres.cpp SCANOWN: the box's DRAWN point (our HUD-layer shrink in)
ID3D11PixelShader *g_scanLinePS = nullptr; int g_scanLineState = 0; float g_scanLineW = 1.3f; long g_scanLineDraws = 0, g_scanBoxDraws = 0;
int g_scanCut = 0; float g_scanCutR = 0.03f;   // SCANCUT: draw only this half-size (sheet units) around the box; export akvr_scan_cut
// SCANBOX 2026-10-13 (JJ: the second line "still there" after hide / transparency / cut; "just fix it"): the callout's box is drawn by
// AKVR itself - four plus signs at the four corner parts' drawn points, coloured from the game's own plus sign at that point (the
// marker sheet, read only there) - and the marker sheet is not shown, so nothing else the game routes there (its own line) can appear.
ID3D11PixelShader *g_scanPlusPS = nullptr; ID3D11Buffer *g_scanPlusCB = nullptr; int g_scanPlusState = 0, g_scanBox = 1; long g_scanPlusDraws = 0;
bool scanplus_build() {
  if (g_scanPlusState) return g_scanPlusState == 1;
  g_scanPlusState = -1;
  HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
  D3DCompileFn compile = m ? (D3DCompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
  if (!compile) return false;
  static const char ps[] =
      "Texture2D t : register(t0); SamplerState s : register(s0);"
      "cbuffer P : register(b0) { float4 cuv; float4 col; };"   // cuv.xy = the plus sign's centre (sheet uv), cuv.zw = its half size
      "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
      " float2 q = (uv - cuv.xy) / cuv.zw; const float th = 0.16;"
      " if (!((abs(q.x) < th && abs(q.y) < 1.0) || (abs(q.y) < th && abs(q.x) < 1.0))) discard;"
      " float4 c = t.SampleLevel(s, cuv.xy, 0); float3 rgb = c.a > 0.15 ? c.rgb / c.a : col.rgb;"   // the game's colour, else the fallback
      " return float4(rgb * col.a, col.a); }";
  ID3DBlob *pb = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(compile(ps, sizeof(ps) - 1, "scanplus", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err)) && pb;
  if (err) err->Release();
  ok = ok && SUCCEEDED(g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_scanPlusPS));
  if (pb) pb->Release();
  D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ok = ok && SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &g_scanPlusCB));
  g_scanPlusState = ok ? 1 : -1;
  mode_log(ok ? "scan box shader ready" : "scan box shader FAILED");
  return ok;
}
extern "C" int akvr_scan_corners(float *out8);   // earlyres.cpp SCANBOX
extern "C" long akvr_scan_plus_draws_c() { return g_scanPlusDraws; }
long g_aimHudFrames = 0, g_aimHudHead = 0;   // AIMHUD: frames the aim sheet was drawn / of them hung on the head
extern "C" long akvr_aim_hud_frames_c(long *headLocked) { if (headLocked) *headLocked = g_aimHudHead; return g_aimHudFrames; }
bool scanline_build() {
  if (g_scanLineState) return g_scanLineState == 1;
  g_scanLineState = -1;
  HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
  D3DCompileFn compile = m ? (D3DCompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
  if (!compile) return false;
  static const char ps[] = "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(0.62, 0.66, 0.68, 0.85); }";   // premultiplied
  ID3DBlob *pb = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(compile(ps, sizeof(ps) - 1, "scanline", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err)) && pb;
  if (err) err->Release();
  ok = ok && SUCCEEDED(g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_scanLinePS));
  if (pb) pb->Release();
  g_scanLineState = ok ? 1 : -1;
  mode_log(ok ? "scan line shader ready" : "scan line shader FAILED");
  return ok;
}
bool hie_build() {
  if (g_hieState) return g_hieState == 1;
  g_hieState = -1;
  HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
  D3DCompileFn compile = m ? (D3DCompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
  if (!compile) { snprintf(g_hieDiag, sizeof(g_hieDiag), "HUD in the eyes: no shader compiler"); return false; }
  static const char vs[] =
      "cbuffer K : register(b0) { float4 c[4]; };"
      "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
      "O main(uint id : SV_VertexID) { O o; o.p = c[id]; o.uv = float2(id & 1, id >> 1); return o; }";
  static const char ps[] =
      "Texture2D t : register(t0); SamplerState s : register(s0); Texture2D<float> cutT : register(t1);"
      "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { float4 c = t.Sample(s, uv); float k = cutT.Load(int3(0, 0, 0)); if (k > 0.0) c.a = saturate(1.0 - (1.0 - c.a) / max(1.0 - k, 1e-3)); return c; }";
  ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(compile(vs, sizeof(vs) - 1, "hievs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, &err));
  if (err) { err->Release(); err = nullptr; }
  ok = ok && SUCCEEDED(compile(ps, sizeof(ps) - 1, "hieps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err));
  if (err) err->Release();
  ok = ok && SUCCEEDED(g_device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_hieVS));
  ok = ok && SUCCEEDED(g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_hiePS));
  if (vb) vb->Release();
  if (pb) pb->Release();
  D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ok = ok && SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &g_hieCB));
  {   // MARKERCUT shaders
    static const char vs2[] =
        "cbuffer K : register(b0) { float4 c[4]; float4 uvr; };"
        "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
        "O main(uint id : SV_VertexID) { O o; o.p = c[id]; o.uv = uvr.xy + float2(id & 1, id >> 1) * uvr.zw; return o; }";
    static const char ps2[] =
        "Texture2D t : register(t0); SamplerState s : register(s0); Texture2D<float> cutT : register(t1);"
        "cbuffer M : register(b0) { float4 circ[8]; float4 mode; };"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
        " bool inside = false;"
        // MARKERRING 2026-10-09 (JJ: holes in the HUD where a marker's circle lay over other HUD - c031225 'INCOMPLETE'
        // panel): a circle is cut only when the ring just outside it is EMPTY (no other HUD around the marker); otherwise
        // the marker stays flat in the sheet this frame. Same test in the sheet pass and the card pass: they always agree.
        " [loop] for (int i = 0; i < 8; ++i) { if (i < (int)mode.x) { float2 d = (uv - circ[i].xy) / circ[i].zw; if (dot(d, d) <= 1.0) {"
        "   float busy = 0.0;"
        "   [loop] for (int k = 0; k < 24; ++k) { float a = k * 0.2617994; float2 q = float2(cos(a), sin(a)) * circ[i].zw;"
        "     busy = max(busy, t.SampleLevel(s, circ[i].xy + q * 1.2, 0).a); busy = max(busy, t.SampleLevel(s, circ[i].xy + q * 1.5, 0).a); }"
        "   if (busy < 0.04 || mode.z < 0.5) inside = true; } } }"
        " if (mode.y > 0.5 ? !inside : inside) discard;"
        " float4 c = t.Sample(s, uv); float k = cutT.Load(int3(0, 0, 0)); if (k > 0.0) c.a = saturate(1.0 - (1.0 - c.a) / max(1.0 - k, 1e-3)); return c; }";
    ID3DBlob *b1 = nullptr, *b2 = nullptr, *e1 = nullptr;
    bool k = SUCCEEDED(compile(vs2, sizeof(vs2) - 1, "mcvs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &b1, &e1));
    if (e1) { e1->Release(); e1 = nullptr; }
    k = k && SUCCEEDED(compile(ps2, sizeof(ps2) - 1, "mcps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b2, &e1));
    if (e1) e1->Release();
    k = k && SUCCEEDED(g_device->CreateVertexShader(b1->GetBufferPointer(), b1->GetBufferSize(), nullptr, &g_mcVS));
    k = k && SUCCEEDED(g_device->CreatePixelShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, &g_mcPS));
    if (b1) b1->Release();
    if (b2) b2->Release();
    D3D11_BUFFER_DESC b{}; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    b.ByteWidth = 80; k = k && SUCCEEDED(g_device->CreateBuffer(&b, nullptr, &g_mcVCB));
    b.ByteWidth = 144; k = k && SUCCEEDED(g_device->CreateBuffer(&b, nullptr, &g_mcPCB));
    if (!k) { g_mcVS = nullptr; g_mcPS = nullptr; }   // no cut: the plain sheet as before
  }
  {   // BLACKKEY: the sheet with its pure-black see-through texels removed
    static const char ps5[] =
        "Texture2D t : register(t0); SamplerState s : register(s0); Texture2D<float> cutT : register(t1);"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
        " float4 c = t.Sample(s, uv); c.a = min(c.a, max(c.r, max(c.g, c.b)) * 4.0); float k = cutT.Load(int3(0, 0, 0)); if (k > 0.0) c.a = saturate(1.0 - (1.0 - c.a) / max(1.0 - k, 1e-3)); return c; }";   // BLACKKEY2: soft (JJ: harsh mask edges on the wheels)
    ID3DBlob *b5 = nullptr, *e5 = nullptr;
    bool k = SUCCEEDED(compile(ps5, sizeof(ps5) - 1, "hiekey", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b5, &e5));
    if (e5) e5->Release();
    k = k && SUCCEEDED(g_device->CreatePixelShader(b5->GetBufferPointer(), b5->GetBufferSize(), nullptr, &g_hieKeyPS));
    if (b5) b5->Release();
    if (!k) g_hieKeyPS = nullptr;
  }
  {   // BACKEXT: the sheet outside its own area, edge texels carried on (clamp sampler); inside the area nothing
    static const char ps4[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
        " if (all(uv >= 0.0) && all(uv <= 1.0)) discard; return t.Sample(s, saturate(uv)); }";
    ID3DBlob *b4 = nullptr, *e4 = nullptr;
    bool k = SUCCEEDED(compile(ps4, sizeof(ps4) - 1, "hieext", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b4, &e4));
    if (e4) e4->Release();
    k = k && SUCCEEDED(g_device->CreatePixelShader(b4->GetBufferPointer(), b4->GetBufferSize(), nullptr, &g_hieExtPS));
    if (b4) b4->Release();
    if (!k) g_hieExtPS = nullptr;
  }
  {   // WHEELEYE: plain colour (the darkening)
    static const char ps3[] = "cbuffer K : register(b0) { float4 col; }; Texture2D<float> cutT : register(t1);"
        "float4 main(float4 p : SV_Position) : SV_Target { float k = cutT.Load(int3(0, 0, 0)); float a = 1.0 - (1.0 - col.a) * (1.0 - k);"
        " if (a <= 0.0) discard; return float4(col.rgb, a); }";   // PERF1: no writes while nothing is darkened
    ID3DBlob *b3 = nullptr, *e3 = nullptr;
    bool k = SUCCEEDED(compile(ps3, sizeof(ps3) - 1, "hiedim", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b3, &e3));
    if (e3) e3->Release();
    k = k && SUCCEEDED(g_device->CreatePixelShader(b3->GetBufferPointer(), b3->GetBufferSize(), nullptr, &g_hieDimPS));
    if (b3) b3->Release();
    D3D11_BUFFER_DESC b{}; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER; b.ByteWidth = 16;
    k = k && SUCCEEDED(g_device->CreateBuffer(&b, nullptr, &g_hieDimCB));
    if (!k) g_hieDimPS = nullptr;   // no in-eye darkening: the dim layer stays
  }
  {   // FULLCUT5: one pixel = this frame's cut, from the sheet (same rules as cut_probe's log)
    static const char ps6[] =
        "Texture2D t : register(t0); cbuffer F : register(b0) { float4 f; };"   // f.x pause look, f.y on
        "float lum(float4 c) { return max(c.r, max(c.g, c.b)); }"
        "float4 main(float4 p : SV_Position) : SV_Target {"
        " uint W, H; t.GetDimensions(W, H); float inX = W * 0.015, inY = H * 0.015;"
        " float aMin = 1, aMax = 0, aSum = 0, rgb = 0;"
        " [loop] for (int n = 0; n < 64; ++n) { int e = n / 16, k = n % 16;"
        "  float fx = e < 2 ? (k + 0.5) / 16.0 * W : (e == 2 ? inX : W - 1 - inX);"
        "  float fy = e < 2 ? (e == 0 ? inY : H - 1 - inY) : (k + 0.5) / 16.0 * H;"
        "  float4 c = t.Load(int3(fx, fy, 0)); aMin = min(aMin, c.a); aMax = max(aMax, c.a); aSum += c.a; rgb = max(rgb, lum(c)); }"
        " float ring = (aMin > 0.02 && aMax - aMin < 0.08 && rgb < 0.10) ? aSum / 64.0 : 0.0;"
        " int cov = 0; float rMin = 1, rMax = 0, rSum = 0; bool mid = false;"
        " [loop] for (int r = 0; r < 32; ++r) { float y = (r + 0.5) / 32.0 * H;"
        "  float4 c0 = t.Load(int3(inX, y, 0)), c1 = t.Load(int3(W * 0.5, y, 0)), c2 = t.Load(int3(W - 1 - inX, y, 0));"
        "  float lo = min(c0.a, min(c1.a, c2.a)), hi = max(c0.a, max(c1.a, c2.a));"
        "  if (lo > 0.02 && hi - lo < 0.08 && max(lum(c0), max(lum(c1), lum(c2))) < 0.10) {"
        "   ++cov; float m = (c0.a + c1.a + c2.a) / 3.0; rMin = min(rMin, m); rMax = max(rMax, m); rSum += m; if (r == 15 || r == 16) mid = true; } }"
        " float box = (mid && cov >= 8 && rMax - rMin < 0.10) ? rSum / cov : 0.0;"
        " float cut = max(ring, box); if (f.x > 0.5 && cut < 0.9) cut = 0.0; if (f.y < 0.5) cut = 0.0;"
        " return cut.xxxx; }";
    ID3DBlob *b6 = nullptr, *e6 = nullptr;
    bool k = SUCCEEDED(compile(ps6, sizeof(ps6) - 1, "hiecut", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b6, &e6));
    if (e6) e6->Release();
    k = k && SUCCEEDED(g_device->CreatePixelShader(b6->GetBufferPointer(), b6->GetBufferSize(), nullptr, &g_cutPS));
    if (b6) b6->Release();
    D3D11_TEXTURE2D_DESC td{}; td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    k = k && SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &g_cutTex));
    k = k && SUCCEEDED(g_device->CreateRenderTargetView(g_cutTex, nullptr, &g_cutRTV));
    k = k && SUCCEEDED(g_device->CreateShaderResourceView(g_cutTex, nullptr, &g_cutSRV));
    D3D11_BUFFER_DESC b{}; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER; b.ByteWidth = 16;
    k = k && SUCCEEDED(g_device->CreateBuffer(&b, nullptr, &g_cutCB));
    if (!k) g_cutPS = nullptr;   // no same-frame cut: the sheet shaders read 0 (unbound)
  }
  D3D11_BLEND_DESC bl{}; auto &t = bl.RenderTarget[0];
  t.BlendEnable = TRUE; t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOp = D3D11_BLEND_OP_ADD;
  t.SrcBlendAlpha = D3D11_BLEND_ZERO; t.DestBlendAlpha = D3D11_BLEND_ONE; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
  ok = ok && SUCCEEDED(g_device->CreateBlendState(&bl, &g_hieBS));
  D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ok = ok && SUCCEEDED(g_device->CreateSamplerState(&sd, &g_hieSS));
  D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
  ok = ok && SUCCEEDED(g_device->CreateRasterizerState(&rd, &g_hieRS));
  D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
  ok = ok && SUCCEEDED(g_device->CreateDepthStencilState(&dd, &g_hieDS));
  if (!ok) { snprintf(g_hieDiag, sizeof(g_hieDiag), "HUD in the eyes: set-up FAILED"); return false; }
  g_hieState = 1;
  return true;
}
XrQuaternionf hie_conj(const XrQuaternionf &q) { return XrQuaternionf{-q.x, -q.y, -q.z, q.w}; }
// MIRRORHUD 2026-10-09 (JJ: "I can't see HUD elements when you have the fake headset running. HUD elements don't show up
// on the desktop"): the desktop window shows the DLSS left eye (DLSSMIRROR) and the HUD went to AKN's sheet only. The
// sheet is laid over the whole window with the game's own blend (as in the flat game). Runs inside the mirror's own
// device-context state (everything set here is thrown away with it).
int g_mirrorHud = 1; long g_mhCalls = 0, g_mhDraws = 0, g_mhNoHud = 0, g_mhNoSrv = 0, g_mhNoBuild = 0;
extern "C" __declspec(dllexport) const char* akvr_mirror_diag() {   // MIRRORHUD diag (AKN command "akvrdiag")
  static char b[160]; snprintf(b, sizeof(b), "mirror HUD: calls %ld draws %ld | no sheet %ld no view %ld not built %ld", g_mhCalls, g_mhDraws, g_mhNoHud, g_mhNoSrv, g_mhNoBuild); return b; }
void mirror_hud(ID3D11DeviceContext *c, ID3D11RenderTargetView *rtv, UINT w, UINT h) {
  ++g_mhCalls;
  if (!g_mirrorHud || !c || !rtv) return;
  if (!hie_build()) { ++g_mhNoBuild; return; }
  long seq = 0;
  ID3D11Texture2D *hud = akn_get_hud(&seq);
  if (!hud) { ++g_mhNoHud; return; }
  if (g_hieSrc != hud) {
    if (g_hieSRV) { g_hieSRV->Release(); g_hieSRV = nullptr; }
    g_hieSrc = hud;
    D3D11_TEXTURE2D_DESC hd{}; hud->GetDesc(&hd);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = hd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : hd.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(hud, &vd, &g_hieSRV))) g_hieSRV = nullptr;
  }
  if (!g_hieSRV) { ++g_mhNoSrv; return; }
  ++g_mhDraws;
  const float full[16] = {-1, 1, 0.5f, 1,  1, 1, 0.5f, 1,  -1, -1, 0.5f, 1,  1, -1, 0.5f, 1};
  c->UpdateSubresource(g_hieCB, 0, nullptr, full, 0, 0);
  c->OMSetRenderTargets(1, &rtv, nullptr);
  c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->VSSetShader(g_hieVS, nullptr, 0); c->VSSetConstantBuffers(0, 1, &g_hieCB);
  c->PSSetShader(g_hiePS, nullptr, 0); c->PSSetShaderResources(0, 1, &g_hieSRV); c->PSSetSamplers(0, 1, &g_hieSS);
  const FLOAT bf[4] = {0, 0, 0, 0}; c->OMSetBlendState(g_hieBS, bf, 0xffffffff);
  c->OMSetDepthStencilState(g_hieDS, 0); c->RSSetState(g_hieRS);
  const D3D11_VIEWPORT vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
  c->RSSetViewports(1, &vp);
  c->Draw(4, 0);
}
// TURNEDEYE (lens mode 2): the centred lens AKN drew for this eye and the turn of its camera from the head (toward the
// centre of the eye's field). (Mode 3 "same way" removed in CLEANTESTS 2026-10-09: the suit gloss is fixed by SUITGLOSS3.)
bool akn_eye_lens(const XrFovf &rf, XrFovf &fov, XrQuaternionf &turn) {
  if (g_aknLensMode != 2) return false;
  const float yc = 0.5f * (rf.angleLeft + rf.angleRight), pc = 0.5f * (rf.angleUp + rf.angleDown);
  const float hH = 0.5f * (rf.angleRight - rf.angleLeft), hV = 0.5f * (rf.angleUp - rf.angleDown);
  fov = XrFovf{-hH, hH, hV, -hV};
  const XrQuaternionf qy{0.0f, sinf(-yc * 0.5f), 0.0f, cosf(-yc * 0.5f)};   // yaw toward the field centre (right = -yaw)
  const XrQuaternionf qp{sinf(pc * 0.5f), 0.0f, 0.0f, cosf(pc * 0.5f)};      // then pitch (up = +X rotation)
  turn = hud_qmul(qy, qp);
  return true;
}
// this eye's view exactly as the submitted projection view describes it (see the views loop: lens mode, turn, offset)
void hie_eye_view(int e, const XrPosef &headPose, XrPosef &pose, XrFovf &fov) {
  fov = g_rawFov[e];
  XrQuaternionf turn{0.0f, 0.0f, 0.0f, 1.0f};
  akn_eye_lens(g_rawFov[e], fov, turn);
  const float hx = menu_one_spot() ? 0.0f : (e == 0 ? -0.5f : 0.5f) * akn_ipd_m();
  const XrVector3f r = quat_rotate(headPose.orientation, XrVector3f{hx, 0.0f, 0.0f});
  pose = headPose;
  pose.position.x += r.x; pose.position.y += r.y; pose.position.z += r.z;
  pose.orientation = hud_qmul(headPose.orientation, turn);
}
// SVRSTRAIGHT 2026-10-09 - JJ in SteamVR: with turned eye cameras (TURNEDEYE, lens mode 2) the eyes were "too far apart"
// to fuse; "same way" (mode 3) fused. SteamVR's OpenXR mishandles projection views turned DIFFERENT ways per eye. JJ
// chose the proper fix over mode 3 (+25 % pixels): keep rendering turned eyes, and turn each eye's finished picture back
// into the runtime's own straight, off-centre lens here (a pure rotation: exact), then declare straight views.
// svrstraight: 0 off, 1 auto (SteamVR only, default), 2 always (fake-headset test). Live: AKN command "svrstraight N".
int g_svrStraight = 1;
bool g_straightNow = false;     // this frame's picture was straightened: the views loop declares straight eyes
long g_straightFrames = 0;
ID3D11Texture2D *g_aknStraight = nullptr; ID3D11RenderTargetView *g_stRTV = nullptr;
ID3D11PixelShader *g_stPS = nullptr; ID3D11Buffer *g_stCB = nullptr; int g_stOk = 0;
bool straight_wanted() {
  if (g_svrStraight == 0 || g_aknLensMode != 2) return false;
  return g_svrStraight == 2 || std::strstr(g_runtime, "SteamVR") != nullptr;
}
bool straight_build() {
  if (g_stOk) return g_stOk == 1;
  g_stOk = -1;
  if (!stretch_build()) return false;
  HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
  StrCompileFn compile = m ? (StrCompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
  if (!compile) return false;
  // uv = this eye's output pixel; out = its direction through the straight lens (tangents L R U D); c0..c2 = columns of
  // the inverse turn (head frame -> turned eye camera); src = the turned camera's symmetric lens (half tangents) and
  // which half of the side-by-side source this eye is
  static const char ps[] =
      "Texture2D t : register(t0); SamplerState s : register(s0);"
      "cbuffer C : register(b0) { float4 lrud; float4 c0; float4 c1; float4 c2; float4 src; };"
      "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
      " float3 d = float3(lerp(lrud.x, lrud.y, uv.x), lerp(lrud.z, lrud.w, uv.y), -1.0);"
      " float3 e = c0.xyz * d.x + c1.xyz * d.y + c2.xyz * d.z;"
      " if (e.z > -1e-4) return float4(0, 0, 0, 1);"
      " float2 tn = float2(e.x, e.y) / -e.z;"
      " float2 su = float2(0.5 + 0.5 * tn.x / src.x, 0.5 - 0.5 * tn.y / src.y);"
      " if (any(su < 0.0) || any(su > 1.0)) return float4(0, 0, 0, 1);"
      " return t.SampleLevel(s, float2((src.z + su.x) * 0.5, su.y), 0); }";
  ID3DBlob *pb = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(compile(ps, sizeof(ps) - 1, "stps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err)) && pb;
  if (err) err->Release();
  ok = ok && SUCCEEDED(g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_stPS));
  if (pb) pb->Release();
  D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ok = ok && SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &g_stCB));
  g_stOk = ok ? 1 : -1;
  if (!ok) mode_log("straighten: shader failed");
  return ok;
}
// turn both eyes of g_aknSbs back to straight eyes into g_aknStraight; true = use g_aknStraight this frame
bool straighten() {
  if (!g_aknSbs || !g_rawFovOk || !straight_build()) return false;
  D3D11_TEXTURE2D_DESC sd{}; g_aknSbs->GetDesc(&sd);
  if (g_aknStraight) { D3D11_TEXTURE2D_DESC td{}; g_aknStraight->GetDesc(&td); if (td.Width != sd.Width || td.Height != sd.Height || td.Format != sd.Format) { if (g_stRTV) g_stRTV->Release(); g_stRTV = nullptr; g_aknStraight->Release(); g_aknStraight = nullptr; } }
  if (!g_aknStraight) {
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &g_aknStraight))) { g_aknStraight = nullptr; return false; }
    if (FAILED(g_device->CreateRenderTargetView(g_aknStraight, nullptr, &g_stRTV))) { g_stRTV = nullptr; return false; }
  }
  if (!g_sbsSRV && FAILED(g_device->CreateShaderResourceView(g_aknSbs, nullptr, &g_sbsSRV))) return false;
  ID3D11DeviceContext1 *c1 = nullptr; ID3DDeviceContextState *prev = nullptr;
  if (FAILED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&c1)) || !c1) return false;
  c1->SwapDeviceContextState(g_strState, &prev);
  // STRBLEND 2026-10-09: g_strState is shared and KEEPS what was set in it - MIRRORHUD leaves the HUD blend
  // (ONE / INV_SRC_ALPHA) set, so every later draw in this state blended onto the old picture: the SVRSTRAIGHT output
  // piled up frame on frame (JJ: "blown out to white ... blurring when moving your head"). Opaque, no depth, default raster.
  { const FLOAT bf0[4] = {0, 0, 0, 0}; c1->OMSetBlendState(nullptr, bf0, 0xffffffff); c1->OMSetDepthStencilState(nullptr, 0); c1->RSSetState(nullptr); }
  c1->OMSetRenderTargets(1, &g_stRTV, nullptr);
  c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c1->VSSetShader(g_strVS, nullptr, 0); c1->PSSetShader(g_stPS, nullptr, 0);
  c1->PSSetShaderResources(0, 1, &g_sbsSRV); c1->PSSetSamplers(0, 1, &g_strSamp); c1->PSSetConstantBuffers(0, 1, &g_stCB);
  const float halfW = 0.5f * (float)sd.Width;
  for (int e = 0; e < 2; ++e) {
    const XrFovf &rf = g_rawFov[e];
    XrFovf tf = rf; XrQuaternionf turn{0.0f, 0.0f, 0.0f, 1.0f};
    akn_eye_lens(rf, tf, turn);   // the lens + turn this eye was drawn with (same call as the views loop)
    const XrQuaternionf inv{-turn.x, -turn.y, -turn.z, turn.w};
    const XrVector3f a = quat_rotate(inv, XrVector3f{1, 0, 0}), b = quat_rotate(inv, XrVector3f{0, 1, 0}), c = quat_rotate(inv, XrVector3f{0, 0, 1});
    const float cb[20] = { tanf(rf.angleLeft), tanf(rf.angleRight), tanf(rf.angleUp), tanf(rf.angleDown),
                           a.x, a.y, a.z, 0, b.x, b.y, b.z, 0, c.x, c.y, c.z, 0,
                           tanf(tf.angleRight), tanf(tf.angleUp), (float)e, 0 };
    c1->UpdateSubresource(g_stCB, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp{ e * halfW, 0.0f, halfW, (float)sd.Height, 0.0f, 1.0f };
    c1->RSSetViewports(1, &vp);
    c1->Draw(3, 0);
  }
  ID3D11ShaderResourceView *none = nullptr; c1->PSSetShaderResources(0, 1, &none);
  ID3D11RenderTargetView *nrt = nullptr; c1->OMSetRenderTargets(1, &nrt, nullptr);
  if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); }
  c1->Release();
  if (!g_straightFrames++) mode_log("straighten on");
  return true;
}
// draw the HUD sheet into both halves of the side-by-side picture; headPose = the pose the picture was drawn with
// PERFTIME: per-frame processor time of the VR submit and of the HUD step (reported by pt_report)
LONGLONG g_ptFreq = 0; double g_ptSubmit = 0, g_ptHie = 0, g_ptHieA = 0, g_ptHieB = 0; long g_ptN = 0, g_ptHieN = 0;
char g_ptDiag[200] = "VR frame time: not measured yet";
inline LONGLONG pt_now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
inline double pt_ms(LONGLONG a, LONGLONG b) { if (!g_ptFreq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ptFreq = f.QuadPart; } return (b - a) * 1000.0 / (double)g_ptFreq; }
bool hud_in_eyes_body(ID3D11Texture2D *sbs, const XrPosef &headPose);
LONGLONG g_ptHieMark = 0;   // PERFTIME: where the marker part ends
bool hud_in_eyes(ID3D11Texture2D *sbs, const XrPosef &headPose) {
  const LONGLONG a = pt_now(); g_ptHieMark = 0;
  const bool r = hud_in_eyes_body(sbs, headPose);
  const LONGLONG b = pt_now();
  g_ptHie += pt_ms(a, b); if (g_ptHieMark) { g_ptHieA += pt_ms(a, g_ptHieMark); g_ptHieB += pt_ms(g_ptHieMark, b); } ++g_ptHieN;
  return r;
}
bool hud_in_eyes_body(ID3D11Texture2D *sbs, const XrPosef &headPose) {
  g_hieDimDone = false;
  if (g_cutLog) cut_probe();   // FULLCUT (PERF1: log only)
  if (!g_hudLastQOk || !g_rawFovOk || !hie_build()) { ++g_hieSkips; return false; }
  long seq = 0;
  ID3D11Texture2D *hud = akn_get_hud(&seq);
  if (!hud) { ++g_hieSkips; return false; }
  if (g_hieSrc != hud) {
    if (g_hieSRV) { g_hieSRV->Release(); g_hieSRV = nullptr; }
    g_hieSrc = hud;
    D3D11_TEXTURE2D_DESC hd{}; hud->GetDesc(&hd);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = hd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : hd.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(hud, &vd, &g_hieSRV))) { g_hieSRV = nullptr; snprintf(g_hieDiag, sizeof(g_hieDiag), "HUD in the eyes: HUD picture cannot be read"); }
  }
  if (g_hieDst != sbs) {
    if (g_hieRTV) { g_hieRTV->Release(); g_hieRTV = nullptr; }
    g_hieDst = sbs;
    if (FAILED(g_device->CreateRenderTargetView(sbs, nullptr, &g_hieRTV))) g_hieRTV = nullptr;
  }
  if (!g_hieSRV || !g_hieRTV) { ++g_hieSkips; return false; }
  D3D11_TEXTURE2D_DESC sd{}; sbs->GetDesc(&sd);
  const float halfW = (float)(sd.Width / 2);
  // the quad in LOCAL space (a VIEW-space quad hangs off the head pose the picture was drawn with)
  XrPosef qp = g_hudLastQ.pose;
  if (g_hudLastQ.space == g_viewSpace) {
    const XrVector3f o = quat_rotate(headPose.orientation, qp.position);
    qp.position = XrVector3f{headPose.position.x + o.x, headPose.position.y + o.y, headPose.position.z + o.z};
    qp.orientation = hud_qmul(headPose.orientation, qp.orientation);
  }
  float hw = 0.5f * g_hudLastQ.size.width, hh = 0.5f * g_hudLastQ.size.height;
  const bool trFull = g_trFullNow;   // TRANSFULL: a plane in front of the head, past the edges of the view
  if (trFull) {
    const XrVector3f f = quat_rotate(headPose.orientation, XrVector3f{0.0f, 0.0f, -g_trDist});
    qp.orientation = headPose.orientation;
    qp.position = XrVector3f{headPose.position.x + f.x, headPose.position.y + f.y, headPose.position.z + f.z};
    D3D11_TEXTURE2D_DESC hd{}; hud->GetDesc(&hd);
    hw = g_trDist * 1.2f * g_trZoom; hh = hw * (float)hd.Height / (float)(hd.Width ? hd.Width : 1);
    ++c_trFrames;
  }
  const XrVector3f R = quat_rotate(qp.orientation, XrVector3f{hw, 0, 0}), U = quat_rotate(qp.orientation, XrVector3f{0, hh, 0});
  const XrVector3f C = qp.position;
  const XrVector3f corner[4] = { {C.x - R.x + U.x, C.y - R.y + U.y, C.z - R.z + U.z},     // top left  (uv 0,0)
                                 {C.x + R.x + U.x, C.y + R.y + U.y, C.z + R.z + U.z},     // top right (uv 1,0)
                                 {C.x - R.x - U.x, C.y - R.y - U.y, C.z - R.z - U.z},     // bottom left
                                 {C.x + R.x - U.x, C.y + R.y - U.y, C.z + R.z - U.z} };   // bottom right
  // MARKERCUT 2026-10-08 (JJ: the distance marker / reticles not on their objects, the distance marker moves against
  // the head; rock solid under geo-11). geo-11's way (TARGETANCHOR points + depth) and Arkham City's (Canvas.Project
  // world point, per-eye placement) on pixels: each marker's HUD area is cut out of the sheet and drawn into each eye
  // where that eye sees the marker's world point - per-eye depth, no head term (VR_WORLD_MARKERS_GUIDE 3-5).
  struct Cut { float hx, hy; float p[2][2]; float q[2][4][3]; bool world; };   // q: MARKERWORLD corners TL TR BL BR per eye (NDC x, y + clip w: CARDPERSP)
  Cut cuts[8]; int nCuts = 0;
  {
    D3D11_TEXTURE2D_DESC hd{}; hud->GetDesc(&hd);
    float tp[16] = {}, toff[8] = {}, txf[4] = {};
    g_hieNPts = akvr_hud_target_points(tp, 8, (int)hd.Width, (int)hd.Height, toff, txf);
    for (int i = 0; i < 16; ++i) g_hiePts[i] = tp[i];
    for (int i = 0; i < 4; ++i) g_hieXf[i] = txf[i];
    if (txf[0] != 0.0f || txf[1] != 0.0f) { for (int i = 0; i < 4; ++i) g_hieXfLast[i] = txf[i]; g_hieXfOk = true; }   // MKEYE
    if (!p_aknMarkers) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (m) { p_aknMarkers = (AknMarkersFn)GetProcAddress(m, "akn_markers"); p_aknPairVp = (AknPairVpFn)GetProcAddress(m, "akn_pair_vp"); } }
    {   using EyeModeFn = int (*)(); static EyeModeFn p_eyeMode = nullptr; static bool s_emTried = false;   // MKEYE
        if (!s_emTried) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (m) { p_eyeMode = (EyeModeFn)GetProcAddress(m, "akn_marker_eye_mode"); s_emTried = true; } }
        g_hieEyeMode = p_eyeMode && p_eyeMode() != 0; }
    float mk[16 * 5]; float vp[32];
    const int nm = (!g_hieEyeMode && g_markerCut && g_mcVS && p_aknMarkers) ? p_aknMarkers(mk, 16) : 0;   // MKEYE: no cards
    const bool vpOk = nm > 0 && p_aknPairVp && p_aknPairVp(vp);
    // the cut centres: the HUD's on-target points (geo-11)
    float cx[16], cy[16]; int nc = 0;
    // only where the HUD tree says a marker IS drawn (MARKERMATCH: the old fallback to bare projected points cut circles
    // out of whatever HUD lay there - hidden markers project too)
    // MKDRAWN 2026-10-11 (JJ: the distance marker "jitters, like tiny little jitters"): the HUD-tree points race the game
    // thread (at Present the next frame's layout can already be in place), so the card's window sat up to a frame off the
    // drawn marker. AKN reports where this HUD frame's marker pieces were really drawn (bounding-box centre); the point is
    // that drawn centre plus the anchor's offset from it, averaged (the race averages out, the layout offset stays).
    using DrawnFn = int (*)(float *, int); static DrawnFn p_drawn = nullptr; static bool s_drawnTried = false;
    if (!s_drawnTried) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (m) { p_drawn = (DrawnFn)GetProcAddress(m, "akn_marker_drawn"); s_drawnTried = true; } }
    // MKDRAWN OFF 2026-10-11 (JJ after it: "looks even worse now ... jittering even more, and also detaching from its
    // position"): the drawn pieces differ frame to frame (text marked ~1 frame in 7, glow pastes, border), so their
    // bounding-box centre jumps - and it moved the HUD window cut for the card, not just the match point. Kept for study.
    constexpr bool kMkDrawnOn = false;
    float dr[8 * 5]; const int nd = (kMkDrawnOn && p_drawn) ? p_drawn(dr, 8) : 0;
    static float s_mkOff[8][2] = {}; static bool s_mkOffOk[8] = {};
    for (int j = 0; j < 8; ++j) if (j >= nd || dr[j * 5 + 4] <= 0.0f) s_mkOffOk[j] = false;
    for (int i = 0; i < g_hieNPts && nc < 16; ++i) {
      float px = tp[i * 2], py = tp[i * 2 + 1];
      int bj = -1; float bd2 = 0.12f * 0.12f;
      for (int j = 0; j < nd; ++j) {
        if (dr[j * 5 + 4] <= 0.0f) continue;
        const float dx = dr[j * 5] - px, dy = dr[j * 5 + 1] - py;
        if (dx * dx + dy * dy < bd2) { bd2 = dx * dx + dy * dy; bj = j; }
      }
      if (bj >= 0) {
        const float gx = dr[bj * 5 + 2], gy = dr[bj * 5 + 3], ox = px - gx, oy = py - gy;
        if (!s_mkOffOk[bj]) { s_mkOff[bj][0] = ox; s_mkOff[bj][1] = oy; s_mkOffOk[bj] = true; }
        else { s_mkOff[bj][0] += (ox - s_mkOff[bj][0]) * 0.05f; s_mkOff[bj][1] += (oy - s_mkOff[bj][1]) * 0.05f; }
        px = gx + s_mkOff[bj][0]; py = gy + s_mkOff[bj][1];
      }
      cx[nc] = px; cy[nc] = py; ++nc;
    }
    // MARKERMATCH 2026-10-08 (JJ: "the compass is broken: chunks taken out of it, some elements stuck to world space
    // objects"): compass icons move with the view, so they count as on-target points, and the old 0.25 match in SHEET
    // units (the sheet is the game's layout shrunk to the HUD box) paired them with the objective's projected point -
    // a circle cut out of the compass and drawn at the objective. Compare in the game's own screen units (the layout map
    // xf, as TARGETDIST does) and only close by: a real marker sits ON its projected point (MARKERBASE answers that
    // point from the same head-free camera the game lays the HUD out with).
    const float kMatch = 0.06f;
    float bF[3], bR[3], bU[3], fF[3], fR[3], fU[3];   // MARKERWORLD: the head-free game camera's axes (world)
    bool axOk = akvr_camera_axes_ago(0, bF, bR, bU, fF, fR, fU);
    {   // PAIRAXES 2026-10-11 (JJ: marker jitter = a head-pose / camera delay unlike the world's?): the card's shape from the
        // head-free camera the SHOWN pair was built with (AKN), not the newest one - a frame apart while the camera turns
        using AxFn = int (*)(float *); static AxFn p_ax = nullptr; static bool s_axTried = false;
        if (!s_axTried) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (m) { p_ax = (AxFn)GetProcAddress(m, "akn_pair_axes"); s_axTried = true; } }
        float a9[9];
        if (p_ax && p_ax(a9)) { for (int k = 0; k < 3; ++k) { bF[k] = a9[k]; bR[k] = a9[3 + k]; bU[k] = a9[6 + k]; } axOk = true; }
    }
    auto projP = [&](const float *M, const float P[3], float &ox, float &oy) {
      const float X = P[0] * M[0] + P[1] * M[4] + P[2] * M[8] + M[12], Y = P[0] * M[1] + P[1] * M[5] + P[2] * M[9] + M[13];
      const float Wc = P[0] * M[3] + P[1] * M[7] + P[2] * M[11] + M[15];
      if (Wc <= 0.0001f) return false;
      ox = X / Wc; oy = Y / Wc; return true;
    };
    // MKCARRY 2026-10-11 (JJ: "the grappling hook reticle pop out randomly also just for single frames", earlier "the text still
    // pops out for random frames"): a frame whose HUD-tree read finds no marker point (the game thread mid-update) drew no
    // cards, so that frame's marker pieces showed flat or not at all. A marker matched by the tree within the last 250 ms is
    // carried through such frames from this pair's own projected point (MKSAMEFRAME slot: world point + anchor offset).
    struct SfSlot { float w[3], o[2], s[2]; ULONGLONG t, tt; unsigned f; bool ok; };
    static SfSlot s_sf[8] = {};
    static unsigned s_sfFrame = 0; ++s_sfFrame;
    const ULONGLONG sfNow = GetTickCount64();
    const bool sfMap = txf[0] != 0.0f || txf[1] != 0.0f;
    for (int i = 0; i < nc + 8 && nCuts < 8 && vpOk; ++i) {
      int best = -1, xs = -1; float hx0 = 0.0f, hy0 = 0.0f;
      if (i < nc) {
        float gx = cx[i], gy = cy[i];
        if (sfMap) { gx += gx * txf[0] + txf[2]; gy += gy * txf[1] + txf[3]; }
        float bd = kMatch * kMatch;
        for (int j = 0; j < nm; ++j) { const float dx = mk[j * 5 + 3] - gx, dy = mk[j * 5 + 4] - gy; if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = j; } }
        if (best < 0) { ++g_mcNoWorld; continue; }
        hx0 = cx[i]; hy0 = cy[i];
      } else {   // MKCARRY: a slot the tree did not match this frame
        const int k = i - nc; SfSlot &n = s_sf[k];
        if (!g_mkSameFrame || !g_mkCarryOn || !n.ok || n.f == s_sfFrame || sfNow - n.tt > 250) continue;
        float bd = 60.0f * 60.0f;
        for (int j = 0; j < nm; ++j) { const float dx = mk[j * 5] - n.w[0], dy = mk[j * 5 + 1] - n.w[1], dz = mk[j * 5 + 2] - n.w[2]; if (dx * dx + dy * dy + dz * dz < bd) { bd = dx * dx + dy * dy + dz * dz; best = j; } }
        if (best < 0) continue;
        xs = k;
      }
      Cut &c = cuts[nCuts]; c.hx = hx0; c.hy = hy0; bool ok = true;
      if (g_mkSameFrame) {   // MKSAMEFRAME 2026-10-11 (JJ: "Still jitters a little bit ... the RB icon doing it ... also")
        // The card's HUD window was centred on the HUD-tree point, read at Present while the game thread can already have
        // laid out the NEXT frame - the marker pixels slid inside the card by one frame of screen motion. Centre it on this
        // pair's own Canvas.Project point (same frame as the HUD sheet, PAIRPTS) plus the layout's anchor offset (tree
        // point - projected point), learnt only while the projected point is still on screen (then both frames agree).
        const float *W = mk + best * 5;
        float sx = mk[best * 5 + 3], sy = mk[best * 5 + 4];
        if (sfMap) { sx = (sx - txf[2]) / (1.0f + txf[0]); sy = (sy - txf[3]) / (1.0f + txf[1]); }
        if (xs >= 0) {   // MKCARRY: no tree point this frame - the slot's learnt offset only
          SfSlot &n = s_sf[xs];
          n.s[0] = sx; n.s[1] = sy; n.w[0] = W[0]; n.w[1] = W[1]; n.w[2] = W[2]; n.t = sfNow; n.f = s_sfFrame;
          c.hx = sx + n.o[0]; c.hy = sy + n.o[1]; ++g_mkCarry;
        } else {
          int sl = -1, fr = -1;
          for (int k = 0; k < 8; ++k) {
            if (!s_sf[k].ok || sfNow - s_sf[k].t > 1000) { if (fr < 0) fr = k; continue; }
            if (s_sf[k].f == s_sfFrame) continue;   // already placed this frame
            const float dx = s_sf[k].w[0] - W[0], dy = s_sf[k].w[1] - W[1], dz = s_sf[k].w[2] - W[2];
            if (dx * dx + dy * dy + dz * dz < 60.0f * 60.0f) { sl = k; break; }
          }
          const float ox = cx[i] - sx, oy = cy[i] - sy;
          if (sl < 0 && fr >= 0) { sl = fr; SfSlot &n = s_sf[sl]; n.w[0] = W[0]; n.w[1] = W[1]; n.w[2] = W[2]; n.o[0] = ox; n.o[1] = oy; n.s[0] = sx; n.s[1] = sy; n.ok = true; ++g_mkSfNew; }
          if (sl >= 0) {
            SfSlot &n = s_sf[sl];
            const float mv = fabsf(sx - n.s[0]) + fabsf(sy - n.s[1]);   // screen motion of the marker since the last frame
            const float dOff = fabsf(ox - n.o[0]) + fabsf(oy - n.o[1]);
            if (dOff > 0.06f && mv < 0.002f) { n.o[0] = ox; n.o[1] = oy; ++g_mkSfNew; }   // a new layout (another widget)
            else if (mv < 0.002f) { n.o[0] += (ox - n.o[0]) * 0.2f; n.o[1] += (oy - n.o[1]) * 0.2f; ++g_mkSfLearn; }
            n.s[0] = sx; n.s[1] = sy; n.w[0] = W[0]; n.w[1] = W[1]; n.w[2] = W[2]; n.t = sfNow; n.tt = sfNow; n.f = s_sfFrame;
            c.hx = sx + n.o[0]; c.hy = sy + n.o[1]; ++g_mkSfUsed;
          }
        }
      }
      for (int e = 0; e < 2 && ok; ++e) {
        const float *M = vp + e * 16, *W = mk + best * 5;
        const float X = W[0] * M[0] + W[1] * M[4] + W[2] * M[8] + M[12];
        const float Y = W[0] * M[1] + W[1] * M[5] + W[2] * M[9] + M[13];
        const float Wc = W[0] * M[3] + W[1] * M[7] + W[2] * M[11] + M[15];
        if (Wc <= 0.0001f) { ok = false; break; }
        c.p[e][0] = X / Wc; c.p[e][1] = Y / Wc;
      }
      c.world = false;
      if (ok && axOk && g_mkWorldOn) {   // MARKERWORLD: world size from the left eye (1 unit along the camera's right = how much NDC)
        const float *W = mk + best * 5; const float *M0 = vp;
        float Pr[3] = { W[0] + bR[0], W[1] + bR[1], W[2] + bR[2] }, Pu[3] = { W[0] + bU[0], W[1] + bU[1], W[2] + bU[2] };
        float rx, ry, ux, uy;
        if (projP(M0, Pr, rx, ry) && projP(M0, Pu, ux, uy)) {
          const float dR = sqrtf((rx - c.p[0][0]) * (rx - c.p[0][0]) + (ry - c.p[0][1]) * (ry - c.p[0][1]));
          const float sR = (rx - c.p[0][0]) >= 0.0f ? 1.0f : -1.0f, sU = (uy - c.p[0][1]) >= 0.0f ? 1.0f : -1.0f;   // card x right, y up
          const float rC = (g_markSheetOn ? g_mkCardR : g_mcRadius) * g_mkScale;
          if (dR > 1e-6f) {
            const float h = rC / dR;   // world half-size
            const float cs[4][2] = { { -1.0f, 1.0f }, { 1.0f, 1.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f } };
            bool cok = true;
            for (int e = 0; e < 2 && cok; ++e)
              for (int k = 0; k < 4 && cok; ++k) {
                float P[3]; for (int a = 0; a < 3; ++a) P[a] = W[a] + h * (cs[k][0] * sR * bR[a] + cs[k][1] * sU * bU[a]);
                cok = projP(vp + e * 16, P, c.q[e][k][0], c.q[e][k][1]);
                { const float *M = vp + e * 16; c.q[e][k][2] = P[0] * M[3] + P[1] * M[7] + P[2] * M[11] + M[15]; }   // CARDPERSP: clip w
              }
            c.world = cok;
            if (nCuts == 0) {   // EYESHOT diagnostics for the first card
              int o = snprintf(g_mkCardDiag, sizeof(g_mkCardDiag), "W %.1f %.1f %.1f | right %.3f %.3f %.3f up %.3f %.3f %.3f fwd %.3f %.3f %.3f | dR %.6f h %.2f sR %.0f sU %.0f | hud %.3f %.3f\n",
                               W[0], W[1], W[2], bR[0], bR[1], bR[2], bU[0], bU[1], bU[2], bF[0], bF[1], bF[2], dR, h, sR, sU, c.hx, c.hy);
              for (int e = 0; e < 2 && o > 0 && o < (int)sizeof(g_mkCardDiag); ++e) {
                o += snprintf(g_mkCardDiag + o, sizeof(g_mkCardDiag) - o, "eye %d centre %.4f %.4f corners TL %.4f %.4f TR %.4f %.4f BL %.4f %.4f BR %.4f %.4f | VP", e, c.p[e][0], c.p[e][1],
                              c.q[e][0][0], c.q[e][0][1], c.q[e][1][0], c.q[e][1][1], c.q[e][2][0], c.q[e][2][1], c.q[e][3][0], c.q[e][3][1]);
                for (int k = 0; k < 16 && o > 0 && o < (int)sizeof(g_mkCardDiag); ++k) o += snprintf(g_mkCardDiag + o, sizeof(g_mkCardDiag) - o, " %.5f", vp[e * 16 + k]);
                if (o > 0 && o < (int)sizeof(g_mkCardDiag)) o += snprintf(g_mkCardDiag + o, sizeof(g_mkCardDiag) - o, "\n");
              }
            }
          }
        }
      }
      if (ok) ++nCuts;
    }
  }
  // MKCARD 2026-10-11 (akvr_native/NOTES.md MKCARD): AKN routes the marker pieces into the flat marker sheet and hands each
  // marker as a perspective card per eye (upright at its world point, facing the camera's heading, world up, the game's
  // layout scale, same frame); drawn below by the CARDPERSP path. No HUD-tree points, no matching here.
  bool cardMode = false;
  if (g_hieEyeMode) {
    using CardModeFn = int (*)(); using CardsFn = int (*)(float, float, float *, int);
    static CardModeFn p_cm = nullptr; static CardsFn p_cards = nullptr; static bool s_ct = false;
    if (!s_ct) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (m) { p_cm = (CardModeFn)GetProcAddress(m, "akn_marker_card_mode"); p_cards = (CardsFn)GetProcAddress(m, "akn_marker_cards"); s_ct = true; } }
    if (p_cm && p_cards && p_cm()) {
      cardMode = true;
      float cd[8 * 26]; const int ncd = p_cards(g_mkCardR, g_mkScale, cd, 8);
      nCuts = 0;
      for (int i = 0; i < ncd && nCuts < 8; ++i) {
        Cut &c = cuts[nCuts]; const float *o = cd + i * 26;
        c.hx = o[0]; c.hy = o[1]; c.world = true;
        for (int e = 0; e < 2; ++e) {
          c.p[e][0] = c.p[e][1] = 0.0f;
          for (int k = 0; k < 4; ++k) { for (int a = 0; a < 3; ++a) c.q[e][k][a] = o[2 + e * 12 + k * 3 + a]; c.p[e][0] += 0.25f * c.q[e][k][0]; c.p[e][1] += 0.25f * c.q[e][k][1]; }
        }
        ++nCuts;
      }
    }
  }
  const bool keyE = g_blackKey && g_pauseLive && g_hieKeyPS && !trFull;   // (pause screens: no cards - nothing routed either)
  for (int i = 0; i < nCuts; ++i) { g_mkExp[i * 2] = cuts[i].hx; g_mkExp[i * 2 + 1] = cuts[i].hy; }   // MARKSHEET: for AKN's next HUD frame
  // MKHOLD 2026-10-09 (JJ: the distance marker flickers): AKN routes the marker pieces by THESE centres on its next HUD
  // frame; a single frame without a match used to drop the routing, and the marker fell back to small cut circles.
  // Keep the last centres for up to 8 frames.
  { static int s_mkHold = 0;
    if (keyE) { g_mkExpN = 0; s_mkHold = 0; }
    else if (nCuts > 0) { g_mkExpN = nCuts; s_mkHold = 8; }
    else if (s_mkHold > 0) --s_mkHold;
    else g_mkExpN = 0; }
  bool msheet = false;   // MARKSHEET: the marker pieces are in their own sheet this frame
  if (g_markSheetOn && nCuts > 0 && !keyE) {
    if (!p_aknMarkerHudTried) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); p_aknMarkerHud = m ? (AknMarkerHudFn)GetProcAddress(m, "akn_get_marker_hud") : nullptr; p_aknMarkerHudTried = m != nullptr; }
    long routed = 0; ID3D11Texture2D *mt = p_aknMarkerHud ? p_aknMarkerHud(&routed) : nullptr;
    if (mt && g_mkSrc != mt) {
      if (g_mkSRV) { g_mkSRV->Release(); g_mkSRV = nullptr; }
      g_mkSrc = mt;
      D3D11_TEXTURE2D_DESC md{}; mt->GetDesc(&md);
      D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = md.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : md.Format;
      vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
      if (FAILED(g_device->CreateShaderResourceView(mt, &vd, &g_mkSRV))) g_mkSRV = nullptr;
    }
    msheet = mt && g_mkSRV && routed > 0;
    if (msheet) ++g_mkSheetFrames;
  }
  // MKNOCUT 2026-10-09 (JJ: corners missing, "cutting out the text that's near it", flicker): with the marker sheet on, a
  // frame AKN could not route is drawn whole - the marker stays flat on the HUD for that frame - instead of cutting
  // small circles out of the HUD sheet (bites out of nearby text, the bracket corners lost).
  // (MKNOCUT reverted 23:1x: the distance marker's pieces are drawn by HUD shaders AKN cannot route, so the marker
  // sheet is empty for it - without the cut card it stayed flat on the HUD, off its target. JJ: "floats away".)
  // MKNOCUT2 2026-10-10 (MKTAG routes the marked pieces reliably; JJ: "cutting out with a circle seems like a pretty bad
  // hack", and the cut on unrouted frames copied the compass around the marker = a flickering duplicate): with the marker
  // sheet on, a frame without routed pieces draws no cards and cuts nothing - the HUD sheet is shown whole.
  if (g_markSheetOn && !msheet) nCuts = 0;
  if (g_hieEyeMode && !cardMode) nCuts = 0;   // MKEYE: the markers come in their per-eye layers (MKCARD: AKN's cards)
  if (trFull) nCuts = 0;   // TRANSFULL: the plain sheet only
  const float rCut = msheet ? g_mkCardR : g_mcRadius;
  float circ[36] = {};   // 8 x (uv centre, uv radii) + mode
  for (int i = 0; i < nCuts; ++i) { circ[i * 4] = cuts[i].hx * 0.5f + 0.5f; circ[i * 4 + 1] = 0.5f - cuts[i].hy * 0.5f; circ[i * 4 + 2] = rCut * 0.5f; circ[i * 4 + 3] = rCut * 0.5f; }
  circ[34] = msheet ? 0.0f : 1.0f;   // mode.z: MARKERRING test only when cutting the HUD sheet
  g_ptHieMark = pt_now();   // PERFTIME: marker work done, drawing starts
  // save what we touch
  ID3D11RenderTargetView *oRTV = nullptr; ID3D11DepthStencilView *oDSV = nullptr; g_ctx->OMGetRenderTargets(1, &oRTV, &oDSV);
  UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT ovp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; g_ctx->RSGetViewports(&nvp, ovp);
  ID3D11VertexShader *oVS = nullptr; ID3D11PixelShader *oPS = nullptr; g_ctx->VSGetShader(&oVS, nullptr, nullptr); g_ctx->PSGetShader(&oPS, nullptr, nullptr);
  ID3D11ShaderResourceView *oSRV = nullptr; g_ctx->PSGetShaderResources(0, 1, &oSRV);
  ID3D11SamplerState *oSS = nullptr; g_ctx->PSGetSamplers(0, 1, &oSS);
  ID3D11Buffer *oCB = nullptr; g_ctx->VSGetConstantBuffers(0, 1, &oCB);
  ID3D11Buffer *oPCB = nullptr; g_ctx->PSGetConstantBuffers(0, 1, &oPCB);
  ID3D11InputLayout *oIL = nullptr; g_ctx->IAGetInputLayout(&oIL);
  D3D11_PRIMITIVE_TOPOLOGY oTop; g_ctx->IAGetPrimitiveTopology(&oTop);
  ID3D11BlendState *oBS = nullptr; FLOAT oBF[4]; UINT oBM = 0; g_ctx->OMGetBlendState(&oBS, oBF, &oBM);
  ID3D11DepthStencilState *oDS = nullptr; UINT oRef = 0; g_ctx->OMGetDepthStencilState(&oDS, &oRef);
  ID3D11RasterizerState *oRS = nullptr; g_ctx->RSGetState(&oRS);
  ID3D11ShaderResourceView *oSRV1 = nullptr; g_ctx->PSGetShaderResources(1, 1, &oSRV1);   // FULLCUT5
  g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  const bool cutGpu = g_fullCut && g_cutPS && g_cutRTV && g_cutSRV && !akvr_xr_main_menu_detected() && !trFull;
  if (cutGpu) {   // FULLCUT5: this frame's cut, measured on the graphics card (no readback delay)
    const float full[16] = {-1, 1, 0.5f, 1,  1, 1, 0.5f, 1,  -1, -1, 0.5f, 1,  1, -1, 0.5f, 1};
    const float fl[4] = {g_pauseLive ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f};
    g_ctx->UpdateSubresource(g_hieCB, 0, nullptr, full, 0, 0); g_ctx->UpdateSubresource(g_cutCB, 0, nullptr, fl, 0, 0);
    ID3D11ShaderResourceView *noSrv = nullptr; g_ctx->PSSetShaderResources(1, 1, &noSrv);
    g_ctx->OMSetRenderTargets(1, &g_cutRTV, nullptr);
    const D3D11_VIEWPORT one{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}; g_ctx->RSSetViewports(1, &one);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff); g_ctx->OMSetDepthStencilState(g_hieDS, 0); g_ctx->RSSetState(g_hieRS);
    g_ctx->VSSetShader(g_hieVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_hieCB);
    g_ctx->PSSetShader(g_cutPS, nullptr, 0); g_ctx->PSSetConstantBuffers(0, 1, &g_cutCB);
    g_ctx->PSSetShaderResources(0, 1, &g_hieSRV);
    g_ctx->Draw(4, 0);
  }
  g_ctx->OMSetRenderTargets(1, &g_hieRTV, nullptr);
  { ID3D11ShaderResourceView *c1 = cutGpu ? g_cutSRV : nullptr; g_ctx->PSSetShaderResources(1, 1, &c1); }
  const bool keyNow = g_blackKey && g_pauseLive && g_hieKeyPS && !trFull;   // BLACKKEY (TRANSFULL: the black is the transition)
  const bool cut = nCuts > 0 && !keyNow;
  if (cut) {   // MARKERCUT: the sheet without the marker areas (MARKSHEET: the whole sheet - the markers are not in it)
    circ[32] = msheet ? 0.0f : (float)nCuts; circ[33] = 0.0f;
    g_ctx->UpdateSubresource(g_mcPCB, 0, nullptr, circ, 0, 0);
    g_ctx->VSSetShader(g_mcVS, nullptr, 0); g_ctx->PSSetShader(g_mcPS, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_mcVCB); g_ctx->PSSetConstantBuffers(0, 1, &g_mcPCB);
  } else {
    g_ctx->VSSetShader(g_hieVS, nullptr, 0); g_ctx->PSSetShader(keyNow ? g_hieKeyPS : g_hiePS, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_hieCB);
  }
  g_ctx->PSSetShaderResources(0, 1, &g_hieSRV); g_ctx->PSSetSamplers(0, 1, &g_hieSS);
  const FLOAT bf[4] = {0, 0, 0, 0}; g_ctx->OMSetBlendState(g_hieBS, bf, 0xffffffff);
  g_ctx->OMSetDepthStencilState(g_hieDS, 0); g_ctx->RSSetState(g_hieRS);
  g_hieDimDone = false;
  if (const float da = g_hieDimPS ? hie_dim_alpha() : 0.0f; da > 0.0f || (cutGpu && g_hieDimPS)) {   // FULLCUT5: + this frame's cut
    // WHEELEYE: darken both eye pictures first (ONE / INV_SRC_ALPHA with black = picture x (1 - a)), then the HUD on top
    const float full[16] = {-1, 1, 0.5f, 1,  1, 1, 0.5f, 1,  -1, -1, 0.5f, 1,  1, -1, 0.5f, 1};
    const float col[4] = {0.0f, 0.0f, 0.0f, da};
    g_ctx->UpdateSubresource(g_hieCB, 0, nullptr, full, 0, 0); g_ctx->UpdateSubresource(g_hieDimCB, 0, nullptr, col, 0, 0);
    g_ctx->VSSetShader(g_hieVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_hieCB);
    g_ctx->PSSetShader(g_hieDimPS, nullptr, 0); g_ctx->PSSetConstantBuffers(0, 1, &g_hieDimCB);
    const D3D11_VIEWPORT whole{0.0f, 0.0f, (float)sd.Width, (float)sd.Height, 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &whole);
    g_ctx->Draw(4, 0);
    g_hieDimDone = da > 0.0f;   // FULLCUT5: only the pause / depth-of-field darkening replaces the dim layer
    if (cut) {   // back to the sheet shaders
      g_ctx->VSSetShader(g_mcVS, nullptr, 0); g_ctx->PSSetShader(g_mcPS, nullptr, 0);
      g_ctx->VSSetConstantBuffers(0, 1, &g_mcVCB); g_ctx->PSSetConstantBuffers(0, 1, &g_mcPCB);
    } else { g_ctx->PSSetShader(keyNow ? g_hieKeyPS : g_hiePS, nullptr, 0); }
  }
  ID3D11ShaderResourceView *eyeSRV[2] = {};   // MKEYE: AKN's per-eye marker layers
  if (g_hieEyeMode && !cardMode && !keyNow) {
    using EyeHudFn = ID3D11Texture2D *(*)(int, long *); static EyeHudFn p_eyeHud = nullptr; static bool s_ehTried = false;
    if (!s_ehTried) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (m) { p_eyeHud = (EyeHudFn)GetProcAddress(m, "akn_get_eye_marker_hud"); s_ehTried = true; } }
    static ID3D11Texture2D *s_eyeSrc[2] = {}; static ID3D11ShaderResourceView *s_eyeSRV[2] = {};
    for (int k = 0; k < 2 && p_eyeHud; ++k) {
      long routed = 0; ID3D11Texture2D *t = p_eyeHud(k, &routed);
      if (t != s_eyeSrc[k]) {
        if (s_eyeSRV[k]) { s_eyeSRV[k]->Release(); s_eyeSRV[k] = nullptr; }
        s_eyeSrc[k] = t;
        if (t) {
          D3D11_TEXTURE2D_DESC md{}; t->GetDesc(&md);
          D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = md.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : md.Format;
          vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
          if (FAILED(g_device->CreateShaderResourceView(t, &vd, &s_eyeSRV[k]))) s_eyeSRV[k] = nullptr;
        }
      }
      eyeSRV[k] = s_eyeSRV[k];
    }
  }
  // SCANHUD: the marker sheet (AKN puts the detective callout's box there, unmoved) and the shift that puts its box where the
  // head sees the object: the object's direction from both eyes (the shown pair's view x lens) -> the ray from the head's centre
  // -> where it meets the HUD plane (u, v on the sheet). Depends on the head's position, not its rotation.
  ID3D11ShaderResourceView *scanSRV = nullptr; bool scanOk = false; float scanDu = 0.0f, scanDv = 0.0f, scanBoxU = 0.5f, scanBoxV = 0.5f;
  if (!cut && g_hieEyeMode && !trFull) {   // (TRANSFULL: no detective box during a transition)
    using MkHudFn = ID3D11Texture2D *(*)(long *); using BoxFn = int (*)(float *); using PairVpFn = int (*)(float *);
    static MkHudFn s_mh = nullptr; static BoxFn s_bx = nullptr; static PairVpFn s_vp = nullptr; static bool s_t = false;
    if (!s_t) { HMODULE am = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (am) { s_mh = (MkHudFn)GetProcAddress(am, "akn_get_marker_hud"); s_bx = (BoxFn)GetProcAddress(am, "akn_scan_box"); s_vp = (PairVpFn)GetProcAddress(am, "akn_pair_vp"); } s_t = am != nullptr; }
    long routed = 0; ID3D11Texture2D *mt = s_mh ? s_mh(&routed) : nullptr;
    static ID3D11Texture2D *s_mSrc = nullptr; static ID3D11ShaderResourceView *s_mSRV = nullptr;
    if (mt != s_mSrc) {
      if (s_mSRV) { s_mSRV->Release(); s_mSRV = nullptr; }
      s_mSrc = mt;
      if (mt) {
        D3D11_TEXTURE2D_DESC md{}; mt->GetDesc(&md);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = md.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : md.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(mt, &vd, &s_mSRV))) s_mSRV = nullptr;
      }
    }
    if (s_mSRV && routed > 0) scanSRV = s_mSRV;
    float bx5[5], vp[32];
    if (scanSRV && s_bx && s_vp && s_bx(bx5) && s_vp(vp)) {
      XrVector3f dsum{0.0f, 0.0f, 0.0f}; bool ok2 = true;
      for (int e2 = 0; e2 < 2 && ok2; ++e2) {
        XrPosef ep2; XrFovf f2; hie_eye_view(e2, headPose, ep2, f2);
        const float *M = vp + e2 * 16; float a4[4];
        for (int k = 0; k < 4; ++k) a4[k] = bx5[0] * M[k] + bx5[1] * M[4 + k] + bx5[2] * M[8 + k] + M[12 + k];
        if (a4[3] <= 1e-3f) { ok2 = false; break; }
        const float nx = a4[0] / a4[3], ny = a4[1] / a4[3];
        const float tl = tanf(f2.angleLeft), tr = tanf(f2.angleRight), tu = tanf(f2.angleUp), td = tanf(f2.angleDown);
        const XrVector3f dw = quat_rotate(ep2.orientation, XrVector3f{tl + (nx + 1.0f) * 0.5f * (tr - tl), td + (ny + 1.0f) * 0.5f * (tu - td), -1.0f});
        const float l = sqrtf(dw.x * dw.x + dw.y * dw.y + dw.z * dw.z);
        dsum.x += dw.x / l; dsum.y += dw.y / l; dsum.z += dw.z / l;
      }
      const XrVector3f N = quat_rotate(qp.orientation, XrVector3f{0.0f, 0.0f, 1.0f});
      const float den = dsum.x * N.x + dsum.y * N.y + dsum.z * N.z;
      const XrVector3f H = headPose.position;
      if (ok2 && fabsf(den) > 1e-5f) {
        const float t = ((C.x - H.x) * N.x + (C.y - H.y) * N.y + (C.z - H.z) * N.z) / den;
        if (t > 0.0f) {
          const XrVector3f Q{H.x + dsum.x * t - C.x, H.y + dsum.y * t - C.y, H.z + dsum.z * t - C.z};
          const float u = 0.5f + (Q.x * R.x + Q.y * R.y + Q.z * R.z) / (2.0f * hw * hw);
          const float v = 0.5f - (Q.x * U.x + Q.y * U.y + Q.z * U.z) / (2.0f * hh * hh);
          float drawn[2] = { bx5[3], bx5[4] };   // SCANOWN: AKN's sheet point is the game's own; the box is DRAWN with our HUD-layer shrink in
          if (akvr_scan_box_drawn(drawn)) { bx5[3] = drawn[0]; bx5[4] = drawn[1]; }
          scanBoxU = 0.5f * (bx5[3] + 1.0f); scanBoxV = 0.5f * (1.0f - bx5[4]);
          scanDu = u - scanBoxU; scanDv = v - scanBoxV; scanOk = true;
        }
      }
    }
  }
  // AIMHUD 2026-10-18 (JJ: the Batmobile's turret aims with the head while the left trigger is held, "but the HUD element is not
  // moving with it ... should be independent of the rest of the HUD and should move around with your head movements"): AKN puts the
  // turret reticle's pieces on their own aim sheet, unmoved (earlyres.cpp kAimTag). While the head is in the game's aim, the game
  // laid that sheet out for a camera that holds the head, so it hangs on the HEAD: the HUD sheet's own size and distance, square to
  // the face, its centre on the head's forward direction. Otherwise it stays on the room HUD plane, exactly where it was.
  ID3D11ShaderResourceView *aimSRV = nullptr; XrVector3f aimCorner[4];
  if (!cut && !trFull && !keyNow) {
    using AimHudFn = ID3D11Texture2D *(*)(long *); static AimHudFn s_ah = nullptr; static bool s_aht = false;
    if (!s_aht) { HMODULE am = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (am) s_ah = (AimHudFn)GetProcAddress(am, "akn_get_aim_hud"); s_aht = am != nullptr; }
    long routed = 0; ID3D11Texture2D *at = s_ah ? s_ah(&routed) : nullptr;
    static ID3D11Texture2D *s_aSrc = nullptr; static ID3D11ShaderResourceView *s_aSRV = nullptr;
    if (at != s_aSrc) {
      if (s_aSRV) { s_aSRV->Release(); s_aSRV = nullptr; }
      s_aSrc = at;
      if (at) {
        D3D11_TEXTURE2D_DESC md{}; at->GetDesc(&md);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = md.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : md.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(at, &vd, &s_aSRV))) s_aSRV = nullptr;
      }
    }
    if (s_aSRV && routed > 0) {
      aimSRV = s_aSRV;
      for (int k = 0; k < 4; ++k) aimCorner[k] = corner[k];
      ++g_aimHudFrames;
      if (akvr_cam_aim_head_now() && akvr_gamepad_left_trigger_ms() < 300) {
        const XrVector3f H = headPose.position;
        float dist = sqrtf((C.x - H.x) * (C.x - H.x) + (C.y - H.y) * (C.y - H.y) + (C.z - H.z) * (C.z - H.z));
        dist = dist < 0.3f ? 0.3f : (dist > 20.0f ? 20.0f : dist);
        const XrVector3f f = quat_rotate(headPose.orientation, XrVector3f{0.0f, 0.0f, -dist});
        const XrVector3f Rh = quat_rotate(headPose.orientation, XrVector3f{hw, 0, 0}), Uh = quat_rotate(headPose.orientation, XrVector3f{0, hh, 0});
        const XrVector3f Ch{H.x + f.x, H.y + f.y, H.z + f.z};
        aimCorner[0] = {Ch.x - Rh.x + Uh.x, Ch.y - Rh.y + Uh.y, Ch.z - Rh.z + Uh.z};
        aimCorner[1] = {Ch.x + Rh.x + Uh.x, Ch.y + Rh.y + Uh.y, Ch.z + Rh.z + Uh.z};
        aimCorner[2] = {Ch.x - Rh.x - Uh.x, Ch.y - Rh.y - Uh.y, Ch.z - Rh.z - Uh.z};
        aimCorner[3] = {Ch.x + Rh.x - Uh.x, Ch.y + Rh.y - Uh.y, Ch.z + Rh.z - Uh.z};
        ++g_aimHudHead;
      }
    }
  }
  for (int e = 0; e < 2; ++e) {
    XrPosef ep; XrFovf f; hie_eye_view(e, headPose, ep, f);
    const float tl = tanf(f.angleLeft), tr = tanf(f.angleRight), tu = tanf(f.angleUp), td = tanf(f.angleDown);
    const XrQuaternionf inv = hie_conj(ep.orientation);
    float c[16];
    for (int k = 0; k < 4; ++k) {
      const XrVector3f d{corner[k].x - ep.position.x, corner[k].y - ep.position.y, corner[k].z - ep.position.z};
      const XrVector3f v = quat_rotate(inv, d);            // eye space: x right, y up, -z ahead
      const float w = -v.z;
      c[k * 4 + 0] = (2.0f * v.x + (tr + tl) * v.z) / (tr - tl);
      c[k * 4 + 1] = (2.0f * v.y + (tu + td) * v.z) / (tu - td);
      c[k * 4 + 2] = 0.5f * w;
      c[k * 4 + 3] = w;
    }
    const D3D11_VIEWPORT vp{e * halfW, 0.0f, halfW, (float)sd.Height, 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
    if (g_backExt && g_pauseLive && g_hieExtPS && g_mcVS && g_mcVCB) {
      // BACKEXT: the sheet's plane grown about its centre (texture coordinates past 0..1, clamped = its edge texels)
      for (float grow = 3.0f; grow >= 1.5f; grow -= 0.5f) {
        float q[20]; bool front = true;
        for (int k = 0; k < 4 && front; ++k) {
          const XrVector3f wpt{C.x + (corner[k].x - C.x) * grow, C.y + (corner[k].y - C.y) * grow, C.z + (corner[k].z - C.z) * grow};
          const XrVector3f v = quat_rotate(inv, XrVector3f{wpt.x - ep.position.x, wpt.y - ep.position.y, wpt.z - ep.position.z});
          const float w = -v.z;
          if (w < 0.05f) { front = false; break; }
          q[k * 4 + 0] = (2.0f * v.x + (tr + tl) * v.z) / (tr - tl);
          q[k * 4 + 1] = (2.0f * v.y + (tu + td) * v.z) / (tu - td);
          q[k * 4 + 2] = 0.5f * w; q[k * 4 + 3] = w;
        }
        if (!front) continue;
        q[16] = 0.5f - 0.5f * grow; q[17] = 0.5f - 0.5f * grow; q[18] = grow; q[19] = grow;
        g_ctx->VSSetShader(g_mcVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_mcVCB);
        g_ctx->PSSetShader(g_hieExtPS, nullptr, 0);
        g_ctx->UpdateSubresource(g_mcVCB, 0, nullptr, q, 0, 0);
        g_ctx->Draw(4, 0);
        if (cut) { g_ctx->PSSetShader(g_mcPS, nullptr, 0); g_ctx->PSSetConstantBuffers(0, 1, &g_mcPCB); }
        else { g_ctx->VSSetShader(g_hieVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_hieCB); g_ctx->PSSetShader(keyNow ? g_hieKeyPS : g_hiePS, nullptr, 0); }
        break;
      }
    }
    if (cut) {
      float c5[20]; for (int k = 0; k < 16; ++k) c5[k] = c[k]; c5[16] = 0.0f; c5[17] = 0.0f; c5[18] = 1.0f; c5[19] = 1.0f;
      g_ctx->UpdateSubresource(g_mcVCB, 0, nullptr, c5, 0, 0);
      g_ctx->Draw(4, 0);
      // the marker cards: each marker's area, in this eye's own picture, centred where this eye sees its world point
      circ[32] = (float)nCuts; circ[33] = 1.0f; g_ctx->UpdateSubresource(g_mcPCB, 0, nullptr, circ, 0, 0);
      if (msheet) g_ctx->PSSetShaderResources(0, 1, &g_mkSRV);   // MARKSHEET: cards from the marker sheet
      for (int i = 0; i < nCuts; ++i) {
        const float r = rCut, rs = rCut * g_mkScale, px = cuts[i].p[e][0], py = cuts[i].p[e][1];
        float q[20] = { px - rs, py + rs, 0.5f, 1.0f,  px + rs, py + rs, 0.5f, 1.0f,  px - rs, py - rs, 0.5f, 1.0f,  px + rs, py - rs, 0.5f, 1.0f,
                        circ[i * 4] - r * 0.5f, circ[i * 4 + 1] - r * 0.5f, r, r };
        if (cuts[i].world)   // MARKERWORLD: the world-locked panel's corners in this eye
          // CARDPERSP 2026-10-10 (JJ: the marker "rotated and shifted differently" per eye, skewing and stretching with head
          // moves): the corners went in as flat NDC (w = 1), so the card's picture was spread over its two triangles in screen
          // space - the card is a trapezoid of a different shape in each turned eye, so the content bent differently per eye.
          // Clip coordinates (x w, y w, z, w) let the GPU lay the picture on the card as on a real surface.
          for (int k = 0; k < 4; ++k) { const float w = cuts[i].q[e][k][2]; q[k * 4] = cuts[i].q[e][k][0] * w; q[k * 4 + 1] = cuts[i].q[e][k][1] * w; q[k * 4 + 2] = 0.5f * w; q[k * 4 + 3] = w; }
        g_ctx->UpdateSubresource(g_mcVCB, 0, nullptr, q, 0, 0);
        g_ctx->Draw(4, 0);
        ++g_mcCards;
      }
      circ[32] = msheet ? 0.0f : (float)nCuts; circ[33] = 0.0f; g_ctx->UpdateSubresource(g_mcPCB, 0, nullptr, circ, 0, 0);
      if (msheet) g_ctx->PSSetShaderResources(0, 1, &g_hieSRV);
    } else {
      g_ctx->UpdateSubresource(g_hieCB, 0, nullptr, c, 0, 0);
      g_ctx->Draw(4, 0);
    }
    if (!(eyeSRV[e] && !cut && !trFull)) { if (!eyeSRV[e]) ++g_eyeSkipNoSrv; if (cut) ++g_eyeSkipCut; if (trFull) ++g_eyeSkipTr; if (keyNow) ++g_eyeSkipKey; if (!g_hieEyeMode || cardMode) ++g_eyeSkipMode; }   // EYESKIP
    if (eyeSRV[e] && !cut && !trFull) {   // MKEYE: this eye's marker layer, drawn in this eye's own picture space = 1:1 over the eye
      const float fullq[16] = {-1, 1, 0.5f, 1,  1, 1, 0.5f, 1,  -1, -1, 0.5f, 1,  1, -1, 0.5f, 1};
      g_ctx->VSSetShader(g_hieVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_hieCB); g_ctx->PSSetShader(g_hiePS, nullptr, 0);
      g_ctx->UpdateSubresource(g_hieCB, 0, nullptr, fullq, 0, 0);
      g_ctx->PSSetShaderResources(0, 1, &eyeSRV[e]);
      g_ctx->Draw(4, 0);
      g_ctx->PSSetShaderResources(0, 1, &g_hieSRV);
      ++g_hieEyeDraws;
    }
    if (aimSRV) {   // AIMHUD: the turret reticle's sheet on its plane (head or room), in this eye
      float ca[16];
      for (int k = 0; k < 4; ++k) {
        const XrVector3f v = quat_rotate(inv, XrVector3f{aimCorner[k].x - ep.position.x, aimCorner[k].y - ep.position.y, aimCorner[k].z - ep.position.z});
        const float w = -v.z;
        ca[k * 4 + 0] = (2.0f * v.x + (tr + tl) * v.z) / (tr - tl);
        ca[k * 4 + 1] = (2.0f * v.y + (tu + td) * v.z) / (tu - td);
        ca[k * 4 + 2] = 0.5f * w;
        ca[k * 4 + 3] = w;
      }
      g_ctx->VSSetShader(g_hieVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_hieCB); g_ctx->PSSetShader(g_hiePS, nullptr, 0);
      g_ctx->UpdateSubresource(g_hieCB, 0, nullptr, ca, 0, 0);
      g_ctx->PSSetShaderResources(0, 1, &aimSRV);
      g_ctx->Draw(4, 0);
      g_ctx->PSSetShaderResources(0, 1, &g_hieSRV);
    }
    // SCANHUD 2026-10-13 (JJ: "the lines and the box should all be at HUD depth for detective mode"; drones' boxes "miss
    // the mark a little"): the callout's box is a HUD piece on AKN's marker sheet; it is drawn ON THE HUD PLANE, shifted so the
    // box sits where the head sees the object (scanDu / scanDv, worked out below the loop's start), and the line runs from it to
    // the label's anchor on the same plane - everything at HUD depth, the same frame's data
    if (!cut && scanSRV) {
      float cs[16];
      for (int k = 0; k < 4; ++k) for (int j = 0; j < 4; ++j) cs[k * 4 + j] = c[k * 4 + j] + scanDu * (c[4 + j] - c[j]) + scanDv * (c[8 + j] - c[j]);
      g_ctx->PSSetShaderResources(0, 1, &scanSRV);
      float cp[8];
      if (g_scanBox && g_mcVS && g_mcVCB && scanplus_build() && akvr_scan_corners(cp) == 4) {
        // SCANBOX: four plus signs, each on a small quad round its corner's point on the (shifted) sheet
        float cu[4], cv[4];
        for (int k = 0; k < 4; ++k) { cu[k] = 0.5f * (cp[k * 2] + 1.0f); cv[k] = 0.5f * (1.0f - cp[k * 2 + 1]); }
        float side = 0.0f; for (int k = 1; k < 4; ++k) { const float du = cu[k] - cu[0], dv = (cv[k] - cv[0]) / 0.9175f, dd = sqrtf(du * du + dv * dv); if (dd > 1e-4f && (side == 0.0f || dd < side)) side = dd; }
        const float hu = (side > 0.0f ? side : 0.02f) * 0.22f, hv = hu * 0.9175f;   // (sheet 2624 x 2860: square on the sheet)
        g_ctx->VSSetShader(g_mcVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_mcVCB);
        g_ctx->PSSetShader(g_scanPlusPS, nullptr, 0); g_ctx->PSSetConstantBuffers(0, 1, &g_scanPlusCB);
        for (int k = 0; k < 4; ++k) {
          const float u0 = cu[k] - hu, v0 = cv[k] - hv, u1 = cu[k] + hu, v1 = cv[k] + hv;
          const float uu[4] = { u0, u1, u0, u1 }, vv[4] = { v0, v0, v1, v1 };
          float q[20];
          for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) q[i * 4 + j] = cs[j] + uu[i] * (cs[4 + j] - cs[j]) + vv[i] * (cs[8 + j] - cs[j]);
          q[16] = u0; q[17] = v0; q[18] = u1 - u0; q[19] = v1 - v0;
          const float pc[8] = { cu[k], cv[k], hu, hv, 0.45f, 0.95f, 0.55f, 0.95f };
          g_ctx->UpdateSubresource(g_mcVCB, 0, nullptr, q, 0, 0); g_ctx->UpdateSubresource(g_scanPlusCB, 0, nullptr, pc, 0, 0);
          g_ctx->Draw(4, 0);
        }
        ++g_scanPlusDraws;
      }
      g_ctx->PSSetShader(g_hiePS, nullptr, 0);
      g_ctx->VSSetShader(g_hieVS, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_hieCB);
      g_ctx->PSSetShaderResources(0, 1, &g_hieSRV);
      ++g_scanBoxDraws;
      float an[2];
      if (scanOk && akvr_scan_anchor(an) && scanline_build()) {
        const float ua = scanBoxU + scanDu, va = scanBoxV + scanDv, ub = 0.5f * (an[0] + 1.0f), vb = 0.5f * (1.0f - an[1]);
        float a4[4], b4[4];
        for (int k = 0; k < 4; ++k) { a4[k] = c[k] + ua * (c[4 + k] - c[k]) + va * (c[8 + k] - c[k]); b4[k] = c[k] + ub * (c[4 + k] - c[k]) + vb * (c[8 + k] - c[k]); }
        if (a4[3] > 1e-3f && b4[3] > 1e-3f) {
          const float ax = a4[0] / a4[3], ay = a4[1] / a4[3], bx = b4[0] / b4[3], by = b4[1] / b4[3];
          const float sxp = 0.5f * halfW, syp = 0.5f * (float)sd.Height;   // NDC -> pixels
          const float dxp = (bx - ax) * sxp, dyp = (by - ay) * syp, L = sqrtf(dxp * dxp + dyp * dyp);
          if (L > 1.0f) {
            const float ox = -dyp / L * g_scanLineW / sxp, oy = dxp / L * g_scanLineW / syp;
            const float q[16] = { ax + ox, ay + oy, 0.5f, 1.0f,  bx + ox, by + oy, 0.5f, 1.0f,  ax - ox, ay - oy, 0.5f, 1.0f,  bx - ox, by - oy, 0.5f, 1.0f };
            g_ctx->PSSetShader(g_scanLinePS, nullptr, 0);
            g_ctx->UpdateSubresource(g_hieCB, 0, nullptr, q, 0, 0);
            g_ctx->Draw(4, 0);
            g_ctx->PSSetShader(g_hiePS, nullptr, 0);
            ++g_scanLineDraws;
          }
        }
      }
    }
  }
  ID3D11ShaderResourceView *none = nullptr; g_ctx->PSSetShaderResources(0, 1, &none);
  g_ctx->PSSetShaderResources(1, 1, &oSRV1); if (oSRV1) oSRV1->Release();   // FULLCUT5
  // restore
  g_ctx->OMSetRenderTargets(1, &oRTV, oDSV);
  if (nvp) g_ctx->RSSetViewports(nvp, ovp);
  g_ctx->VSSetShader(oVS, nullptr, 0); g_ctx->PSSetShader(oPS, nullptr, 0);
  g_ctx->PSSetShaderResources(0, 1, &oSRV); g_ctx->PSSetSamplers(0, 1, &oSS); g_ctx->VSSetConstantBuffers(0, 1, &oCB);
  g_ctx->PSSetConstantBuffers(0, 1, &oPCB); if (oPCB) oPCB->Release();
  g_ctx->IASetInputLayout(oIL); g_ctx->IASetPrimitiveTopology(oTop);
  g_ctx->OMSetBlendState(oBS, oBF, oBM); g_ctx->OMSetDepthStencilState(oDS, oRef); g_ctx->RSSetState(oRS);
  if (oRTV) oRTV->Release(); if (oDSV) oDSV->Release(); if (oVS) oVS->Release(); if (oPS) oPS->Release();
  if (oSRV) oSRV->Release(); if (oSS) oSS->Release(); if (oCB) oCB->Release(); if (oIL) oIL->Release();
  if (oBS) oBS->Release(); if (oDS) oDS->Release(); if (oRS) oRS->Release();
  ++g_hieDraws;
  snprintf(g_hieDiag, sizeof(g_hieDiag), "HUD in the eyes: %ld frames (skipped %ld) | eye layers skipped: no layer %u, cut %u, transition %u, pause key %u, mode %u | target points %d: (%.3f %.3f) (%.3f %.3f) map %.3f %.3f %.3f %.3f | markers per eye (MKEYE) %s, layers drawn %u, layout map %s | markers cut now %d, cards %ld, no world point %ld | same-frame centre %u, learnt %u, new %u, carried through a missed frame %u | first: hud (%.3f %.3f) L (%.3f %.3f) R (%.3f %.3f)",
           g_hieDraws, g_hieSkips, g_eyeSkipNoSrv, g_eyeSkipCut, g_eyeSkipTr, g_eyeSkipKey, g_eyeSkipMode, g_hieNPts, g_hiePts[0], g_hiePts[1], g_hiePts[2], g_hiePts[3], g_hieXf[0], g_hieXf[1], g_hieXf[2], g_hieXf[3],
           g_hieEyeMode ? "on" : "off", g_hieEyeDraws, g_hieXfOk ? "known" : "not yet", nCuts, g_mcCards, g_mcNoWorld, g_mkSfUsed, g_mkSfLearn, g_mkSfNew, g_mkCarry, nCuts ? cuts[0].hx : 0.f, nCuts ? cuts[0].hy : 0.f, nCuts ? cuts[0].p[0][0] : 0.f, nCuts ? cuts[0].p[0][1] : 0.f, nCuts ? cuts[0].p[1][0] : 0.f, nCuts ? cuts[0].p[1][1] : 0.f);
  return true;
}

// ZOOMVIG 2026-09-29 — JJ: the right-stick-click zoom shows a 2D vignette overlay that "doesn't work very well"
// in VR (our FOV lock cancels the magnification, so only the overlay is left); wanted instead: "a strong
// vignette effect that covers all your field of vision, at the peripherals". A VIEW-space quad 1 m ahead,
// 5 m square (+-68 deg, past the headset's view), black with an alpha ramp: clear inside g_vigClearDeg, full
// strength g_vigRampDeg further out. On while the game's own FOV is below g_vigBelowDeg in gameplay (the zoom),
// or while previewing. The image is rewritten only when its look changes (a fade steps through 8 levels).
float g_vigStrength = 1.0f;    // 0 = off (JJ: match the game's own overlay)
float g_vigClearDeg = 18.0f;   // clear centre, half-angle
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
  if (XR_FAILED(sc_create("vignette layer", &sci, &g_vigSwap))) { g_vigSwap = XR_NULL_HANDLE; g_vigErr = 2; return false; }
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
// DETVIG 2026-10-09 (JJ: detective mode's vignette "should be removed per eye and a new one added across both eyes"):
// AKN hides the game's per-eye one and reports detective mode (akn_detective_active); this layer then draws one
// vignette fixed to the view, the same for both eyes, with its own gentler shape. Test switch akvr_set_detvig_test.
float g_detVigStrength = 0.85f, g_detVigClear = 12.0f, g_detVigRamp = 28.0f; int g_detVigTest = 0;   // HF5: 26/30 sat outside the view
bool akn_detective_now() {
  using DetFn = int (*)(); static DetFn f = nullptr; static bool tried = false;
  if (!tried) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); f = m ? (DetFn)GetProcAddress(m, "akn_detective_active") : nullptr; tried = m != nullptr; }
  return g_detVigTest || (f && f());
}
bool vig_layer(XrCompositionLayerQuad &q, bool gameplay) {
  const float fov = akvr_camera_game_fov();
  const bool det = akvr_engine_stereo() && akn_detective_now();   // DETVIG
  if (fov > 1.0f) {
    if (g_gameFovMin <= 0.0f || fov < g_gameFovMin) g_gameFovMin = fov;
    if (fov > g_gameFovMax) g_gameFovMax = fov;
  }
  const bool want = det || (g_vigStrength > 0.0f && (g_vigPreview || (gameplay && fov > 1.0f && fov < g_vigBelowDeg)));
  const float step = 1.0f / 8.0f;   // ~0.1 s at 90 Hz for the whole fade
  g_vigAmt = want ? (g_vigAmt + step > 1.0f ? 1.0f : g_vigAmt + step) : (g_vigAmt - step < 0.0f ? 0.0f : g_vigAmt - step);
  if (g_vigAmt <= 0.0f || g_viewSpace == XR_NULL_HANDLE) return false;
  bool filled;
  if (det) {   // DETVIG: its own shape (the zoom one's numbers put back after)
    const float c0 = g_vigClearDeg, r0 = g_vigRampDeg; g_vigClearDeg = g_detVigClear; g_vigRampDeg = g_detVigRamp;
    filled = vig_fill(g_detVigStrength * g_vigAmt);
    g_vigClearDeg = c0; g_vigRampDeg = r0;
  } else filled = vig_fill(g_vigStrength * g_vigAmt);
  if (!filled) return false;
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

// PAUSEDIM 2026-09-30 — JJ: in the pause "the whole 360 world to freeze, darken, and then add the pause screen menus
// over the top", "not fully darken to black. just darker". A plain see-through black VIEW-space quad (1 m ahead,
// 5 m square = past the lenses) between the world and the HUD layer while the pause look runs; fades in and out.
float g_pauseDimAmt = 0.0f;   // 0..1 fade
XrSwapchain g_dimSwap = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D *> g_dimImages;
float g_dimDrawn = -1.0f;
int g_dimErr = 0;
constexpr int kDimPx = 16;
bool dim_fill(float a) {
  if (!g_ctx) return false;
  if (g_dimSwap == XR_NULL_HANDLE) {
    if (g_dimErr) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
    int64_t chosen = 0;   // black: only the alpha byte matters
    for (int64_t f : formats)
      if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
          f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { chosen = f; break; }
    if (!chosen) { g_dimErr = 1; return false; }
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = chosen;
    sci.sampleCount = 1;
    sci.width = kDimPx;
    sci.height = kDimPx;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(sc_create("dim layer", &sci, &g_dimSwap))) { g_dimSwap = XR_NULL_HANDLE; g_dimErr = 2; return false; }
    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(g_dimSwap, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(g_dimSwap, imgCount, &imgCount, (XrSwapchainImageBaseHeader *)imgs.data());
    for (auto &i : imgs) g_dimImages.push_back(i.texture);
  }
  if (g_dimDrawn == a) return true;
  uint32_t idx = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(g_dimSwap, &ai, &idx))) return false;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  const bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(g_dimSwap, &wi)) && idx < g_dimImages.size();
  if (ok) {
    uint32_t px[kDimPx * kDimPx];
    const uint32_t v = (uint32_t)(a * 255.0f + 0.5f) << 24;   // black, premultiplied
    for (uint32_t &p : px) p = v;
    g_ctx->UpdateSubresource(g_dimImages[idx], 0, nullptr, px, kDimPx * 4, 0);
  }
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(g_dimSwap, &ri);
  if (!ok) return false;
  g_dimDrawn = a;
  return true;
}
bool dim_layer(XrCompositionLayerQuad &q) {
  const float step = 1.0f / 12.0f;
  const bool want = g_pauseLive && g_pauseDim > 0.0f;
  g_pauseDimAmt = want ? (g_pauseDimAmt + step > 1.0f ? 1.0f : g_pauseDimAmt + step)
                       : (g_pauseDimAmt - step < 0.0f ? 0.0f : g_pauseDimAmt - step);
  const float base = 0.0f;   // PANELTIDY5: the depth-of-field-off darkening is gone (depth of field stays on)
  if (g_hieDimDone) { g_hieDimDone = false; return false; }   // WHEELEYE: already in the eye pictures, under the HUD
  // FULLCUT2 2026-10-10: full-screen cuts are drawn into the eye pictures only (hud_in_eyes), never by this layer - the
  // first FULLCUT build made this layer create its swapchain at the main menu's black start screen and Virtual Desktop
  // crashed on that creation (OpenXR.log 01:42:07 xrCreateSwapchain ovrResult -7000, then a fail-fast in its DLL)
  if ((g_pauseDimAmt <= 0.0f && base <= 0.0f) || g_viewSpace == XR_NULL_HANDLE) return false;
  // 64 levels, so a fade rewrites the image a handful of times only
  const float a = floorf((1.0f - (1.0f - base) * (1.0f - g_pauseDim * g_pauseDimAmt)) * 64.0f + 0.5f) / 64.0f;
  if (a <= 0.0f || !dim_fill(a)) return false;
  q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  q.space = g_viewSpace;
  q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  q.subImage.swapchain = g_dimSwap;
  q.subImage.imageArrayIndex = 0;
  q.subImage.imageRect.offset = {0, 0};
  q.subImage.imageRect.extent = {kDimPx, kDimPx};
  q.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  q.pose.position = {0.0f, 0.0f, -kVigDist};
  q.size = {kVigSize, kVigSize};
  return true;
}
// WHEELEYE: the dim layer's current darkness (same sum, without stepping the fade)
float hie_dim_alpha() {
  const float base = 0.0f;   // PANELTIDY5
  if (g_pauseDimAmt <= 0.0f && base <= 0.0f) return 0.0f;
  return floorf((1.0f - (1.0f - base) * (1.0f - g_pauseDim * g_pauseDimAmt)) * 64.0f + 0.5f) / 64.0f;   // (FULLCUT5: the cut is added on the graphics card)
}
XrCompositionLayerQuad g_dimQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
bool g_dimHas = false;
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
float akvr_xr_eye_v_offset() { return akvr_engine_stereo() ? 0.0f : g_vOffNdc; }   // GEO11OFF: AKN builds each eye's own off-centre lens
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
    // FPS72: the refresh-rate extension, only when the runtime lists it (the 72 lock asks
    // the headset for 72 Hz through it; without it the panel tells the player to set VD).
    g_fbRefresh = runtime_has_ext(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
                          XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "AKVR");
    ci.applicationInfo.apiVersion =
        XR_MAKE_VERSION(1, 0, 0); // 1.0 = universally supported
    ci.enabledExtensionCount = g_fbRefresh ? 2 : 1;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r) && g_fbRefresh) {   // FPS72: never let the optional extension cost the session
      g_fbRefresh = false;
      g_inst = XR_NULL_HANDLE;
      ci.enabledExtensionCount = 1;
      r = xrCreateInstance(&ci, &g_inst);
    }
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
int    g_fpsLock = 0;          // 0 = off, else the target (120 / 100 / 90 / 80 / 72 / 60 / 50 / 45 / 40 / 36 / 30)
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
// FPSMORE 2026-10-05: 90 / 80 / 60 / 36 added (JJ: "also add 90, 80, 60, and 36"), then 120, 50, 100,
// 48 and 96. FPSSLIDER: the panel is a slider that stops only on these (ascending).
static const int kLockRates[] = {30, 36, 40, 45, 48, 50, 60, 72, 80, 90, 96, 100, 120};
int akvr_xr_fps_lock_choices(const int **list) {
  *list = kLockRates;
  return (int)(sizeof(kLockRates) / sizeof(kLockRates[0]));
}
void akvr_xr_fps_lock_set(int fps) {
  g_fpsLock = 0;
  for (int r : kLockRates)
    if (fps == r) g_fpsLock = fps;
}

// FPS72 2026-10-05 — JJ: "Add a setting to lock the frame rate at 72 FPS." The lock holds
// every game frame for a WHOLE number of refreshes (see FPSLOCK above), so a rate is only even
// when the headset's refresh is a multiple of it: 72 at 90 Hz rounds to every refresh (= no
// lock), and a 72-in-90 mix is exactly the 1-or-2-refresh judder FPSLOCK was built to remove.
// FPSMORE 2026-10-05 (90 / 80 / 60 / 36 added) made this general: when a lock is picked and the
// headset's rate is not a multiple of it, we ask for the LOWEST listed rate that is (60 -> 120,
// 36 -> 72, 30 -> 90; 45 at 90 Hz is left alone), through XR_FB_display_refresh_rate (playbook
// PERF-004: only when advertised, every call checked). "off" puts the original rate back.
// Without the extension the panel tells the player which rate to set in Virtual Desktop.
static PFN_xrRequestDisplayRefreshRateFB s_reqRate = nullptr;
static PFN_xrGetDisplayRefreshRateFB s_getRate = nullptr;
static PFN_xrEnumerateDisplayRefreshRatesFB s_enumRates = nullptr;
static XrSession s_rateSession = XR_NULL_HANDLE;
static float s_rateOrig = 0.0f;   // the headset's rate before our first request (0 = nothing to restore)
static int s_lockDone = -1;       // the lock value already handled, so each change is acted on once
static int s_lockSeen = -1;       // FPSSLIDER: the lock last seen, and since when (settle before switching)
static ULONGLONG s_lockSince = 0;
static char g_rateDiag[160] = "";
static bool rate_fits(float hz, int lock) {   // hz is a whole multiple of the lock
  const int k = (int)floorf(hz / (float)lock + 0.5f);
  return k >= 1 && fabsf(hz - (float)(k * lock)) < 0.5f;
}
static void refresh_rate_tick() {
  if (!g_fbRefresh || g_session == XR_NULL_HANDLE)
    return;
  if (s_rateSession != g_session) {   // a new session: resolve the calls once
    s_rateSession = g_session;
    s_rateOrig = 0.0f;
    s_lockDone = -1;
    s_lockSeen = -1;
    s_reqRate = nullptr; s_getRate = nullptr; s_enumRates = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_inst, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&s_reqRate)))
      s_reqRate = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_inst, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&s_getRate)))
      s_getRate = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_inst, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&s_enumRates)))
      s_enumRates = nullptr;
    if (!s_reqRate || !s_getRate || !s_enumRates)
      snprintf(g_rateDiag, sizeof(g_rateDiag), "refresh-rate calls missing from the runtime");
  }
  if (!s_reqRate || !s_getRate || !s_enumRates)
    return;
  const int lock = fps_lock_eff();
  if (lock != s_lockSeen) {
    s_lockSeen = lock;
    s_lockSince = GetTickCount64();
  }
  if (lock == s_lockDone)
    return;
  // FPSSLIDER: wait until the slider has rested for a second - one headset switch, not one per step.
  if (s_lockDone != -1 && GetTickCount64() - s_lockSince < 1000)
    return;
  s_lockDone = lock;
  float cur = 0.0f;
  if (XR_FAILED(s_getRate(g_session, &cur)))
    cur = 0.0f;
  if (lock == 0) {   // off: put back what the headset had before our first request
    if (s_rateOrig > 0.0f && fabsf(cur - s_rateOrig) >= 0.5f) {
      const XrResult r = s_reqRate(g_session, s_rateOrig);
      snprintf(g_rateDiag, sizeof(g_rateDiag), "put the headset back to %.0f Hz (was %.0f): %s", s_rateOrig, cur,
               XR_SUCCEEDED(r) ? "accepted" : "refused");
    }
    s_rateOrig = 0.0f;
    return;
  }
  if (cur > 0.0f && rate_fits(cur, lock)) {
    snprintf(g_rateDiag, sizeof(g_rateDiag), "headset at %.0f Hz already suits %d fps", cur, lock);
    return;
  }
  uint32_t n = 0;
  float rates[16] = {};
  float best = 0.0f;
  if (XR_SUCCEEDED(s_enumRates(g_session, 16, &n, rates)))
    for (uint32_t i = 0; i < n && i < 16; ++i)
      if (rates[i] >= (float)lock - 0.5f && rate_fits(rates[i], lock) && (best == 0.0f || rates[i] < best))
        best = rates[i];
  if (best == 0.0f) {
    snprintf(g_rateDiag, sizeof(g_rateDiag), "no headset rate shows %d fps evenly (%u rates listed, now %.0f Hz)", lock, n, cur);
    return;
  }
  if (s_rateOrig <= 0.0f)
    s_rateOrig = cur;
  const XrResult r = s_reqRate(g_session, best);
  snprintf(g_rateDiag, sizeof(g_rateDiag), "asked the headset for %.0f Hz (was %.0f): %s", best, cur,
           XR_SUCCEEDED(r) ? "accepted" : "refused");
}
bool akvr_xr_refresh_ext() { return g_fbRefresh; }
const char *akvr_xr_refresh_diag() { return g_rateDiag; }
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
  space_choose(true);   // SVRSPACE: live switch
  if (g_state == XR_SESSION_STATE_EXITING ||
      g_state == XR_SESSION_STATE_LOSS_PENDING) {
    g_running = false;
    return;
  }
  if (!g_running)
    return;
  refresh_rate_tick();   // FPS72: does nothing unless the lock moved to or from 72

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
      const XrCompositionLayerBaseHeader *rl[10];
      uint32_t rn = 0;
      if (g_rpHasProj) rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_rpLayer;
      if (g_rpHasProj && g_scrQuadNow && akvr_engine_stereo())   // SCREENQUAD2: one quad
        rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_scrQuad;
      if (g_rpHasProj && g_dimHas) rl[rn++] = (const XrCompositionLayerBaseHeader *)&g_dimQuad;   // PAUSEDIM
      // HUDLAYER4: the HUD is re-placed for THIS frame's display time (head-locked at 90 Hz).
      if (g_rpHasProj && g_rpHasHud && hud_place(g_rpHud, fs.predictedDisplayTime)) {
        if (g_hudEyes && !(g_roomScrNow && g_hudOnScreen)) {   // HUDLAYER5 (SCREENQUAD2: not on a room screen)
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
      const bool tilt = !akvr_engine_stereo() && g_tiltFill && g_fullView && !g_projvrOn && g_native && !g_forceScreen &&   // GEO11OFF
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
      // PAUSESTAMP: engine stereo stamps pictures with finalizes + pause-look camera writes
      const uint64_t fc = akvr_engine_stereo() ? akvr_camera_write_count() : akvr_camera_finalize_count();
      if (g_finRecorded == 0 || fc < g_finRecorded || fc - g_finRecorded > 200)
        g_finRecorded = fc;                        // first call, or a reset: no history
      while (g_finRecorded < fc) {
        ++g_finRecorded;
        g_seqForFin[g_finRecorded & 255u] = g_poseHead;
      }
    }
    // POSEJUMP (SVRSPACE): no head turns 25 deg or moves 25 cm in one frame - such a jump is the tracking space
    // itself moving (SteamVR's silent LOCAL switch, any runtime recentre without an event): re-zero like a recentre.
    {
      static bool s_prevOk = false; static XrPosef s_prev{};
      const bool ok = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
      if (ok && s_prevOk) {
        const XrQuaternionf &a = s_prev.orientation, &b = newPose.orientation;
        const float d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        const float deg = 2.0f * acosf(d > 1.0f ? 1.0f : d) * 57.29578f;
        const float dx = newPose.position.x - s_prev.position.x, dy = newPose.position.y - s_prev.position.y,
                    dz = newPose.position.z - s_prev.position.z;
        const float m = sqrtf(dx * dx + dy * dy + dz * dz);
        if (deg > 25.0f || m > 0.25f) {
          ++g_poseJumps; g_recenterPending = true; g_recenterAt = 0;
          char j[64]; snprintf(j, sizeof(j), "pose jump %.0f deg %.2f m: re-zeroed", deg, m); mode_log(j);
        }
      }
      if (ok) { s_prevOk = true; s_prev = newPose; }
    }
    g_poseHist[++g_poseHead & 63u] = newPose;
    // Which pose drew the picture being presented now? Exactly, when the engine's
    // own frame counters are found (frameid.cpp); otherwise the fixed delay JJ set.
    uint32_t delay = (uint32_t)(akvr_xr_menu3d_active() ? g_menuPoseDelay : g_poseDelay);   // MENUDELAY
    // PAUSESTAMP 2026-10-09: in engine stereo the exact delay is 1 (run 14: 4370 of 4370 stamped pictures); the geo-11
    // fixed delay (JJ's ini: 2) only ever applied when a stamp failed - e.g. all through a pause - and was then wrong
    if (akvr_engine_stereo()) delay = 1;
    g_poseMatched = false;
    uint64_t pf = 0;
    if (g_poseAuto && akvr_frameid_presented_finalize(pf) && pf <= g_finRecorded &&
        g_finRecorded - pf < 200) {
      const uint32_t d = g_poseHead - g_seqForFin[pf & 255u];
      if (d >= 1 && d <= 30) { delay = d; g_poseMatched = true; }
    }
    // POSESTAMP (JJ's 4th Quest test: jitter on head turns): in engine stereo the pair names the camera finalize its
    // views were built from; present it with exactly the pose that finalize consumed.
    if (akvr_engine_stereo()) {
      static long s_match = 0, s_total = 0;
      ++s_total;
      const unsigned long long st = akn_get_pair_stamp();
      if (st && st <= g_finRecorded && g_finRecorded - st < 200) {
        const uint32_t d = g_poseHead - g_seqForFin[st & 255u];
        if (d >= 1 && d <= 30) { delay = d; g_poseMatched = true; ++s_match; }
      }
      akn_send_pose_report(s_match, s_total, (long)delay);
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
        // HUDEYES: both eyes' positions, LOCAL space, display time (for hud_eye_pair); the distance logged once
        if (vst.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) {
          g_eyeLocPos[0] = vv[0].pose.position; g_eyeLocPos[1] = vv[1].pose.position; g_eyeLocOk = true;
          g_eyeLocDistMm = 1000.0f * ipd;
          static bool s_logged = false;
          if (!s_logged) { s_logged = true; char m[48]; snprintf(m, sizeof(m), "HUD eyes %.1f mm", g_eyeLocDistMm); mode_log(m); }
        }
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
      // AKN M5: each eye renders exactly its own lens (asymmetric tangents) from its own position: half the measured
      // eye distance either side of the head, in game units (world scale = camera.cpp's lean gain, 1.00 = 1:1).
      if (akvr_engine_stereo()) {
        float tan8[8];
        for (int e = 0; e < 2; ++e) {
          tan8[e * 4 + 0] = tanf(vv[e].fov.angleLeft);
          tan8[e * 4 + 1] = tanf(vv[e].fov.angleRight);
          tan8[e * 4 + 2] = tanf(vv[e].fov.angleUp);
          tan8[e * 4 + 3] = tanf(vv[e].fov.angleDown);
        }
        // MENUCLOSE (JJ's Quest test 2026-10-07: "Batman is completely out of focus" in the main menu): the main menu's
        // camera sits centimetres from Batman - true eye separation there cannot be fused. In the main menu both eyes
        // render from ONE spot with the game's own lens and AKVR shows that one picture to both eyes (as under geo-11).
        // JJ's 2nd test: the game's own lens made head motion look exaggerated in the menu; now both eyes render from
        // ONE spot (no depth to fuse) but each through its own real lens, and are shown exactly as rendered (1:1 motion).
        const bool menuFlatNow = menu_one_spot();   // MENU3D: only when asked for
        const bool flatScr = g_mapFlat && g_aknFlatScreen;   // MAPFLAT: the game's own lens, both eyes at the head
        const float half = (menuFlatNow || flatScr) ? 0.0f : 0.5f * akn_ipd_m() * akvr_head_pos_scale();
        const float off2[2] = {-half, half};
        akn_send_eyes(tan8, off2, flatScr ? 0 : g_aknLensMode);   // TURNEDEYE: 2 = centred lens on a turned eye camera (default), 1 = off-centre
      }
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

// PERFTIME 2026-10-10 (PERFSWEEP: "HUD in the eyes off" = +8.8 pairs/s, and the graphics card was idle 12 % more with
// it on = the render thread waits, not graphics work): processor time per frame of this whole function and of the HUD
// step, averaged per second, printed by AKN (akvr_perf_diag).
void pt_report() {
  static ULONGLONG s_t = 0; const ULONGLONG now = GetTickCount64();
  if (now - s_t < 1000) return;
  s_t = now;
  snprintf(g_ptDiag, sizeof(g_ptDiag), "VR frame (processor): submit %.2f ms | HUD in the eyes %.2f ms (markers part %.2f, drawing part %.2f) | %ld frames, %ld HUD",
           g_ptN ? g_ptSubmit / g_ptN : 0.0, g_ptHieN ? g_ptHie / g_ptHieN : 0.0, g_ptHieN ? g_ptHieA / g_ptHieN : 0.0, g_ptHieN ? g_ptHieB / g_ptHieN : 0.0, g_ptN, g_ptHieN);
  g_ptSubmit = g_ptHie = g_ptHieA = g_ptHieB = 0; g_ptN = g_ptHieN = 0;
}
struct PtScope { LONGLONG a = pt_now(); ~PtScope() { g_ptSubmit += pt_ms(a, pt_now()); ++g_ptN; pt_report(); } };
void akvr_xr_frame_submit(IDXGISwapChain *swapChain, float gameFovDeg,
                          bool gameplay) {
  if (!g_frameBegun)
    return; // nothing was begun this Present
  PtScope ptScope;   // PERFTIME
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
    // MENUGAP 2026-10-02 — JJ: from the start-up screens into the first menu "there's a pause of a few seconds where
    // everything's black, but the audio continues". Every gap of 300 ms+ between frames is logged until the main menu
    // has closed: frames stopping = the game is loading; frames running = it draws black.
    static ULONGLONG s_lastFrame = 0;
    const ULONGLONG tf = GetTickCount64();
    static int s_gaps = 0;   // at most 12, so the timeline keeps room
    if (s_lastFrame && tf - s_lastFrame >= 300 && (!g_autoMainMenu || g_menuPhase <= 1) && s_gaps < 12) {
      ++s_gaps;
      char gap[40]; snprintf(gap, sizeof(gap), "no frame %llu ms", (unsigned long long)(tf - s_lastFrame));
      mode_log(gap);
    }
    s_lastFrame = tf;
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
  // PAUSELOOK 2026-09-30 — JJ: the pause as in Sekiro, "the whole 360 world to freeze, darken, and then add the pause
  // screen menus over the top", instead of "a scaled down window floating in black space". A pause starts straight
  // out of gameplay, after the main menu, with the world still 3D and still drawn through the player camera
  // (camera.cpp: the main view's FOV = the camera's; the map draws from its own camera and keeps the old window).
  // Live: shown exactly like gameplay (full view, the head drives the paused camera, camera.cpp pause_look_write),
  // darkened (PAUSEDIM), and the whole UI on the room layer, hung in front of where you look, at the pause size.
  // CAMRUN 2026-10-01: "the camera is running" = 3 finalize changes within 0.5 s, the newest under 150 ms old (used by
  // PAUSELOOK's end, MENUFIRST and the fast way back into gameplay below).
  static uint64_t s_mfFc = 0; static ULONGLONG s_mfTicks[3] = {};
  {
    const uint64_t fc = akvr_camera_finalize_count();
    if (fc != s_mfFc) { s_mfFc = fc; s_mfTicks[2] = s_mfTicks[1]; s_mfTicks[1] = s_mfTicks[0]; s_mfTicks[0] = GetTickCount64(); }
  }
  const ULONGLONG nowMf = GetTickCount64();
  const bool camTicking = s_mfTicks[2] && nowMf - s_mfTicks[0] < 150 && s_mfTicks[0] - s_mfTicks[2] < 500;
  {
    // It may start within 1 s of gameplay ending (the FOV check can lag the pause by a few frames), never later:
    // once a screen has settled as a window, it stays one.
    static ULONGLONG s_lastGame = 0;
    const ULONGLONG nowMs = GetTickCount64();
    // PAUSEFAST 2026-10-01 — JJ: "I can still see the black overlay appear briefly when I hit pause". The smoothed
    // gameplay verdict drops ~0.2 s after the camera stops, and until then the pause menu (with its dark backing) was
    // drawn on the gameplay HUD layer. The camera's own stop is the earlier sign: 3 Presents without a finalize.
    static uint64_t s_pFc = 0; static int s_pStill = 0;
    {
      const uint64_t fc = akvr_camera_finalize_count();
      s_pStill = fc == s_pFc ? s_pStill + 1 : 0;
      s_pFc = fc;
    }
    // PAUSEEDGE 2026-10-03 - JJ: "there's a flicker when entering and exiting pause". F2 01:08:54 (mode.csv 51.33 s):
    // the first paused frames were drawn from the camera CAMRESTORE had handed back (no head, the game's 57 deg FOV)
    // until the pause look started 3 Presents later. Now camera.cpp writes the drawn camera from the first Present
    // without a finalize (g_pauseCand), the pause look starts after 2, and the picture of the first 2 is held.
    g_stillPresents = s_pStill;
    const bool camStopped = s_pStill >= 2;
    const bool fits = g_pauseLook && g_anamorphic && !g_forceScreen && !akvr_xr_screen_mode() &&
                      (!g_autoMainMenu || g_menuPhase >= 2) && akvr_camera_main_view_live();
    // MAPFLY 2026-10-06 - JJ: the map "flies in and rotates from the bottom ... all the HUD elements are strangely flying
    // in". F2 16:34 mode timeline: every map open went "pause look on" -> 0.1 s "screen" -> "pause look off": for its
    // first frames the map still passes the pause test (the game draws the player view while it fades), so the pause
    // look started (world frozen, HUD on its room layer at the pause anchor) and then gave way to the floating screen
    // at another place and size. The pause look now starts only after the test has held for 250 ms; the eye picture
    // is held meanwhile (pauseHold below), so a real pause still freezes cleanly and the map goes straight to its screen.
    static ULONGLONG s_fitsSince = 0;
    if (!fits) s_fitsSince = 0; else if (!s_fitsSince) s_fitsSince = nowMs;
    g_pauseWaiting = fits && !g_pauseLive && s_fitsSince && nowMs - s_fitsSince < 250;
    // UNPAUSE 2026-10-01 — JJ: "when exiting the pause menu, I'm seeing the entire thing being framed in a small window
    // before it pops back out to the full 360". It ended on the first camera tick, but gameplay was only shown again
    // once the camera was clearly running (or the slower verdict): a few frames fell back to the floating window.
    // The pause now holds until the camera is clearly running; from there CAMRUN shows gameplay (below).
    // UNPAUSE2 (JJ: "the pause menu has gone back to being within a smaller window. That's a regression"): camTicking
    // still counted the LAST ticks before the pause (newest < 150 ms), so the pause ended on its next frame and could
    // not restart. It ends only on a NEW tick (s_pStill == 0) with the camera clearly running.
    if (g_pauseLive) {
      if (!fits || (s_pStill == 0 && camTicking)) { g_pauseLive = false; mode_log("pause look off"); hud_pause_anchor(false); s_lastGame = 0; if (akvr_engine_stereo()) akn_command("hudskipon 0"); }
    } else if (fits && camStopped && s_lastGame && nowMs - s_lastGame < 1000 && nowMs - s_fitsSince >= 250) {
      g_pauseLive = true; mode_log("pause look on"); hud_pause_anchor(true); if (akvr_engine_stereo()) akn_command("hudskipon 1");   // HUDSKIP: pause / wheel backdrops off
    }
    if (gameplay && !camStopped && !akvr_xr_screen_mode()) s_lastGame = nowMs;
    g_pauseCand = g_pauseLive || (fits && s_lastGame && nowMs - s_lastGame < 1000);
  }
  // MENUFIRST 2026-10-01 — JJ: "when exiting out of the title screens and coming into the main menu, for a second or two,
  // before the three D camera calibrates, it shows Batman at the very bottom of the screen". His F2: the menu camera ran
  // (raw camera verdict 1 from 12.72 s) but the smoothed gameplay verdict came only at 13.23 s, so the first 3D menu
  // frames were shown as the small flat start-up screen - the menu shot drawn flat, Batman at its bottom. Before the
  // main menu is confirmed (phase 0) every start-up screen is flat, so a 3D frame can only be the menu: shown live.
  // The camera's finalize count rising is the earliest sign (12.72 s in that F2; the 3D flag came at 13.0 s): CAMRUN.
  const bool menuFirst3D = g_autoMainMenu && g_menuPhase == 0 && (g_anamorphic || camTicking) && !g_forceScreen;
  // CAMRUN: a running camera with a 3D picture is gameplay at once (the smoothed verdict follows ~0.2-0.5 s later).
  const bool camRun3D = camTicking && g_anamorphic && !g_forceScreen;
  bool effGameplay = (gameplay || g_pauseLive || menuFirst3D || camRun3D) && !akvr_xr_screen_mode();
  // MAPFLAT: a floating screen over a 3D picture (map, plain pause) - AKN draws its next frames the flat game's way
  g_aknFlatScreen = akvr_engine_stereo() && !effGameplay && !g_pauseWaiting && !akvr_xr_main_menu_detected() && !g_forceScreen;
  g_hudOnScreen = g_mapFlat && akvr_engine_stereo() && !effGameplay;
  {   // MENUSTART: when the 3D main menu first comes up, AKVR captures it (hooks.cpp) - JJ still sees "the view of
      // Batman from the top at the bottom of the screen" there, too briefly for F2.
    static bool s_msDone = false;
    if (!s_msDone && g_autoMainMenu && g_menuPhase == 0 && (g_anamorphic || camTicking)) { s_msDone = true; g_menuStartTick = GetTickCount64(); }
  }
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
    g_hudMenuNow = menuLive || g_pauseLive;   // MENUSIZE; PAUSELOOK: the pause menu too (whole UI, pause size)
    // HUDNOW 2026-09-30 — JJ: entering the game "the HUD elements are attached to your face and then it takes a couple
    // of seconds before they lock onto the world". The 1.5 s wait is the eye view's (EVGAME); the layer starts at once.
    akvr_hudsplit_layer_gate(gp && !menuLive && !g_pauseLive, menuLive || g_pauseLive);
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
    // MAPUPRIGHT 2026-10-06 - JJ: "The map screen should open vertically in world space, not according to head tilt".
    // Since PAUSELOOK the pause shows the live world, so this frozen picture is mostly the map (drawn by the game's own
    // map camera, not the head): hang it upright, straight ahead. The drawn pose is kept only with the pause look off.
    g_fixedPose = (g_anamorphic && !g_pauseLook) ? drawn : level_pose(g_curPose);
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
  // PAUSEEDGE: the first 2 Presents without a finalize show a picture drawn from the handed-back camera: keep the last one
  const bool pauseHold = g_pauseCand && g_stillPresents >= 1 && (g_stillPresents <= 2 || g_pauseWaiting);   // MAPFLY: also while the pause test settles
  const bool holdScreen = ((g_holdScreen && !effGameplay) || pauseHold) && g_swapInit[0] && g_swapInit[1];
  if (g_shouldRender && swapChain && g_ctx && !holdScreen) {
    ID3D11Texture2D *backBuffer = nullptr;
    bool ownBackBuffer = false;
    bool fromKatanga = false;
    // GEO-11 NATIVE STEREO: the stereo pair does NOT live in the swapchain.
    // geo-11's katanga_vr mode publishes a separate shared surface ("DoubleTex",
    // the window doubled in WIDTH = side-by-side) and puts an OVER/UNDER preview
    // on the monitor. Judging the layout by the screen is the trap that cost us
    // a session — always take the shared surface. See geo11-works-on-ak-fresh-install.
    g_straightNow = false;   // SVRSTRAIGHT
    static int s_gapFrames = 0;   // BLACKGAP: gameplay frames since the last pair (0 outside gameplay)
    const bool aknOk = akvr_engine_stereo() && g_native && aknSbsUpdate(swapChain, effGameplay && s_gapFrames < 180);
    s_gapFrames = (!effGameplay || g_aknFresh) ? 0 : (s_gapFrames < 100000 ? s_gapFrames + 1 : s_gapFrames);
    if (aknOk) {
      backBuffer = g_aknSbs;     // AKN M5: the game's own two eyes side by side (or the flat picture twice)
      fromKatanga = true;        // SBS by construction: no probe
      // HUDINEYE: gameplay HUD drawn into the eyes with the game's own blend (menus / pause / flat frames keep the layer)
      // WHEELEYE 2026-10-08 (JJ: the weapon wheel "still banding and posterized" with HUDINEYE): opening the wheel stops
      // the game camera, so the pause look goes live (captures 19:44 / 19:59: "pause look LIVE now", camera 3.5 s without
      // a finalize) and the HUD fell back to the linear-blended quad layer. The pause look's HUD is drawn into the eyes too.
      g_trFullNow = tr_decide();   // TRANSFULL
      const bool hieNormal = g_hudInEye && g_aknFresh && effGameplay && (!g_hudMenuNow || g_pauseLive) && g_hudColour != 0;
      g_hieThisFrame = (hieNormal || (g_trFullNow && g_aknFresh)) && hud_in_eyes(g_aknSbs, submitPose);
      if (g_hieThisFrame) g_sbsHasHud = true;   // PATHREC
      g_prSbsShown = true;   // PATHREC: this frame shows g_aknSbs
      // SVRSTRAIGHT: the same frames the views loop declares turned eyes for (keep the two conditions identical)
      g_straightNow = straight_wanted() && g_aknFresh && g_rawFovOk && (effGameplay || akvr_xr_main_menu_detected() || g_pauseLive) &&
                      straighten();
      if (g_straightNow) backBuffer = g_aknStraight;
    } else if (g_native && g_katFeed && g_katTex) {
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
              bright_probe(backBuffer, g_eyeW, g_swH);   // STARTBRIGHT: start-up only
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
                if ((g_menuFlat && !effGameplay && !akvr_engine_stereo()) || (akvr_engine_stereo() && !effGameplay && !akvr_xr_main_menu_detected() && !g_pauseLive)) sourceEye = swap_eff() ? 1 : 0;   // engine mode: floating screens flat
                if (g_roomScrNow && akvr_engine_stereo()) sourceEye = e;   // SCREENROOM: each half is drawn for its own eye
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
  // LOADSIZE / WHOLEFRAME: the kind of screen is latched too - the menu phase closes 5 s into the loading screen,
  // which must not switch its size or framing half way (the SIZELATCH jump).
  static bool s_latchedSplash = false, s_latchedLoad = false;
  if (!effGameplay && s_wasEff) {
    s_latchedMenuSize = akvr_xr_screen_mode();
    s_latchedSplash = !g_forceScreen && g_autoMainMenu && g_menuPhase == 0;
    s_latchedLoad = !g_forceScreen && g_autoMainMenu && g_menuPhase == 1 && !akvr_xr_screen_mode();
  }
  if (akvr_xr_screen_mode()) s_latchedMenuSize = true;   // F6 / menu mid-screen: menu size
  // LOADALL 2026-09-30 — JJ: the loading screens "aren't any smaller". In-game loads (fast travel, death, quit to
  // menu) had the pause size. The mode log tells them apart: a loading screen runs FLAT (no 3D camera), pause and map
  // keep the frozen 3D picture. Once a screen after the main menu goes flat it is a loading screen until gameplay.
  if (!effGameplay && !g_forceScreen && !s_latchedMenuSize && !g_anamorphic &&
      (!g_autoMainMenu || g_menuPhase >= 2))
    s_latchedLoad = true;
  s_wasEff = effGameplay;
  const bool splash = s_latchedSplash && !g_forceScreen;   // SPLASHSIZE
  const bool loadIn = s_latchedLoad && !g_forceScreen;     // LOADSIZE
  g_nearScreenNow = !effGameplay && (splash || loadIn) && g_nearScreenM > 0.1f;   // SCREENNEAR
  {   // SCR30 2026-10-11: the game's own frame rate per kind of screen (JJ: title and loading screens run at ~30/s)
    const int kind = splash && !effGameplay ? 0 : akvr_xr_main_menu_detected() ? 1 : loadIn && !effGameplay ? 2 : effGameplay ? 3 : 4;
    static int s_kind = -1; static ULONGLONG s_t0 = 0; static long s_n = 0;
    const ULONGLONG now = GetTickCount64();
    if (kind != s_kind) {
      if (s_kind >= 0 && now - s_t0 > 700) {
        static const char *nm[5] = {"start-up/title", "main menu", "loading", "gameplay", "other screen"};
        char m[64]; snprintf(m, sizeof(m), "%s %.0f frames/s (%.0fs)", nm[s_kind], s_n * 1000.0 / (double)(now - s_t0), (now - s_t0) / 1000.0);
        mode_log(m);
      }
      s_kind = kind; s_t0 = now; s_n = 0;
    }
    ++s_n;
  }
  float fovScale = effGameplay ? 1.0f
                   : (g_forceScreen ? g_floatScreenScale
                      : (splash ? g_splashFovScale
                         : (loadIn ? g_loadFovScale : (s_latchedMenuSize ? g_screenFovScale : g_pauseFovScale))));
  // SCREENTAN 2026-09-30 — JJ: the loading screens "feel a bit too stretched vertically". The sizes scaled the
  // half-ANGLES, which is not a uniform scale of the picture: at this FOV (~52 deg half) and 70% the width shrinks
  // more than the height and a 16:9 screen showed ~13% too tall (SKVR's SCREENTAN lesson, same code here). Keep each
  // screen's width exactly (horizontal half-angle x size, as tuned) and scale the TANGENTS uniformly by the same
  // factor, so the picture keeps its true shape.
  const float kTan = fovScale < 0.999f && halfH > 0.01f ? tanf(halfH * fovScale) / tanf(halfH) : 1.0f;
  // Publish what we ACTUALLY submit, so the startup log can put it beside what
  // projVR left in the render matrix. Those two must agree or the picture is
  // stretched by exactly the ratio of their tangents.
  g_subHalfH = halfH * fovScale;
  g_subHalfV = atanf(tanf(halfV) * kTan);
  XrFovf fov;
  fov.angleRight = halfH * fovScale;
  fov.angleLeft = -halfH * fovScale;
  fov.angleUp = g_subHalfV;
  fov.angleDown = -g_subHalfV;
  // FULLVIEW: the projection was shifted by g_vOffNdc (camera.cpp adds it to m[9]),
  // so its top edge is at tan = (1 - o) * tan(halfV) and its bottom at (1 + o) *
  // tan(halfV). Submit exactly those edges or the world swims when you look up/down.
  if (g_vOffNdc != 0.0f && g_projvrOn && (effGameplay || g_anamorphic)) {
    const float t = tanf(halfV);
    fov.angleUp = atanf((1.0f - g_vOffNdc) * t * kTan);
    fov.angleDown = -atanf((1.0f + g_vOffNdc) * t * kTan);
  }
  // SCREENWIDE: crop the floating screen to a wider shape (see g_screenAspect).
  float vFrac = 1.0f;
  // PAUSE169 2026-09-27 — JJ: "the pause screen and the map screen are too high, they should be
  // restricted to 16 by 9 because the mask that's over them is restricted to that". The render is
  // taller than 16:9 (3560x3120, or 2864x3120 with the eye view), so every non-gameplay screen is
  // cropped to 16:9, centred; a floating F6 screen keeps its own shape setting.
  // WHOLEFRAME 2026-09-30 — JJ: the start-up logo's bottom lines were cut off (F2: the game draws these full-screen
  // pictures filling most of the frame's height; the 16:9 slice kept only the middle). Screens before gameplay has
  // started (logos, the loading screen after the main menu) show the whole frame at its real shape: nothing cut,
  // nothing stretched. Pause and map keep the 16:9 slice (their mask is 16:9, PAUSE169).
  const bool preGame = splash || loadIn;
  const float scrAspect = g_forceScreen ? g_screenAspect : (!effGameplay && !preGame ? g_pauseAspect : 0.0f);
  if (scrAspect > 0.5f) {
    const float tH = tanf(halfH), tV = tanf(halfV);
    if (tH > 0.0f && tV > 0.0f && tH / scrAspect < tV)
      vFrac = (tH / scrAspect) / tV;
  }
  if (vFrac < 0.999f) {
    fov.angleUp = atanf(tanf(halfV) * vFrac * kTan);
    fov.angleDown = -fov.angleUp;
  }
  g_scrVFrac = effGameplay ? 1.0f : vFrac; g_scrCropW = effGameplay ? 1.0f : cropWidthRatio;   // MAPFLAT: the HUD sheet gets the same crop
  if (loadIn && !effGameplay && g_loadUpDeg != 0.0f) {   // LOADUP
    const float t = tanf(g_loadUpDeg * (3.14159265f / 180.0f));
    fov.angleUp = atanf(tanf(fov.angleUp) + t);
    fov.angleDown = atanf(tanf(fov.angleDown) + t);
  }
  if (g_nearScreenNow) {   // SCREENROOM: the screen's window, for drawing it into the eyes next frame
    g_roomScrT[0] = tanf(fov.angleLeft); g_roomScrT[1] = tanf(fov.angleRight);
    g_roomScrT[2] = tanf(fov.angleUp); g_roomScrT[3] = tanf(fov.angleDown);
    g_roomScrTOk = true;
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
      if ((g_menuFlat && !effGameplay && !akvr_engine_stereo()) || (akvr_engine_stereo() && !effGameplay && !akvr_xr_main_menu_detected() && !g_pauseLive)) sourceEye = swap_eff() ? 1 : 0;   // engine mode: floating screens flat
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
    // AKN M5: in gameplay each half IS that eye's own render: its lens exactly as the runtime gave it (declared =
    // rendered, VR_ESSENTIALS 3) and its own position, half the eye distance along the head's right from the head
    // pose the frame was drawn with.
    // engine mode, a fresh pair: each eye shown exactly as rendered - its own lens, its own position (the main menu: both
    // eyes at the head, as AKN rendered them there). Menus and pause too, not just gameplay: a picture drawn through the
    // per-eye lens must be declared with that lens, or head motion stretches it (JJ's 2nd test, main menu).
    if (akvr_engine_stereo() && g_aknFresh && g_rawFovOk && (effGameplay || akvr_xr_main_menu_detected() || g_pauseLive)) {
      views[e].fov = g_rawFov[e];
      XrQuaternionf turn{0.0f, 0.0f, 0.0f, 1.0f};
      akn_eye_lens(g_rawFov[e], views[e].fov, turn);   // TURNEDEYE / SAMEDIR: the centred lens AKN drew, its camera's turn
      if (g_straightNow) { views[e].fov = g_rawFov[e]; turn = XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f}; }   // SVRSTRAIGHT: picture turned back
      const float hx = menu_one_spot() ? 0.0f : (e == 0 ? -0.5f : 0.5f) * akn_ipd_m();   // MENU3D
      const XrVector3f r = quat_rotate(eyePose.orientation, XrVector3f{hx, 0.0f, 0.0f});
      views[e].pose = eyePose;
      views[e].pose.position.x += r.x; views[e].pose.position.y += r.y; views[e].pose.position.z += r.z;
      views[e].pose.orientation = hud_qmul(eyePose.orientation, turn);   // TURNEDEYE (identity for lens mode 1)
      // halfX: this eye's half when both eyes share ONE side-by-side swapchain (SBSONE); with one swapchain per eye
      // the whole image starts at 0 (5c run 1: e * width there read past the right eye's edge = streaks)
      views[e].subImage.imageRect.offset = {halfX, 0};
      views[e].subImage.imageRect.extent.width = (int32_t)g_eyeW;
      views[e].subImage.imageRect.extent.height = (int32_t)g_swH;
    }
    if (g_roomScrNow && akvr_engine_stereo() && g_rawFovOk && !g_aknFresh) {
      // SCREENROOM: each half holds the room panel as this eye saw it from g_roomScrHead through the runtime's own lens
      views[e].fov = g_rawFov[e];
      const XrVector3f r = quat_rotate(g_roomScrHead.orientation, XrVector3f{(e == 0 ? -0.5f : 0.5f) * akn_ipd_m(), 0.0f, 0.0f});
      views[e].pose = g_roomScrHead;
      views[e].pose.position.x += r.x; views[e].pose.position.y += r.y; views[e].pose.position.z += r.z;
      views[e].subImage.imageRect.offset = {g_sbsActive ? e * (int32_t)g_eyeW : 0, 0};   // one swapchain per eye: its own image from 0
      views[e].subImage.imageRect.extent.width = (int32_t)g_eyeW;
      views[e].subImage.imageRect.extent.height = (int32_t)g_swH;
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
  // PANELUPRIGHT 2026-10-05 — JJ: "make the VR overlay always open perfectly vertical in world
  // space, not head-orientated". Only the head's HEADING is used: the panel stands upright (no
  // pitch, no roll; local space is gravity-aligned), 1.30 m ahead and 0.10 m below eye height.
  if (!g_ovAnchored) {
    const XrVector3f f = quat_rotate(g_curPose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
    float fx = f.x, fz = f.z;
    if (fx * fx + fz * fz < 0.01f) {   // looking straight up or down: the head's up axis gives the heading
      const XrVector3f u = quat_rotate(g_curPose.orientation, XrVector3f{0.0f, 1.0f, 0.0f});
      fx = f.y < 0.0f ? u.x : -u.x;
      fz = f.y < 0.0f ? u.z : -u.z;
    }
    const float yaw = atan2f(-fx, -fz);   // about +Y; yaw 0 faces -Z
    const XrQuaternionf upright{0.0f, sinf(0.5f * yaw), 0.0f, cosf(0.5f * yaw)};
    const XrVector3f off = quat_rotate(upright, XrVector3f{0.0f, -0.10f, -1.30f});
    g_ovAnchor.orientation = upright;
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
  const bool hieNow = g_hieThisFrame; g_hieThisFrame = false;   // HUDINEYE
  const bool hasHud = submitted && hud_layer_build(hudq, fov) && !hieNow && !(g_trFullNow && g_sbsHasHud);   // HUDINEYE: built (keeps its place current), not shown; TRANSFULL: a repeated picture already holds it
  {   // PATHREC
    ++c_prFrames; if (g_aknFresh) ++c_prFresh; if (hieNow) ++c_prHie; if (hasHud) ++c_prQuad;
    if (g_prSbsShown && !hieNow && g_sbsHasHud) ++c_prStale;   // the old picture with its HUD shown again
    if (hasHud && g_prSbsShown && g_sbsHasHud) ++c_prBoth;      // ...and the HUD layer on top = two HUDs
    g_prSbsShown = false;
  }
  const XrCompositionLayerBaseHeader *layers[10];
  uint32_t nLayers = 0;
  if (submitted)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&layer;
  // SCREENQUAD2: ONE quad - Virtual Desktop already draws a quad per eye (PANELONE: the per-eye pair read "too close"; JJ
  // here: "a bit too close ... I didn't feel like it was at the distance"), the pair doubled the depth
  if (submitted && g_scrQuadNow && akvr_engine_stereo())
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&g_scrQuad;
  g_dimHas = submitted && dim_layer(g_dimQuad);   // PAUSEDIM: over the world, under the menus
  if (g_dimHas)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&g_dimQuad;
  XrCompositionLayerQuad hudL{XR_TYPE_COMPOSITION_LAYER_QUAD}, hudR{XR_TYPE_COMPOSITION_LAYER_QUAD};
  if (hasHud && g_hudEyes && !(g_roomScrNow && g_hudOnScreen)) {   // HUDLAYER5: eye shift (SCREENQUAD2: not on a room screen)
    hud_eye_pair(hudq, hudL, hudR);
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&hudL;
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&hudR;
  } else if (hasHud)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&hudq;
  g_vigHas = submitted && vig_layer(g_vigQuad, g_eyeWantGameplay);   // ZOOMVIG: over the game and HUD, under the panel
  if (g_vigHas)
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&g_vigQuad;
  // PANELDEPTH 2026-10-06 - JJ: the upright panel "looks like it's tilting backwards; it's not vertical in world
  // space". It IS upright in LOCAL space, but Virtual Desktop draws a quad the same in both eyes (no parallax), so the
  // only depth cue left is its perspective when you look down at it, which reads as leaning back. One quad per eye,
  // shifted by that eye's offset (the HUD's HUDEYES pair), gives it real depth so it reads as a flat upright panel.
  // PANELONE 2026-10-08 - JJ: "the VR overlay is too close to my face. Make it the same as the other mods, the same
  // depth." His reference is Batman City's panel: ONE quad for both eyes, 1.30 m ahead, 0.984 m wide, 0.10 m down
  // (SWBF2 notes: per-eye shifted panel quads read too close). The per-eye pair (PANELDEPTH) is gone.
  XrCompositionLayerQuad ovL{XR_TYPE_COMPOSITION_LAYER_QUAD}, ovR{XR_TYPE_COMPOSITION_LAYER_QUAD};
  (void)ovL; (void)ovR;
  if (g_ovDrewThisFrame) {
    layers[nLayers++] = (const XrCompositionLayerBaseHeader *)&quad;
  } else
    g_ovAnchored = false; // panel hidden → re-anchor in front of you when it reopens

  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
  fei.displayTime = g_predicted;
  fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  fei.layerCount =
      nLayers; // always end the frame we began (0 layers if nothing to show)
  fei.layers = nLayers ? layers : nullptr;
  xrEndFrame(g_session, &fei);
  if (akvr_engine_stereo()) akn_send_present();   // AKN M5: this frame's HUD is complete
  if (g_mirrorPending) {   // MIRRORLATE: the desktop mirror only now, outside AKN's HUD collection
    g_mirrorPending = false;
    ID3D11Texture2D *mb = nullptr;
    if (swapChain && SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&mb)) && mb) {
      D3D11_TEXTURE2D_DESC md{}; mb->GetDesc(&md); g_mirrorHasHud = hieNow; dlss_mirror(mb, md); g_mirrorHasHud = false; mb->Release();
    }
  }
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
    if (akvr_engine_stereo()) akn_send_game_device(dev);   // AKN: hook THIS device's context table (VD has its own)

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
    if (akvr_engine_stereo()) {
      g_native = true;   // AKN M5: the game draws both eyes; the pair comes from AKN (aknSbsUpdate)
    } else if (akvr_early_geo11()) {
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
bool akvr_xr_pause_live() { return g_pauseLive; }
bool akvr_xr_pause_candidate() { return g_pauseCand; }   // PAUSEEDGE
bool akvr_xr_pause_look() { return g_pauseLook; }
void akvr_xr_pause_look_set(bool on) { g_pauseLook = on; }
float akvr_xr_pause_dim() { return g_pauseDim; }
void akvr_xr_pause_dim_set(float v) { g_pauseDim = v < 0.0f ? 0.0f : (v > 0.9f ? 0.9f : v); }
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
bool akvr_xr_menu_one_spot() { return g_menuOneSpot; }
void akvr_xr_menu_one_spot_set(bool on) { g_menuOneSpot = on; }
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
// MENUSIDE5: the menu scene from its first live camera frame (start-up phase), or the recognised main menu
bool akvr_xr_menu_scene() { return g_autoMainMenu && (g_menuPhase == 1 || (g_menuPhase == 0 && g_menuGameplaySince != 0)); }
int  akvr_xr_menu_phase() { return g_autoMainMenu ? g_menuPhase : 2; }   // AUDIOSYNC: 0 = start-up
void akvr_xr_mode_note(const char *what) { mode_log(what); }            // AUDIOSYNC3: into the mode timeline
bool akvr_xr_startup_3d() { return g_autoMainMenu && g_menuPhase == 0 && g_anamorphic; }   // AUDIOSYNC3: the menu scene has begun
void akvr_xr_set_anamorphic(bool on) { g_anamorphic = on; }
void akvr_xr_set_screen_hold(bool on) { g_holdScreen = on; }
void akvr_xr_set_camera_pos(bool ok, float x, float y, float z) {
  g_camOk = ok; g_camPos[0] = x; g_camPos[1] = y; g_camPos[2] = z;
}
const char *akvr_xr_mode_log() { return g_modeLog; }
bool akvr_xr_auto_main_menu() { return g_autoMainMenu; }
void akvr_xr_auto_main_menu_set(bool on) { g_autoMainMenu = on; }
float akvr_xr_pause_zoom() { return g_pauseFovScale; }
float akvr_xr_splash_zoom() { return g_splashFovScale; }
float akvr_xr_load_zoom() { return g_loadFovScale; }
void  akvr_xr_load_zoom_set(float v) { g_loadFovScale = v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v); }
float akvr_xr_load_up() { return g_loadUpDeg; }
void  akvr_xr_load_up_set(float v) { g_loadUpDeg = v < -30.0f ? -30.0f : (v > 30.0f ? 30.0f : v); }
float akvr_xr_screen_near() { return g_nearScreenM; }   // SCREENNEAR
void  akvr_xr_screen_near_set(float v) { g_nearScreenM = v < 0.5f ? 0.0f : (v < 1.0f ? 1.0f : (v > 20.0f ? 20.0f : v)); }   // 0 = far
void  akvr_xr_splash_zoom_set(float v) { g_splashFovScale = v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v); }
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
// PANELTIDY5 2026-10-11: akvr_xr_pose_pair (FRAMEPAIR2, only the removed RAINSPLIT rain test read it) and the MKREC eye-pair getter removed.
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
int akvr_xr_menu_pose_delay() { return g_menuPoseDelay; }
void akvr_xr_menu_pose_delay_set(int frames) { g_menuPoseDelay = frames < 0 ? 0 : (frames > 4 ? 4 : frames); }
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
ID3D11Texture2D *akvr_xr_eyes_texture() { return g_hieDst; }   // EYESHOT
const char *akvr_xr_card_diag() { return g_mkCardDiag; }
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
  else snprintf(d, sizeof(d), "headset HUD layer: %ld frames sent, %ux%u fmt %d, at %.1f m, %.2f x %.2f m, %s, %s, place fails %ld, "
                              "depth %s (eyes %.1f mm apart)%s",
                g_hudSubmits, g_hudW, g_hudH, (int)g_hudFmt, g_hudDistUsed, g_hudSizeW, g_hudSizeH,
                g_hudSpace == 1 ? "attached to the head" : (g_hudSpace == 2 ? "fixed in the room" : "room space, re-placed each frame"),
                g_hudColour == 1 ? g_hudConvDiag : "raw colour copy", g_hudPlaceFails,
                !g_hudEyes ? "off (one quad)" : (g_hudEyeFollow && g_eyeLocOk && g_hudSpace == 2 ? "follows the eyes (HUDEYES)" : "along the quad's right (old)"),
                g_eyeLocDistMm, g_rpHasHud ? "" : " (not in the last frame)");
  return d;
}
int  akvr_xr_hud_space() { return g_hudSpace; }
void akvr_xr_hud_space_set(int v) { g_hudSpace = (v == 1 || v == 2) ? v : 0; g_hudWorldOk = false; }
void akvr_xr_hud_reanchor() { g_hudWorldOk = false; }
const char *akvr_xr_hie_diag() { return g_hieDiag; }
int akvr_xr_hud_in_eye() { return g_hudInEye; }
int akvr_xr_space_mode() { return g_xrSpaceMode; }                         // SVRSPACE
int akvr_xr_svr_straight() { return g_svrStraight; }                       // SVRSTRAIGHT
void akvr_xr_svr_straight_set(int v) { g_svrStraight = (v >= 0 && v <= 2) ? v : 1; }
long akvr_xr_straight_frames() { return g_straightFrames; }
void akvr_xr_space_mode_set(int v) { g_xrSpaceMode = (v >= 0 && v <= 2) ? v : 0; }
void akvr_xr_hud_in_eye_set(int v) { g_hudInEye = v ? 1 : 0; }
extern "C" __declspec(dllexport) void akvr_set_markercut(int v) { g_markerCut = v ? 1 : 0; }   // MARKERCUT test switch
extern "C" __declspec(dllexport) void akvr_set_map_flat(int v) { g_mapFlat = v ? 1 : 0; }   // MAPFLAT test switch
extern "C" __declspec(dllexport) int akvr_flat_screen_now() { return g_aknFlatScreen ? 1 : 0; }   // VSSPLIT (AKN): map / flat screens up
extern "C" __declspec(dllexport) void akvr_set_backext(int v) { g_backExt = v ? 1 : 0; }   // BACKEXT test switch
extern "C" __declspec(dllexport) const char *akvr_perf_diag() { return g_ptDiag; }   // PERFTIME (AKN logs it)
extern "C" __declspec(dllexport) void akvr_set_cutlog(int v) { g_cutLog = v ? 1 : 0; }   // PERF1: FULLCUT border readback for the log
extern "C" __declspec(dllexport) void akvr_set_fullcut(int v) { g_fullCut = v ? 1 : 0; if (!v) g_cutA = 0.0f; }   // FULLCUT switch
extern "C" __declspec(dllexport) int akvr_fullcut_state(float *alpha) { if (alpha) *alpha = g_cutA; return (int)g_cutFrames; }   // FULLCUT telemetry (AKN log)
extern "C" __declspec(dllexport) const char *akvr_fullcut_diag() { return g_cutDiag; }   // FULLCUT3: last second's strongest border reading
extern "C" __declspec(dllexport) void akvr_set_blackkey(int v) { g_blackKey = v ? 1 : 0; }   // BLACKKEY test switch
bool akvr_trans_full() { return g_trOn != 0; }
void akvr_trans_full_set(bool on) { g_trOn = on ? 1 : 0; }   // TRANSFULL (panel)
extern "C" __declspec(dllexport) void akvr_transfull(int on, float dist, float zoom) { g_trOn = on; if (dist > 0.2f) g_trDist = dist; if (zoom >= 1.0f) g_trZoom = zoom; }   // TRANSFULL (AKN "transfull on [dist zoom]")
extern "C" __declspec(dllexport) const char* akvr_path_diag()   // PATHREC: counts since the last call
{
  static char d[200]; static long f = 0, fr = 0, h = 0, q = 0, b = 0, st = 0;
  _snprintf_s(d, sizeof(d), _TRUNCATE, "PATHREC: frames %ld, fresh pairs %ld, HUD in eyes %ld, HUD layer %ld, old picture with HUD shown again %ld, BOTH (two HUDs) %ld",
              c_prFrames - f, c_prFresh - fr, c_prHie - h, c_prQuad - q, c_prStale - st, c_prBoth - b);
  f = c_prFrames; fr = c_prFresh; h = c_prHie; q = c_prQuad; b = c_prBoth; st = c_prStale;
  return d;
}
extern "C" __declspec(dllexport) void akvr_scan_cut(int on, float r) { g_scanCut = on; if (r > 0.0f) g_scanCutR = r; }   // SCANCUT (AKN "scancut 0|1 [r]")
extern "C" __declspec(dllexport) void akvr_set_detvig_test(int v) { g_detVigTest = v ? 1 : 0; }   // DETVIG test switch
int akvr_xr_lens_mode() { return g_aknLensMode; }
// SAMEDIR: the eye picture's shape (wide:tall x1000) for the lens AKN draws in engine stereo - square pixels at the next
// start. 0 = not known yet (no headset lens).
int akvr_xr_engine_shape() {
  if (!g_rawFovOk) return 0;
  XrFovf f = g_rawFov[0]; XrQuaternionf t{0.0f, 0.0f, 0.0f, 1.0f};
  akn_eye_lens(g_rawFov[0], f, t);
  const float w = tanf(f.angleRight) - tanf(f.angleLeft), h = tanf(f.angleUp) - tanf(f.angleDown);
  return h > 0.01f ? (int)(w / h * 1000.0f + 0.5f) : 0;
}
void akvr_xr_lens_mode_set(int v) { if (v == 1 || v == 2) g_aknLensMode = v; }   // 3 (same way) removed: falls back to 2
int akvr_xr_markercut() { return g_markerCut; }
float akvr_xr_marker_scale() { return g_mkScale; }   // MARKERWORLD
void akvr_xr_marker_scale_set(float v) { g_mkScale = v < 0.5f ? 0.5f : (v > 3.0f ? 3.0f : v); }
void akvr_xr_markercut_set(int v) { g_markerCut = v ? 1 : 0; }
bool akvr_xr_akn_fresh() { return g_aknFresh; }   // HUDHOLD
bool akvr_xr_akn_3d() { return g_aknNoFresh < 4; }   // LOADFIT2 (JJ: the loading screen first appears too big, then rescales): 30 -> 4 frames
int  akvr_xr_hud_colour() { return g_hudColour; }
void akvr_xr_hud_colour_set(int v) { g_hudColour = v == 0 ? 0 : (v == 2 ? 2 : 1); }   // HUDGLOW: 2 = old clipping conversion
bool akvr_xr_hud_eye_follow() { return g_hudEyeFollow; }                    // HUDEYES
void akvr_xr_hud_eye_follow_set(bool on) { g_hudEyeFollow = on; }
unsigned long long akvr_xr_menu_start_tick() { return g_menuStartTick; }    // MENUSTART
float akvr_xr_pause_menu_size() { return g_pauseMenuScale; }                // PAUSESIZE
void  akvr_xr_pause_menu_size_set(float v) { g_pauseMenuScale = v < 0.2f ? 0.2f : (v > 1.0f ? 1.0f : v); }
bool akvr_xr_pause_no_back() { return g_pauseNoBack; }                      // PAUSENOBACK
void akvr_xr_pause_no_back_set(bool on) { g_pauseNoBack = on; }
float akvr_xr_menu_back_feather() { return g_menuBackFeather; }             // MENUFEATHER
void  akvr_xr_menu_back_feather_set(float v) { g_menuBackFeather = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float akvr_xr_menu_back_opacity() { return g_menuBackOpacity; }             // MENUBACK
void  akvr_xr_menu_back_opacity_set(float v) { g_menuBackOpacity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
bool akvr_xr_menu_no_back() { return g_menuNoBack; }                        // MENUNOBACK
void akvr_xr_menu_no_back_set(bool on) { g_menuNoBack = on; }
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
// ZOOMHIDE: zoomed right now. ZOOMHIDE0 2026-10-05 - a player: "It won't let me fully disable zoom vignette. When I put
// it at 0.0 vignette sticks to my screen". The game's own 2D zoom overlay was hidden only while OUR vignette showed, so
// strength 0 brought the game's overlay back. It now follows the zoom itself, whatever the strength.
// ZOOMEDGE 2026-10-06 - a player: the game's overlay "still briefly appears when zooming, even when it's dialled down to
// zero". The game fades its overlay in as soon as the zoom starts, but the hide waited for the FOV to pass vigbelow (45),
// and dropped as soon as it climbed back. Now "zoomed" also starts the moment the FOV narrows fast below its recent level
// (a baseline that follows slow changes at up to 15 deg/s and is frozen while narrowed), and holds 0.4 s after.
unsigned long long akvr_gamepad_zoom_press_ms();   // gamepad.cpp: ZOOMPRESS
// ZOOMPRESS 2026-10-07 - JJ again: at vignette 0 the game's overlay "still briefly appears for a fraction of a second
// when zooming". The game starts its overlay on the button press, before its view narrows, so ZOOMEDGE (narrowing)
// still came a moment late. A right-stick click in play now hides it at once, for 0.8 s; the zoom itself keeps it.
bool  akvr_xr_vig_active() {
  const float fov = akvr_camera_game_fov();
  static float s_base = 0.0f; static bool s_narrow = false; static ULONGLONG s_last = 0, s_offAt = 0, s_steadySince = 0;
  static float s_prevFov = 0.0f;
  const ULONGLONG now = GetTickCount64();
  const float dt = s_last ? (float)(now - s_last) * 0.001f : 0.0f;
  s_last = now;
  bool zoomed = false;
  if (g_eyeWantGameplay && fov > 1.0f) {
    if (s_base <= 0.0f || fov >= s_base) { s_base = fov; s_narrow = false; }
    if (!s_narrow) {
      if (fov < s_base - 3.0f) { s_narrow = true; s_steadySince = now; }
      else s_base = (std::max)(fov, s_base - 15.0f * (dt > 0.1f ? 0.1f : dt));
    }
    if (s_narrow) {
      if (std::fabs(fov - s_prevFov) > 0.05f) s_steadySince = now;
      // a lasting narrower view that is not a zoom (fov above vigbelow, unchanged 3 s): take it as the new normal
      if (fov >= s_base - 3.0f || (fov >= g_vigBelowDeg && now - s_steadySince > 3000)) { s_narrow = false; s_base = fov; }
    }
    zoomed = s_narrow || fov < g_vigBelowDeg;
  } else { s_base = 0.0f; s_narrow = false; }
  s_prevFov = fov;
  if (zoomed) s_offAt = now + 400;
  if (g_eyeWantGameplay && akvr_gamepad_zoom_press_ms() < 800) return true;   // ZOOMPRESS
  return (g_vigAmt > 0.0f && !g_vigPreview) || zoomed || now < s_offAt;
}
const char *akvr_xr_vig_diag() {
  static char d[200];
  snprintf(d, sizeof(d), "game's own view now %.1f deg (lowest %.1f, widest %.1f this session) | vignette %s%s",
           akvr_camera_game_fov(), g_gameFovMin, g_gameFovMax, g_vigAmt > 0.0f ? "ON" : "off",
           g_vigErr ? (g_vigErr == 1 ? " - no image format" : " - could not make its image") : "");
  return d;
}
float akvr_xr_startup_bright() { return startup_bright_impl(); }   // STARTBRIGHT

const char *akvr_xr_share_probe() { return g_shareProbe; }   // SHAREPROBE
