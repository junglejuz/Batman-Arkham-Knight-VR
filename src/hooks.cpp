// AKVR â€” Milestone 1: DX11 Present hook + ImGui overlay
// ------------------------------------------------------------------
// kiero finds the game's swapchain vtable; MinHook detours Present
// (index 8) and ResizeBuffers (index 13). On the first hooked frame
// we grab the device/context from the swapchain and bring up ImGui.
// Every frame after: draw a small read-only overlay, then hand the
// frame back to the game untouched.
//
// M1 is display-only on purpose â€” we don't capture mouse/keyboard
// input for the overlay (no WndProc hook), so game input can't break.
// F8 toggles overlay visibility (edge-detected GetAsyncKeyState).
// ------------------------------------------------------------------
#define _CRT_SECURE_NO_WARNINGS
#include "akn_link.h"
extern "C" int akvr_shader_save(unsigned long long h, const wchar_t* path);   // DIAGKIT (hudsplit.cpp)
#include <windows.h>
#include <intrin.h>  // _ReturnAddress (desktop guard)
#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Multithread (thread-safety on the shared immediate context)
#include <dxgi1_2.h>   // IDXGIFactory2 / DXGI_SWAP_CHAIN_DESC1 (forced-size creation hook)
#include <MinHook.h>   // creation hooks (kiero uses MinHook too; MH_Initialize is idempotent)
#include <xinput.h>    // gamepad (settings-menu chord)
#include <cstdio>
#include <cstdarg>     // note() forwards a formatted line into the startup transcript
#include <cstring>     // strstr (probe-status parsing)
#include <cstdint>
#include <cmath>      // tanf â€” engine-render shape from the headset frusta

#include "kiero/kiero.h"
#include "camera.h"
#include "xr.h"
#include "present_probe.h"
#include "frameid.h"
#include "geo11conv.h"
#include "hudprobe.h"
#include "hudsplit.h"   // HUDSPLIT: the one HUD draw, watched
#include "openvr_beacon.h"
#include "gamepad.h"
#include "mouse_block.h"   // PANELMOUSE: the game's DirectInput mouse is still under the panel
#include <mutex>
#include <string>
#include <vector>   // PARTOPEN
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_internal.h"   // TempInputIsActive (SLIDEROWN)
// The backend leaves this one inside an `#if 0` on purpose (so the header doesn't
// have to include <windows.h>) and tells you to copy the line into your own .cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// earlyres.cpp â€” the render-size spoof installed from DllMain, before the game runs.
// It owns its own settings read (it happens far too early to use settings_load), so
// these are read-back/round-trip accessors only.
const char* akvr_early_log();
bool        akvr_early_enabled();
int         akvr_early_width();     // derived â€” no setter, it can't drift from the size
int         akvr_early_height();
bool        akvr_early_geo11();   // geo-11 is installed next to us (d3dxdm.ini present)
const wchar_t* akvr_gamepad_hook_diag();   // gamepad.cpp: which XInput the panel chord reads (AKN)
const char* akvr_xr_share_probe();   // xr.cpp: SHAREPROBE
bool        akvr_early_dxgi11();
int         akvr_early_dxgi11_mode();  // DXGI11 setting (dxgi11=0 turns the factory upgrade off)
bool        akvr_early_dxgi11_auto();   // SVRAUTO: no dxgi11 key = decided by engine stereo + SteamVR
bool        akvr_early_square();
int         akvr_early_chrome();
int         akvr_early_patched();
long        akvr_early_calls();
void        akvr_early_enable(bool on);
void        akvr_early_size_set(int h);
int         akvr_early_max_height();                   // RESMAX: largest picture height allowed
void        akvr_early_square_set(bool s);
void        akvr_early_note(const char* s);            // append to the startup transcript
void        akvr_early_fake_screen(int& w, int& h);    // the desktop size we claim
// Engine render size â€” patched into the game's own code in DllMain (RE 2026-08-05).
// All of these take effect on the NEXT launch; nothing here can change a live render.
int         akvr_engine_res();
void        akvr_engine_res_set(int h);                // 0 = off; height only
int         akvr_dlss_mode();                          // DLSSPANEL (earlyres.cpp): 0 off .. 5 performance
float       akvr_xr_marker_scale();                   // MARKERWORLD (xr.cpp)
void        akvr_xr_marker_scale_set(float v);
int         akvr_xr_lens_mode();                       // SAMEDIR (xr.cpp): 2 turned eyes, 3 one direction
void        akvr_xr_lens_mode_set(int v);
int         akvr_xr_engine_shape();                    // SAMEDIR: the drawn lens's wide:tall x1000, 0 = not known yet
void        akvr_dlss_mode_set(int m);
void        akvr_flareoff_set(int v);                  // FLAREOFF3 (hudsplit.cpp)
int         g_afwSet = 0, g_afwModeSet = 3;            // AFW (akvr_native/NOTES.md "AFW PLAN"): saved here, AKN reads afw= / afwmode= at start
int         akvr_engine_shape();                       // wide:tall x1000
void        akvr_engine_shape_set(int s);
bool        akvr_engine_res_on();
int         akvr_engine_res_w();
int         akvr_engine_res_h();
int         akvr_geo11_shape();          // GEO11SHAPE: geo-11 render width/height x1000
void        akvr_geo11_shape_set(int s); // EYEVIEW: next launch's shape
const char* akvr_engine_res_diag();
void        akvr_menu_res_tick();       // MENURES: keep the graphics menu's resolution at ours
const char* akvr_menu_res_diag();
// HUD size â€” LIVE (no restart). Every Scaleform movie's viewport is built from one
// global screen rectangle; shrinking and re-centring it shrinks all UI uniformly.
// akvr_hud_tick() must run once per frame.
void        akvr_hud_tick();
float       akvr_hud_scale();
bool        akvr_hud_aspect_fix();
void        akvr_hud_aspect_fix_set(bool on);
float       akvr_hud_aspect_factor();
void        akvr_hud_scale_set(float s);
float       akvr_hud_scale_v();                 // UIGATES: separate HUD height
void        akvr_hud_scale_v_set(float s);
bool        akvr_hud_menu_fill();               // menus use the full frame height
void        akvr_hud_menu_fill_set(bool on);
bool        akvr_hud_global();                  // HUDAREA: also resize the game's UI rectangle
void        akvr_hud_global_set(bool on);
float       akvr_hud_raise();                   // HUDPOS: gameplay HUD up/down, fraction of height
void        akvr_audiosync_frame();             // audiosync.cpp
bool        akvr_cam_restore();                 // camera.cpp: CAMRESTORE
bool        akvr_cam_smooth();                  // camera.cpp: CAMSMOOTH
void        akvr_cam_smooth_set(bool on);
long        akvr_cam_smooth_events();
bool        akvr_cam_swing();                   // camera.cpp: SWINGEASE
bool        akvr_cam_aim_head();                // camera.cpp: AIMHEAD
bool        akvr_cam_aim_head_now();            // camera.cpp: AIMTRIGGER2
long        akvr_cam_aim_head_frames();
long        akvr_cam_exact_writes();            // camera.cpp: DIVEFIX
bool        akvr_cam_exact_on();
void        akvr_cam_aim_head_set(bool on);
void        akvr_cam_swing_set(bool on);
void        akvr_cam_restore_set(bool on);
void        akvr_audiosync_shutdown();
bool        akvr_audiosync();
bool        akvr_trans_full();          // TRANSFULL (xr.cpp)
void        akvr_trans_full_set(bool on);
void        akvr_audiosync_set(bool on);
const char* akvr_audiosync_diag();
float       akvr_menu_text_left();              // MENUTEXT: main-menu text slides left, fraction of width
void        akvr_menu_text_left_set(float f);
void        akvr_hud_raise_set(float r);
bool        akvr_hud_scale_found();
const char* akvr_hud_diag();
int         akvr_hud_piece_count();             // HUDPIECES (earlyres.cpp)
const char* akvr_hud_piece_name(int i, int& bufW, int& bufH, void*& view);
bool        akvr_hud_piece_shrink(int i);
int         akvr_hud_piece_mode(int i, int& now);   // earlyres.cpp: MENUFIT
bool        akvr_menu_fit();
void        akvr_menu_fit_set(bool on);
long        akvr_menu_fit_writes();
long        akvr_menu_fit_res_seen();   // MENUFIT2
void        akvr_hud_piece_shrink_set(int i, bool on);
const char* akvr_hud_noshrink();
bool        akvr_hud_tex_fix();                 // TEXDRAW: shrink the off-screen HUD draws too
void        akvr_hud_tex_fix_set(bool on);
bool        akvr_hud_root_mode();               // ROOTSHRINK: shrink inside the movie (radar fix)
void        akvr_hud_root_mode_set(bool on);
bool        akvr_hud_scale_mode();              // SCALEMODE: shrink via Scaleform's own scale
void        akvr_hud_scale_mode_set(bool on);
bool        akvr_hud_dual();                    // RADARTEX: shrink pieces in their own texture too
void        akvr_hud_dual_set(bool on);
void        akvr_hud_noshrink_set(const char* list);
void        akvr_hud_piece_xf(int i, float& s, float& x, float& y);   // PIECEXF
void        akvr_hud_piece_xf_set(int i, float s, float x, float y);
const char* akvr_hud_piece_xf_list();
void        akvr_hud_piece_xf_list_set(const char* list);
int         akvr_hud_id_depth();
void        akvr_hud_layers_discover();                 // HUDLAYERS (earlyres.cpp)
void        akvr_hud_steady_set(float v);               // HUDSTEADY (earlyres.cpp)
float       akvr_hud_steady();
const char* akvr_hud_steady_diag();
const char* akvr_hud_layers_diag();
int         akvr_hud_layer_count();
bool        akvr_hud_layer_get(int i, int& depth, int& parent, int& kids, const char*& label,
                               float& s, float& x, float& y, bool& hide, bool& is3d);
void        akvr_hud_layer_set(int i, float s, float x, float y, bool hide);
bool        akvr_hud_layer_room(int i);              // PARTTAG: part marked "hang in the room"
void        akvr_hud_layer_room_set(int i, bool on);
long        akvr_hud_layer_tag_sets();
const char* akvr_hud_layer_tag_diag();      // PARTTAG2
bool        akvr_hud_layer_zoomhide(int i);  // ZOOMHIDE
bool        akvr_hud_layer_world(int i);     // ROOMALL: "stays on its target"
bool        akvr_hud_layer_autoroom(int i);  // ROOMALL: marked by the whole-HUD rule
void        akvr_hud_layer_world_set(int i, bool on);
bool        akvr_hud_room_all();
void        akvr_hud_room_all_set(bool on);
void        akvr_hud_layer_zoomhide_set(int i, bool on);
const char* akvr_hud_layer_xf_list();
void        akvr_hud_layer_xf_list_set(const char* list);
const char* akvr_hud_containers_list();                  // CONTAINERFP
void        akvr_hud_containers_list_set(const char* list);
void        akvr_hud_layers_dump(const wchar_t* path);   // HUDLAYERS (earlyres.cpp)
int  akvr_xr_menu_phase();   // BLACKHUNT (xr.cpp)
bool akvr_xr_pause_live();   // BLACKHUNT (xr.cpp)
volatile ULONGLONG g_ringLayersAt = 0; volatile LONG g_ringLayersN = 0;   // HUDRING2: the HUD tree mid-bats
void akvr_ring_layers_request(int ms) { if (g_ringLayersN < 12 && !g_ringLayersAt) g_ringLayersAt = GetTickCount64() + (ULONGLONG)ms; }
// Gameplay-only HUD shrinking: on a 2D screen the picture IS the UI, so scaling it just
// wraps it in black. Set from Present before akvr_hud_tick().
void        akvr_hud_gameplay(bool on);
void        akvr_hud_anamorphic(bool on);   // squash the UI to match a projVR frame
void        akvr_hud_dump_viewports(const wchar_t* path);   // F1: every HUD viewport set

namespace
{
    void write_startup_diag(bool requested = false);
    using PresentFn       = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    PresentFn       oPresent       = nullptr;
    bool g_nativeAfterPresent = true; // reversible geo-11 timing comparison
    ResizeBuffersFn oResizeBuffers = nullptr;

    ID3D11Device*           g_device  = nullptr;
    ID3D11DeviceContext*    g_context = nullptr;
    ID3D11RenderTargetView* g_rtv     = nullptr;
    HWND                    g_hwnd    = nullptr;

    bool      g_imguiReady = false;
    volatile LONG g_panelCmd = 0, g_chordFires = 0;   // PANELTEST: 1 open / 2 close from AKN; chord count
    bool      g_showOverlay = false;   // hidden by default; both stick clicks (menu) or F8 shows it
    int       g_traceRows   = 0;       // rows written by the last F1 camera-trace dump
    // Last frame's gameplay verdict. The verdict is computed near the END of Present but
    // the HUD tick runs earlier, so it uses the previous frame's answer â€” one frame of
    // lag on a state that only changes at level boundaries.
    bool      g_lastGameplay = true;
    // HUDFIT: the 3D scene in THIS frame went through projVR (pause and main menu too),
    // so present it at the projVR shape and squash the UI to match.
    bool      g_lastAnamorphic = false;

    // ---- MODE TRACE ---------------------------------------------------------
    // The loading screen still snaps head-locked once per load, and I have now guessed
    // wrong about it twice. So stop guessing: record the DECISION and its inputs every
    // Present, and dump it with F1 next to the camera trace.
    //
    // It has to live here, not in camera.cpp's trace: that one records on camera
    // FINALIZE, and a loading screen is precisely when finalize stalls â€” so it would
    // have no rows for the interesting stretch.
    //
    // `proj_hits` is the candidate NEW signal. projVR counts BuildProjectionMatrix
    // rewrites; a 3D scene being rendered produces them continuously and a 2D loading
    // image should produce none, which would discriminate far better than camera
    // liveness (AK demonstrably ticks its camera for seconds behind a loading image).
    struct ModeRec
    {
        double   t;            // seconds since the first record
        uint32_t sinceFin;     // ms since the camera-finalize counter advanced
        uint32_t sinceMove;    // ms since the camera VALUES changed
        uint8_t  raw, verdict, camValid, pad;
        int      projHits;     // matched projection builds since the previous record
        float    camX, camY, camZ, camYawDeg, camFov;
    };
    const int  kModeCap  = 16384;      // ~3.5 min at 75 fps, far more during a stutter
    ModeRec*   g_mode    = nullptr;
    int        g_modeHead = 0, g_modeLen = 0;
    LARGE_INTEGER g_modeFreq{}, g_modeT0{};
    int        g_modeRows = 0;

    void mode_record(uint32_t sinceFin, uint32_t sinceMove, bool raw, bool verdict,
                     const CameraView& cam, int projHits)
    {
        if (!g_mode)
        {
            g_mode = (ModeRec*)calloc(kModeCap, sizeof(ModeRec));
            if (!g_mode) return;
            QueryPerformanceFrequency(&g_modeFreq);
            QueryPerformanceCounter(&g_modeT0);
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        ModeRec& r = g_mode[g_modeHead];
        r.t = g_modeFreq.QuadPart
                ? (double)(now.QuadPart - g_modeT0.QuadPart) / (double)g_modeFreq.QuadPart : 0.0;
        r.sinceFin = sinceFin; r.sinceMove = sinceMove;
        r.raw = raw ? 1 : 0; r.verdict = verdict ? 1 : 0;
        r.camValid = cam.valid ? 1 : 0; r.pad = 0;
        r.projHits = projHits;
        r.camX = cam.x; r.camY = cam.y; r.camZ = cam.z;
        r.camYawDeg = cam.yaw * (360.0f / 65536.0f);
        r.camFov = cam.fov;
        g_modeHead = (g_modeHead + 1) % kModeCap;
        if (g_modeLen < kModeCap) ++g_modeLen;
    }

    int mode_trace_dump(const wchar_t* path)
    {
        if (!g_mode || g_modeLen <= 0) return 0;
        FILE* f = _wfopen(path, L"wb");
        if (!f) return 0;
        fprintf(f, "t_sec,ms_since_finalize,ms_since_camera_moved,raw,verdict,cam_valid,"
                   "proj_hits,cam_x,cam_y,cam_z,cam_yaw_deg,cam_fov\n");
        int start = (g_modeLen == kModeCap) ? g_modeHead : 0;
        for (int i = 0; i < g_modeLen; ++i)
        {
            const ModeRec& r = g_mode[(start + i) % kModeCap];
            fprintf(f, "%.6f,%u,%u,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.2f\n",
                    r.t, r.sinceFin, r.sinceMove, (int)r.raw, (int)r.verdict,
                    (int)r.camValid, r.projHits, r.camX, r.camY, r.camZ,
                    r.camYawDeg, r.camFov);
        }
        fclose(f);
        return g_modeLen;
    }

    // Desktop mirror: when the panel goes to the headset as a quad layer it never
    // touches the game's backbuffer, so nothing shows on the monitor and there's
    // nothing to screenshot. We re-draw the same ImGui output onto the backbuffer
    // AFTER the headset copy has been taken â€” do it before and the panel appears
    // twice in VR (once flat inside the game frame, once as the floating quad).
    bool      g_panelDrewThisFrame = false;
    bool      g_panelToVR          = false;
    ULONGLONG g_injectTick = 0;
    ULONGLONG g_frameCount = 0;

    // Resolution: we keep the render SQUARE (matches the Quest eye; a 16:9 render
    // letterboxes into a mail-slot view). The game's own video-options menu keeps
    // re-applying its stored 16:9 resolution, clobbering ours â€” so we self-heal:
    // watch the render shape and snap it back to g_targetRes whenever it drifts.
    bool g_resWritten = false;
    int  g_resValue   = 0;
    int  g_targetRes  = 2048;   // desired square render size (user-selectable, persisted)
    // (dead levers, kept only so old settings files still parse â€” do NOT revive)
    // dpiaware: the DPI theory was wrong. The desktop is a real 2560x1440 with no
    //   Windows scaling, so there was never a hidden higher resolution to unlock.
    // clientscale: faking the window's client rect never reached the render either.
    // Render size is now owned by earlyres.cpp (spoofed from DllMain, before the game).
    bool  g_dpiAware = true;
    float g_clientScale = 1.0f;
    // (superseded) the resScale screen-spoof inflated buffers without reaching the render
    // and tanked FPS; kept as a no-op float so old settings files still parse.
    float g_resScale = 1.0f;
    // DEFAULT OFF as of 2026-07-25. Forcing the swapchain square works, but it does NOT
    // make the GAME render square, so it actively hurts: measured on a 2560x1440 desktop
    // the game asks for 2423x1363, which is exactly the largest 16:9 that fits the
    // desktop WORK AREA (1392 - 29 px of caption/border = 1363; 1363 * 16/9 = 2423).
    // It ignores ResX/ResY in the .ini entirely and hard-wires 16:9 in windowed mode. So
    // the scene still gets drawn at 2423x1363 into the top-left of a 2048x2048 buffer:
    // clipped on the right, sitting high, and sheared because the projection is built
    // for a shape the buffer doesn't have. Squaring only the buffer LOSES image.
    bool g_forceSquare = false;

    // Resolution diagnostics + live self-heal. Writing the .ini and overriding the
    // game's own ResizeBuffers wasn't enough (reported 2026-07-25: still 16:9). Two
    // possibilities: the game never calls ResizeBuffers at all, or it applies its
    // stored resolution some other way. So we now (a) count the game's resize calls
    // and (b) actively call ResizeBuffers ourselves when the backbuffer isn't our
    // square target. If the game holds a reference to the backbuffer that call
    // legitimately fails (DXGI_ERROR_INVALID_CALL 0x887A0001) â€” which is exactly what
    // we want to find out, so the result is reported in the panel either way.
    ULONGLONG g_resizeCalls = 0;   // times the GAME asked DXGI to resize
    int       g_forceTries  = 0;   // self-heal attempts (capped â€” never fight forever)
    ULONGLONG g_lastForce   = 0;   // throttle stamp
    char      g_resDiag[160] = "res self-heal: idle";

    // --- forced render size at swapchain CREATION --------------------------------
    // The only lever that works on this game. It sizes its swapchain once at startup
    // and then ignores WM_SIZE and never calls ResizeBuffers (measured: 0 all session),
    // so both live routes fail. We load as a version.dll proxy â€” i.e. before the game's
    // own code runs â€” so we can be sitting on the creation calls when it gets there and
    // rewrite the size on the way through.
    // Luke Ross does the same thing in the R.E.A.L. mods: his per-game RealVR.ini
    // carries a "ForcedRes=2048,2048" (2704,2704 for Horizon), square, which is
    // independent confirmation both of the square shape and of the size we picked.
    bool      g_creationHooked = false;
    volatile long g_dxgi11Calls = 0;   // DXGI11: the game's CreateDXGIFactory calls answered with CreateDXGIFactory1
    char g_dxgi11Who[256] = "";       // DXGI11d: every caller, for the status line
    int       g_creationSeen   = 0;    // swapchains we intercepted
    UINT      g_createdW = 0, g_createdH = 0;   // what the game ASKED for
    // What the window's client area ACTUALLY became after we set it. The engine
    // sizes its viewport from the window, so if Windows clamped our resize this is
    // the number the scene ends up rendering at â€” and until now nothing read it back.
    int       g_clientW = 0, g_clientH = 0;
    bool      g_createdWindowed = true;         // what the game asked for (diagnostic)

    // Force windowed at creation. This is an ESCAPE HATCH, not a preference: exclusive
    // fullscreen can leave the game unreachable (2026-07-25 â€” controller dead in-game,
    // and the only way to change the mode back is the very menu you can't drive). The
    // mode is stored somewhere the game owns, so a config edit can't rescue it either.
    // Forcing it here means a bad fullscreen state is always one restart from fixed.
    // 2026-07-25: DEFAULT OFF. Forcing windowed pushed the game window to the
    // background under VR, and Batman pauses input when it isn't the foreground window
    // â†’ dead controller. Fullscreen (the mode that worked before) keeps it in front.
    // 2026-09-26: DEFAULT ON again. The July failure is what keep-focus (below) now
    // handles, and fullscreen is what tied the render size to the monitor's modes and
    // minimized the game on focus loss. See hkSetFullscreenState.
    bool g_forceWindowed = true;

    // RE02 square-viewport patch. Must be applied at STARTUP â€” the game computes its
    // viewport once, so switching it on later does nothing until something forces a
    // recompute. That means it can't be an in-game toggle, and a bad one would be
    // baked in before the settings menu exists. So it's opt-in from the settings file:
    // set `vpsquare=1` in akvr_settings.ini with the game closed.
    bool g_vpSquareWanted = false;
    bool g_vpSquareApplied = false;

    // Viewport FILL â€” stretch the scene viewport to cover the forced backbuffer, so the
    // engine stops drawing in the top-left corner of it. Declared here (not down with the
    // rest of the viewport code) only because settings_save/load below needs it. Full
    // rationale at the override itself, in hkRSSetViewports.
    bool g_vpFill = false;

    // Make a window's CLIENT area exactly size x size.
    void set_client_square(HWND hw, int size)
    {
        if (!hw || size < 512) return;
        RECT rc{ 0, 0, (LONG)size, (LONG)size };
        DWORD style   = (DWORD)GetWindowLongPtrW(hw, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongPtrW(hw, GWL_EXSTYLE);
        AdjustWindowRectEx(&rc, style, FALSE, exstyle);
        SetWindowPos(hw, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }

    // ---- keep-focus: stop the game pausing input when its window is in the background ---
    // Batman AK (UE3) ignores the controller/keyboard whenever its window isn't the
    // FOREGROUND window. Running in VR the game sits behind the compositor, so it thinks
    // it lost focus and drops all pad input â€” the exact "controller does nothing in-game,
    // only the overlay responds" symptom, and why exclusive fullscreen (which keeps the
    // game in front) worked before. Fix from both directions the engine can check focus:
    //   (a) swallow / rewrite the window messages that announce deactivation, and
    //   (b) make the foreground/active-window API calls report the game window.
    // Toggle with F3 if it ever misbehaves.
    bool     g_keepFocus    = true;
    // Declared this high because BOTH the window-proc size-ceiling override and
    // the per-frame client resync below need it.
    bool g_showAdvanced = false;    // TIDY3: advancedpanel=1 in the settings file shows Advanced
    // MENUBLACK 2026-10-02 â€” JJ: "when entering the first menu screen from the title logo screens, there's a pause with
    // just a black screen ... we're hearing audio for the menu happen before the menu appears". frames.csv: OUR Present
    // hook took 2197 ms on that frame (and ~106 ms on the ones before): MENUSTART's synchronous picture grabs (66 MB
    // read back each, waiting on the GPU while the menu loads), on because test tools were on. The automatic menu and
    // start-up pictures now need menucapture=1 in the settings file (no panel switch), never just the test tools.
    bool g_menuCapture = false;
    bool g_forceRes = true;         // settings key `forceres` â€” restart to apply. Default ON
                                    // 2026-09-26: keeps window + buffer at the render size in windowed mode.
    // Use the PER-USER OpenXR runtime (SteamVR here) instead of the machine-wide one
    // (Virtual Desktop). Settings key `xrhkcu`. See init_thread for why.
    bool g_useHkcuRuntime = false;
    // Headset probe + Present-hook kill switch, both for isolating geo-11 conflicts.
    // `xrprobe` writes akvr_xr_probe.txt from our own thread; `presenthook=0` stops us
    // detouring the swapchain vtable at all, so geo-11 can have it to itself.
    bool g_xrProbe     = false;
    bool g_presentHook = true;

    // Did geo-11 load under an alias instead of as d3d11.dll? Set by
    // load_geo11_hooked(); declared here so the startup log can read it.
    bool g_geo11Hooked = false;

    WNDPROC  g_origWndProc  = nullptr;
    HWND     g_focusHwnd    = nullptr;

    // PANELMOUSE (ported from Batman City 2026-10-13, JJ: "Arkham City's mouse works now in the overlay, port it to ...
    // Arkham Knight"): the mouse on the VR panel. Window messages arrive on the game's window thread, the panel frame runs
    // on the render thread: both touch ImGui's input queue, so one RECURSIVE lock (City: a plain one crashed when a
    // Win32 call re-entered the window procedure on the same thread). Buttons and the wheel are handed to ImGui directly,
    // NOT through the backend's message handler: that one takes and releases mouse capture, and its release on button-up
    // would take the GAME's own capture away (the 2026-08-05 "camera not behaving" build forwarded everything).
    std::recursive_mutex g_imguiInputLock;
    unsigned g_panelMouseButtons = 0;   // buttons pressed on the panel, so their releases reach ImGui after it closes
    bool panel_takes_mouse() { return g_imguiReady && g_panelToVR && (g_showOverlay || akvr_gamepad_menu()); }

    // PANELMOUSE: the backend queues the cursor in window pixels (it reads it every frame while the game is in front);
    // map the queued positions from the part of the window on its monitor (the VR window can be bigger than the
    // monitor) onto the whole panel. Mapping the queue, not io.MousePos (City's offset / drifting / double cursor).
    void map_mouse_to_panel(ImVec2 panel)
    {
        HWND wnd = g_focusHwnd ? g_focusHwnd : g_hwnd;
        RECT cr{};
        if (!wnd || !GetClientRect(wnd, &cr)) return;
        POINT tl{cr.left, cr.top}, br{cr.right, cr.bottom};
        ClientToScreen(wnd, &tl);
        ClientToScreen(wnd, &br);
        RECT vis{tl.x, tl.y, br.x, br.y}, on{};
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi) && IntersectRect(&on, &vis, &mi.rcMonitor))
            vis = on;
        const float vx = float(vis.left - tl.x), vy = float(vis.top - tl.y);
        const float vw = float(vis.right - vis.left), vh = float(vis.bottom - vis.top);
        if (vw <= 0 || vh <= 0) return;
        for (ImGuiInputEvent& e : GImGui->InputEventsQueue)
        {
            if (e.Type != ImGuiInputEventType_MousePos || e.MousePos.PosX <= -FLT_MAX) continue;
            e.MousePos.PosX = ImClamp((e.MousePos.PosX - vx) / vw, 0.0f, 1.0f) * panel.x;
            e.MousePos.PosY = ImClamp((e.MousePos.PosY - vy) / vh, 0.0f, 1.0f) * panel.y;
        }
    }

    LRESULT CALLBACK focus_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
    {
        // RAISE THE SIZE CEILING. Measured 2026-08-04: after rewriting AdjustWindowRect
        // the client reached 3200 WIDE but stuck at 3200x**2160** â€” exactly the height
        // of the display the window sits on. That clamp is Windows', not the game's:
        // the window manager fills in a default maximum tracking size from the monitor,
        // and our IAT spoof only fools calls the GAME makes, never USER32's own internal
        // arithmetic. WM_GETMINMAXINFO is where Windows asks the window how big it may
        // get â€” answer generously and the clamp is gone. Only while forcing is on, so a
        // normal run keeps normal window behaviour.
        if (msg == WM_GETMINMAXINFO && g_forceRes && l)
        {
            MINMAXINFO* mmi = (MINMAXINFO*)l;
            LRESULT r = CallWindowProcW(g_origWndProc, h, msg, w, l);
            mmi->ptMaxTrackSize.x = 16384;
            mmi->ptMaxTrackSize.y = 16384;
            if (mmi->ptMaxSize.x < 16384) mmi->ptMaxSize.x = 16384;
            if (mmi->ptMaxSize.y < 16384) mmi->ptMaxSize.y = 16384;
            return r;
        }
        // TYPING INTO THE PANEL (JJ, 2026-08-05: every slider's value should be
        // editable from the keyboard). Ctrl+click on any ImGui slider turns it into a
        // text box â€” but only if ImGui ever SEES the keyboard, and it never did: we
        // install our own window procedure and had not been forwarding messages to the
        // backend at all. Forward them only while the panel is actually up, so a normal
        // play session behaves exactly as before.
        //
        // NARROWED 2026-08-05. The first version handed the backend EVERY message while
        // the panel was open. That is more than typing needs and it is not inert: the
        // backend also processes mouse messages, takes/releases mouse capture, and
        // tracks focus â€” none of which it was doing before, because this WndProc never
        // forwarded anything. JJ reported the camera not behaving as it did before that
        // build, so the extra surface goes: forward KEYBOARD messages ONLY.
        // Nothing is lost â€” ImGui never received mouse buttons before either (the panel
        // is driven by the controller), and the cursor position is read directly by
        // ImGui_ImplWin32_NewFrame, not from here.
        // PANELMOUSE: buttons + wheel while the panel shows in the headset; the cursor position comes from the panel frame.
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEMOVE && ImGui::GetCurrentContext() &&
            (panel_takes_mouse() || g_panelMouseButtons))
        {
            int button = -1; bool down = false;
            switch (msg)
            {
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button = 0; down = true; break;
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button = 1; down = true; break;
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: button = 2; down = true; break;
            case WM_LBUTTONUP: button = 0; break;
            case WM_RBUTTONUP: button = 1; break;
            case WM_MBUTTONUP: button = 2; break;
            }
            {
                std::lock_guard<std::recursive_mutex> lk(g_imguiInputLock);
                ImGuiIO& io = ImGui::GetIO();
                if (button >= 0 && (down ? panel_takes_mouse() : (g_panelMouseButtons & (1u << button)) != 0))
                {
                    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                    io.AddMouseButtonEvent(button, down);
                    if (down) g_panelMouseButtons |= 1u << button; else g_panelMouseButtons &= ~(1u << button);
                }
                else if (msg == WM_MOUSEWHEEL && panel_takes_mouse())
                    io.AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA);
            }
            if (panel_takes_mouse()) return 0;   // the panel's click, not Batman's
        }
        const bool isKeyMsg = (msg == WM_KEYDOWN || msg == WM_KEYUP ||
                               msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                               msg == WM_CHAR || msg == WM_SYSCHAR);
        if (isKeyMsg && g_showOverlay && ImGui::GetCurrentContext())
        {
            {
                std::lock_guard<std::recursive_mutex> lk(g_imguiInputLock);
                ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
            }
            // While a text box has focus the keystrokes are the panel's, not Batman's â€”
            // otherwise typing "3" into a value also throws a batarang.
            if (ImGui::GetIO().WantCaptureKeyboard)
                return 0;
        }
        if (g_keepFocus)
        {
            switch (msg)
            {
            // WINDOWED 2026-09-26: a minimized game window is what killed the controller
            // (startup log: window client 0x0, resync attempts 117). Never minimize.
            case WM_SYSCOMMAND:
                if ((w & 0xFFF0) == SC_MINIMIZE) return 0;
                break;
            case WM_ACTIVATE:      w = MAKEWPARAM(WA_ACTIVE, 0); break; // never report inactive
            case WM_ACTIVATEAPP:   w = TRUE;                     break; // app is always active
            case WM_NCACTIVATE:    w = TRUE;                     break; // paint + report active
            case WM_KILLFOCUS:     return 0;                            // we never "lost focus"
            case WM_MOUSEACTIVATE: return MA_ACTIVATE;                  // stay active on click-through
            }
        }
        return CallWindowProcW(g_origWndProc, h, msg, w, l);
    }

    using GetFgWinFn  = HWND(WINAPI*)();
    GetFgWinFn oGetForegroundWindow = nullptr;
    GetFgWinFn oGetActiveWindow     = nullptr;
    HWND WINAPI hkGetForegroundWindow() { return (g_keepFocus && g_focusHwnd) ? g_focusHwnd : (oGetForegroundWindow ? oGetForegroundWindow() : nullptr); }
    HWND WINAPI hkGetActiveWindow()     { return (g_keepFocus && g_focusHwnd) ? g_focusHwnd : (oGetActiveWindow     ? oGetActiveWindow()     : nullptr); }
    // The third way to ask "am I in front?" (2026-09-26): keyboard focus. Only the
    // game's own thread is lied to; other threads (the overlay) see the truth.
    GetFgWinFn oGetFocus = nullptr;
    DWORD      g_focusThread = 0;
    HWND WINAPI hkGetFocus()
    {
        if (g_keepFocus && g_focusHwnd && GetCurrentThreadId() == g_focusThread) return g_focusHwnd;
        return oGetFocus ? oGetFocus() : nullptr;
    }
    // No minimizing the game window, from anyone (see WM_SYSCOMMAND above).
    using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
    ShowWindowFn oShowWindow = nullptr;
    void note(const char* fmt, ...);
    BOOL WINAPI hkShowWindow(HWND h, int cmd)
    {
        if (g_keepFocus && h && h == g_focusHwnd &&
            (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE))
        {
            static int s_blocked = 0;
            if (++s_blocked <= 3) note("minimize #%d of the game window blocked (ShowWindow %d)", s_blocked, cmd);
            cmd = SW_SHOWNOACTIVATE;
        }
        return oShowWindow ? oShowWindow(h, cmd) : FALSE;
    }

    void install_focus_keeper(HWND hw)
    {
        if (g_origWndProc || !hw) return;
        g_focusHwnd   = hw;
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(hw, GWLP_WNDPROC, (LONG_PTR)&focus_wndproc);

        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32)
        {
            MH_Initialize();   // idempotent
            void* gfw = (void*)GetProcAddress(u32, "GetForegroundWindow");
            void* gaw = (void*)GetProcAddress(u32, "GetActiveWindow");
            if (gfw && MH_CreateHook(gfw, (void*)&hkGetForegroundWindow, (void**)&oGetForegroundWindow) == MH_OK)
                MH_EnableHook(gfw);
            if (gaw && MH_CreateHook(gaw, (void*)&hkGetActiveWindow, (void**)&oGetActiveWindow) == MH_OK)
                MH_EnableHook(gaw);
            g_focusThread = GetWindowThreadProcessId(hw, nullptr);
            void* gf = (void*)GetProcAddress(u32, "GetFocus");
            void* sw = (void*)GetProcAddress(u32, "ShowWindow");
            if (gf && MH_CreateHook(gf, (void*)&hkGetFocus, (void**)&oGetFocus) == MH_OK)
                MH_EnableHook(gf);
            if (sw && MH_CreateHook(sw, (void*)&hkShowWindow, (void**)&oShowWindow) == MH_OK)
                MH_EnableHook(sw);
            if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE);
        }
    }

    // (removed 2026-07-27) The old screen-metric MinHook block + DPI-awareness call lived
    // here. Both ran from init_thread, i.e. on a worker thread racing the game's startup â€”
    // which is exactly why they never moved the render size. Superseded by earlyres.cpp,
    // which does the same job from DllMain, before the game executes anything. DPI itself
    // turned out to be irrelevant: the desktop really is 2560x1440, no Windows scaling.

    // Forcing the BUFFER square isn't enough on its own â€” confirmed in-game 2026-07-25.
    // The picture came out square but the scene was projected off-centre and sheared as
    // the head moved, because the game kept building its viewport and projection from a
    // 16:9 size while drawing into a 1:1 buffer. It takes that size from its WINDOW, so
    // the window has to be square too, and it has to happen BEFORE the swapchain is made
    // â€” by the time the game is running it has already cached the shape.
    void note(const char* fmt, ...);   // startup-transcript logger, defined with the DXGI hooks

    // Make a window's CLIENT area exactly w x h (the square-only version is above).
    void set_client_size(HWND hw, int w, int h)
    {
        if (!hw || w < 512 || h < 512) return;
        RECT rc{ 0, 0, (LONG)w, (LONG)h };
        DWORD style   = (DWORD)GetWindowLongPtrW(hw, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongPtrW(hw, GWL_EXSTYLE);
        AdjustWindowRectEx(&rc, style, FALSE, exstyle);
        const int fullW = rc.right - rc.left, fullH = rc.bottom - rc.top;

        // ...and CENTRE it on the PRIMARY monitor while we are here (JJ 2026-08-05: it
        // was landing in the bottom-left of monitor two). The window is deliberately far
        // bigger than any real monitor, so "centred" means it overhangs on every side â€”
        // which is what we want: the middle of the render, where the picture actually
        // is, lands in the middle of the screen JJ is looking at instead of spilling
        // across the second one.
        //
        // This must use the REAL primary monitor, not the spoofed metrics earlyres feeds
        // the game: GetSystemMetrics is redirected for the GAME's benefit, so we ask the
        // monitor directly and get the truth.
        int x = 0, y = 0;
        MONITORINFO mi{ sizeof(MONITORINFO) };
        HMONITOR hm = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        if (hm && GetMonitorInfoW(hm, &mi))
        {
            x = mi.rcMonitor.left + ((mi.rcMonitor.right  - mi.rcMonitor.left) - fullW) / 2;
            y = mi.rcMonitor.top  + ((mi.rcMonitor.bottom - mi.rcMonitor.top)  - fullH) / 2;
        }
        SetWindowPos(hw, nullptr, x, y, fullW, fullH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // MEASURED 2026-08-03 (build WINQ, in JJ's headset): asking once at swapchain
    // creation gets a client of 3200x**2160** â€” the width takes, the HEIGHT IS
    // REFUSED. A window cannot grow past its max TRACK size, and the default one is
    // derived from the monitor; the game also re-asserts its own geometry during
    // startup. Either way a single early attempt cannot win.
    //
    // vrframework guide 14 s4 prescribes exactly this: "Sync them from somewhere that
    // runs every frame, but make the function a no-op when already correct." So: raise
    // the ceiling (WM_GETMINMAXINFO is answered by the window's own proc, which we do
    // not own â€” instead we drop the resizable-frame constraint by asking for the size
    // through a POPUP-style adjust, which has no caption/border to fight) and retry
    // from Present until the client matches, then stop touching it forever.
    HWND g_gameHwnd  = nullptr;
    int  g_syncTries = 0;
    ULONGLONG g_lastSync = 0;
    void resync_client_size()
    {
        // WINDOWHOME 2026-10-08 (AKN DLSS): the fake screen is sized from the full headset eye now, so the game centred
        // its window far right/down - half off the monitor (Windows throttles that, VR_ESSENTIALS 4; test clicks missed).
        // Engine stereo: put the window's top-left on the main monitor's top-left (a few tries, the game may move it).
        static int s_home = 0;
        if (g_gameHwnd && akvr_engine_stereo() && s_home < 5)
        {
            RECT wr{};
            if (GetWindowRect(g_gameHwnd, &wr) && (wr.left != 0 || wr.top != 0))
            {
                SetWindowPos(g_gameHwnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                ++s_home;
            }
        }
        if (!g_gameHwnd || !g_forceRes) return;
        int want_w = akvr_early_width(), want_h = akvr_early_height();
        if (want_w < 512 || want_h < 512) return;

        RECT cr{};
        if (!GetClientRect(g_gameHwnd, &cr)) return;
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        g_clientW = cw; g_clientH = ch;
        if (cw == want_w && ch == want_h) return;      // already correct: do nothing

        ULONGLONG now = GetTickCount64();
        if (now - g_lastSync < 500) return;            // throttle: resizing is expensive
        g_lastSync = now;
        if (++g_syncTries > 40) return;                // never fight the game forever

        // Strip the caption/thick frame the first time we retry: those are what carry
        // the size constraint, and in VR nobody is looking at the desktop window.
        if (g_syncTries == 1)
        {
            LONG_PTR st = GetWindowLongPtrW(g_gameHwnd, GWL_STYLE);
            LONG_PTR want = (st & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX |
                                    WS_MINIMIZEBOX | WS_SYSMENU)) | WS_POPUP;
            if (want != st)
            {
                SetWindowLongPtrW(g_gameHwnd, GWL_STYLE, want);
                SetWindowPos(g_gameHwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
            }
        }
        set_client_size(g_gameHwnd, want_w, want_h);
        if (GetClientRect(g_gameHwnd, &cr))
        { g_clientW = cr.right - cr.left; g_clientH = cr.bottom - cr.top; }
        // MENUFLICKER 2026-10-01 â€” JJ: "the entire rendering flickers on and off for the first 10 to 20 seconds"
        // of the main menu. Every refused resize still makes the game rebuild its screen (a ~114 ms stall twice a
        // second in the mode trace), and under geo-11 Windows refuses all of them (client stays 2560x1440; the render
        // size comes from the ResX/ResY lever anyway). Two refusals in a row = stop for good.
        static int s_refused = 0;
        if (g_clientW == cw && g_clientH == ch)
        {
            if (++s_refused >= 2)
            {
                g_syncTries = 1000;
                note("window resize refused twice (client %dx%d, wanted %dx%d) - stopped retrying", cw, ch, want_w, want_h);
            }
        }
        else s_refused = 0;
    }

    // FORCE THE SIZE AT SWAPCHAIN CREATION ("the dxgi route").
    // 2026-07-27: every read-side lie has now been tried and measured. The engine ignores
    // the desktop work area beyond a point, ignores its own ini, and ignores the DXGI mode
    // list â€” proven by deleting 3840x2160 from that list and still getting 3840x2160. So we
    // stop asking and write the size on the way through, at the one moment the buffer is
    // actually created. This matches the report from a modder who solved it for UE3: force
    // ResX/ResY "with the dxgi route", e.g. 3200x3200.
    //
    // Both surfaces get set, which is what the old attempt got wrong: the swapchain buffer
    // AND the window's client area, because the engine sizes its viewport from the window.
    // vrframework guide 14 s4 says the same thing â€” the OS window and the engine's internal
    // render target must agree, and setting only one leaves the engine rendering at the old
    // size. The earlier failure (scene drawn 16:9 into the corner of a square buffer) was
    // this hook alone, before projVR existed to own the projection and before the work-area
    // spoof made the window itself grow.
    void apply_forced_size(UINT& w, UINT& h, HWND hw)
    {
        g_creationSeen++;
        g_createdW = w; g_createdH = h;

        if (g_forceRes)
        {
            int fw = akvr_early_width(), fh = akvr_early_height();
            if (fw >= 512 && fh >= 512)
            {
                g_gameHwnd = hw;   // remembered for the per-frame resync
                set_client_size(hw, fw, fh);
                // VERIFY, don't assume. set_client_size fires SetWindowPos and never
                // checked the result; Windows can clamp a resize (WM_GETMINMAXINFO)
                // and the engine sizes its viewport from the window, so a silently
                // clamped client would explain a scene smaller than the swapchain
                // (2496x2688 measured inside a 3200x3200 buffer, 2026-08-03).
                RECT cr{};
                if (hw && GetClientRect(hw, &cr))
                {
                    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
                    g_clientW = cw; g_clientH = ch;
                    note("swapchain FORCED %ux%u -> %dx%d; window client is %dx%d%s",
                         g_createdW, g_createdH, fw, fh, cw, ch,
                         (cw == fw && ch == fh) ? "" : "  <-- CLAMPED, does not match");
                }
                else
                    note("swapchain FORCED %ux%u -> %dx%d (client rect unreadable)",
                         g_createdW, g_createdH, fw, fh);
                w = (UINT)fw; h = (UINT)fh;
                return;
            }
        }
        if (!g_forceSquare || g_targetRes < 512) return;   // legacy square-only path
        set_client_square(hw, g_targetRes);
        w = (UINT)g_targetRes;
        h = (UINT)g_targetRes;
    }

    std::wstring config_ini_path()
    {
        wchar_t buf[MAX_PATH];
        if (!GetModuleFileNameW(GetModuleHandleW(nullptr), buf, MAX_PATH)) return L"";
        std::wstring p = buf;   // ...\Binaries\Win64\BatmanAK.exe
        for (int i = 0; i < 3; ++i)   // strip exe name, then Win64, then Binaries
        {
            size_t s = p.find_last_of(L"\\/");
            if (s == std::wstring::npos) return L"";
            p = p.substr(0, s);
        }
        return p + L"\\BmGame\\Config\\BmSystemSettings.ini";
    }

    // Writes ResX/ResY into the game's own BmSystemSettings.ini. MEASURED 2026-07-27:
    // this really is the request the engine honours â€” it was only ever being clamped to
    // the desktop work area, which earlyres.cpp now lifts. Applies on the NEXT launch
    // (the engine reads it once at startup), so we write it every session.
    // It used to be called with a single square number from the old force-square path,
    // which quietly overwrote whatever resolution we'd set up. Now it takes both axes.
    bool write_resolution(int resX, int resY)
    {
        std::wstring path = config_ini_path();
        if (path.empty()) return false;
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); return false; }
        std::string data((size_t)n, '\0');
        fread(&data[0], 1, (size_t)n, f); fclose(f);

        auto replace_line = [&](const char* key, int val) {
            size_t pos = 0;
            while ((pos = data.find(key, pos)) != std::string::npos)
            {
                if (pos == 0 || data[pos - 1] == '\n' || data[pos - 1] == '\r')   // line start only
                {
                    size_t eol = data.find_first_of("\r\n", pos);
                    if (eol == std::string::npos) eol = data.size();
                    char repl[32]; snprintf(repl, sizeof(repl), "%s%d", key, val);
                    data.replace(pos, eol - pos, repl);
                    return;
                }
                pos += 1;
            }
        };
        replace_line("ResX=", resX);
        replace_line("ResY=", resY);

        FILE* o = _wfopen(path.c_str(), L"wb");
        if (!o) return false;
        fwrite(data.data(), 1, data.size(), o); fclose(o);
        return true;
    }

    // User picked a resolution: make it the target + persist to the .ini. The
    // force-square hook (hkResizeBuffers) applies this SIZE on the next backbuffer
    // resize (entering the game's video menu triggers one) or on restart. The
    // square SHAPE is always enforced regardless.
    void set_game_resolution(int n)
    {
        g_targetRes = n;
        write_resolution(n, n);
        g_forceTries = 0;   // let the live self-heal have a fresh go at the new size
        g_lastForce  = 0;
    }

    // ---- settings persistence (remembered across launches) ----
    // Stored next to the game exe / our DLL as a small text file.
    std::wstring settings_path()
    {
        wchar_t buf[MAX_PATH];
        if (!GetModuleFileNameW(GetModuleHandleW(nullptr), buf, MAX_PATH)) return L"";
        std::wstring p = buf;
        size_t s = p.find_last_of(L"\\/");
        if (s == std::wstring::npos) return L"";
        return p.substr(0, s) + L"\\akvr_settings.ini";
    }


    // The eye-separation value that JJ calibrated in-game as correct 1:1 world scale
    // (2026-08-05). The panel and the ini both work in world-scale units now, where
    // this reads exactly 1.00; the engine side is unchanged.
    const float kStereoAt1 = 2.8f;

    // Same treatment for the positional-lean gain, on JJ's instruction 2026-08-05:
    // the value he had tuned to (102 engine units per metre) is the correct one, so it
    // now reads 1.00. The slider is gone from the panel; PageUp/PageDown and the ini
    // still tune it, and the diagnostic line shows it in these units.
    const float kLeanAt1 = 102.0f;

    // EYEVIEW 2026-09-27 (SKVR port, see xr.cpp g_eyeView): the saved choice (applies at the
    // next launch, because the render width is fixed per launch), geo-11's NDC shift per
    // separation unit (SKVR measured 0.00105; unverified on Arkham until an F2 pair is
    // measured), and the shape to go back to when it is switched off.
    bool  g_eyeViewSaved = false;
    float g_eyeUnit = 0.00105f;
    int   g_eyeBaseShape = 0;

    // EDGES 2026-09-27 (JJ: "copy the same sliders that Sekiro has for extending the edges of the
    // window a bit horizontally so that you don't see framing where the nose is"). SKVR OVERSCAN +
    // VEDGE: the sides margin is the existing live FOV trim (akvr_head_fov_delta: render FOV and the
    // submitted frustum grow together); the top/bottom margin sets the NEXT launch's picture shape,
    // because the vertical extent follows the render's shape. g_ovV < 0 = same as the sides.
    float g_ovV = -1.0f;
    // Next launch's geo-11 render shape (wide:tall x1000) from the measured headset tangents, the
    // eye-view choice and both margins; -1 until the headset has reported its shape.
    int next_geo11_shape(bool eyeView)
    {
        float o, i, u, d;
        if (!akvr_xr_eye_tangents(o, i, u, d)) return -1;
        const float d2r = 3.14159265f / 180.0f;
        const float ovH = akvr_head_fov_delta() > 0.0f ? akvr_head_fov_delta() : 0.0f;
        const float ovV = g_ovV < 0.0f ? ovH : g_ovV;
        const float hHalf = eyeView ? (o + i) * 0.5f : o;
        const float vHalf = (u + d) * 0.5f;              // tilt fill centres the vertical view
        const float h = tanf(atanf(hHalf) + ovH * 0.5f * d2r);
        const float v = tanf(atanf(vHalf) + ovV * 0.5f * d2r);
        int sh = (int)lroundf(1000.0f * h / v);
        return sh == 1000 ? 992 : sh;                    // exact square breaks geo-11 depth here
    }

    bool g_toneTest = false;   // TONETEST: settings tonetest=1 (one-off diagnosis, not saved)
    void settings_save()
    {
        std::wstring path = settings_path();
        if (path.empty()) return;
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return;
        fprintf(f, "worldscale=%.4f\n", akvr_head_stereo() / kStereoAt1);
        fprintf(f, "geoscale=%.4f\n", akvr_geo11conv_scale());
        fprintf(f, "eyeview=%d\n", g_eyeViewSaved ? 1 : 0);
        fprintf(f, "eyek=%.4f\n", akvr_xr_eye_k_setting());
        fprintf(f, "eyeflip=%d\n", akvr_xr_eye_flip() ? 1 : 0);
        fprintf(f, "eyeunit=%.6f\n", g_eyeUnit);
        fprintf(f, "eyebaseshape=%d\n", g_eyeBaseShape);
        fprintf(f, "overscanv=%.2f\n", g_ovV);
        fprintf(f, "huddist=%.2f\n", akvr_geo11_hud_dist());
        fprintf(f, "hudlayer=%d\n", akvr_hudsplit_layer() ? 1 : 0);   // HUDLAYER
        // PANELTIDY: hudlayerspace / hudlayercolour / hudlayereyes / hudlayersquash are fixed now, not saved.
        fprintf(f, "hudattach=%d\n", akvr_xr_hud_space() == 1 ? 1 : 0);   // HUDWORLD: 0 = fixed in the room
        fprintf(f, "hudroomall=%d\n", akvr_hud_room_all() ? 1 : 0);   // ROOMALL
        fprintf(f, "menufit=%d\n", akvr_menu_fit() ? 1 : 0);   // MENUFIT
        fprintf(f, "vigstrength=%.2f\n", akvr_xr_vig_strength());   // ZOOMVIG
        fprintf(f, "vigclear=%.0f\n", akvr_xr_vig_clear());
        fprintf(f, "vigbelow=%.0f\n", akvr_xr_vig_below());
        fprintf(f, "hudlayersplit=%d\n", akvr_hudsplit_split() ? 1 : 0);   // HUDSPLIT
        fprintf(f, "hudsteady=%.2f\n", akvr_hud_steady());
        fprintf(f, "convergence=%.4f\n", akvr_head_convergence());
        fprintf(f, "spinfoldon=%d\n", akvr_xr_yawfold_on() ? 1 : 0);
        fprintf(f, "spinfold=%.4f\n", akvr_xr_yawfold());
        fprintf(f, "wide=%d\n",      akvr_head_wide_render() ? 1 : 0);
        fprintf(f, "leanscale=%.4f\n", akvr_head_pos_scale() / kLeanAt1);
        fprintf(f, "shoulder=%.3f\n", akvr_head_shoulder());
        fprintf(f, "menuside=%.1f\n", akvr_head_menu_side());   // MENUSIDE
        fprintf(f, "camrestore=%d\n", akvr_cam_restore() ? 1 : 0);   // CAMRESTORE
        fprintf(f, "camsmooth=%d\n", akvr_cam_smooth() ? 1 : 0);   // CAMSMOOTH
        fprintf(f, "camswing=%d\n", akvr_cam_swing() ? 1 : 0);   // SWINGEASE
        fprintf(f, "camaimhead=%d\n", akvr_cam_aim_head() ? 1 : 0);   // AIMHEAD
        fprintf(f, "menutextleft=%.3f\n", akvr_menu_text_left());   // MENUTEXT
        fprintf(f, "pitchkeep=%.3f\n", akvr_head_pitch_keep());
        fprintf(f, "pitchunlink=%d\n", akvr_head_pitch_unlink());
        fprintf(f, "sbsswapchain=%d\n", akvr_xr_sbs_one() ? 1 : 0);
        fprintf(f, "stickheight=%.2f\n", akvr_head_stick_height());
        fprintf(f, "offsetup=%.3f\n",  akvr_head_offset_up());
        fprintf(f, "offsetfwd=%.3f\n", akvr_head_offset_fwd());
        fprintf(f, "fov=%.3f\n",    akvr_head_fov_delta());
        fprintf(f, "screen=%.3f\n", akvr_xr_menu_zoom());
        fprintf(f, "pausescreen=%.3f\n", akvr_xr_pause_zoom());
        fprintf(f, "splashscreen=%.3f\n", akvr_xr_splash_zoom());   // SPLASHSIZE
        fprintf(f, "loadscreen=%.3f\n", akvr_xr_load_zoom());   // LOADSIZE
        fprintf(f, "loadup=%.1f\n", akvr_xr_load_up());   // LOADUP
        fprintf(f, "screendist=%.2f\n", akvr_xr_screen_near());   // SCREENNEAR
        fprintf(f, "pauselook=%d\npausedim=%.2f\n", akvr_xr_pause_look() ? 1 : 0, akvr_xr_pause_dim());   // PAUSELOOK / PAUSEDIM
        // PANELTIDY5 2026-10-11: nearrain / nearrainlag / rainnostretch / rainframefix / testtools no longer saved (old keys are ignored)
        fprintf(f, "markerhead=%d\n", akvr_marker_head() ? 1 : 0);   // TARGETMOVE / TARGETSCALE
        fprintf(f, "markerface=%d\n", akvr_marker_face() ? 1 : 0);   // TARGETFACE
        fprintf(f, "markeruplag=%d\n", akvr_marker_up_lag());   // TARGETUP2
        fprintf(f, "markerdist=%d\n", akvr_marker_dist() ? 1 : 0);   // TARGETDIST
        fprintf(f, "framefix=%d\nframedelay=%d\n", akvr_frame_fix() ? 1 : 0, akvr_frame_world());   // FRAMEPAIR(2)
        if (g_menuCapture) fprintf(f, "menucapture=1\n");             // MENUBLACK: written only when on
        fprintf(f, "farrain=%d\n", akvr_far_rain() ? 1 : 0);   // FARRAIN
        fprintf(f, "hudeyefollow=%d\npausenoback=%d\n", akvr_xr_hud_eye_follow() ? 1 : 0, akvr_xr_pause_no_back() ? 1 : 0);   // HUDEYES / PAUSENOBACK
        fprintf(f, "menubackopacity=%.2f\n", akvr_xr_menu_back_opacity());   // MENUBACK (replaces menunoback)
        fprintf(f, "audiosync=%d\n", akvr_audiosync() ? 1 : 0);   // AUDIOSYNC
        fprintf(f, "transfull2=%d\n", akvr_trans_full() ? 1 : 0);   // TRANSFULL
        fprintf(f, "menubackfeather=%.2f\n", akvr_xr_menu_back_feather());   // MENUFEATHER
        fprintf(f, "pausemenusize=%.2f\n", akvr_xr_pause_menu_size());   // PAUSESIZE
        fprintf(f, "pauseaspect=%.3f\n", akvr_xr_pause_aspect());
        fprintf(f, "automainmenu=%d\n", akvr_xr_auto_main_menu() ? 1 : 0);
        fprintf(f, "fullview=%d\n", akvr_xr_full_view() ? 1 : 0);
        fprintf(f, "tiltfill=%d\n", akvr_xr_tilt_fill() ? 1 : 0);
        fprintf(f, "screentrack=%d\n", akvr_xr_screen_track() ? 1 : 0);
        fprintf(f, "screenaspect=%.3f\n", akvr_xr_screen_aspect());
        fprintf(f, "floatscreen=%.3f\n", akvr_xr_float_screen_scale());
        fprintf(f, "menuflat=%d\n", akvr_xr_menu_flat() ? 1 : 0);
        fprintf(f, "menuonespot=%d\n", akvr_xr_menu_one_spot() ? 1 : 0);   // MENU3D
        fprintf(f, "lensmode=%d\n", akvr_xr_lens_mode());   // SAMEDIR (engine stereo): 2 turned eyes, 3 one direction
        fprintf(f, "hudineye=%d\n", akvr_xr_hud_in_eye());   // HUDINEYE
        fprintf(f, "markercut=%d\n", akvr_xr_markercut());   // MARKERCUT
        fprintf(f, "markersize=%.2f\n", akvr_xr_marker_scale());   // MARKERWORLD
        fprintf(f, "menu3d=%d\n", akvr_xr_menu3d() ? 1 : 0);
        fprintf(f, "res=%d\n",      g_targetRes);
        fprintf(f, "dpiaware=%d\n",  g_dpiAware ? 1 : 0);
        fprintf(f, "clientscale=%.3f\n", g_clientScale);
        // MUST persist: the square decision is taken at swapchain creation, before the
        // settings menu exists. Without this, unticking the box can't survive a restart
        // and a bad forced size would be unescapable from inside the game.
        fprintf(f, "square=%d\n",   g_forceSquare ? 1 : 0);
        fprintf(f, "vpsquare=%d\n", g_vpSquareWanted ? 1 : 0);
        fprintf(f, "vpfill=%d\n",   g_vpFill ? 1 : 0);
        fprintf(f, "windowed=%d\n", g_forceWindowed ? 1 : 0);
        fprintf(f, "squareview=%d\n", akvr_xr_square_present() ? 1 : 0);
        // bigres â€” read by earlyres.cpp at DLL load, long before this file's loader
        // runs, so it has its own tiny parser. Written here so the panel's slider
        // sticks. Takes effect on the NEXT launch by definition.
        fprintf(f, "bigres=%d\n",       akvr_early_enabled() ? 1 : 0);
        fprintf(f, "rendersize=%d\n",   akvr_early_height());
        fprintf(f, "engineres=%d\n",    akvr_engine_res());
        fprintf(f, "dlss=%d\n",         akvr_dlss_mode());   // DLSSPANEL: read by earlyres.cpp at load
        fprintf(f, "afw=%d\n",          g_afwSet);           // AFW: read by AKN (afw.cpp) at start
        fprintf(f, "first_person=%d\n", akvr_fp() ? 1 : 0);   // FIRSTPERSON
        fprintf(f, "fp_forward_cm=%.1f\n", akvr_fp_fwd());
        fprintf(f, "fp_up_cm=%.1f\n", akvr_fp_up());
        fprintf(f, "fp_head=%d\n", akvr_fp_head() ? 1 : 0);
        fprintf(f, "fp_head_smooth=%.3f\n", akvr_fp_smooth());
        fprintf(f, "afwmode=%d\n",      g_afwModeSet);
        fprintf(f, "engineshape=%d\n",  akvr_engine_shape());
        fprintf(f, "geo11shape=%d\n",   akvr_geo11_shape());   // read by earlyres.cpp at load
        fprintf(f, "hudscale=%d\n",     (int)(akvr_hud_scale() * 1000.0f + 0.5f));
        fprintf(f, "hudscalev=%d\n",    (int)(akvr_hud_scale_v() * 1000.0f + 0.5f));
        fprintf(f, "menuuifill=%d\n",   akvr_hud_menu_fill() ? 1 : 0);
        fprintf(f, "hudglobal=%d\n",    akvr_hud_global() ? 1 : 0);
        fprintf(f, "noshrink=%s\n",     akvr_hud_noshrink());
        fprintf(f, "piecexf=%s\n",      akvr_hud_piece_xf_list());
        fprintf(f, "hudlayers=%s\n",    akvr_hud_layer_xf_list());
        fprintf(f, "hudcontainers=%s\n", akvr_hud_containers_list());
        fprintf(f, "huddual=%d\n",      akvr_hud_dual() ? 1 : 0);
        fprintf(f, "hudscalemode=%d\n", akvr_hud_scale_mode() ? 1 : 0);
        fprintf(f, "hudrootmode=%d\n",  akvr_hud_root_mode() ? 1 : 0);
        fprintf(f, "hudtexfix=%d\n",    akvr_hud_tex_fix() ? 1 : 0);
        fprintf(f, "hudraise=%d\n",     (int)(akvr_hud_raise() * 1000.0f + (akvr_hud_raise() >= 0 ? 0.5f : -0.5f)));
        fprintf(f, "hudaspect=%d\n",    akvr_hud_aspect_fix() ? 1 : 0);
        fprintf(f, "rendersquare=%d\n", akvr_early_square() ? 1 : 0);
        fprintf(f, "bigreschrome=%d\n", akvr_early_chrome());
        fprintf(f, "forceres=%d\n",     g_forceRes ? 1 : 0);
        // projVR was session-only ("experimental"). It has to persist now: with a forced
        // square frame, launching with the square lens OFF renders a 16:9 view stretched
        // into a square buffer, which looks like a broken mod rather than an unticked box.
        fprintf(f, "projvr=%d\n",       akvr_projvr() ? 1 : 0);
        // geo-11 path: take the eyes from its shared stereo surface rather than
        // from the swapchain (which only carries its over/under monitor preview).
        fprintf(f, "katfeed=%d\n",      akvr_xr_kat_feed() ? 1 : 0);
        fprintf(f, "swapeyes=%d\n",     akvr_xr_native_swap_eyes() ? 1 : 0);
        fprintf(f, "nativeafterpresent=%d\n", g_nativeAfterPresent ? 1 : 0);
        fprintf(f, "posedelay=%d\n",    akvr_xr_pose_delay());
        fprintf(f, "menuposedelay=%d\n", akvr_xr_menu_pose_delay());   // MENUDELAY
        fprintf(f, "fpslock=%d\n",      akvr_xr_fps_lock());
        fprintf(f, "fpslockssw=%d\n",   akvr_xr_fps_lock_ssw() ? 1 : 0);
        fprintf(f, "poseauto=%d\n",     akvr_xr_pose_auto() ? 1 : 0);
        fprintf(f, "xrhkcu=%d\n",       g_useHkcuRuntime ? 1 : 0);
        fprintf(f, "xrspace=%d\n",      akvr_xr_space_mode());   // SVRSPACE
        fprintf(f, "svrstraight=%d\n",  akvr_xr_svr_straight());   // SVRSTRAIGHT
        fclose(f);
    }

    void settings_load()
    {
        std::wstring path = settings_path();
        if (path.empty()) return;
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return;
        char line[4608];                 // PIECEXF / HUDLAYERS lists can be long
        while (fgets(line, sizeof(line), f))
        {
            float v; int iv;
            // 2026-10-06: newer keys as separate checks (the else-if chain below hit MSVC's nesting limit, C1061)
            if (sscanf(line, "menufit=%d", &iv) == 1) akvr_menu_fit_set(iv != 0);   // MENUFIT
            if (sscanf(line, "afwmode=%d", &iv) == 1) g_afwModeSet = iv >= 1 && iv <= 3 ? iv : 3;   // AFW
            if (sscanf(line, "afw=%d", &iv) == 1) g_afwSet = iv ? 1 : 0;
            if (sscanf(line, "first_person=%d", &iv) == 1) akvr_fp_set(iv != 0);   // FIRSTPERSON
            if (sscanf(line, "fp_forward_cm=%f", &v) == 1) akvr_fp_fwd_set(v);
            if (sscanf(line, "fp_up_cm=%f", &v) == 1) akvr_fp_up_set(v);
            if (sscanf(line, "fp_head=%d", &iv) == 1) akvr_fp_head_set(iv != 0);
            if (sscanf(line, "fp_head_smooth=%f", &v) == 1) akvr_fp_smooth_set(v);
            if (sscanf(line, "flareoff=%d", &iv) == 1) akvr_flareoff_set(iv);   // FLAREOFF3
            if (sscanf(line, "tonetest=%d", &iv) == 1) g_toneTest = iv != 0;   // TONETEST
            // "worldscale" is the current unit; "depth" is the old raw one, still read
            // so an existing settings file keeps working.
            if      (sscanf(line, "worldscale=%f", &v) == 1) akvr_head_stereo_set(v * kStereoAt1);
            else if (sscanf(line, "geoscale=%f",   &v) == 1) akvr_geo11conv_scale_set(v);
            else if (sscanf(line, "eyeview=%d", &iv) == 1) { g_eyeViewSaved = iv != 0; akvr_xr_eye_view_set(g_eyeViewSaved); }
            else if (sscanf(line, "eyek=%f", &v) == 1) akvr_xr_eye_k_set(v);
            else if (sscanf(line, "eyeflip=%d", &iv) == 1) akvr_xr_eye_flip_set(iv != 0);
            else if (sscanf(line, "eyeunit=%f", &v) == 1) { if (v > 0.0001f && v < 0.01f) g_eyeUnit = v; }
            else if (sscanf(line, "eyebaseshape=%d", &iv) == 1) g_eyeBaseShape = iv;
            else if (sscanf(line, "overscanv=%f", &v) == 1) g_ovV = v < 0.0f ? -1.0f : (v > 40.0f ? 40.0f : v);
            else if (sscanf(line, "huddist=%f", &v) == 1) akvr_geo11_hud_dist_set(v);
            else if (sscanf(line, "hudlayer=%d", &iv) == 1) akvr_hudsplit_layer_set(iv != 0);   // HUDLAYER
            else if (sscanf(line, "hudattach=%d", &iv) == 1) akvr_xr_hud_space_set(iv ? 1 : 2);   // HUDWORLD
            else if (sscanf(line, "hudroomall=%d", &iv) == 1) akvr_hud_room_all_set(iv != 0);   // ROOMALL
            else if (sscanf(line, "vigstrength=%f", &v) == 1) akvr_xr_vig_strength_set(v);   // ZOOMVIG
            else if (sscanf(line, "vigclear=%f", &v) == 1) akvr_xr_vig_clear_set(v);
            else if (sscanf(line, "vigbelow=%f", &v) == 1) akvr_xr_vig_below_set(v);
            else if (sscanf(line, "hudlayersplit=%d", &iv) == 1) akvr_hudsplit_split_set(iv != 0);   // HUDSPLIT
            else if (sscanf(line, "hudsteady=%f", &v) == 1) akvr_hud_steady_set(0.0f);   // HUDDEPTH: parked, always off
            else if (sscanf(line, "depth=%f",      &v) == 1) akvr_head_stereo_set(v);
            else if (sscanf(line, "convergence=%f", &v) == 1) akvr_head_convergence_set(v);
            else if (sscanf(line, "spinfoldon=%d", &iv) == 1)
            { akvr_xr_yawfold_on_set(iv != 0); akvr_head_camfix_set(iv != 0); }
            else if (sscanf(line, "spinfold=%f",    &v) == 1) akvr_xr_yawfold_set(v);
            else if (sscanf(line, "wide=%d",       &iv) == 1) akvr_head_wide_render_set(iv != 0);
            else if (sscanf(line, "leanscale=%f",  &v) == 1) akvr_head_pos_scale_set(v * kLeanAt1);
            else if (sscanf(line, "lean=%f",       &v) == 1) akvr_head_pos_scale_set(v);
            else if (sscanf(line, "shoulder=%f",   &v) == 1) akvr_head_shoulder_set(v);
            else if (sscanf(line, "menuside=%f",   &v) == 1) akvr_head_menu_side_set(v);   // MENUSIDE
            else if (sscanf(line, "camrestore=%d", &iv) == 1) akvr_cam_restore_set(iv != 0);   // CAMRESTORE
            else if (sscanf(line, "camsmooth=%d", &iv) == 1) akvr_cam_smooth_set(iv != 0);   // CAMSMOOTH
            else if (sscanf(line, "camswing=%d", &iv) == 1) akvr_cam_swing_set(iv != 0);   // SWINGEASE
            else if (sscanf(line, "camaimhead=%d", &iv) == 1) akvr_cam_aim_head_set(iv != 0);   // AIMHEAD
            else if (sscanf(line, "menutextleft=%f", &v) == 1) akvr_menu_text_left_set(v);   // MENUTEXT
            else if (sscanf(line, "pitchkeep=%f",  &v) == 1) akvr_head_pitch_keep_set(0.0f);   // settled: view level (v ignored)
            else if (sscanf(line, "pitchunlink=%d", &iv) == 1) akvr_head_pitch_unlink_set(iv == 2 ? 2 : 0);   // 1 (horizon-level tilt) retired; 2 = UEVR-style
            else if (sscanf(line, "stickheight=%f", &v) == 1) akvr_head_stick_height_set(v);
            else if (sscanf(line, "offsetup=%f",   &v) == 1) akvr_head_offset_up_set(v);
            else if (sscanf(line, "offsetfwd=%f",  &v) == 1) akvr_head_offset_fwd_set(v);
            else if (sscanf(line, "fov=%f",    &v) == 1) akvr_head_fov_set(v);
            else if (sscanf(line, "screen=%f", &v) == 1) akvr_xr_menu_zoom_set(v);
            else if (sscanf(line, "pausescreen=%f", &v) == 1) akvr_xr_pause_zoom_set(v);
            else if (sscanf(line, "splashscreen=%f", &v) == 1) akvr_xr_splash_zoom_set(v);   // SPLASHSIZE
            else if (sscanf(line, "loadscreen=%f", &v) == 1) akvr_xr_load_zoom_set(v);   // LOADSIZE
            else if (sscanf(line, "loadup=%f", &v) == 1) akvr_xr_load_up_set(v);   // LOADUP
            else if (sscanf(line, "screendist=%f", &v) == 1) akvr_xr_screen_near_set(v);   // SCREENNEAR
            else if (sscanf(line, "pauselook=%d", &iv) == 1) akvr_xr_pause_look_set(iv != 0);   // PAUSELOOK
            else if (sscanf(line, "pausedim=%f", &v) == 1) akvr_xr_pause_dim_set(v);   // PAUSEDIM
            else if (sscanf(line, "markerhead=%d", &iv) == 1) akvr_marker_head_set(iv != 0);   // TARGETMOVE
            else if (sscanf(line, "markerface=%d", &iv) == 1) akvr_marker_face_set(iv != 0);   // TARGETFACE
            else if (sscanf(line, "markeruplag=%d", &iv) == 1) akvr_marker_up_lag_set(iv);   // TARGETUP2
            else if (sscanf(line, "markerdist=%d", &iv) == 1) akvr_marker_dist_set(iv != 0);   // TARGETDIST
            else if (sscanf(line, "markerlag=%d", &iv) == 1) akvr_marker_lag_set(iv);
            else if (sscanf(line, "markerhudlag=%d", &iv) == 1) akvr_marker_hud_lag_set(iv);   // TARGETMOVE2
            else if (sscanf(line, "menucapture=%d", &iv) == 1) g_menuCapture = iv != 0;   // MENUBLACK (no panel switch)
            else if (sscanf(line, "framefix=%d", &iv) == 1) akvr_frame_fix_set(iv != 0);   // FRAMEPAIR
            else if (sscanf(line, "framedelay=%d", &iv) == 1) akvr_frame_world_set(iv);
            else if (sscanf(line, "farrain=%d", &iv) == 1) akvr_far_rain_set(iv != 0);   // FARRAIN
            else if (sscanf(line, "hudeyefollow=%d", &iv) == 1) akvr_xr_hud_eye_follow_set(iv != 0);   // HUDEYES
            else if (sscanf(line, "pausenoback=%d", &iv) == 1) akvr_xr_pause_no_back_set(iv != 0);   // PAUSENOBACK
            else if (sscanf(line, "menubackopacity=%f", &v) == 1) akvr_xr_menu_back_opacity_set(v);   // MENUBACK
            else if (sscanf(line, "audiosync=%d", &iv) == 1) akvr_audiosync_set(iv != 0);   // AUDIOSYNC
            else if (sscanf(line, "transfull2=%d", &iv) == 1) akvr_trans_full_set(iv != 0);   // TRANSFULL
            else if (sscanf(line, "menubackfeather=%f", &v) == 1) akvr_xr_menu_back_feather_set(v);   // MENUFEATHER
            else if (sscanf(line, "pausemenusize=%f", &v) == 1) akvr_xr_pause_menu_size_set(v);   // PAUSESIZE
            else if (sscanf(line, "pauseaspect=%f", &v) == 1) akvr_xr_pause_aspect_set(v);
            else if (sscanf(line, "automainmenu=%d", &iv) == 1) akvr_xr_auto_main_menu_set(iv != 0);
            else if (sscanf(line, "fullview=%d", &iv) == 1) akvr_xr_full_view_set(iv != 0);
            else if (sscanf(line, "tiltfill=%d", &iv) == 1) akvr_xr_tilt_fill_set(iv != 0);
            else if (sscanf(line, "screentrack=%d", &iv) == 1) akvr_xr_screen_track_set(iv != 0);
            else if (sscanf(line, "screenaspect=%f", &v) == 1) akvr_xr_screen_aspect_set(v);
            else if (sscanf(line, "floatscreen=%f", &v) == 1) akvr_xr_float_screen_scale_set(v);
            else if (sscanf(line, "menuflat=%d", &iv) == 1) akvr_xr_menu_flat_set(iv != 0);
            else if (sscanf(line, "menuonespot=%d", &iv) == 1) akvr_xr_menu_one_spot_set(iv != 0);   // MENU3D
            else if (sscanf(line, "lensmode=%d", &iv) == 1) akvr_xr_lens_mode_set(iv);   // SAMEDIR
            else if (sscanf(line, "hudineye=%d", &iv) == 1) akvr_xr_hud_in_eye_set(iv);   // HUDINEYE
            else if (sscanf(line, "markercut=%d", &iv) == 1) akvr_xr_markercut_set(iv);   // MARKERCUT
            else if (sscanf(line, "menu3d=%d", &iv) == 1) akvr_xr_menu3d_set(iv != 0);
            else if (sscanf(line, "res=%d",    &iv) == 1 && iv >= 512 && iv <= 8192) g_targetRes = iv;
            else if (sscanf(line, "dpiaware=%d", &iv) == 1) g_dpiAware = (iv != 0);
            else if (sscanf(line, "clientscale=%f", &v) == 1 && v >= 1.0f && v <= 3.0f) g_clientScale = v;
            else if (sscanf(line, "square=%d", &iv) == 1) g_forceSquare = (iv != 0);
            else if (sscanf(line, "vpsquare=%d", &iv) == 1) g_vpSquareWanted = (iv != 0);
            // vpfill and squareview are RETIRED (their panel controls were removed
            // 2026-08-05). Both are actively harmful now â€” vpfill stretches the drawing
            // area and squareview's crop fights projVR â€” so the keys are still parsed,
            // to stay a valid settings file, but the values are DISCARDED. A control
            // that can no longer be reached must not still be switchable from a file.
            else if (sscanf(line, "vpfill=%d", &iv) == 1)     { g_vpFill = false; }
            else if (sscanf(line, "windowed=%d", &iv) == 1) g_forceWindowed = (iv != 0);
            else if (sscanf(line, "squareview=%d", &iv) == 1) { akvr_xr_square_present_set(false); }
            // bigres keys are consumed by earlyres.cpp at load time; mirrored here only
            // so the panel shows the live values and settings_save() round-trips them.
            else if (sscanf(line, "bigres=%d", &iv) == 1) akvr_early_enable(iv != 0);
            else if (sscanf(line, "rendersize=%d", &iv) == 1) akvr_early_size_set(iv);
            else if (sscanf(line, "rendersquare=%d", &iv) == 1) akvr_early_square_set(iv != 0);
            else if (sscanf(line, "engineres=%d",    &iv) == 1) akvr_engine_res_set(iv);
            else if (sscanf(line, "dlss=%d", &iv) == 1) akvr_dlss_mode_set(iv);   // DLSSPANEL
            else if (sscanf(line, "engineshape=%d",  &iv) == 1) akvr_engine_shape_set(iv);
            else if (sscanf(line, "hudscale=%d",     &iv) == 1) akvr_hud_scale_set(iv / 1000.0f);
            else if (sscanf(line, "hudscalev=%d",    &iv) == 1) akvr_hud_scale_v_set(iv / 1000.0f);
            else if (sscanf(line, "menuuifill=%d",   &iv) == 1) akvr_hud_menu_fill_set(iv != 0);
            else if (sscanf(line, "hudglobal=%d",    &iv) == 1) akvr_hud_global_set(iv != 0);
            else if (sscanf(line, "huddual=%d",      &iv) == 1) akvr_hud_dual_set(iv != 0);
            else if (sscanf(line, "hudscalemode=%d", &iv) == 1) akvr_hud_scale_mode_set(false);   // retired
            else if (sscanf(line, "hudrootmode=%d",  &iv) == 1) akvr_hud_root_mode_set(true);     // the radar fix, settled
            else if (sscanf(line, "hudtexfix=%d",    &iv) == 1) akvr_hud_tex_fix_set(iv != 0);
            else if (strncmp(line, "noshrink=", 9) == 0)
            {
                char list[1024]; strncpy_s(list, line + 9, _TRUNCATE);
                list[strcspn(list, "\r\n")] = 0;
                akvr_hud_noshrink_set(list);
            }
            else if (sscanf(line, "advancedpanel=%d", &iv) == 1) g_showAdvanced = iv != 0;
            else if (sscanf(line, "sbsswapchain=%d", &iv) == 1) akvr_xr_sbs_one_set(iv != 0);
            else if (strncmp(line, "hudcontainers=", 14) == 0)
            {
                static char list[4600]; strncpy_s(list, line + 14, _TRUNCATE);
                list[strcspn(list, "\r\n")] = 0;
                akvr_hud_containers_list_set(list);
            }
            else if (strncmp(line, "hudlayers=", 10) == 0)
            {
                char list[4096]; strncpy_s(list, line + 10, _TRUNCATE);
                list[strcspn(list, "\r\n")] = 0;
                akvr_hud_layer_xf_list_set(list);
            }
            else if (strncmp(line, "piecexf=", 8) == 0)
            {
                char list[2048]; strncpy_s(list, line + 8, _TRUNCATE);
                list[strcspn(list, "\r\n")] = 0;
                akvr_hud_piece_xf_list_set(list);
            }
            else if (sscanf(line, "hudraise=%d",     &iv) == 1) akvr_hud_raise_set(iv / 1000.0f);
            else if (sscanf(line, "hudaspect=%d",    &iv) == 1) akvr_hud_aspect_fix_set(iv != 0);
            else if (sscanf(line, "forceres=%d", &iv) == 1) g_forceRes = (iv != 0);
            else if (sscanf(line, "projvr=%d", &iv) == 1) akvr_projvr_set(iv != 0);
            else if (sscanf(line, "katfeed=%d", &iv) == 1) akvr_xr_kat_feed_set(iv != 0);
            else if (sscanf(line, "swapeyes=%d", &iv) == 1) akvr_xr_native_swap_eyes_set(iv != 0);
            else if (sscanf(line, "nativeafterpresent=%d", &iv) == 1) g_nativeAfterPresent = iv != 0;
            else if (sscanf(line, "posedelay=%d", &iv) == 1) akvr_xr_pose_delay_set(iv);
            else if (sscanf(line, "menuposedelay=%d", &iv) == 1) akvr_xr_menu_pose_delay_set(iv);   // MENUDELAY
            else if (sscanf(line, "fpslock=%d", &iv) == 1) akvr_xr_fps_lock_set(iv);
            else if (sscanf(line, "fpslockssw=%d", &iv) == 1) akvr_xr_fps_lock_ssw_set(iv != 0);
            else if (sscanf(line, "poseauto=%d", &iv) == 1) akvr_xr_pose_auto_set(iv != 0);
            else if (sscanf(line, "xrhkcu=%d",  &iv) == 1) g_useHkcuRuntime = (iv != 0);
            else if (sscanf(line, "xrprobe=%d", &iv) == 1) g_xrProbe        = (iv != 0);
            else if (sscanf(line, "presenthook=%d", &iv) == 1) g_presentHook = (iv != 0);
            // SVRSPACE / SVRSTRAIGHT / MARKERWORLD: outside the chain above (the compiler's nesting limit, C1061)
            { float fv = 0.0f; if (sscanf(line, "markersize=%f", &fv) == 1) akvr_xr_marker_scale_set(fv); }
            if (sscanf(line, "xrspace=%d", &iv) == 1) akvr_xr_space_mode_set(iv);
            if (sscanf(line, "svrstraight=%d", &iv) == 1) akvr_xr_svr_straight_set(iv);
        }
        fclose(f);
    }

    bool create_rtv(IDXGISwapChain* swapChain)
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)) || !backBuffer)
            return false;
        HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
        return SUCCEEDED(hr);
    }

    // --- scene-viewport observation (RE: find the game's TRUE scene render size) ---
    // Hook ID3D11DeviceContext::RSSetViewports (vtable slot 44). The 3D scene is drawn
    // into the biggest viewport the game sets each frame; its size + aspect are the
    // render resolution and shape we've been trying to control. We only OBSERVE for now
    // (report it), so we target the override precisely instead of guessing at game code.
    using RSSetViewportsFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
    RSSetViewportsFn oRSSetViewports = nullptr;
    bool  g_vpHooked = false;
    // Track two candidates each frame: the biggest 16:9-ish viewport (the SCENE) and the
    // biggest overall (usually a shadow/reflection pass â€” a red herring like 2496x2688).
    float g_vpSceneA = 0, g_vpSceneW = 0, g_vpSceneH = 0;   // scene candidate (this frame)
    float g_vpBigA = 0,   g_vpBigW = 0,   g_vpBigH = 0;     // biggest overall (this frame)
    float g_rSceneW = 0, g_rSceneH = 0, g_rBigW = 0, g_rBigH = 0;   // reported (last frame)

    bool g_vpForceSquare = false;   // TEST: reshape the scene viewport to a centred square

    // Live backbuffer size, published by enforce_square() each Present (it already reads
    // the desc). The viewport FILL mode below needs it and must not call GetDesc itself â€”
    // RSSetViewports runs many times a frame and on the render thread.
    UINT g_bbW = 0, g_bbH = 0;
    // F2 sets this; the grab itself must run on the render thread (in Present).
    bool g_grabWanted = false;

    // --- viewport FILL (the fix for the top-left corner) --------------------------
    // Forcing the swapchain bigger does NOT move the engine's own drawing rectangle:
    // AK keeps setting its stock ~2423x1363 scene viewport, so the picture lands in the
    // top-left of the forced buffer and the rest stays black (the symptom seen in the
    // headset 2026-08-03, and already recorded in the g_forceSquare note above).
    // vrframework guide 14 s4: "Two surfaces need to agree: the OS window/swapchain AND
    // the engine's internal render target." This is the second surface, at its cheapest
    // point â€” we stretch the scene viewport to cover the whole backbuffer, so the
    // rasteriser fills it. The PROJECTION shape is projVR's job (camera.cpp); this only
    // decides which pixels get drawn to. Unlike g_vpForceSquare (a centred CROP, which
    // discards image) this is a pure expansion: nothing is thrown away.
    // (g_vpFill itself is declared up with g_vpSquareWanted â€” settings_save needs it early.)
    int  g_vpFilled = 0;            // viewports filled this frame (diag)
    int  g_rFilled  = 0;            // reported (last frame)
    // "filled 0" told us nothing twice running: the gate could be failing at any of
    // four places and they all look identical from outside. Count each stage so the
    // panel names the one that is actually false, instead of me guessing again.
    int  g_fillGate = 0, g_fillScene = 0;   // outer gate passed / is_scene_vp matched
    int  g_rFillGate = 0, g_rFillScene = 0; // reported (last frame)
    int  g_vpReshaped = 0;          // viewports reshaped this frame (diag: is it matching?)
    int  g_rReshaped = 0;           // reported (last frame)
    int  g_vpCalls = 0, g_vpMatch = 0;   // this frame: total RSSetViewports calls / scene matches
    int  g_rCalls = 0, g_rMatch = 0;     // reported (last frame)
    // Distinct viewport sizes seen this frame â€” reveals the actual render passes.
    struct VpSize { int w, h, count; };
    VpSize g_vpList[10]; int g_vpListN = 0;
    VpSize g_rList[10];  int g_rListN = 0;
    void vp_note(int w, int h)
    {
        for (int i = 0; i < g_vpListN; ++i)
            if (g_vpList[i].w == w && g_vpList[i].h == h) { g_vpList[i].count++; return; }
        if (g_vpListN < 10) { g_vpList[g_vpListN++] = { w, h, 1 }; }
    }

    // Is this the big 16:9 scene viewport (not a tiny UI 16:9 rect, not a shadow pass)?
    static bool is_scene_vp(const D3D11_VIEWPORT& v)
    {
        if (v.Height < 1.0f) return false;
        float a = v.Width / v.Height;
        return a > 1.55f && a < 2.0f && v.Width > 1000.0f;
    }

    void __stdcall hkRSSetViewports(ID3D11DeviceContext* ctx, UINT num, const D3D11_VIEWPORT* vps)
    {
        if (vps && num >= 1)
        {
            g_vpCalls++;
            for (UINT i = 0; i < num; ++i)
            {
                float w = vps[i].Width, h = vps[i].Height, a = w * h;
                vp_note((int)w, (int)h);
                if (is_scene_vp(vps[i]))
                {
                    g_vpMatch++;
                    if (a > g_vpSceneA) { g_vpSceneA = a; g_vpSceneW = w; g_vpSceneH = h; }
                }
                if (a > g_vpBigA) { g_vpBigA = a; g_vpBigW = w; g_vpBigH = h; }
            }
        }

        // FILL override: stretch the scene viewport to the whole backbuffer. Runs before
        // the square-crop test below and wins if both are on, because filling is the one
        // that keeps all the image. Guarded on a sane backbuffer and on the viewport
        // actually being smaller than it, so a correctly-sized frame is left untouched.
        if (g_vpFill && vps && num >= 1 && num <= 16 && g_bbW > 0 && g_bbH > 0)
        {
            ++g_fillGate;
            D3D11_VIEWPORT local[16];
            bool changed = false;
            for (UINT i = 0; i < num; ++i)
            {
                local[i] = vps[i];
                // Matching the scene by SHAPE (is_scene_vp: 16:9-ish and wide) reported
                // scene-match=0 live on 2026-08-03 while the hook was demonstrably
                // running â€” the rect we need is no longer the shape that test assumes.
                // Match on IDENTITY instead: the size the game asked for when it created
                // the swapchain (g_createdW/H, e.g. 3840x2160) is exactly the stock rect
                // it keeps drawing into our forced frame. That is precise, so it cannot
                // catch a shadow or reflection pass the way a loose size test would.
                // Shape stays as a fallback for the case where creation size is unknown.
                // MEASURED 2026-08-03 (build OWNER): in gameplay the game sets exactly
                // ONE viewport, four times a frame â€” 2496x2688 â€” into a 3200x3200 frame.
                // It is PORTRAIT (0.93), which is why every shape-based matcher missed:
                // is_scene_vp wants 1.55-2.0, and the creation size (3840x2160) never
                // appears either. Both earlier guesses were matching things that do not
                // exist. Ownership settled the same run: our own surfaces are 3200x3200
                // (eyes) and 1440x1440 (overlay), so this rect is the GAME's.
                // With only one viewport in the entire frame there is no shadow or
                // reflection pass here to damage, so "big and not already the frame" is
                // both sufficient and safe. Keep the size floor so a UI rect can't match.
                bool isScene = (vps[i].Width >= 1000.0f && vps[i].Height >= 1000.0f);
                if (isScene) ++g_fillScene;
                // No "is it smaller" guard: AK sets a 3840x2160 viewport into a
                // 3200x3200 buffer, i.e. WIDER on one axis and SHORTER on the other,
                // so "smaller" was the wrong question and skipped the case that
                // matters. Any scene viewport that isn't exactly the buffer gets
                // stretched to it.
                if (isScene &&
                    ((UINT)vps[i].Width != g_bbW || (UINT)vps[i].Height != g_bbH))
                {
                    local[i].TopLeftX = 0.0f;
                    local[i].TopLeftY = 0.0f;
                    local[i].Width  = (float)g_bbW;
                    local[i].Height = (float)g_bbH;
                    changed = true;
                    g_vpFilled++;
                }
            }
            if (changed) { oRSSetViewports(ctx, num, local); return; }
        }

        // TEST override: hand the game a centred SQUARE where it asked for the 16:9 scene.
        // If the picture comes out a correct square, the projection follows the viewport
        // and we're basically done; if it's squished, the projection is separate.
        if (g_vpForceSquare && vps && num >= 1 && num <= 16)
        {
            D3D11_VIEWPORT local[16];
            bool changed = false;
            for (UINT i = 0; i < num; ++i)
            {
                local[i] = vps[i];
                if (is_scene_vp(vps[i]))
                {
                    float side = (local[i].Width < local[i].Height) ? local[i].Width : local[i].Height;
                    local[i].TopLeftX += (local[i].Width  - side) * 0.5f;
                    local[i].TopLeftY += (local[i].Height - side) * 0.5f;
                    local[i].Width = side; local[i].Height = side;
                    changed = true;
                    g_vpReshaped++;
                }
            }
            if (changed) { oRSSetViewports(ctx, num, local); return; }
        }
        oRSSetViewports(ctx, num, vps);
    }

    void install_viewport_hook()
    {
        if (g_vpHooked || !g_context) return;
        void** vtbl = *(void***)g_context;      // ID3D11DeviceContext vtable
        void*  target = vtbl[44];               // RSSetViewports
        MH_Initialize();
        if (MH_CreateHook(target, (void*)&hkRSSetViewports, (void**)&oRSSetViewports) == MH_OK &&
            MH_EnableHook(target) == MH_OK)
            g_vpHooked = true;
    }

    void viewport_frame_tick()   // once per Present: snapshot this frame's candidates, reset
    {
        if (g_vpSceneA > 0.0f) { g_rSceneW = g_vpSceneW; g_rSceneH = g_vpSceneH; }
        if (g_vpBigA   > 0.0f) { g_rBigW   = g_vpBigW;   g_rBigH   = g_vpBigH;   }
        g_rReshaped = g_vpReshaped; g_rCalls = g_vpCalls; g_rMatch = g_vpMatch;
        g_rFilled = g_vpFilled; g_vpFilled = 0;
        g_rFillGate = g_fillGate; g_rFillScene = g_fillScene; g_fillGate = g_fillScene = 0;
        for (int i = 0; i < g_vpListN; ++i) g_rList[i] = g_vpList[i];
        g_rListN = g_vpListN;
        g_vpSceneA = g_vpBigA = 0.0f; g_vpReshaped = g_vpCalls = g_vpMatch = 0; g_vpListN = 0;
    }

    // --- render-target binds (real immediate context, slots 33 / 34) ---
    // Hook ID3D11DeviceContext::OMSetRenderTargets: the binds go to the HUD split (hudsplit.cpp), which records them
    // only while the HUD draw runs, and install_rt_hook also installs the HUD split itself.
    using OMSetRTFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    OMSetRTFn oOMSetRT = nullptr;
    bool g_rtHooked = false;

    // PANELTIDY5 2026-10-11: the scene-buffer / depth observer (test tools only) and the scene TAP it fed removed.
    void __stdcall hkOMSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
    {
        akvr_hudsplit_note_bind(ctx, rtvs, n);   // HUDSPLIT: only records while the HUD draw runs
        oOMSetRT(ctx, n, rtvs, dsv);
    }
    // Slot 34: OMSetRenderTargetsAndUnorderedAccessViews â€” the other way a DX11 game binds
    // scene colour+depth (common in UE3 with compute/UAV passes). Same observation.
    using OMSetRTUAVFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                          UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
    OMSetRTUAVFn oOMSetRTUAV = nullptr;
    void __stdcall hkOMSetRTUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                UINT uavStart, UINT numUAV, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
    {
        akvr_hudsplit_note_bind(ctx, rtvs, n);   // HUDSPLIT
        oOMSetRTUAV(ctx, n, rtvs, dsv, uavStart, numUAV, uavs, counts);
    }
    void install_rt_hook()
    {
        if (g_rtHooked || !g_context) return;
        void** vtbl = *(void***)g_context;
        MH_Initialize();
        void* t33 = vtbl[33];               // OMSetRenderTargets
        if (MH_CreateHook(t33, (void*)&hkOMSetRT, (void**)&oOMSetRT) == MH_OK &&
            MH_EnableHook(t33) == MH_OK)
            g_rtHooked = true;
        void* t34 = vtbl[34];               // OMSetRenderTargetsAndUnorderedAccessViews
        if (MH_CreateHook(t34, (void*)&hkOMSetRTUAV, (void**)&oOMSetRTUAV) == MH_OK)
            MH_EnableHook(t34);
        // AKN M5: in engine stereo the HUD is redirected by AKN itself; the geo-11 HUD capture stays out
        if (!akvr_engine_stereo()) akvr_hudsplit_install(g_context);   // HUDSPLIT: HUD draw + draw counters
    }
    bool init_imgui(IDXGISwapChain* swapChain)
    {
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) || !g_device)
            return false;
        g_device->GetImmediateContext(&g_context);
        // Seed the backbuffer size HERE, not from the Present path. The viewport FILL
        // override runs during scene rendering and needs a size that is already valid
        // on the very first frame; enforce_square() publishes it too, but only once a
        // Present has happened, which is after the frame we wanted to fix.
        {
            DXGI_SWAP_CHAIN_DESC sd{};
            if (SUCCEEDED(swapChain->GetDesc(&sd)))
            { g_bbW = sd.BufferDesc.Width; g_bbH = sd.BufferDesc.Height; }
        }
        install_viewport_hook();   // start observing the game's real scene viewport
        install_rt_hook();         // observe the render targets (find the display-ready scene buffer)

        // The OpenXR session (xr.cpp) drives this same device/immediate context
        // from its own thread. The immediate context is NOT thread-safe, so we
        // must tell D3D11 to serialize access â€” otherwise the game's render
        // thread and the XR thread collide and the game hard-freezes (splash
        // hang after a handful of Presents). Enable this BEFORE XR gets the device.
        {
            ID3D11Multithread* mt = nullptr;
            if (SUCCEEDED(g_context->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt)) && mt)
            {
                mt->SetMultithreadProtected(TRUE);
                mt->Release();
            }
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        swapChain->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
        install_focus_keeper(g_hwnd);   // keep the game from pausing input while VR backgrounds it

        if (!create_rtv(swapChain))
            return false;

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;              // never write imgui.ini next to the game exe
        // Let the panel be driven by controller navigation (we feed the pad in
        // menu mode). We turned the backend's own gamepad reader off (CMake), which
        // ALSO stops it telling ImGui "a gamepad exists" â€” and ImGui ignores all
        // gamepad nav unless BackendFlags has HasGamepad set. So we set it ourselves
        // (we're the ones supplying the gamepad input now).
        io.ConfigFlags  |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        // Keyboard too, so any slider's value can be typed rather than dragged
        // (JJ, 2026-08-05). The messages are forwarded from focus_wndproc, which only
        // does so while the panel is open.
        io.ConfigFlags  |= ImGuiConfigFlags_NavEnableKeyboard;
        // Bigger font so the overlay is legible inside the headset (default 13px is
        // tiny across the VR field of view). 26 = clean 2x of the pixel font.
        ImFontConfig fcfg; fcfg.SizePixels = 26.0f;
        io.Fonts->AddFontDefault(&fcfg);
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_device, g_context);
        akvr_mouse_block_install([] { return panel_takes_mouse() || g_panelMouseButtons != 0; }); // PANELMOUSE
        akvr_xr_set_device(g_device);   // hand the game's device to OpenXR for its session

        g_injectTick = GetTickCount64();
        return true;
    }

    // Hotkey handling â€” runs EVERY Present, even when the panel is hidden (so F8 can
    // unhide it, F11/F12 still work, etc.). Split out from the panel rendering so the
    // two are independent now that the UI lives in its own quad layer.
    // F2 and the panel's "save a recording" button (JJ: no more leaving VR to reach the
    // virtual keyboard): the logs, the traces and the pictures, all timestamped.
    // CAPTUREDIR 2026-10-01 â€” JJ: "the game folder is filling up with many many files, this is bad file management
    // practice". Everything a capture writes now goes into akvr_captures\<date_time>\ (one folder per F2), and the
    // working copies of the traces into akvr_captures\. The game folder keeps only akvr_settings.ini and
    // akvr_startup_log.txt.
    std::wstring g_capDir;   // relative to the game folder, with a trailing backslash; the latest F2's folder
    void capture_all()
    {
        g_grabWanted = true;
        std::wstring base = settings_path();
        size_t s = base.find_last_of(L"\\/");
        if (s != std::wstring::npos) base = base.substr(0, s + 1);
        const std::wstring root = base + L"akvr_captures\\";
        CreateDirectoryW(root.c_str(), nullptr);
        g_traceRows = akvr_camera_trace_dump((root + L"akvr_camera_trace.csv").c_str());
        g_modeRows  = mode_trace_dump((root + L"akvr_mode_trace.csv").c_str());
        // Preserve each test, including the settings/mode at the time of F1.
        write_startup_diag(true);
        SYSTEMTIME stamp{}; GetLocalTime(&stamp);
        wchar_t tag[80]{};
        swprintf_s(tag, L"%04u%02u%02u_%02u%02u%02u_%03u",
            stamp.wYear, stamp.wMonth, stamp.wDay, stamp.wHour,
            stamp.wMinute, stamp.wSecond, stamp.wMilliseconds);
        g_capDir = std::wstring(L"akvr_captures\\") + tag + L"\\";
        CreateDirectoryW((base + g_capDir).c_str(), nullptr);
        // CAPPAIR 2026-10-08: in engine stereo the window shows one eye only - also save BOTH eyes (akn\pair_c<time>.bmp)
        if (akvr_engine_stereo())
        {
            char pc[48]; snprintf(pc, sizeof(pc), "capmk c%02u%02u%02u", stamp.wHour, stamp.wMinute, stamp.wSecond);   // MK3: + HUD + marker data
            {   // DIAGKIT: 2 s later (AKN has listed the shaders drawn now) their code goes into this capture's "shaders" folder
                struct Job { std::wstring base, dir; }; Job* j = new Job{ base, base + g_capDir + L"shaders\\" };
                CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                    Job* j = (Job*)p; Sleep(2000); CreateDirectoryW(j->dir.c_str(), nullptr); int n = 0;
                    for (const wchar_t* inv : { L"akn\\vs_inventory.txt", L"akn\\ps_inventory.txt" })
                    {
                        FILE* f = _wfopen((j->base + inv).c_str(), L"r"); if (!f) continue;
                        char line[64]; const bool vs = wcsstr(inv, L"vs_") != nullptr;
                        while (fgets(line, sizeof(line), f)) { unsigned long long h = _strtoui64(line, nullptr, 16); if (!h) continue;
                            wchar_t fn[MAX_PATH]; swprintf_s(fn, L"%s%016llx_%s.txt", j->dir.c_str(), h, vs ? L"vs" : L"ps"); n += akvr_shader_save(h, fn); }
                        fclose(f);
                    }
                    delete j; (void)n; return 0; }, j, 0, nullptr));
            }
            akn_command(pc);
            // (BLACKHUNT on menu captures removed 2026-10-14 - release tidy; command "blackhunt" stays)
        }
        const std::wstring capture = base + g_capDir;
        akvr_present_probe_dump((capture + L"present.csv").c_str());
        akvr_present_probe_dump_frames((capture + L"frames.csv").c_str());
        akvr_hud_dump_viewports((capture + L"viewports.csv").c_str());
        akvr_hudprobe_dump((capture + L"hudprobe.csv").c_str());
        akvr_hudsplit_dump((capture + L"hudsplit.csv").c_str());   // HUDSPLIT
        akvr_hudsplit_layer_shot((capture + L"hudlayer.bmp").c_str());   // LAYERSHOT
        akvr_hudsplit_marks_dump((capture + L"hudmarks.csv").c_str());   // MARKREC
        akvr_hud_layers_dump((capture + L"hudlayers.txt").c_str());   // HUDLAYERS phase 1
        CopyFileW((root + L"akvr_camera_trace.csv").c_str(), (capture + L"camera.csv").c_str(), TRUE);
        CopyFileW((root + L"akvr_mode_trace.csv").c_str(), (capture + L"mode.csv").c_str(), TRUE);
        CopyFileW((base + L"akvr_startup_log.txt").c_str(), (capture + L"status.txt").c_str(), TRUE);
    }

    // PANELTIDY5 2026-10-11: SHADERFINDER (panel buttons pressing geo-11's hunting keys) removed.

    void process_hotkeys()
    {
        // F8 edge-detect toggle (no input hook needed). Hiding the overlay ALSO forces
        // menu mode off, so the game can never be left with a blanked pad behind a
        // hidden panel â€” the invariant is "pad is blanked only while the panel shows."
        static bool f8Held = false;
        bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Down && !f8Held) { g_showOverlay = !g_showOverlay; if (!g_showOverlay) akvr_gamepad_set_menu(false); }
        f8Held = f8Down;

        // Gamepad Menu+View (Start+Back together) = open/close the interactive
        // settings MENU. While the menu is open the controller drives the panel and
        // the game ignores the pad (so Batman doesn't move while you tune). Close it
        // to hand control back. Reads the REAL pad even while the game is blanked.
        akvr_gamepad_install();   // lazy + idempotent: hooks the controller reads once loaded
        // PANELTEST 2026-10-07 (JJ, engine stereo: "I can't open the VR overlay with the double stick click"): the AKN
        // test script opens/closes the panel exactly like the chord (akvr_panel_set below) and reads its state
        const LONG pc = InterlockedExchange(&g_panelCmd, 0);
        if (pc) { const bool m = pc == 1; akvr_gamepad_set_menu(m); g_showOverlay = m; }
        if (akvr_gamepad_chord_double_tap()) { akvr_fp_set(!akvr_fp()); settings_save(); }   // FIRSTPERSON: both sticks clicked twice
        if (akvr_gamepad_menu_chord_pressed())
        {
            InterlockedIncrement(&g_chordFires);
            bool m = !akvr_gamepad_menu();
            akvr_gamepad_set_menu(m);
            g_showOverlay = m;    // menu mode and the visible panel move together
        }

        // F4 = PANIC RELEASE. Hand the controller straight back to the game. If menu
        // mode ever sticks on, every button press goes to our panel and the game sees a
        // dead pad â€” indistinguishable from the mod having broken the controller. This
        // is the one-key way to rule that out (and to recover) without a restart.
        static bool f4Held = false;
        bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (f4Down && !f4Held) akvr_gamepad_set_menu(false);
        f4Held = f4Down;

        // F2 = the logs AND the pictures (was F1 for the logs). geo-11 0.7.11 binds
        // Ctrl+F1, and this key never checked Ctrl, so both fired (JJ, 2026-09-26).
        // One key for everything: the camera trace holds the last ~48 s, so the spin is
        // performed FIRST and captured afterwards.
        static bool f2Held = false;
        bool f2Down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (f2Down && !f2Held) capture_all();   // (Shift+F2 sweep removed, CLEANTESTS)
        f2Held = f2Down;

        // F3 = toggle keep-focus (the "don't pause when backgrounded by VR" override)
        static bool f3Held = false;
        bool f3Down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3Down && !f3Held) g_keepFocus = !g_keepFocus;
        f3Held = f3Down;

        // F9 = (re)try installing the camera hook
        static bool f9Held = false;
        bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9Down && !f9Held) akvr_camera_install();
        f9Held = f9Down;

        // F7 = toggle freecam (take control of the camera). Ctrl+F7 is geo-11's "save
        // settings" (0.7.11), so it no longer steps the pose delay: use the panel.
        static bool f7Held = false;
        bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7Down && !f7Held && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))
            akvr_freecam_toggle();
        f7Held = f7Down;

        // Pause/Break = float as a screen (was F6: Ctrl+F6 is geo-11's convergence key and
        // plain F6 the fix pack's depth-of-field toggle, so F6 fired three things; JJ).
        static bool pauseHeld = false;
        bool pauseDown = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
        if (pauseDown && !pauseHeld) akvr_xr_screen_mode_toggle();
        pauseHeld = pauseDown;

        // F5 REMOVED 2026-09-26: it toggled the pre-geo-11 wide render, which squashed the
        // picture vertically when hit by accident (JJ); F5 is also the fix pack's ivy key
        // and Ctrl+F5 geo-11's convergence. The panel still has the setting.

        // F10 = retry OpenXR connect now
        static bool f10Held = false;
        bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10Down && !f10Held) akvr_xr_try_init();
        f10Held = f10Down;

        // F11 = toggle additive VR head-tracking; F12 = recenter
        static bool f11Held = false;
        bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11Down && !f11Held) akvr_head_toggle();
        f11Held = f11Down;

        static bool f12Held = false;
        bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12Down && !f12Held) akvr_head_recenter();
        f12Held = f12Down;

        // PageUp / PageDown = live-tune the positional-lean gain (world-scale).
        static bool puHeld = false;
        bool puDown = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
        if (puDown && !puHeld) akvr_head_pos_scale_mul(1.15f);
        puHeld = puDown;

        static bool pdHeld = false;
        bool pdDown = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
        if (pdDown && !pdHeld) akvr_head_pos_scale_mul(1.0f / 1.15f);
        pdHeld = pdDown;

        // Home / End = live-tune the VR FOV offset (Home narrower, End wider).
        static bool homeHeld = false;
        bool homeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (homeDown && !homeHeld) akvr_head_fov_add(-2.0f);
        homeHeld = homeDown;

        static bool endHeld = false;
        bool endDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (endDown && !endHeld) akvr_head_fov_add(+2.0f);
        endHeld = endDown;

        // [ / ] = stereo depth (eye separation): [ weaker, ] stronger; 0 = flat/mono
        static bool lbrkHeld = false;
        bool lbrkDown = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;
        if (lbrkDown && !lbrkHeld) akvr_head_stereo_add(-0.5f);
        lbrkHeld = lbrkDown;
        static bool rbrkHeld = false;
        bool rbrkDown = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;
        if (rbrkDown && !rbrkHeld) akvr_head_stereo_add(+0.5f);
        rbrkHeld = rbrkDown;

        // \ = switch stereo mode: NATIVE (geo-11 SBS dual-render) <-> AER fallback
        static bool bslHeld = false;
        bool bslDown = (GetAsyncKeyState(VK_OEM_5) & 0x8000) != 0;
        if (bslDown && !bslHeld) akvr_xr_native_toggle();
        bslHeld = bslDown;

        // Insert / Delete (menu size x1.1) REMOVED 2026-09-30 - JJ: "main menu size keeps reverting to 100%". The keys
        // are read system-wide, so Insert/Delete typed in any other window resized the menu (saved values drifted by
        // exactly x1.1). The panel's "main menu size" slider is the control.
    }

    // SLIDERSTEP 2026-09-26 â€” JJ: "it would be good for the sliders to choose every
    // number, I can't set it to exactly one. It skips over that." Every panel slider
    // now snaps to the precision it displays (%.2f -> 0.01, %.0f -> 1), so a drag lands
    // on exact values such as 1.00 instead of 0.996 / 1.004.
    // SAVEFIX 2026-09-26 â€” JJ: a new render height "reverted" after a restart. The panel
    // saved sliders only on IsItemDeactivatedAfterEdit, which controller edits often never
    // fire (the same bug once lost a HUD size). Any slider change now marks the panel
    // dirty; draw_panel saves once nothing is being dragged.
    bool g_sliderDirty = false;
    ULONGLONG g_sliderOwnFrame = 0;   // ONESTEP: panel frame on which a slider was last controller-active
    // SLIDEROWN 2026-09-27 â€” JJ: "all of the sliders were fighting me" on the D-pad. The
    // same SliderStep in a stand-alone ImGui 1.92.9 harness steps cleanly, so something in
    // the live panel adds a second writer; rather than chase it, the controller path no
    // longer uses ImGui's tweak at all. While a slider is active and not being dragged with
    // the mouse or typed into, ImGui's change for the frame is discarded and the mod moves
    // it exactly one displayed step per press, from the REAL pad (D-pad or left stick) and
    // the keyboard arrows, with its own repeat (350 ms, then every 70 ms).
    int panel_tweak_dir()
    {
        int dir = 0;
        const GamepadDiag gd = akvr_gamepad_diag();
        if (gd.connected)
        {
            if ((gd.buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || gd.lx > 16000) dir += 1;
            if ((gd.buttons & XINPUT_GAMEPAD_DPAD_LEFT)  || gd.lx < -16000) dir -= 1;
        }
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) dir += 1;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  dir -= 1;
        return dir > 0 ? 1 : (dir < 0 ? -1 : 0);
    }
    bool SliderStep(const char* label, float* v, float lo, float hi, const char* fmt,
                    ImGuiSliderFlags flags = 0)
    {
        int dec = 2;
        const char* pct = strchr(fmt, '.');
        if (pct && pct[1] >= '0' && pct[1] <= '9') dec = pct[1] - '0';
        else if (strstr(fmt, "%d") || strstr(fmt, "%.0f")) dec = 0;
        const float step = powf(10.0f, (float)-dec);
        const ImGuiID id = ImGui::GetID(label);
        const float before = *v;
        const bool mouse  = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool typing = ImGui::TempInputIsActive(id);
        static ImGuiID   s_id = 0;
        static int       s_dir = 0;
        static ULONGLONG s_next = 0, s_held = 0;
        // ONESTEP 2026-09-27 â€” JJ: "I can see the values go up then back down by a tiny
        // amount each time I click." ImGui's own gamepad tweak (1% of the slider's range per
        // press, see SliderBehaviorT) was applied and DRAWN inside SliderFloat, then our step
        // replaced it: two writers per press, visible for one frame. That is also the
        // original "fighting" since SLIDERSTEP rounding. Now (a) the D-pad / left stick
        // left-right are not fed to ImGui while one of these sliders is active
        // (g_sliderOwnFrame -> akvr_gamepad_feed_imgui), so ImGui never tweaks, and (b) our
        // step is applied BEFORE the widget draws, so the new value shows on the same frame.
        const bool owned = ImGui::GetActiveID() == id && !mouse && !typing;
        if (owned)
        {
            g_sliderOwnFrame = g_frameCount;
            const int dir = panel_tweak_dir();
            const ULONGLONG now = GetTickCount64();
            bool stepNow = false;
            if (dir == 0) s_dir = 0;
            else if (s_id != id || s_dir != dir) { s_id = id; s_dir = dir; s_next = now + 300; s_held = now; stepNow = true; }
            else if (now >= s_next) { s_next = now + 70; stepNow = true; }
            // SLIDERFAST 2026-09-27 â€” JJ: "very slow to move them". One displayed unit per
            // repeat is ~14 units/s (world scale 0.14/s). Holding now speeds up: x5 after
            // 1 s, x25 after 2.5 s; a single tap is still exactly one unit.
            const ULONGLONG heldMs = now - s_held;
            const float mult = heldMs > 2500 ? 25.0f : (heldMs > 1000 ? 5.0f : 1.0f);
            if (stepNow) *v = roundf(before / step) * step + (float)dir * step * mult;
            if (*v < lo) *v = lo;
            if (*v > hi) *v = hi;
        }
        const float stepped = *v;
        const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt, flags);
        if (owned || (ImGui::IsItemActive() && !mouse && !typing))
            *v = stepped;                                       // keyboard-arrow tweak by ImGui discarded
        else if (changed)
            *v = roundf(*v / step) * step;                      // mouse drag / typed value
        if (*v < lo) *v = lo;
        if (*v > hi) *v = hi;
        const bool moved = *v != before;
        if (moved) g_sliderDirty = true;
        return moved;
    }

    // The UI panel content. Rendered into the overlay's OWN swapchain (a floating
    // quad layer in the headset), so io.DisplaySize IS the panel resolution â€” one
    // clean window filling the panel, no FOV counter-scale, no eye-seam, no per-eye
    // split. (When there's no headset it falls back to the monitor at window size.)
    // HUDLAYERS: one HUD part per row - hide box, then its size/position and its children.
    // PARTOPEN 2026-09-30 â€” JJ: "keep that HUD element open so I don't have to keep drilling down". Parts that are
    // hidden, moved, resized or hang in the room open their whole branch once per session (the player can close it).
    // hint[i] = i is such a part or has one below it; rebuilt when the list or a part's setting changes.
    std::vector<char> s_partHint;
    int  s_partHintCount = -1;
    bool s_partHintDirty = true;
    bool part_hint(int i)
    {
        const int n = akvr_hud_layer_count();
        if (n != s_partHintCount || s_partHintDirty)
        {
            s_partHint.assign(n > 0 ? n : 0, 0);
            for (int k = 0; k < n; ++k)
            {
                int d, par, kids; const char* lab; float ls, lx, ly; bool lh, l3;
                if (!akvr_hud_layer_get(k, d, par, kids, lab, ls, lx, ly, lh, l3)) continue;
                const bool custom = lh || akvr_hud_layer_room(k) || akvr_hud_layer_zoomhide(k) || akvr_hud_layer_world(k) || fabsf(ls - 1.0f) > 0.001f || fabsf(lx) > 0.0005f || fabsf(ly) > 0.0005f;
                for (int j = custom ? k : -1, guard = 0; j >= 0 && j < n && guard < 64; ++guard)
                {
                    if (s_partHint[j]) break;
                    s_partHint[j] = 1;
                    int d2, p2, k2; const char* l2; float a2, b2, c2; bool h2, e2;
                    if (!akvr_hud_layer_get(j, d2, p2, k2, l2, a2, b2, c2, h2, e2)) break;
                    j = p2;
                }
            }
            s_partHintCount = n; s_partHintDirty = false;
        }
        return i >= 0 && i < (int)s_partHint.size() && s_partHint[i];
    }
    bool part_hint_any() { part_hint(-1); for (char c : s_partHint) if (c) return true; return false; }

    void draw_hud_layer_children(int parent)
    {
        for (int i = 0; i < akvr_hud_layer_count(); ++i)
        {
            int d, par, kids; const char* lab; float ls, lx, ly; bool lh, l3;
            if (!akvr_hud_layer_get(i, d, par, kids, lab, ls, lx, ly, lh, l3) || par != parent) continue;
            ImGui::PushID(i);
            bool hide = lh;
            if (ImGui::Checkbox("hide", &hide)) { akvr_hud_layer_set(i, ls, lx, ly, hide); settings_save(); s_partHintDirty = true; }
            ImGui::SameLine();
            // PARTTAG (JJ 2026-09-30): the whole part hangs in the room with the rest of the HUD, never at scene depth.
            bool room = akvr_hud_layer_room(i);
            if (ImGui::Checkbox("hang in the room", &room)) { akvr_hud_layer_room_set(i, room); settings_save(); s_partHintDirty = true; }
            ImGui::SameLine();
            bool wd = akvr_hud_layer_world(i);   // ROOMALL (JJ: everything in the room except what points at the world)
            if (ImGui::Checkbox("stays on its target", &wd)) { akvr_hud_layer_world_set(i, wd); settings_save(); s_partHintDirty = true; }
            ImGui::SameLine();
            bool zh = akvr_hud_layer_zoomhide(i);   // ZOOMHIDE (JJ: the game's zoom overlay shares its part with the tips)
            if (ImGui::Checkbox("hide while zoomed", &zh)) { akvr_hud_layer_zoomhide_set(i, zh); settings_save(); s_partHintDirty = true; }
            ImGui::SameLine();
            if (part_hint(i)) ImGui::SetNextItemOpen(true, ImGuiCond_Once);   // PARTOPEN
            const bool open = ImGui::TreeNode("part", "%s%s%s%s", lab, akvr_hud_layer_autoroom(i) ? "  [in the room]" : "", l3 ? "  (3D panel)" : "",
                                              kids ? "" : "  (single)");
            if (open)
            {
                {
                    float s100 = ls * 100.0f, x100 = lx * 100.0f, y100 = ly * 100.0f;
                    bool ch = false;
                    ch |= SliderStep("size %", &s100, 20.0f, 300.0f, "%.0f");
                    ch |= SliderStep("left / right %", &x100, -60.0f, 60.0f, "%.1f");
                    ch |= SliderStep("down / up %", &y100, -60.0f, 60.0f, "%.1f");
                    if (ch) akvr_hud_layer_set(i, s100 / 100.0f, x100 / 100.0f, y100 / 100.0f, lh);
                    if (ImGui::Button("reset")) { akvr_hud_layer_set(i, 1.0f, 0.0f, 0.0f, false); settings_save(); }
                }
                if (d < 12) draw_hud_layer_children(i);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void draw_panel()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.FontGlobalScale = 0.78f;   // FONTFIT 2026-09-28 (JJ): the text did not fit the panel width (0.85 -> 0.78)
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.86f);
        ImGui::Begin("AKVR", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        // SLIDERFIT 2026-09-28 (JJ): sliders were too long and pushed their labels past the window edge.
        // Every slider / box is 42% of the width, leaving the rest for its label.
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
        // TEXTWRAP 2026-09-28 (JJ): no text may run past the window edge; every Text* call wraps there.
        ImGui::PushTextWrapPos(0.0f);

        // When the menu is first opened, give this window nav focus so the very
        // first controller press moves the highlight instead of doing nothing.
        {
            static bool s_wasMenu = false;
            bool menu = akvr_gamepad_menu();
            if (menu && !s_wasMenu) ImGui::SetWindowFocus();
            s_wasMenu = menu;
        }

        // REBUILT 2026-09-26 on JJ's request: "clean up this overlay, get rid of things
        // we don't need, hide things we might need later, simplify it and make it more
        // user-friendly and easier to understand." Rule from here on: the top level holds
        // only what a player adjusts, in plain words. Tests and experiments live under
        // Advanced, numbers under Diagnostics, keys under Keyboard shortcuts. No setting
        // was removed â€” every key a settings file can hold is still reachable.
        const ImVec4 kGreen(0.3f, 1.0f, 0.55f, 1.0f), kAmber(1.0f, 0.7f, 0.25f, 1.0f),
                     kHead(0.6f, 0.85f, 1.0f, 1.0f);
        const ImGuiSliderFlags kNum = ImGuiSliderFlags_AlwaysClamp;
        HeadTrackState ht = akvr_head_state();

        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Arkham Knight VR");
        ImGui::SameLine();
        ImGui::TextDisabled("   version 0.3.0  (" __DATE__ ")");   // PANELTIDY4: the release number

        ImGui::Separator();
        // ---- one status line ----------------------------------------------------
        // TIDY4 2026-09-27 (JJ: "cleaned up and reformatted to be a bit more consistent with the
        // Sekiro overlay. Simplify it and remove values that we don't need to adjust anymore"):
        // SKVR's order - status, recenter/capture, VIEW, HUD, MENUS AND SCREENS, FRAME RATE; long
        // status texts moved to Diagnostics; settled switches under the (hidden) Advanced header.
        {
            const bool vrOn = akvr_xr_session_running() && akvr_xr_displaying();
            ImGui::TextColored(vrOn ? kGreen : kAmber, "%s",
                vrOn ? "VR on" : (akvr_xr_session_running() ? "VR connected, not showing yet" : "VR not connected"));
            ImGui::SameLine();
            const char* stereo = akvr_engine_stereo() ? "native stereo"
                : akvr_xr_native()
                ? (akvr_xr_kat_feed() ? (akvr_xr_eye_applied() ? "true 3D, each eye's own view" : "true 3D (geo-11)") : "3D OFF (geo-11 feed off)")
                : "3D by alternating eyes";
            ImGui::TextDisabled("|  %.0f fps  |  %s  |  head tracking %s",
                                io.Framerate, stereo, ht.on ? "on" : "OFF (F11)");
        }
        if (ht.on && fabsf(ht.leanX) + fabsf(ht.leanY) + fabsf(ht.leanZ) > 0.5f)
            ImGui::TextColored(kAmber, "Your position looks off-centre - press Recenter (F12).");
        if (!g_keepFocus)
            ImGui::TextColored(kAmber, "keep-focus is OFF (F3) - the game may ignore the controller");
        if (ImGui::Button("Recenter  (F12)")) akvr_head_recenter();
        ImGui::SameLine();
        if (ImGui::Button("Save capture  (F2)")) capture_all();
        ImGui::SameLine();
        // HTBUTTON 2026-10-02 â€” JJ: "Is there an overlay button for switching head tracking off? I'm not in front of the
        // keyboard." (for the Batmobile enter/exit A/B)
        if (ImGui::Button(akvr_head_state().on ? "Head tracking: ON  (F11)" : "Head tracking: OFF  (F11)")) akvr_head_toggle();
        // PANELTIDY5 2026-10-11 (JJ: "remove the depth of field on off options and the skip unneeded steps and the game's own
        // anti-aliasing ... remove the mod having any option to add extra anti-aliasing SMAA or soft - not required now we have
        // DLSS"): gone with their code; depth of field stays on, the game's own look.
        if (g_traceRows || g_modeRows)
        { ImGui::SameLine(); ImGui::TextColored(kGreen, "saved"); }
        if (akvr_gamepad_menu())
            ImGui::TextDisabled("controller: left stick = move, A = select / change, B = back, both stick clicks = close");
        else
            ImGui::TextDisabled("both stick clicks (L3 + R3) = use this menu with the controller");

        // ---- VIEW ---------------------------------------------------------------
        // DRAWERS 2026-09-28 (JJ): "put all VR overlay elements in drawers and close the drawers by
        // default". Each section is a closed collapsing header; the status line and the two buttons
        // above stay visible.
        ImGui::Separator();
        // PANELTIDY3: the suit gloss test section is gone - JJ chose "direction shaders" + "game camera without the head",
        // now AKN's defaults (gloss 3, glosssrc 1); the commands stay for tests (akvr_native/HANDOFF.md SUITGLOSS3).
        // DLSSPANEL2 2026-10-08 (JJ: "there is no DLSS [row] in the VR overlay" - it sat inside the folded VIEW section):
        // its own section, open by default, engine stereo only.
        if (akvr_engine_stereo())
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader("DLSS"))
            {
                static const char* kDl[] = { "Off", "DLAA", "Ultra quality", "Quality", "Balanced", "Performance" };
                static const float kSc[] = { 1.0f, 1.0f, 0.77f, 0.667f, 0.58f, 0.5f };
                const int dm = akvr_dlss_mode();
                for (int i = 0; i < 6; ++i)
                {
                    if (i) ImGui::SameLine();
                    char lbl[48]; snprintf(lbl, sizeof(lbl), "%s##dlss%d", kDl[i], i);
                    if (ImGui::RadioButton(lbl, dm == i)) { akvr_dlss_mode_set(i); settings_save(); }
                }
                // PANELTIDY3: the eye-camera choice (turned out / same way / straight) is gone - turned out everywhere
                // (SteamVR via SVRSTRAIGHT, suit gloss via SUITGLOSS3); `lensmode` stays a setting for tests.
                const int oh = akvr_engine_res(), ow = (oh * akvr_engine_shape() / 1000) & ~7;
                const float sc = kSc[dm < 0 || dm > 5 ? 0 : dm];
                if (dm > 0) ImGui::TextDisabled("   the game draws about %dx%d per eye, DLSS (transformer) builds %dx%d - change applies on the next start",
                                                (int)(ow * sc) & ~7, (int)(oh * sc) & ~7, ow, oh);
                else ImGui::TextDisabled("   off: the game draws %dx%d per eye with its own anti-aliasing - change applies on the next start", ow, oh);
                if (dm > 0)
                {   // DLSSMODEL 2026-10-08 (JJ: "adjust the DLSS preset on the fly?"): the transformer model, live (DLSS
                    // rebuilds its history for a moment). 310.9: K = recommended, J = a little less ghosting, more flicker.
                    static int s_model = 0;
                    static const struct { const char* name; int preset; } kModels[] = { {"default (K)", 0}, {"K", 11}, {"J", 10}, {"M", 13}, {"L", 12} };
                    ImGui::TextUnformatted("DLSS model (live):");
                    for (int i = 0; i < 5; ++i)
                    {
                        ImGui::SameLine();
                        char lbl[48]; snprintf(lbl, sizeof(lbl), "%s##dlm%d", kModels[i].name, i);
                        if (ImGui::RadioButton(lbl, s_model == i)) { s_model = i; char c[32]; snprintf(c, sizeof(c), "dlsspreset %d", kModels[i].preset); akn_command(c); }
                    }
                    // MIPBIAS 2026-10-08: texture detail picked for the headset size, not the small render (Luma's method), live
                    static int s_mip = 1;
                    static const char* kMip[] = { "off", "full (recommended)", "mild" };
                    ImGui::TextUnformatted("texture sharpness (live):");
                    for (int i = 0; i < 3; ++i)
                    {
                        ImGui::SameLine();
                        char lbl[48]; snprintf(lbl, sizeof(lbl), "%s##mip%d", kMip[i], i);
                        if (ImGui::RadioButton(lbl, s_mip == i)) { s_mip = i; char c[24]; snprintf(c, sizeof(c), "mipbias %d", i); akn_command(c); }
                    }
                }
            }
        }
        // AFW 2026-10-10 (JJ: "in the VR overlay, choose between native and AFW" - alternate frame warping; akvr_native/
        // NOTES.md "AFW PLAN"): native = the game draws both eyes every frame; AFW = one eye per frame, the other built from
        // that same frame by AKN's warp. Live; saved as afw= / afwmode=.
        if (akvr_engine_stereo())
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader("STEREO RENDERING"))
            {
                const int st = akn_query("akn_afw_state");
                if (st >= 0) { g_afwSet = st & 1; g_afwModeSet = (st >> 4) & 15; }   // AKN's live state (command.txt tests change it too)
                ImGui::TextUnformatted("3D rendering");   // AFWNAMES 2026-10-11 (JJ): named as in the Arkham City mod
                if (ImGui::RadioButton("Native (both eyes drawn every frame)##afw0", g_afwSet == 0)) { g_afwSet = 0; akn_command("afw 0"); settings_save(); }
                if (ImGui::RadioButton("AFW (one eye drawn per frame, the other warped)##afw1", g_afwSet == 1)) { g_afwSet = 1; akn_command("afw 1"); settings_save(); }
                if (g_afwSet)
                {
                    static const char* kM[] = { "this frame", "previous frame", "both (best)" };
                    ImGui::TextUnformatted("warp from:");
                    for (int i = 1; i <= 3; ++i)
                    {
                        ImGui::SameLine();
                        char lbl[64]; snprintf(lbl, sizeof(lbl), "%s##afwm%d", kM[i - 1], i);
                        if (ImGui::RadioButton(lbl, g_afwModeSet == i)) { g_afwModeSet = i; char c[24]; snprintf(c, sizeof(c), "afwmode %d", i); akn_command(c); settings_save(); }
                    }
                    if (akvr_dlss_mode() <= 0) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "   AFW needs DLSS on (any setting, DLAA too): without it both eyes show the same flat picture");
                    ImGui::TextDisabled("   smooth only when the game keeps up with the headset's rate (one eye per frame)");
                    using StFn = const char* (*)(); static StFn s_st = nullptr;
                    if (!s_st) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); if (m) s_st = (StFn)GetProcAddress(m, "akn_afw_status"); }
                    if (s_st && g_showAdvanced) { ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("   %s", s_st()); ImGui::PopTextWrapPos(); }
                }
                // PANELTIDY4 2026-10-11: the marker method switch and the marker recorder are gone - markers are placed per
                // vertex in 3D (MKGS, JJ "perfect"); AKN commands mkeye / mkrec stay for tests.
            }
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (ImGui::CollapsingHeader("VIEW"))
        {
            {   // FIRSTPERSON 2026-10-11, in the VIEW drawer since PANELTIDY5 (JJ): Batman City's panel rows
                static bool s_fpDirty = false;
                bool fp = akvr_fp();
                // FPFIX 2026-10-11 (JJ: fix first person before the release): back on the panel while it is being fixed
                if (ImGui::Checkbox("FIRST PERSON  (or click both sticks twice quickly)", &fp)) { akvr_fp_set(fp); settings_save(); }
                if (fp)
                {
                    float f = akvr_fp_fwd(), u = akvr_fp_up();
                    if (SliderStep("   camera in front of his face (cm)", &f, -20.0f, 80.0f, "%.0f")) { akvr_fp_fwd_set(f); s_fpDirty = true; }
                    if (SliderStep("   camera up / down (cm)", &u, -40.0f, 40.0f, "%.0f")) { akvr_fp_up_set(u); s_fpDirty = true; }
                    bool hf = akvr_fp_head();
                    if (ImGui::Checkbox("   camera follows his head (gliding, animations)", &hf)) { akvr_fp_head_set(hf); settings_save(); }
                    if (hf)
                    {
                        float sm = akvr_fp_smooth();
                        if (SliderStep("   head smoothing (s) - higher = steadier, lags more", &sm, 0.0f, 0.5f, "%.2f")) { akvr_fp_smooth_set(sm); s_fpDirty = true; }
                    }
                    ImGui::TextDisabled("   %s", akvr_fp_status());
                }
                if (s_fpDirty && !ImGui::IsAnyItemActive()) { settings_save(); s_fpDirty = false; }
            }

            if (akvr_engine_stereo())
            {   // PANELTIDY4 2026-10-11: native stereo's world scale = game units per metre (the eye distance sent to AKN and
                // the head lean both use it; xr.cpp akn_send_eyes); the geo-11 slider below did nothing here
                static ULONGLONG s_wsT = 0;
                float ws = akvr_head_pos_scale() / kLeanAt1;
                if (SliderStep("world scale  (1.00 = life size)", &ws, 0.5f, 2.0f, "%.2f")) { akvr_head_pos_scale_set(ws * kLeanAt1); s_wsT = GetTickCount64(); }
                if (ImGui::IsItemDeactivatedAfterEdit() || (s_wsT && GetTickCount64() - s_wsT > 1000 && !ImGui::IsAnyItemActive())) { settings_save(); s_wsT = 0; }
                ImGui::SameLine();
                if (ImGui::Button("1.00##wsn")) { akvr_head_pos_scale_set(kLeanAt1); settings_save(); }
            }
            else if (akvr_xr_native())
            {
                // World scale: geo-11's separation x convergence, live (GEOLIVE / VRSEP).
                float gs = akvr_geo11conv_scale();
                // SCALESAVE 2026-10-02: JJ's 1.1 never reached the files (geoscale 1.05, d3dxdm convergence 2381): a
                // controller step does not always end an edit. Any change is now saved 1 s after the last one too.
                static ULONGLONG s_wsChanged = 0;
                if (SliderStep(akvr_geo11conv_live() ? "world scale" : "world scale  (next launch)", &gs, 0.25f, 2.0f, "%.2f"))
                { akvr_geo11conv_scale_set(gs); s_wsChanged = GetTickCount64(); }
                if (ImGui::IsItemDeactivatedAfterEdit() ||
                    (s_wsChanged && GetTickCount64() - s_wsChanged > 1000 && !ImGui::IsAnyItemActive()))
                { akvr_geo11conv_commit(); settings_save(); s_wsChanged = 0; }
                ImGui::SameLine();
                if (ImGui::Button("1.00##ws")) { akvr_geo11conv_scale_set(1.0f); akvr_geo11conv_commit(); settings_save(); }
            }
            else
            {
                float world = akvr_head_stereo() / kStereoAt1;
                if (SliderStep("world scale  (1.00 = life size)", &world, 0.0f, 3.0f, "%.2f"))
                    akvr_head_stereo_set(world * kStereoAt1);
                if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
            }

            // PANELTIDY2 2026-10-01 â€” JJ: "look through the rest of the VR overlay ... remove any other things that aren't
            // required as options". Settled or test-only controls are shown only with advancedpanel=1 (g_showAdvanced);
            // their settings keep working from akvr_settings.ini.
            // Camera pitch (TILTBOX / ROLLSIGN): follows the game's tilt by default, like UEVR (JJ's settled choice).
            bool decouple = akvr_head_pitch_unlink() != 2;
            if (g_showAdvanced && ImGui::Checkbox("decouple the camera pitch  (view stays level)", &decouple))
            { akvr_head_pitch_unlink_set(decouple ? 0 : 2); settings_save(); }
            if (g_showAdvanced && decouple)
            {
                float sh = akvr_head_stick_height();
                if (SliderStep("right stick up/down: extra camera height  (m)", &sh, 0.0f, 8.0f, "%.1f"))
                    akvr_head_stick_height_set(sh);
                if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
            }

            if (akvr_xr_native())
            {
                // EDGES (SKVR OVERSCAN + VEDGE): margins beyond the lenses.
                float ft = akvr_head_fov_delta();
                if (ft < 0.0f) ft = 0.0f;
                bool shapeCh = false;
                if (SliderStep("extra view at the sides  (deg)", &ft, 0.0f, 30.0f, "%.1f"))
                { akvr_head_fov_set(ft); shapeCh = true; }
                float fv = g_ovV < 0.0f ? ft : g_ovV;
                if (g_showAdvanced)   // PANELTIDY2
                {
                    if (SliderStep("extra view top and bottom  (deg, restart)", &fv, 0.0f, 30.0f, "%.1f"))
                    { g_ovV = fv; shapeCh = true; }
                    ImGui::SameLine();
                    if (ImGui::Button("same as sides")) { g_ovV = -1.0f; shapeCh = true; }
                }
                if (shapeCh)
                {
                    const int calc = next_geo11_shape(g_eyeViewSaved);
                    if (calc > 0) akvr_geo11_shape_set(calc);
                    g_sliderDirty = true;
                }
            }

            // Picture size (the engine's own render height; width follows the shape).
            // RESMAX 2026-10-05 â€” JJ: "a slider for resolution that can increase even more than our
            // existing high setting for people with better headsets". The 1-pixel 0..4320 slider is now
            // a scale of the installer's High (2860 tall, the tested size) like Batman City's
            // resolution scale, 0.50x .. 2.00x, plus the installer's sizes and three above High.
            // rendersize follows engineres (the installer writes both; the window height reads it).
            {
                const int kHigh = 2860;
                const int maxH = akvr_early_max_height();
                auto set_h = [&](int h) {
                    akvr_engine_res_set(h);
                    if (akvr_engine_res() > 0) akvr_early_size_set(akvr_engine_res());
                    g_sliderDirty = true;
                };
                uint32_t bbw = 0, bbh = 0; akvr_xr_backbuffer_size(bbw, bbh);
                const int cur = akvr_engine_res();
                float sc = (float)(cur > 0 ? cur : kHigh) / (float)kHigh;
                if (SliderStep("picture size per eye  (1.00 = High, restart)", &sc, 0.50f, (float)maxH / (float)kHigh, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp))
                    set_h(((int)lroundf(sc * (float)kHigh) + 2) & ~3);   // steps of 4, so 1.00 stays exactly High
                static const struct { const char* name; int h; } kSizes[] = {
                    {"Low", 2016}, {"Medium", 2432}, {"High", 2860}, {"Very high", 3600}, {"Ultra", 4320}, {"Max", 5760} };
                ImGui::TextUnformatted("   ");
                for (const auto& s : kSizes)
                {
                    ImGui::SameLine();
                    char lbl[48]; snprintf(lbl, sizeof(lbl), "%s##res%d", s.name, s.h);
                    if (ImGui::RadioButton(lbl, cur == s.h)) { set_h(s.h > maxH ? maxH : s.h); settings_save(); }
                }
                const int h = akvr_engine_res();
                const int w = ((akvr_xr_native() && !akvr_engine_stereo()) ? (h * akvr_geo11_shape()) / 1000 : (h * akvr_engine_shape()) / 1000) & ~7;   // PANELTIDY4: native stereo uses engineshape
                if (h)
                {
                    ImGui::TextDisabled("   now %u x %u per eye  ->  next launch %d x %d  (%.1f million pixels per eye; higher = sharper, slower)",
                                        bbw, bbh, w, h, (float)w * (float)h / 1e6f);
                    if (h > kHigh)
                        ImGui::TextColored(kAmber, "   above High: for sharper headsets and faster graphics cards than the tested RTX 4070 Ti. Every picture is drawn twice.");
                    uint32_t rw = 0, rh = 0, mw = 0, mh = 0;
                    akvr_xr_recommended_size(rw, rh, mw, mh);
                    if (mw && mh && ((uint32_t)w > mw || (uint32_t)h > mh))
                        ImGui::TextColored(kAmber, "   bigger than this headset accepts (%u x %u per eye): it may show black. Pick a smaller size.", mw, mh);
                }
                else
                    ImGui::TextDisabled("   off: the game picks its own size (now %u x %u)", bbw, bbh);
            }

            if (akvr_xr_native() && g_showAdvanced && !akvr_engine_stereo())   // PANELTIDY2: an experiment (the compass doubles with it on)
            {
                // EYEVIEW (SKVR port): each eye's picture turned outward to match the lens.
                bool ev = g_eyeViewSaved;
                if (ImGui::Checkbox("use each eye's full view  (~20% fewer pixels, restart)", &ev))
                {
                    float o, i, u, d;
                    const float fct = akvr_xr_eye_tangents(o, i, u, d) ? (o + i) / (2.0f * o) : 0.8048f;
                    const int calc = next_geo11_shape(ev);
                    if (ev && !g_eyeViewSaved)
                    {
                        g_eyeBaseShape = akvr_geo11_shape();
                        akvr_geo11_shape_set(calc > 0 ? calc : (int)lroundf((float)g_eyeBaseShape * fct));
                    }
                    else if (!ev && g_eyeViewSaved)
                        akvr_geo11_shape_set(calc > 0 ? calc : (g_eyeBaseShape > 0 ? g_eyeBaseShape : (int)lroundf((float)akvr_geo11_shape() / fct)));
                    g_eyeViewSaved = ev;
                    settings_save();
                }
                if (g_eyeViewSaved != akvr_xr_eye_view())
                    ImGui::TextColored(kAmber, "   restart the game to %s it", g_eyeViewSaved ? "start" : "stop");
                if (akvr_xr_eye_view())
                {
                    float k = akvr_xr_eye_k();
                    if (SliderStep("distance alignment  (make far things single)", &k, 0.10f, 0.40f, "%.3f"))
                        akvr_xr_eye_k_set(k);
                    ImGui::SameLine();
                    if (ImGui::Button("reset##eyek")) { akvr_xr_eye_k_set(-1.0f); settings_save(); }
                    bool fl = akvr_xr_eye_flip();
                    if (ImGui::Checkbox("flip eye turn  (if everything is badly doubled)", &fl)) { akvr_xr_eye_flip_set(fl); settings_save(); }
                }
            }

            // CAMRESTORE 2026-10-02 (JJ: the camera "pops" getting in and out of the Batmobile)
            // PANELTIDY3: "smooth Batmobile get in / out" is settled ON (camrestore) - no toggle
            // AIMHEAD 2026-10-03 (JJ: the Batmobile's weapon reticle no longer follows the head)
            bool aih = akvr_cam_aim_head();
            if (ImGui::Checkbox("aim with your head while holding the left trigger  (Batmobile battle mode, gadget aim)", &aih)) { akvr_cam_aim_head_set(aih); settings_save(); }
            ImGui::SameLine();
            if (akvr_cam_aim_head_now()) ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "AIMING");
            else ImGui::TextDisabled("(trigger not held)");
            // CAMSMOOTH 2026-10-02 (JJ: getting out of the Batmobile still pops - the game's camera jumps ~30 cm in a frame)
            // PANELTIDY3: "soften the game's sudden camera jumps" is settled ON (camsmooth) - no toggle
            // SWINGEASE 2026-10-02 (JJ: getting out of the Batmobile still pops - the game swings its camera ~55 deg in 0.37 s)
            bool csw = akvr_cam_swing();
            if (ImGui::Checkbox("ease the game's fast camera tilts  (comfort - not while you use the right stick)", &csw)) { akvr_cam_swing_set(csw); settings_save(); }

            // CAMERA POSITION: all three slide the viewpoint along the camera's own axes,
            // so they stay glued to it however it is pitched or rolled (JJ, 2026-08-05).
            if (ImGui::TreeNode("where you stand  (camera offset)"))
            {
                float x = akvr_head_shoulder();
                if (SliderStep("left / right", &x, -300.0f, 300.0f, "%.0f", kNum))
                    akvr_head_shoulder_set(x);
                if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
                float y = akvr_head_offset_up();
                if (SliderStep("down / up", &y, -300.0f, 300.0f, "%.0f", kNum))
                    akvr_head_offset_up_set(y);
                if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
                float z = akvr_head_offset_fwd();
                if (SliderStep("back / forward", &z, -300.0f, 300.0f, "%.0f", kNum))
                    akvr_head_offset_fwd_set(z);
                if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
                if (ImGui::Button("Reset position"))
                {
                    akvr_head_shoulder_set(0.0f);
                    akvr_head_offset_up_set(0.0f);
                    akvr_head_offset_fwd_set(0.0f);
                    settings_save();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("standing LEFT of what you look at? drag left/right RIGHT.");
                ImGui::TreePop();
            }
        }

        // ---- HUD ----------------------------------------------------------------
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);   // closed at each launch
        // PARTSHUT 2026-10-01 â€” JJ: "the move resize single HUD parts drawer should be closed by default now while we
        // work on the rain": the HUD section and the parts list no longer open themselves (PARTOPEN off at these two).
        if (ImGui::CollapsingHeader("HUD"))
        {
            // PANELTIDY3: see-through HUD fix, HUD glows, HUD drawn into the eyes and markers on their objects are settled
            // ON (hudineye=1, markercut=1, AKN hudblend / hudcolour defaults) - no toggles
            static bool s_hudDirty = false;
            if (akvr_engine_stereo())
            {   // MARKERWORLD 2026-10-09 (JJ: the distance marker "a little bit too small")
                float ms = akvr_xr_marker_scale();
                if (SliderStep("distance marker / reticle size", &ms, 0.5f, 3.0f, "%.2f")) { akvr_xr_marker_scale_set(ms); s_hudDirty = true; }
            }
            float hud = akvr_hud_scale();
            if (SliderStep("HUD size", &hud, 0.25f, 2.0f, "%.2f"))
            { akvr_hud_scale_set(hud); s_hudDirty = true; }
            float raise = akvr_hud_raise() * 100.0f;
            if (SliderStep("HUD up / down  (%% of view height)", &raise, -30.0f, 30.0f, "%.0f"))
            { akvr_hud_raise_set(raise / 100.0f); s_hudDirty = true; }
            // HUDLIVE: the whole HUD's distance, live through geo-11's stereo object (geo11conv.cpp).
            // One distance for every HUD part: geo-11 has one HUD shift, per part is not possible.
            {
                float dist = akvr_geo11_hud_dist();
                if (SliderStep("HUD distance  (m, 0 = far away)", &dist, 0.0f, 20.0f,
                               dist <= 0.05f ? "far away  %.1f" : "%.1f m"))
                { akvr_geo11_hud_dist_set(dist); s_hudDirty = true; }
                ImGui::SameLine();
                if (ImGui::Button("far away##hd")) { akvr_geo11_hud_dist_set(0.0f); s_hudDirty = true; }
            }
            // HUDLAYER: the HUD on its own head-locked headset layer (steady at 90 Hz).
            {
                bool lay = akvr_hudsplit_layer();
                if (g_showAdvanced && ImGui::Checkbox("HUD on its own layer (steady HUD)", &lay)) { akvr_hudsplit_layer_set(lay); s_hudDirty = true; }   // PANELTIDY2
                // HUDWORLD 2026-09-29 (JJ): the HUD hangs fixed in the room by default; this glues it to the head.
                bool attach = akvr_xr_hud_space() == 1;
                if (!lay) ImGui::BeginDisabled();
                if (ImGui::Checkbox("attach UI to head movement", &attach)) { akvr_xr_hud_space_set(attach ? 1 : 2); s_hudDirty = true; }
                if (!lay) ImGui::EndDisabled();
                ImGui::TextDisabled(lay ? (attach ? "   the HUD moves with your head"
                                                  : "   the HUD stays put in the room; F12 hangs it in front of you again")
                                        : "   needs the HUD on its own layer");
                // HUDEYES 2026-10-01 (VR_HUD_GUIDE.md section 1): off = the old shift along the HUD's own right.
                // JJ: "much better when it's on. I don't know whether we need this as an option anymore" - advanced only
                // (kept for a headset app that adds quad parallax itself: VR_HUD_GUIDE section 1, "Keep a switch").
                bool eyeF = akvr_xr_hud_eye_follow();
                if (g_showAdvanced && ImGui::Checkbox("HUD depth follows your eyes", &eyeF)) { akvr_xr_hud_eye_follow_set(eyeF); s_hudDirty = true; }
                // PANELTIDY3: reticle / distance-marker fixes are settled ON (markerhead, markerface, markerdist) - no toggles
                // FRAMEPAIR / FRAMEPAIR2 panel controls removed 2026-10-02: with OneFrameThreadLag=False the world, the rain
                // and the markers share one frame (JJ: "perfectly stable"); settings framefix / framedelay still read.
                if (g_showAdvanced && lay && ImGui::TreeNode("HUD layer settings"))
                {
                    // PANELTIDY 2026-09-28 (JJ: confusing for new users): placement (view space since HUDVIEW),
                    // see-through colour conversion and the per-eye distance fix are fixed in xr.cpp.
                    bool split = akvr_hudsplit_split();
                    if (ImGui::Checkbox("   keep the reticle at the depth it points at##hsp", &split)) { akvr_hudsplit_split_set(split); s_hudDirty = true; }
                    ImGui::TextDisabled("      %s", akvr_hudsplit_split_diag());
                    // Compass band slider removed 2026-09-29 (JJ): fixed at 28% in hudsplit.cpp.
                    // DIAG RB record button removed 2026-09-28 (done: the RB icon is a glyph in the shared font cache).
                    // HUDVIEW 2026-09-28: lazy follow removed (JJ: a "cheap trick"). Attached = glued to the head by
                    // the headset itself every refresh (xr.cpp g_hudSpace = 1); HUDWORLD default = fixed in the room (2).
                    if (akvr_xr_fps_lock_ssw())
                        ImGui::TextColored(kAmber, "   frame rate is on Virtual Desktop SSW: the HUD can only move 45 times a second. Try \"repeat the frame\".");
                    ImGui::TextDisabled("   %s", akvr_hudsplit_layer_diag());
                    ImGui::TextDisabled("   %s", akvr_xr_hud_layer_diag());
                    ImGui::TextDisabled("   %s", akvr_geo11_hud_diag());
                    ImGui::TreePop();
                }
            }
            // HUDSPLIT test section removed from the panel (JJ, 2026-09-28); the probe still logs to F2 / status.
            // HUDSTEADY slider removed (HUDDEPTH, 2026-09-28): JJ saw lag and no calmer HUD at 0.5 and 1.0.
            // The jitter is the HUD baked into a 45 Hz picture that the headset shows twice and re-aims
            // (and SSW warps) at 90 Hz - no shift inside the picture can remove that. Real fix: the HUD
            // on its own compositor layer (VR_IMPROVEMENT_BACKLOG item 6). Code kept, always off.
            if (s_hudDirty && !ImGui::IsAnyItemActive()) { settings_save(); s_hudDirty = false; }
            if (!akvr_hud_scale_found())
                ImGui::TextColored(kAmber, "   HUD size control not found in this game build");

            bool menuFill = akvr_hud_menu_fill();
            if (g_showAdvanced && ImGui::Checkbox("menus and map use the full height", &menuFill))   // PANELTIDY2
            { akvr_hud_menu_fill_set(menuFill); settings_save(); }
            // HUDLAYERS (earlyres.cpp): move / resize / hide single parts inside the HUD.
            ImGui::SetNextItemOpen(false, ImGuiCond_Once);   // PARTSHUT: closed until opened
            if (ImGui::TreeNode("move / resize single HUD parts  (radar, compass ...)"))
            {
                if (ImGui::Button("find the HUD parts  (with the HUD on screen)")) akvr_hud_layers_discover();
                ImGui::TextDisabled("%s", akvr_hud_layers_diag());
                ImGui::TextDisabled("tick 'hide' on a part to see which one it is, then open it to move or resize it.");
                if (g_showAdvanced)
                {   // ROOMALL (settled, PANELTIDY2)
                    bool all = akvr_hud_room_all();
                    if (ImGui::Checkbox("whole HUD hangs in the room  (tick 'stays on its target' on parts that point at the world)", &all))
                    { akvr_hud_room_all_set(all); settings_save(); }
                }
                ImGui::TextDisabled("'hang in the room': the whole part joins the room-fixed HUD (for parts that do not point at the world).");
                ImGui::TextDisabled("   %s", akvr_hud_layer_tag_diag());
                for (int i = 0; i < akvr_hud_layer_count(); ++i)
                {
                    int d, par, kids; const char* lab; float ls, lx, ly; bool lh, l3;
                    if (akvr_hud_layer_get(i, d, par, kids, lab, ls, lx, ly, lh, l3) && d == 0)
                    {
                        int cn = 0; for (int j = 0; j < i; ++j) { int d2, p2, k2; const char* l2; float a2, b2, c2; bool h2, e2;
                                                                   if (akvr_hud_layer_get(j, d2, p2, k2, l2, a2, b2, c2, h2, e2) && d2 == 0) ++cn; }
                        char t[128]; _snprintf_s(t, sizeof(t), _TRUNCATE, "HUD container %d  (%s)##lc%d", cn, lab, i);
                        if (part_hint(i)) ImGui::SetNextItemOpen(true, ImGuiCond_Once);   // PARTOPEN
                        if (ImGui::TreeNode(t)) { draw_hud_layer_children(i); ImGui::TreePop(); }
                    }
                }
                ImGui::TreePop();
            }
        }

        // ---- ZOOM VIGNETTE (ZOOMVIG 2026-09-29, JJ) --------------------------------------
        // The right-stick-click zoom: our lock keeps the view 1:1, so only the game's 2D overlay was left.
        // Our own dark edge instead, while the game's own view is narrower than the threshold.
        // PANELTIDY5: the POST EFFECTS drawer (the game's look, glow, grading) is gone - it only acted with depth of field off.
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (ImGui::CollapsingHeader("ZOOM VIGNETTE"))
        {
            static bool s_vigDirty = false;
            float vs = akvr_xr_vig_strength();
            if (SliderStep("strength  (0 = off)", &vs, 0.0f, 1.0f, "%.1f")) { akvr_xr_vig_strength_set(vs); s_vigDirty = true; }
            float vc = akvr_xr_vig_clear();
            if (SliderStep("clear centre  (deg)", &vc, 5.0f, 60.0f, "%.0f")) { akvr_xr_vig_clear_set(vc); s_vigDirty = true; }
            float vb = akvr_xr_vig_below();
            if (g_showAdvanced && SliderStep("zoom when the game's view is narrower than  (deg)", &vb, 0.0f, 120.0f, "%.0f")) { akvr_xr_vig_below_set(vb); s_vigDirty = true; }   // PANELTIDY2
            static bool s_prev = false;
            if (ImGui::Checkbox("preview the vignette now", &s_prev)) akvr_xr_vig_preview(s_prev);
            if (g_showAdvanced) ImGui::TextDisabled("   %s", akvr_xr_vig_diag());
            if (s_vigDirty && !ImGui::IsAnyItemActive()) { settings_save(); s_vigDirty = false; }
        }
        else akvr_xr_vig_preview(false);   // never leave a preview on behind a closed section

        // PANELTIDY4 2026-10-11: the RAIN TEST section and the old rain-hunt controls are gone from the panel (the rain is
        // fixed: RAINONCE / FARRAIN2 / RAINDEPTH). AKN commands rainview / rainpause / farrain stay for tests.

        // ---- MENUS AND SCREENS ------------------------------------------------------
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (ImGui::CollapsingHeader("MENUS AND SCREENS"))
        {
            {   // MENUFIT 2026-10-06 - JJ: the gadget wheel stretched, the AR challenges screen clipped
                bool mf = akvr_menu_fit();
                if (ImGui::Checkbox("keep pop-up menus in their own shape  (gadget wheel, challenges)", &mf)) { akvr_menu_fit_set(mf); settings_save(); }
                if (g_showAdvanced) ImGui::TextDisabled("   %ld fixes so far (%ld screen-size questions)", akvr_menu_fit_writes(), akvr_menu_fit_res_seen());
            }
            bool scrMode = akvr_xr_screen_mode();
            if (ImGui::Checkbox("float as a screen now  (Pause key)", &scrMode))
                akvr_xr_screen_mode_toggle();
            if (g_showAdvanced)   // PANELTIDY2: settled (head tracking on, the picture's own shape)
            {
                ImGui::SameLine();
                bool scrTrack = akvr_xr_screen_track();
                if (ImGui::Checkbox("with head tracking", &scrTrack))
                { akvr_xr_screen_track_set(scrTrack); settings_save(); }
                const float a = akvr_xr_screen_aspect();
                ImGui::TextUnformatted("floating screen shape:");
                ImGui::SameLine(); if (ImGui::RadioButton("picture", a < 0.5f)) { akvr_xr_screen_aspect_set(0.0f); settings_save(); }
                ImGui::SameLine(); if (ImGui::RadioButton("16:9", a > 1.7f && a < 1.9f)) { akvr_xr_screen_aspect_set(1.778f); settings_save(); }
                ImGui::SameLine(); if (ImGui::RadioButton("21:9", a > 2.2f)) { akvr_xr_screen_aspect_set(2.333f); settings_save(); }
            }
            float fs = akvr_xr_float_screen_scale() * 100.0f;
            if (SliderStep("floating screen size %", &fs, 10.0f, 100.0f, "%.0f"))
                akvr_xr_float_screen_scale_set(fs / 100.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
            float scr = akvr_xr_menu_zoom() * 100.0f;
            if (SliderStep("main menu size %", &scr, 10.0f, 100.0f, "%.0f"))
                akvr_xr_menu_zoom_set(scr / 100.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
            // MENUSIDE 2026-10-02 (JJ: move the main-menu camera left so Batman sits right of the menu items)
            float ms = akvr_head_menu_side();
            // MENUSIDE3 2026-10-02 â€” JJ: MENUTEXT left the menu "exactly how it was before"; "can't we just shift the camera
            // over a little bit to the left without breaking anything?" The slide now rides the game camera's own level
            // right (MENUSIDE2), so head turns no longer swing it. The text slider is gone (no effect on that screen).
            if (SliderStep("main menu camera: left / right  (minus = Batman further right, 0 = the game's own)", &ms, -400.0f, 400.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
            { akvr_head_menu_side_set(ms); settings_save(); }
            // MENUBACK (JJ: the backing back, "a slider for opacity"; MENUNOBACK's on/off made the items "look a bit weird")
            float mbo = akvr_xr_menu_back_opacity() * 100.0f;
            if (SliderStep("main menu: dark background behind the items %  (100 = the game's own)", &mbo, 0.0f, 100.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
            { akvr_xr_menu_back_opacity_set(mbo / 100.0f); settings_save(); }
            // MENUFEATHER (JJ: "can we feather the edges of it so it's not as noticeable when looking around?")
            float mbf = akvr_xr_menu_back_feather() * 100.0f;
            if (SliderStep("main menu: soften the dark background's edges %", &mbf, 0.0f, 100.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
            { akvr_xr_menu_back_feather_set(mbf / 100.0f); settings_save(); }
            // AUDIOSYNC (JJ: "maybe we need to pause the audio so it syncs up with ... the menu screen appearing")
            bool as = akvr_audiosync();
            if (ImGui::Checkbox("start-up: hold the sound while the game loads the menu", &as)) { akvr_audiosync_set(as); settings_save(); }
            // (TRANSFULL test checkbox removed 2026-10-14 - not ready; parked, HUD_REBUILD_PLAN HR4)
            if (g_showAdvanced) ImGui::TextDisabled("   %s", akvr_audiosync_diag());
            float pscr = akvr_xr_pause_zoom() * 100.0f;
            if (SliderStep("map size %", &pscr, 10.0f, 100.0f, "%.0f"))   // PAUSESIZE: the pause menu has its own size now
                akvr_xr_pause_zoom_set(pscr / 100.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
            float pms = akvr_xr_pause_menu_size() * 100.0f;   // PAUSESIZE (JJ: "so you don't have to turn your head")
            if (SliderStep("pause menu size %", &pms, 20.0f, 100.0f, "%.0f"))
            { akvr_xr_pause_menu_size_set(pms / 100.0f); settings_save(); }
            bool pl = akvr_xr_pause_look();   // PAUSELOOK (JJ: the pause as in Sekiro)
            if (g_showAdvanced && ImGui::Checkbox("pause: world stays around you, menu on top  (off: a floating window)", &pl))
            { akvr_xr_pause_look_set(pl); settings_save(); }
            bool nb = akvr_xr_pause_no_back();   // PAUSENOBACK
            if (g_showAdvanced && ImGui::Checkbox("pause: leave out the menu's dark background", &nb)) { akvr_xr_pause_no_back_set(nb); settings_save(); }
            float pd = akvr_xr_pause_dim() * 100.0f;   // PAUSEDIM
            if (SliderStep("pause: darken the world %", &pd, 0.0f, 90.0f, "%.0f"))
            { akvr_xr_pause_dim_set(pd / 100.0f); settings_save(); }
            float sscr = akvr_xr_splash_zoom() * 100.0f;   // SPLASHSIZE (JJ: the start-up logos were a bit too big)
            if (SliderStep("start-up screens size %", &sscr, 10.0f, 100.0f, "%.0f"))
            { akvr_xr_splash_zoom_set(sscr / 100.0f); settings_save(); }
            float lscr = akvr_xr_load_zoom() * 100.0f;   // LOADSIZE
            if (SliderStep("loading screens size %", &lscr, 10.0f, 100.0f, "%.0f"))
            { akvr_xr_load_zoom_set(lscr / 100.0f); settings_save(); }
            float lup = akvr_xr_load_up();   // LOADUP (JJ: move the loading screen up and down)
            if (SliderStep("loading screens up / down (degrees)", &lup, -30.0f, 30.0f, "%.0f"))
            { akvr_xr_load_up_set(lup); settings_save(); }
            float snr = akvr_xr_screen_near();   // SCREENNEAR (JJ: title and loading screens a bit closer)
            if (SliderStep("start-up / loading screens distance (metres, 0 = far)", &snr, 0.0f, 20.0f, snr < 0.5f ? "far" : "%.1f"))
            { akvr_xr_screen_near_set(snr); settings_save(); }
            if (g_showAdvanced)   // PANELTIDY2: settled
            {
                float pa = akvr_xr_pause_aspect();
                if (pa < 0.5f) pa = 1.0f;
                if (SliderStep("pause / map / loading shape  (1.78 = 16:9)", &pa, 1.0f, 2.4f, "%.2f"))
                    akvr_xr_pause_aspect_set(pa <= 1.001f ? 0.0f : pa);
                bool menuLive = akvr_xr_menu3d();
                if (ImGui::Checkbox("main menu: live 3D, follows your head", &menuLive))
                { akvr_xr_menu3d_set(menuLive); settings_save(); }
                if (akvr_xr_main_menu_detected())
                { ImGui::SameLine(); ImGui::TextColored(kGreen, "main menu detected"); }
                bool menuFlat = akvr_xr_menu_flat();
                if (!akvr_engine_stereo() && ImGui::Checkbox("floating screens show a flat picture", &menuFlat))
                { akvr_xr_menu_flat_set(menuFlat); settings_save(); }
            }
        }

        // ---- FRAME RATE (geo-11 only) -------------------------------------------------
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (akvr_xr_native() && ImGui::CollapsingHeader("FRAME RATE"))
        {
            // FPSLOCK (xr.cpp): every game frame held for the same number of refreshes.
            {
                if (akvr_xr_ofxr_active())
                    ImGui::TextColored(kAmber, "OFXR Bridge frame generation is on: it paces the game, so this lock is paused.");
                int lock = akvr_xr_fps_lock();
                // FPSNOTE 2026-09-27 â€” JJ expected "repeat the frame" to raise the frame rate:
                // both in-between modes hold the GAME at the chosen rate; only "off" frees it.
                // FPS72 + FPSMORE + FPSSLIDER 2026-10-05 â€” JJ: more rates, then "change this to a slider that
                // just goes between these values only". 30 .. 120, then "off" (no limit) at the far right.
                {
                    const int* rates = nullptr;
                    const int nr = akvr_xr_fps_lock_choices(&rates);
                    int idx = nr;   // off
                    for (int i = 0; i < nr; ++i)
                        if (rates[i] == lock) idx = i;
                    char txt[32];
                    if (idx >= nr) snprintf(txt, sizeof(txt), "off (no limit)");
                    else snprintf(txt, sizeof(txt), "%d fps", rates[idx]);
                    if (ImGui::SliderInt("hold the game at", &idx, 0, nr, txt, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
                    {
                        akvr_xr_fps_lock_set(idx < nr ? rates[idx] : 0);
                        g_sliderDirty = true;
                    }
                }
                lock = akvr_xr_fps_lock();
                // FPSSHOW 2026-09-28 â€” JJ: "the option for double frames or SSW seems to be gone" (it was only
                // drawn while a lock rate was chosen, and his lock was off). Always shown now, with a note.
                {
                    const bool ssw = akvr_xr_fps_lock_ssw();
                    ImGui::TextUnformatted("between game frames:");
                    ImGui::SameLine(); if (ImGui::RadioButton("repeat the frame", !ssw)) { akvr_xr_fps_lock_ssw_set(false); settings_save(); }
                    ImGui::SameLine(); if (ImGui::RadioButton("Virtual Desktop SSW", ssw)) { akvr_xr_fps_lock_ssw_set(true); settings_save(); }
                    if (!lock)
                        ImGui::TextColored(kAmber, "   only used when the game is held at a rate. With 'off' the HUD layer moves only when the game draws.");
                }
                if (lock)
                {
                    const bool ssw = akvr_xr_fps_lock_ssw();
                    ImGui::TextDisabled("   the game runs at this rate either way; 'off' lets it run as fast as it can.");
                    if (!ssw)
                        ImGui::TextDisabled("   back from SSW? set Virtual Desktop's SSW off Always, or VD keeps the game at half rate itself.");
                    int div = 1; double hz = 0.0; long late = 0, frames = 0;
                    akvr_xr_fps_lock_info(div, hz, late, frames);
                    if (hz > 1.0)
                    {
                        const double got = hz / div;
                        if (ssw)
                        {
                            ImGui::TextDisabled("   headset %.0f Hz: a new frame every %d refreshes = %.1f fps, SSW fills the rest.  late %ld, early %ld of %ld",
                                                hz, div, got, late, akvr_xr_fps_lock_early(), frames);
                            ImGui::TextDisabled("   set Virtual Desktop's SSW to Always (Streaming tab) for this mode.");
                        }
                        else
                            ImGui::TextDisabled("   headset %.0f Hz: each frame shown %d times = %.1f fps;  late frames %ld of %ld",
                                                hz, div, got, late, frames);
                        if (fabs(got - lock) > 0.5)
                        {
                            // The Quest 3 rates through Virtual Desktop that are a whole multiple of the lock.
                            char fit[32] = ""; int nf = 0;
                            for (int q : {72, 80, 90, 120})
                                if (q % lock == 0)
                                {
                                    const size_t len = strlen(fit);
                                    snprintf(fit + len, sizeof(fit) - len, "%s%d", nf++ ? " or " : "", q);
                                }
                            if (nf)
                                ImGui::TextColored(kAmber, "   %d fps needs the headset at %s Hz (Virtual Desktop setting). Running %.0f now.",
                                                   lock, fit, got);
                            else
                                ImGui::TextColored(kAmber, "   %d fps needs a headset rate that is a multiple of it; the Quest 3 has none (72 / 80 / 90 / 120 Hz), so it runs %.0f.",
                                                   lock, got);
                        }
                    }
                    // FPS72 / FPSMORE: the mod switches the headset's rate itself when the runtime allows it.
                    const char* rd = akvr_xr_refresh_diag();
                    if (akvr_xr_refresh_ext())
                        ImGui::TextDisabled("   headset refresh: %s", rd[0] ? rd : "waiting for VR");
                    else
                        ImGui::TextDisabled("   Virtual Desktop does not let the mod change the refresh rate: set it in its Streaming tab.");
                    ImGui::TextDisabled("   %d fps needs the game to finish a frame in under %.0f ms: if 'late frames' climbs, lower the picture size.",
                                        lock, 1000.0 / lock);
                }
            }
            // PANELTIDY3: head-pose delays are measured and settled (posedelay 2, menuposedelay 2; engine stereo stamps
            // each picture exactly) - no sliders
        }

        // ---- ADVANCED (hidden) ---------------------------------------------------
        // TIDY3 2026-09-27 (JJ): experiments and settled switches off the panel. Shown only
        // with advancedpanel=1 in akvr_settings.ini (not written back, so it stays opt-in).
        if (g_showAdvanced) ImGui::Separator();
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (g_showAdvanced && ImGui::CollapsingHeader("Advanced  (numbers)"))
        {
            uint32_t bbw = 0, bbh = 0; akvr_xr_backbuffer_size(bbw, bbh);

            // PANELTIDY3: projVR (off), full view + tilt fill (on), auto pose match (off), HUD proportions (on), geo-11
            // true 3D (on), swap eyes (on), capture after Present (off), AER spin correction, main-menu spotting (on), render
            // bigger / force size (on) are settled: no switches. Their settings are still read and saved.
            {
                const char* d = akvr_engine_res_diag();
                bool bad = (strstr(d, "NOT FOUND") || strstr(d, "FAILED"));
                ImGui::TextColored(bad ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f) : ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "   %s", d);
            }
            // ------- everything below is EVIDENCE, not a decision: keep it folded ----
            if (ImGui::TreeNode("image detail (numbers)"))
            {
                uint32_t rw = 0, rh = 0, mw = 0, mh = 0;
                akvr_xr_recommended_size(rw, rh, mw, mh);
                ImGui::TextDisabled("frame (backbuffer): %u x %u", bbw, bbh);
                if (rw && rh)
                    ImGui::TextDisabled("headset wants %u x %u per eye - supplying %.0f%% x %.0f%%",
                        rw, rh, 100.0f * (float)bbw / (float)rw, 100.0f * (float)bbh / (float)rh);
                else
                    ImGui::TextDisabled("headset target: not measured yet");
                // STICKY: only overwritten on a frame that matched. Trust the LIST below.
                ImGui::TextDisabled("last matched viewport (stale-able): %.0f x %.0f   filled %d/frame",
                                    g_rSceneW, g_rSceneH, g_rFilled);
                ImGui::TextDisabled("fill gate: on=%d  bb=%ux%u  entered=%d  scene-match=%d",
                                    g_vpFill ? 1 : 0, g_bbW, g_bbH, g_rFillGate, g_rFillScene);
                {
                    uint32_t oew = 0, oeh = 0, oow = 0, ooh = 0;
                    akvr_xr_our_sizes(oew, oeh, oow, ooh);
                    ImGui::TextDisabled("ours: eye %ux%u  overlay %ux%u", oew, oeh, oow, ooh);
                }
                ImGui::TextDisabled("viewports set last frame (%d calls):", g_rCalls);
                if (g_rListN == 0)
                    ImGui::TextDisabled("   (none)");
                for (int i = 0; i < g_rListN; ++i)
                    ImGui::TextDisabled("   %d x %d   (x%d)%s",
                        g_rList[i].w, g_rList[i].h, g_rList[i].count,
                        ((UINT)g_rList[i].w == g_bbW && (UINT)g_rList[i].h == g_bbH)
                            ? "  <- already the frame" : "");
                if (akvr_projvr_ok())
                {
                    float ratio = 0.0f, origFov = 0.0f; int hits = 0;
                    akvr_projvr_diag(ratio, hits, origFov);
                    ImGui::TextDisabled("lens: aspect %.3f  game HFOV %.0f deg  rewrites %d",
                                        ratio, origFov, hits);
                }
                if (g_forceRes)
                {
                    ImGui::TextDisabled("creation: game asked %ux%u", g_createdW, g_createdH);
                    if (g_clientW || g_clientH)
                    {
                        bool ok = ((UINT)g_clientW == g_bbW && (UINT)g_clientH == g_bbH);
                        ImGui::TextColored(ok ? ImVec4(0.6f,0.6f,0.6f,1.0f)
                                              : ImVec4(1.0f,0.5f,0.2f,1.0f),
                            "window client: %d x %d%s", g_clientW, g_clientH,
                            ok ? "" : "   <-- CLAMPED, engine sizes from THIS");
                        if (!ok) ImGui::TextDisabled("  resync attempts: %d", g_syncTries);
                    }
                }
                ImGui::TextDisabled("next launch: %d x %d  (%.1f MP; %d imports, %ld calls)",
                                    akvr_early_width(), akvr_early_height(),
                                    akvr_early_width() * akvr_early_height() / 1000000.0f,
                                    akvr_early_patched(), akvr_early_calls());
                if (akvr_early_patched() == 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f),
                                       "NOT INSTALLED - the game imports none of the size APIs");
                if (ImGui::TreeNode("startup size log"))
                {
                    ImGui::TextUnformatted(akvr_early_log());
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        }

        // ---- DIAGNOSTICS (folded) ------------------------------------------------
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (g_showAdvanced && ImGui::CollapsingHeader("Diagnostics"))   // PANELTIDY2: F2 captures carry the same
        {
            float hz = akvr_xr_headset_hz();
            ImGui::TextDisabled("%.0f fps   headset %.0f Hz   VR: %s", io.Framerate, hz,
                akvr_xr_session_running() ? (akvr_xr_displaying() ? "displaying" : "connected, not displaying yet")
                                          : akvr_xr_status());
            GamepadDiag gd = akvr_gamepad_diag();
            ImGui::TextColored(gd.blanking ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(0.2f, 1.0f, 0.5f, 1.0f),
                "pad: hook %s  pad %s  -> game gets %s   (F4 release)",
                gd.hooked ? "on" : "OFF", gd.connected ? "connected" : "NOT CONNECTED",
                gd.blanking ? "NOTHING (menu mode)" : "full input");
            ImGui::TextDisabled("  raw 0x%04X  stick %+6d %+6d   keep-focus %s (F3)",
                gd.buttons, gd.lx, gd.ly, g_keepFocus ? "ON" : "OFF");

            { uint32_t bbw = 0, bbh = 0; akvr_xr_backbuffer_size(bbw, bbh);
              ImGui::Text("render %u x %u   frames %llu   up %.0fs",
                          bbw, bbh, g_frameCount, (GetTickCount64() - g_injectTick) / 1000.0); }

            CameraView cam = akvr_camera_read();
            if (!akvr_camera_installed())
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "camera: hook NOT installed (F9 retry)");
            else if (!cam.valid)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "camera: armed, waiting for gameplay");
            else
            {
                ImGui::Text("camera pos %+.1f %+.1f %+.1f   fov %.1f", cam.x, cam.y, cam.z, cam.fov);
                ImGui::Text("       rot yaw %+.1f pitch %+.1f roll %+.1f",
                            cam.yaw * 360.0f / 65536.0f, cam.pitch * 360.0f / 65536.0f, cam.roll * 360.0f / 65536.0f);
            }

            if (akvr_xr_ok() && akvr_xr_session_running())
            {
                float hy, hp, hr; akvr_xr_head_deg(hy, hp, hr);
                ImGui::Text("head yaw %+.1f pitch %+.1f roll %+.1f", hy, hp, hr);
                ImGui::TextDisabled("  %s", akvr_xr_session_status());
                const char* ps = akvr_xr_probe_status();
                if (ps) ImGui::TextDisabled("  %s", ps);
                if (akvr_xr_screen_mode())
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "  SCREEN MODE (Pause key) - floating world-fixed");
            }

            if (ht.on)
            {
                ImGui::Text("head-track lean %+.2f %+.2f %+.2f m  gain %.2f  stereo %s",
                            ht.leanX, ht.leanY, ht.leanZ, ht.posScale / kLeanAt1,
                            akvr_xr_native() ? "native" : "AER");
                // ROLL diag: head = what we intend, game = what the game renders
                // (rotator), display = what the compositor shows the frame at.
                CameraView rc = akvr_camera_read();
                float gameRoll = rc.valid ? (rc.roll * 360.0f / 65536.0f) : 0.0f;
                ImGui::TextDisabled("ROLL  head %+.1f   game %+.1f   display %+.1f",
                    ht.roll, gameRoll, akvr_xr_layer_roll());
            }

            FreecamState fc = akvr_freecam_state();
            ImGui::TextDisabled("freecam: %s (F7)", fc.on ? "ON" : "off");
            if (akvr_xr_native())
            {
                ImGui::TextWrapped("%s", akvr_geo11conv_diag());
                ImGui::TextWrapped("%s   headset k %.4f%s", akvr_geo11_eyeview_diag(), akvr_xr_eye_k_head(),
                                   akvr_xr_eye_narrow_ok() ? "" : "   RENDER NOT NARROW: eye view off");
                ImGui::TextWrapped("%s", akvr_menu_res_diag());
            }
        }

        // ---- KEYBOARD SHORTCUTS (folded) -----------------------------------------
        // RAINVIEW panel 2026-10-14 (JJ: "I don't have the setting anymore to isolate the rain"): the rain test back, in its own drawer
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (ImGui::CollapsingHeader("TROUBLESHOOTING"))
        {
            // DIAGKIT 2026-10-14 (JJ: test features "for whenever we run into HUD element bugs or sprite element bugs"): permanent tools.
            // The capture button always saves both eyes, the HUD, the marker data, the last 8 s of marker numbers and the code of every
            // shader on screen (akvr_captures\<time>\shaders). Guide: VR_DEBUG_TOOLKIT.md.
            ImGui::TextWrapped("Press CAPTURE (F2) right after you see a problem: it saves both eyes, the HUD and the effects on screen for a bug report.");
            static bool s_replay = false;
            if (ImGui::Checkbox("keep a replay of the last half second (adds it to each capture; uses a lot of disk)", &s_replay)) akn_command(s_replay ? "hudring 1" : "hudring 0");
            static bool s_detail = false;
            if (ImGui::Checkbox("detailed HUD and marker logs", &s_detail)) akn_command(s_detail ? "recorders 1" : "recorders 0");
            if (ImGui::Button("watch for effects that start appearing")) akn_command("psnewarm");
            ImGui::SameLine(); ImGui::TextDisabled("(2 s to learn what is on screen, then new ones are noted)");
            ImGui::Separator();
            ImGui::Text("Find a broken effect: hide them one at a time");
            if (ImGui::Button("collect the effects on screen")) akn_command("fxcollect");
            ImGui::SameLine(); if (ImGui::Button("<  previous")) akn_command("fxstep -1");
            ImGui::SameLine(); if (ImGui::Button("next  >")) akn_command("fxstep 1");
            ImGui::SameLine(); if (ImGui::Button("show all")) akn_command("fxstep 0");
            {   using StFn = int (*)(char*, int); static StFn st = nullptr; static bool t = false;
                if (!t) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); st = m ? (StFn)GetProcAddress(m, "akn_diag_status") : nullptr; t = m != nullptr; }
                char line[160] = "(engine stereo only)"; if (st) st(line, sizeof(line)); ImGui::TextDisabled("   %s - capture while the problem is gone to record which effect it is", line); }
            ImGui::Separator();
            // (SMOKELIGHT / LIGHTDIR always on; smoke cards always upright = AKN default spriteaxes 2 - JJ 2026-10-14; commands stay for tests)
            ImGui::Separator();
            static int s_rainView = 0;
            if (ImGui::RadioButton("rain as normal", s_rainView == 0)) { s_rainView = 0; akn_command("rainview 0"); }
            ImGui::SameLine(); if (ImGui::RadioButton("show only the rain", s_rainView == 2)) { s_rainView = 2; akn_command("rainview 2"); }
            ImGui::SameLine(); if (ImGui::RadioButton("hide the rain", s_rainView == 1)) { s_rainView = 1; akn_command("rainview 1"); }
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once); if (ImGui::CollapsingHeader("Keyboard shortcuts"))
        {
            ImGui::TextDisabled("F8 show/hide this panel   |   hold both stick clicks = use it with the controller");
            ImGui::TextDisabled("F12 recenter   |   F11 head tracking on/off   |   Pause/Break float as a screen");
            ImGui::TextDisabled("F2 save a capture (both eyes + settings, in akvr_captures)");
            ImGui::TextDisabled("F3 keep-focus   |   F4 release the pad");   // PANELTIDY4: geo-11 and old test keys no longer listed
        }
        // SAVEFIX: save any slider change once nothing is being dragged (see g_sliderDirty).
        if (g_sliderDirty && !ImGui::IsAnyItemActive())
        {
            g_sliderDirty = false;
            if (akvr_xr_native()) akvr_geo11conv_commit();
            settings_save();
        }
        ImGui::PopTextWrapPos();  // TEXTWRAP
        ImGui::PopItemWidth();   // SLIDERFIT
        ImGui::End();
    }

    // Dump the startup transcript to DISK. It lives in an ImGui tree, which is fine to
    // glance at and impossible to send to anyone (JJ, 2026-08-04: "How am I supposed to
    // send this to you? It's too long"). Written once, a few seconds in, so every
    // startup question has been asked and answered by then. Same folder as the settings.
    // Written THREE times, not once: at 6 s, 25 s and 60 s, each overwriting the last.
    // 2026-08-06 cost us a launch to learn why â€” with geo-11 patching every shader at
    // startup, the 6-second snapshot caught the game still on its legal screen with the
    // OpenXR session not yet up, so the file said "no views located yet" and could not
    // distinguish "the headset never connected" from "ask again in a moment".
    int g_diagWrites = 0;
    void write_startup_diag(bool requested)
    {
        static const ULONGLONG kAt[] = { 6000, 25000, 60000 };
        const int kMax = (int)(sizeof(kAt) / sizeof(kAt[0]));
        static ULONGLONG first = 0;
        ULONGLONG now = GetTickCount64();
        if (!first) { first = now; return; }

        // Plus one more the FIRST time the headset actually starts displaying â€”
        // whenever that happens. 2026-08-06: JJ put the headset on after the timed
        // snapshots had all been taken, so the file said "no HMD yet" about a session
        // that went on to work. A timer cannot know when someone picks up a headset.
        static bool sawDisplay = false;
        bool nowDisplaying = akvr_xr_displaying();
        bool force = requested || (nowDisplaying && !sawDisplay);
        if (nowDisplaying) sawDisplay = true;

        if (!force)
        {
            if (g_diagWrites >= kMax) return;
            if (now - first < kAt[g_diagWrites]) return;
        }
        if (g_diagWrites < kMax) ++g_diagWrites;

        std::wstring p = settings_path();
        if (p.empty()) return;
        p = p.substr(0, p.find_last_of(L"\\/")) + L"\\akvr_startup_log.txt";
        FILE* f = _wfopen(p.c_str(), L"wb");
        if (!f) return;
        fprintf(f, "AKVR startup diagnostic  (snapshot %d%s, %.0f s after the first frame)\n",
                g_diagWrites, force ? " - requested/display-start snapshot" : "",
                (double)(now - first) / 1000.0);
        // The VR side, FIRST â€” everything below is meaningless if the session never
        // came up, and that is exactly the case the old log could not report.
        fprintf(f, "\nOpenXR:\n   %s\n   %s\n   %s\n",
                akvr_xr_status(), akvr_xr_session_status(), akvr_xr_display_status());

        fprintf(f, "frame(backbuffer) : %ux%u\n", g_bbW, g_bbH);
        fprintf(f, "window client     : %dx%d   (resync attempts %d)\n",
                g_clientW, g_clientH, g_syncTries);
        // LUIDCHECK 2026-09-28: where the window opened (JJ saw it in a new place when VR stopped starting).
        if (g_hwnd)
        {
            RECT wr{}; GetWindowRect(g_hwnd, &wr);
            MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            fprintf(f, "window at         : (%ld,%ld)-(%ld,%ld) on %ls%s\n", wr.left, wr.top, wr.right, wr.bottom,
                    mi.szDevice, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
        }
        fprintf(f, "game asked at creation: %ux%u\n", g_createdW, g_createdH);
        fprintf(f, "we want next launch   : %dx%d\n",
                akvr_early_width(), akvr_early_height());
        fprintf(f, "early-res imports patched: %d, intercepted calls: %ld\n",
                akvr_early_patched(), akvr_early_calls());
        fprintf(f, "DXGI 1.1 factory upgrade (SteamVR frame sharing): %s, %ld game call(s) answered\n",
                akvr_early_dxgi11() ? (akvr_early_dxgi11_mode() == 2 ? "on, every call" : (akvr_early_dxgi11_auto() ? "on (auto: SteamVR), game calls only" : "on, game calls only")) : (akvr_early_dxgi11_auto() ? "off (auto: not SteamVR)" : "off (dxgi11=0)"), g_dxgi11Calls);
        fprintf(f, "   callers: %s\n", g_dxgi11Who[0] ? g_dxgi11Who : "none");
        fprintf(f, "SteamVR share probe: %s\n", akvr_xr_share_probe());   // SHAREPROBE
        fprintf(f, "\nviewports set last frame (%d calls):\n", g_rCalls);
        for (int i = 0; i < g_rListN; ++i)
            fprintf(f, "   %d x %d  (x%d)\n", g_rList[i].w, g_rList[i].h, g_rList[i].count);
        // Patch status. vpsquare + projVR are AOB scans that fail SILENTLY (they just
        // return false), so a wanted-but-missing patch used to be indistinguishable from
        // a patch that ran and did nothing. Report both, in the file, every launch.
        {
            float pvRatio = 0.0f, pvFov = 0.0f; int pvHits = 0;
            akvr_projvr_diag(pvRatio, pvHits, pvFov);
            fprintf(f, "\npatches:\n");
            fprintf(f, "   build: AKN5C-0.2.2 " __DATE__ " " __TIME__ "\n");
            fprintf(f, "   zoom vignette: %s\n", akvr_xr_vig_diag());
            fprintf(f, "   %s\n", akvr_audiosync_diag());   // AUDIOSYNC
            fprintf(f, "   pause look: %s, %s now, main view through the player camera: %s, head writes into the paused camera: %ld, darken %.0f%%\n",
                    akvr_xr_pause_look() ? "ON" : "off", akvr_xr_pause_live() ? "LIVE" : "not live",
                    akvr_camera_main_view_live() ? "yes" : "no", akvr_camera_pause_writes(), akvr_xr_pause_dim() * 100.0f);
            fprintf(f, "   pause test: %s\n", akvr_camera_main_view_diag());   // PAUSEFOV2
            fprintf(f, "   head aiming: %s now, %ld frames so far | %s\n", akvr_cam_aim_head_now() ? "ON" : "off",
                    akvr_cam_aim_head_frames(), akvr_gamepad_trigger_diag());   // AIMTRIGGER2
            fprintf(f, "   head on the shown camera (dive fix): %s, %ld camera frames so far\n", akvr_cam_exact_on() ? "on" : "off",
                    akvr_cam_exact_writes());   // DIVEFIX
            fprintf(f, "   %s\n", akvr_far_rain_diag());    // FARRAIN
            // PANELTIDY5 2026-10-11: the rain probe, near rain, rain writer and draw-kind lines removed.
            fprintf(f, "   native capture timing: %s Present (comparison test)\n", g_nativeAfterPresent ? "AFTER" : "BEFORE");
            {
                int div = 1; double hz = 0.0; long late = 0, frames = 0;
                akvr_xr_fps_lock_info(div, hz, late, frames);
                if (akvr_xr_fps_lock())
                    fprintf(f, "   steady frame rate: %d wanted, headset %.1f Hz x%d = %.1f fps, late %ld of %ld\n",
                            akvr_xr_fps_lock(), hz, div, hz > 0 ? hz / div : 0.0, late, frames);
                else
                    fprintf(f, "   steady frame rate: off\n");
                fprintf(f, "   refresh-rate extension: %s  %s\n", akvr_xr_refresh_ext() ? "yes" : "no",
                        akvr_xr_refresh_diag());   // FPS72
            }
            {   // AKN 2026-10-07: which XInput the panel chord (both stick clicks) reads
                char pd[600]; WideCharToMultiByte(CP_UTF8, 0, akvr_gamepad_hook_diag(), -1, pd, sizeof(pd), nullptr, nullptr);
                fprintf(f, "   %s\n", pd);
            }
            fprintf(f, "   head-pose delay: %d fixed, %d used now (%s); exact matches so far: %s\n", akvr_xr_pose_delay(),
                    akvr_xr_pose_delay_used(), akvr_xr_pose_matched() ? "EXACT" : "fixed", akvr_xr_pose_delay_hist());
            fprintf(f, "   %s\n", akvr_frameid_diag());
            fprintf(f, "   mode timeline: %s\n", akvr_xr_mode_log());
            fprintf(f, "   projections built (ratio@HFOVxcount): %s\n", akvr_projection_seen());
            fprintf(f, "   full view: %s  (vertical centre shift %.3f NDC)\n", akvr_xr_full_view() ? "ON" : "off", akvr_xr_eye_v_offset());
            fprintf(f, "   %s\n", akvr_geo11conv_diag());
            fprintf(f, "   %s\n   %s\n   %s\n", akvr_geo11_hud_diag(), akvr_hud_steady_diag(),
                    akvr_hud_layers_diag());   // HUDLIVE, HUDSTEADY, PAUSESTUTTER
            fprintf(f, "   %s | this launch %d saved %d k %.4f (headset %.4f) flip %d unit %.6f narrow %d applied %d\n",
                    akvr_geo11_eyeview_diag(), akvr_xr_eye_view() ? 1 : 0, g_eyeViewSaved ? 1 : 0, akvr_xr_eye_k(),
                    akvr_xr_eye_k_head(), akvr_xr_eye_flip() ? 1 : 0, g_eyeUnit, akvr_xr_eye_narrow_ok() ? 1 : 0,
                    akvr_xr_eye_applied() ? 1 : 0);
            fprintf(f, "   %s\n", akvr_hudprobe_diag());
            fprintf(f, "   %s\n", akvr_hudsplit_diag());   // HUDSPLIT
            fprintf(f, "   %s\n   %s\n   %s\n", akvr_hudsplit_layer_diag(), akvr_xr_hud_layer_diag(), akvr_hudsplit_split_diag());   // HUDLAYER
            fprintf(f, "   bottom-fill tilt: %s, %.2f deg down applied to the head pose\n", akvr_xr_tilt_fill() ? "ON" : "off", akvr_xr_tilt_used_deg());
            fprintf(f, "   game camera tilt kept in gameplay: %.2f (0 = level horizon)\n", akvr_head_pitch_keep());
            fprintf(f, "   OFXR Bridge frame generation: %s\n", akvr_xr_ofxr_active() ? "LOADED (mod frame lock paused)" : "not loaded");
            fprintf(f, "   headset images: %s\n", akvr_xr_sbs_active() ? "ONE side-by-side swapchain (SBSONE)" : "one swapchain per eye");
            fprintf(f, "   right stick tilts the view (pitch unlinked from head yaw): %s\n", akvr_head_pitch_unlink() == 2 ? "TIPPED (head turns slide)" : (akvr_head_pitch_unlink() == 1 ? "level horizon" : "off"));
            fprintf(f, "   native eye order: %s\n",
                    akvr_xr_native_swap_eyes() ? "SWAPPED (right half -> left eye)" : "NORMAL (left half -> left eye)");
            fprintf(f, "   projection observations=%llu  gameplay detector=%s\n",
                    (unsigned long long)akvr_projection_observation_count(),
                    g_lastGameplay ? "GAMEPLAY" : "SCREEN");
            fprintf(f, "   vpsquare (16:9 un-constrain): wanted=%d applied=%d on=%d addr=%p%s\n",
                    g_vpSquareWanted ? 1 : 0, g_vpSquareApplied ? 1 : 0,
                    akvr_vp_square_on() ? 1 : 0, (void*)akvr_vp_square_addr(),
                    (g_vpSquareWanted && !akvr_vp_square_on())
                        ? "   <-- WANTED BUT NOT INSTALLED (pattern not found)" : "");
            fprintf(f, "   projVR (projection rewrite):  hooked=%d on=%d  rewrites=%d  game ratio=%.3f  game HFOV=%.1f\n",
                    akvr_projvr_ok() ? 1 : 0, akvr_projvr() ? 1 : 0, pvHits, pvRatio, pvFov);
            {
                // BOTH axes, before and after. `after` is read back out of the matrix,
                // so if it does not equal what we submit to the headset the render and
                // the display disagree â€” which is a stretch, in that exact ratio.
                float oH=0, oV=0, pH=0, pV=0, sf=0, sr=0; int sk=0;
                akvr_projvr_diag2(oH, oV, pH, pV, sk, sf, sr);
                float subH = 0.0f, subV = 0.0f; akvr_xr_submitted_fov_deg(subH, subV);
                fprintf(f, "      game built  H %.1f  V %.1f deg\n", oH, oV);
                fprintf(f, "      hook output H %.1f  V %.1f deg   (render use unverified)\n", pH, pV);
                fprintf(f, "      we submit   H %.1f  V %.1f deg   <- the DISPLAY%s\n",
                        subH, subV,
                        (g_lastGameplay && pV > 1.0f && fabsf(pV - subV) > 2.0f)
                            ? "   <-- differs from hook output; verify world render" : "");
                fprintf(f, "      projections skipped: %d  (widest H %.1f, ratio %.3f)\n",
                        sk, sf, sr);
            }
            fprintf(f, "   %s  (shape %d/1000 wide:tall)\n",
                    akvr_engine_res_diag(), akvr_engine_shape());
            fprintf(f, "   %s\n", akvr_menu_res_diag());
            fprintf(f, "   %s\n", akvr_hud_diag());
            for (int i = 0; i < akvr_hud_piece_count(); ++i)
            {
                int bw = 0, bh = 0; void* v = nullptr;
                const char* nm = akvr_hud_piece_name(i, bw, bh, v);
                int mNow = -1; const int mOrig = akvr_hud_piece_mode(i, mNow);   // MENUFIT: 0 NoScale 1 ShowAll 2 ExactFit 3 NoBorder
                fprintf(f, "      piece %2d %p %dx%d %s %s  (fit mode %d, now %d)\n", i, v, bw, bh,
                        akvr_hud_piece_shrink(i) ? "shrink" : "FULL", nm[0] ? nm : "(name not found)", mOrig, mNow);
            }
            fprintf(f, "   HUD projection aspect: %s, factor %.4f\n",
                akvr_hud_aspect_fix() ? "enabled" : "disabled", akvr_hud_aspect_factor());
            // Yaw folding. The log is written ~6 s in, while JJ is standing still, so
            // the live figure will read ~0 â€” the PEAK is the one that proves it fired.
            fprintf(f, "   spin correction (yaw folding): %s  gain %.2f   %s\n",
                    akvr_xr_yawfold_on() ? "ON" : "OFF (comparison mode)",
                    akvr_xr_yawfold(), akvr_xr_yawfold_diag());
            // THE geo-11 co-existence answer, in one place. If the mapping is missing
            // here while geo-11 is installed, our load killed its Katanga channel and
            // Â§6.1 of GEO11_PLAN (the hook-conflict hunt) is finally justified.
            fprintf(f, "   geo-11 stereo feed: mode=%s  feed=%s\n      %s\n",
                    akvr_xr_native() ? "NATIVE" : "AER",
                    akvr_xr_kat_feed() ? "ON (shared surface)" : "OFF (swapchain)",
                    akvr_xr_katanga_status());
            // WRAP vs HOOK â€” the 2026-08-08 experiment, in one line. HOOK means
            // geo-11 is loaded as geo11.dll and the genuine d3d11.dll is present,
            // which is the ONLY reason VDXR might find the headset this run.
            // "d3d11.dll = <real>" is the proof: under WRAP that path is the game
            // folder. See load_geo11_hooked() and geo11-blocks-inprocess-openxr.
            {
                wchar_t mp[MAX_PATH] = L"(not loaded)";
                if (HMODULE m = GetModuleHandleW(L"d3d11.dll"))
                    GetModuleFileNameW(m, mp, MAX_PATH);
                fprintf(f, "   geo-11 install: %s   d3d11.dll = %S\n",
                        g_geo11Hooked ? "HOOK (alias geo11.dll)"
                                      : (akvr_early_geo11() ? "WRAP (geo-11 is d3d11.dll)"
                                                            : "none"),
                        mp);
            }
        }
        {
            char fovbuf[512]; akvr_xr_fov_report(fovbuf, sizeof(fovbuf));
            fprintf(f, "\nheadset frusta:\n   %s\n", fovbuf);
        }
        fprintf(f, "\n--- startup size log (every size question, in order) ---\n%s\n",
                akvr_early_log());
        fclose(f);
    }

    // ---- FRAMEGRAB -------------------------------------------------------------
    // Save the actual backbuffer to a .bmp next to the log. Every hook we own reports
    // a SIZE; none of them show what the game DREW. Our viewport/RT observers have
    // never once seen AK's scene (see memory ak-renders-invisible-to-our-hooks), so a
    // picture of the finished frame is the only unmediated evidence of where the image
    // sits inside the square buffer and what shape it really is. Downsampled to ~1024
    // so the file stays readable.
    char g_grabDiag[128] = "frame grab: none yet";

    // Save ANY texture as a BMP. Split out from grab_backbuffer 2026-08-08 so we can
    // also photograph geo-11's shared surface: the swapchain only carries the
    // over/under PREVIEW, in which each eye is already squashed 2:1, so it cannot
    // answer "is the render anamorphic?". Each half of the shared surface is
    // full height, so it can. The caller owns the texture; we do not release it.
    // SOFTSWEEP: a full-resolution patch of a texture (no downscale), so fine softening shows. 24-bit BMP.
    void grab_patch(ID3D11Texture2D* tex, const wchar_t* name, UINT x0, UINT y0, UINT w, UINT h)
    {
        if (!tex || !g_device || !g_context) return;
        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        if (d.SampleDesc.Count != 1 || x0 + w > d.Width || y0 + h > d.Height) return;
        D3D11_TEXTURE2D_DESC sd = d; sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr;
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
        D3D11_BOX b{ x0, y0, 0, x0 + w, y0 + h, 1 };
        g_context->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, &b);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_context->Map(st, 0, D3D11_MAP_READ, 0, &m)))
        {
            std::wstring p = settings_path();
            const std::wstring path = p.substr(0, p.find_last_of(L"\\/") + 1) + name;
            const int rowBytes = ((int)(w * 3) + 3) & ~3;
            std::vector<uint8_t> px((size_t)rowBytes * h);
            const bool bgr = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
            for (UINT y = 0; y < h; ++y)
            {
                const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                uint8_t* dst = &px[(size_t)(h - 1 - y) * rowBytes];
                for (UINT x = 0; x < w; ++x)
                {
                    const uint8_t* s4 = src + x * 4;
                    dst[x * 3 + 0] = bgr ? s4[0] : s4[2]; dst[x * 3 + 1] = s4[1]; dst[x * 3 + 2] = bgr ? s4[2] : s4[0];
                }
            }
            g_context->Unmap(st, 0);
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (f)
            {
                BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + (DWORD)px.size();
                ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = (LONG)h; ih.biPlanes = 1; ih.biBitCount = 24;
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f);
            }
        }
        st->Release();
    }
    void grab_texture(ID3D11Texture2D* bb, const wchar_t* name, int maxDim = 1024)
    {
        if (!bb || !g_device || !g_context) return;

        D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);

        // Multisampled backbuffers cannot be copied to staging directly.
        ID3D11Texture2D* src = bb; ID3D11Texture2D* resolved = nullptr;
        if (d.SampleDesc.Count > 1)
        {
            D3D11_TEXTURE2D_DESC rd = d;
            rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
            rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0;
            if (SUCCEEDED(g_device->CreateTexture2D(&rd, nullptr, &resolved)) && resolved)
            { g_context->ResolveSubresource(resolved, 0, bb, 0, d.Format); src = resolved; }
        }

        D3D11_TEXTURE2D_DESC sd = d;
        sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0; sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* stage = nullptr;
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &stage)) || !stage)
        {
            snprintf(g_grabDiag, sizeof(g_grabDiag), "frame grab: staging alloc FAILED (fmt %d)", (int)d.Format);
            if (resolved) resolved->Release(); return;
        }
        g_context->CopyResource(stage, src);

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g_context->Map(stage, 0, D3D11_MAP_READ, 0, &m)))
        {
            snprintf(g_grabDiag, sizeof(g_grabDiag), "frame grab: Map FAILED");
            stage->Release(); if (resolved) resolved->Release(); return;
        }

        const int W = (int)d.Width, H = (int)d.Height;
        int step = 1; while ((W / step) > maxDim || (H / step) > maxDim) ++step;
        const int ow = W / step, oh = H / step;
        const int rowBytes = ((ow * 3) + 3) & ~3;   // BMP rows are 4-byte aligned

        // Both BGRA and RGBA appear on backbuffers; BMP wants BGR.
        const bool rgba = (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                           d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

        std::wstring p = settings_path();
        if (!p.empty()) p = p.substr(0, p.find_last_of(L"\\/")) + L"\\" + name;
        FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"wb");
        if (!f)
        {
            snprintf(g_grabDiag, sizeof(g_grabDiag), "frame grab: file open FAILED");
            g_context->Unmap(stage, 0);
            stage->Release(); if (resolved) resolved->Release(); return;
        }

        const uint32_t pix = (uint32_t)rowBytes * (uint32_t)oh;
        uint8_t hdr[54]{};
        hdr[0] = 'B'; hdr[1] = 'M';
        *(uint32_t*)(hdr + 2)  = 54 + pix;
        *(uint32_t*)(hdr + 10) = 54;
        *(uint32_t*)(hdr + 14) = 40;
        *(int32_t*) (hdr + 18) = ow;
        *(int32_t*) (hdr + 22) = oh;
        *(uint16_t*)(hdr + 26) = 1;
        *(uint16_t*)(hdr + 28) = 24;
        *(uint32_t*)(hdr + 34) = pix;
        fwrite(hdr, 1, sizeof(hdr), f);

        std::string row((size_t)rowBytes, '\0');
        for (int y = oh - 1; y >= 0; --y)          // BMP is bottom-up
        {
            const uint8_t* s = (const uint8_t*)m.pData + (size_t)(y * step) * m.RowPitch;
            for (int x = 0; x < ow; ++x)
            {
                const uint8_t* px = s + (size_t)(x * step) * 4;
                uint8_t b = rgba ? px[2] : px[0];
                uint8_t g = px[1];
                uint8_t r = rgba ? px[0] : px[2];
                row[(size_t)x * 3 + 0] = (char)b;
                row[(size_t)x * 3 + 1] = (char)g;
                row[(size_t)x * 3 + 2] = (char)r;
            }
            fwrite(row.data(), 1, (size_t)rowBytes, f);
        }
        fclose(f);
        g_context->Unmap(stage, 0);
        stage->Release(); if (resolved) resolved->Release();

        snprintf(g_grabDiag, sizeof(g_grabDiag), "frame grab: saved %dx%d (from %dx%d, fmt %d)",
                 ow, oh, W, H, (int)d.Format);
    }

    // PANELTIDY5 2026-10-11: RADARREC (test tools: the radar flight recorder saved on F2) removed.

    void grab_backbuffer(IDXGISwapChain* sc, const wchar_t* name)
    {
        if (!sc || !g_device || !g_context) return;
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        { snprintf(g_grabDiag, sizeof(g_grabDiag), "frame grab: GetBuffer FAILED"); return; }
        grab_texture(bb, name);
        bb->Release();
    }

    // Actively drag the backbuffer to our square target when something has moved it
    // off. Throttled and capped: a game that re-asserts its own size every frame must
    // win rather than have us fight it into a stutter loop.
    void enforce_square(IDXGISwapChain* sc)
    {
        if (!sc) return;

        DXGI_SWAP_CHAIN_DESC d{};
        if (FAILED(sc->GetDesc(&d))) return;
        const UINT w = d.BufferDesc.Width, h = d.BufferDesc.Height;
        g_bbW = w; g_bbH = h;   // publish for the viewport FILL override (render thread)

        // Report even when forcing is OFF â€” "what did the game ASK for" is the whole
        // diagnostic, and switching forcing off must not switch the readout off too.
        if (!g_forceSquare || g_targetRes < 512)
        {
            snprintf(g_resDiag, sizeof(g_resDiag),
                     "res: %ux%u  (forcing OFF)  game asked %ux%u  creation-hook %s",
                     w, h, g_createdW, g_createdH, g_creationHooked ? "ON" : "FAILED");
            return;
        }

        if (w == (UINT)g_targetRes && h == (UINT)g_targetRes)
        {
            snprintf(g_resDiag, sizeof(g_resDiag),
                     "res: OK %ux%u square  (forced at creation; game asked %ux%u)",
                     w, h, g_createdW, g_createdH);
            return;
        }
        // BOTH live routes are now ruled out in-game (2026-07-25):
        //   1. Calling ResizeBuffers ourselves -> DXGI_ERROR_INVALID_CALL (0x887A0001);
        //      the game still holds references to the backbuffer.
        //   2. Resizing the WINDOW -> SetWindowPos succeeded and the client area really
        //      did become 2048x2048, but the backbuffer stayed 2423x1363 and the resize
        //      counter stayed at 0. This game never calls ResizeBuffers at all: the
        //      swapchain size is fixed at creation and it ignores WM_SIZE.
        // So there is nothing to self-heal at runtime â€” the only remaining lever is to
        // intercept swapchain CREATION at startup (hook D3D11CreateDeviceAndSwapChain /
        // IDXGIFactory::CreateSwapChain from our proxy, before the game gets there, and
        // rewrite BufferDesc). Until that exists, just report the state; resizing the
        // window achieved nothing except leaving an oversized window on the desktop.
        snprintf(g_resDiag, sizeof(g_resDiag),
                 "res: %ux%u  want %d  creation-hook %s, intercepted %d (asked %ux%u)",
                 w, h, g_targetRes,
                 g_creationHooked ? "ON" : "FAILED", g_creationSeen, g_createdW, g_createdH);
        g_forceTries = 8;   // stop here; nothing further to try at runtime
    }

    // JJ 2026-09-26: "when the game launches I have to keep clicking its taskbar icon."
    // For the first 20 s of frames, once a second: if our window is not in front, pull
    // it forward. Windows only lets the process that owns the foreground hand focus
    // over, so borrow its input queue for the call (AttachThreadInput). Three checks in
    // a row already in front ends it, and so does the 20 s window, so a deliberate
    // alt-tab later is never fought.
    void claim_focus_at_launch()
    {
        static double s_start = 0.0, s_lastTry = 0.0;
        static int    s_inFront = 0;
        static bool   s_done = false;
        if (!g_hwnd) return;
        const double now = akvr_probe_clock_ms();
        // WINDOWED: a minimized game window loses the controller, so un-minimize it
        // whenever it happens, for the whole session (once a second at most).
        {
            static double s_lastRestore = 0.0;
            if (g_keepFocus && now - s_lastRestore > 1000.0 && IsIconic(g_hwnd))
            {
                s_lastRestore = now;
                ShowWindow(g_hwnd, SW_RESTORE);
            }
        }
        if (s_done) return;
        if (s_start == 0.0) s_start = now;
        if (now - s_start > 20000.0) { s_done = true; return; }
        if (now - s_lastTry < 1000.0) return;
        s_lastTry = now;

        HWND fg = GetForegroundWindow();
        if (fg == g_hwnd) { if (++s_inFront >= 3) s_done = true; return; }
        s_inFront = 0;
        // JJ, DISPMODE: "starting minimized" â€” bringing a minimized window forward
        // leaves it minimized, so restore it first.
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        const DWORD me = GetCurrentThreadId();
        const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
        BringWindowToTop(g_hwnd);
        SetForegroundWindow(g_hwnd);
        SetActiveWindow(g_hwnd);
        if (attached) AttachThreadInput(me, fgThread, FALSE);
    }

    HRESULT __stdcall hkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        akvr_present_probe_begin(g_context);
        claim_focus_at_launch();
        bool deferNativeSubmit = false, submitGameplay = false;
        float submitFov = 0.0f;
        double submitMs = 0.0;
        if (!g_imguiReady)
            g_imguiReady = init_imgui(swapChain);
        else if (!g_rtv)
            create_rtv(swapChain);              // rebuilt after ResizeBuffers

        if (g_imguiReady) enforce_square(swapChain);
        resync_client_size();    // guide 14 s4: retry until the window IS the size
        write_startup_diag();    // one-shot transcript to akvr_startup_log.txt

        // Keep the engine-render SHAPE honest: it must be the headset's angular
        // aspect (tan halfH / tan halfV), which we only know once OpenXR has located
        // views. Measure it once per session and persist, so the next launch patches
        // the game with the right shape without JJ ever setting a second number.
        {
            static bool shapeDone = false;
            // Gate on akvr_xr_fov_measured(), NOT on the values looking sane: the
            // placeholders are 0.86/0.86, whose ratio is exactly 1.000, and that
            // sailed through the old sanity check and got SAVED as the shape.
            if (!shapeDone && akvr_xr_fov_measured())
            {
                float hH = 0.0f, hV = 0.0f;
                akvr_xr_eye_half_fov(hH, hV);
                // SAMEDIR 2026-10-08: in engine stereo the picture's shape is that of the lens AKN draws (lens mode 2 or
                // 3), so its pixels stay square; the geo-11 era measure below gave 811-925 depending on the moment
                const int es = akvr_engine_stereo() ? akvr_xr_engine_shape() : 0;
                if (es > 0 || (hH > 0.1f && hV > 0.1f))
                {
                    int s = es > 0 ? es : (int)((tanf(hH) / tanf(hV)) * 1000.0f + 0.5f);
                    if (s >= 300 && s <= 3000 && s != akvr_engine_shape())
                    { akvr_engine_shape_set(s); settings_save(); }
                    shapeDone = true;
                }
            }
        }

        akvr_menu_res_tick();   // MENURES
        if (akvr_xr_native())
        {
            // EYEVIEW: geo-11's turn S is sized from the HEADSET's k (the alignment slider only
            // moves the submitted frustum, to match whatever geo-11 really does).
            // EVGAME: gameplay only - flat 2D (menus, loading, pause, map) stays unshifted in geo-11.
            akvr_geo11_eyeview(akvr_xr_eye_view() && akvr_xr_eye_narrow_ok() && akvr_xr_eye_gameplay(),
                               akvr_xr_eye_k_head() / g_eyeUnit);
            akvr_geo11_swapchain(swapChain); // HUDLIVE: geo-11's stereo object hangs off it
            akvr_geo11conv_tick(g_device);   // GEOSCALE
            akvr_xr_eye_applied_set(akvr_geo11_eyeview_applied());
        }
        akvr_hudsplit_tick();            // HUDSPLIT

        // Framegrab. Taken here â€” before the ImGui panel is drawn into the backbuffer â€”
        // so the picture is the game's frame and nothing of ours. One automatic grab
        // early (whatever is on screen at startup) plus F2 on demand during gameplay.
        if (g_imguiReady)
        {
            static ULONGLONG grabFirst = 0; static bool autoGrabbed = false;
            ULONGLONG gnow = GetTickCount64();
            if (!grabFirst) grabFirst = gnow;
            if (!autoGrabbed && gnow - grabFirst > 6000 && g_menuCapture)   // CLEANUP / MENUBLACK: no automatic picture in play
            {
                autoGrabbed = true;   // CAPTUREDIR
                std::wstring p = settings_path();
                if (!p.empty()) CreateDirectoryW((p.substr(0, p.find_last_of(L"\\/") + 1) + L"akvr_captures").c_str(), nullptr);
                grab_backbuffer(swapChain, L"akvr_captures\\frame_early.bmp");
            }
            // PANELTIDY5 2026-10-11: MKREC (20 s of eye pictures for the marker test) removed.
            // HUDRING2 2026-10-13: the HUD tree (visible parts, positions) while the bats play - which part is the bat swarm and which
            // the menus' black backdrop (akvr_captures folder, ringlayers_<n>_<time>.txt), asked for by an A / B press in a menu (gamepad.cpp)
            if (g_ringLayersAt && gnow >= g_ringLayersAt)
            {
                g_ringLayersAt = 0; const LONG n = InterlockedIncrement(&g_ringLayersN);
                std::wstring p = settings_path(); const std::wstring base = p.empty() ? L"" : p.substr(0, p.find_last_of(L"\\/") + 1);
                CreateDirectoryW((base + L"akvr_captures").c_str(), nullptr);
                SYSTEMTIME st{}; GetLocalTime(&st);
                wchar_t name[200]; swprintf_s(name, L"%sakvr_captures\\ringlayers_%02ld_%02u%02u%02u_%03u.txt", base.c_str(), n, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                akvr_hud_layers_dump(name);
            }
            if (g_grabWanted)
            {
                // NUMBERED, not overwritten. Comparing two head positions needs two
                // frames, and a single fixed filename forced JJ and me to hand-shake
                // between every press. Press F2 as many times as you like; they queue up
                // as akvr_frame_01.bmp, _02, ...
                g_grabWanted = false;
                wchar_t name[160];
                swprintf_s(name, L"%sframe.bmp", g_capDir.c_str());   // CAPTUREDIR: inside this F2's folder
                grab_backbuffer(swapChain, name);
                // ...and geo-11's shared surface beside it. The swapchain holds only the
                // over/under preview, whose eyes are already squashed 2:1 â€” useless for
                // judging whether the RENDER is anamorphic. Each half of this one is a
                // full-height eye, so it shows the true shape.
                // EYESHOT 2026-10-10 (JJ: the distance marker "rotated differently in each eye" - no capture showed the HUD as
                // the headset gets it): both eyes with the HUD and marker cards drawn in, side by side
                if (ID3D11Texture2D* et = akvr_xr_eyes_texture())
                {
                    swprintf_s(name, L"%seyes.bmp", g_capDir.c_str());
                    grab_texture(et, name);
                    swprintf_s(name, L"%scards.txt", g_capDir.c_str());
                    if (FILE* cf = _wfopen(name, L"w")) { fputs(akvr_xr_card_diag(), cf); fclose(cf); }
                }
                if (ID3D11Texture2D* kt = akvr_xr_katanga_texture())
                {
                    swprintf_s(name, L"%skatanga.bmp", g_capDir.c_str());
                    grab_texture(kt, name);
                    // The full-resolution copy (~45 MB, added 2026-09-26 for the edge blur) is
                    // gone: JJ 2026-09-28, "that capture takes a long time".
                }
            }
            // CLEANTESTS 2026-10-09: the SWEEP / SOFTSWEEP / BLURAB / COSTSWEEP test runners are gone (answers in
            // akvr/CLAUDE_HANDOFF.md; the overlay buttons went in PANELTIDY3).
            // MENUSTART 2026-10-01 â€” JJ: "still briefly seeing the view of Batman from the top at the bottom of the
            // screen after exiting the title screens and first entering the menu screen" - too brief for F2. When the
            // 3D main menu first comes up, the stereo picture is saved 10 times over 3 s with the headset's display
            // state, then the camera and mode traces, into akvr_captures\menustart_<time>\.
            {
                static int s_ms = -1; static ULONGLONG s_msT0 = 0; static std::wstring s_msDir;
                // MENUFLICKER 2026-10-01 (JJ: "Batman flickers for the first ten to twenty seconds of being in the menu"):
                // after the first 3 s, PAIRS of back-to-back frames (the +1 ms entry = the very next Present) up to 20 s.
                static const int kAt[20] = { 0, 100, 200, 350, 500, 750, 1000, 1500, 2000, 3000,
                                             5000, 5001, 8000, 8001, 12000, 12001, 16000, 16001, 20000, 20001 };
                // CLEANUP 2026-10-01: both questions are answered (RECENTERTIME, RESIZESTOP) - test tools only.
                const unsigned long long t0 = g_menuCapture ? akvr_xr_menu_start_tick() : 0;   // MENUBLACK
                if (t0 && s_ms < 0)
                {
                    s_ms = 0; s_msT0 = t0;
                    std::wstring p = settings_path();
                    const std::wstring base = p.empty() ? L"" : p.substr(0, p.find_last_of(L"\\/") + 1);
                    SYSTEMTIME st{}; GetLocalTime(&st);
                    wchar_t tag[64]; swprintf_s(tag, L"menustart_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                    CreateDirectoryW((base + L"akvr_captures").c_str(), nullptr);
                    s_msDir = std::wstring(L"akvr_captures\\") + tag + L"\\";
                    CreateDirectoryW((base + s_msDir).c_str(), nullptr);
                }
                if (s_ms >= 0 && s_ms < 20 && gnow >= s_msT0 + (ULONGLONG)kAt[s_ms])
                {
                    std::wstring p = settings_path();
                    const std::wstring base = p.empty() ? L"" : p.substr(0, p.find_last_of(L"\\/") + 1);
                    wchar_t name[200];
                    swprintf_s(name, L"%sshot%02d_%04dms.bmp", s_msDir.c_str(), s_ms, (int)(gnow - s_msT0));
                    if (ID3D11Texture2D* kt = akvr_xr_katanga_texture()) grab_texture(kt, name);
                    if (FILE* lf = _wfopen((base + s_msDir + L"shots.txt").c_str(), L"a"))
                    {
                        fprintf(lf, "shot %02d at %4d ms: %s | %s\n", s_ms, (int)(gnow - s_msT0), akvr_xr_display_status(), akvr_xr_mode_log());
                        fclose(lf);
                    }
                    ++s_ms;
                    if (s_ms == 20)
                    {
                        akvr_camera_trace_dump((base + s_msDir + L"camera.csv").c_str());
                        mode_trace_dump((base + s_msDir + L"mode.csv").c_str());
                    }
                }
            }
        }

        // PAUSEEDGE: the pause look is not gameplay for the HUD from its first frame (the smoothed verdict dropped ~0.2 s
        // later and changed the pause menu's layout mid-fade-in)
        akvr_hud_gameplay(g_lastGameplay && !akvr_xr_pause_live());   // don't shrink a screen that IS the UI
        akvr_hud_anamorphic(g_lastAnamorphic);
        // PANELTIDY5 2026-10-11: AIMSHOT (test tools: automatic captures while aiming) removed.
        akvr_hud_tick();         // hold the Scaleform reference resolution at our HUD size
        viewport_frame_tick();   // snapshot the largest scene viewport this frame

        // Load saved settings once, the first frame after ImGui is up. Also write our
        // square target into the game's resolution .ini so the NEXT launch starts
        // square (the game only reads that file at launch); this session's options
        // menu is handled live by the force-square resize hook.
        {
            static bool s_loaded = false;
            // Push our wanted render size into the game's ini for the NEXT launch.
            // (This used to write the old square g_targetRes and silently clobbered
            // whatever resolution had been set up â€” that bug cost us a test cycle.)
            if (g_imguiReady && !s_loaded)
            {
                s_loaded = true;
                settings_load();
                // â›” NOT on the geo-11 build. This line writes OUR square target into
                // the game's own ResX/ResY every launch â€” and a square render destroys
                // geo-11's depth (foreground objects read as flat cards in the wrong
                // plane; see geo11-works-on-ak-fresh-install). It is the reason a
                // hand-set 2560x1440 silently came back as 2880x2880 on the next run,
                // 2026-08-06. In geo-11 mode the game's resolution is the GAME'S, and
                // nothing of ours may touch it.
                if (!akvr_early_geo11())
                    write_resolution(akvr_early_width(), akvr_early_height());
            }
        }

        // Auto-save whenever a tunable changes (slider or hotkey), debounced to
        // once/second so a slider drag doesn't hammer the disk.
        {
            static float lastD = -1e9f, lastL = -1e9f, lastF = -1e9f, lastS = -1e9f;
            static ULONGLONG lastSave = 0; static bool dirty = false;
            float d = akvr_head_stereo(), l = akvr_head_pos_scale(), fv = akvr_head_fov_delta(), sc = akvr_xr_menu_zoom();
            if (d != lastD || l != lastL || fv != lastF || sc != lastS)
            { lastD = d; lastL = l; lastF = fv; lastS = sc; dirty = true; }
            if (dirty && GetTickCount64() - lastSave > 1000) { settings_save(); dirty = false; lastSave = GetTickCount64(); }
        }


        // auto-connect OpenXR once the runtime is up (throttled to every 2s)
        {
            static ULONGLONG lastXrTry = 0;
            if (!akvr_xr_ok() && GetTickCount64() - lastXrTry > 2000)
            {
                lastXrTry = GetTickCount64();
                akvr_xr_try_init();
            }
        }

        // OpenVR presence beacon â€” DISABLED 2026-07-24. It answered its one diagnostic
        // question (native stereo is shelved either way â€” see plan/memory) and is now
        // the prime suspect for a "crashes with SteamVR at launch" regression. No
        // reason to carry that risk for code we no longer need. Left commented (not
        // deleted) in case the beacon result needs re-checking later; openvr_beacon.*
        // and the vendored SDK stay in the tree, just unused.
        // {
        //     static ULONGLONG lastOvrTry = 0;
        //     if (GetTickCount64() - lastOvrTry > 2000)
        //     {
        //         lastOvrTry = GetTickCount64();
        //         akvr_ovr_beacon_try_init();
        //     }
        // }

        process_hotkeys();   // F-keys work every frame, even while the panel is hidden

        // --- one OpenXR frame per Present (M4b) ---
        // begin: sample the single predicted head pose; then the camera injection
        // consumes that exact pose; then submit the game frame to the headset.
        const double beginStart = akvr_probe_clock_ms();
        akvr_xr_frame_begin();
        const double xrBeginMs = akvr_probe_clock_ms() - beginStart;
        akvr_head_set_eye(akvr_xr_inject_eye());   // AER: which eye this frame renders for
        akvr_freecam_update();   // push our commanded camera in, if freecam is on
        akvr_head_update();      // compute head-pose delta for the additive hook (M4a-3)
        akvr_audiosync_frame();  // AUDIOSYNC: a frame came - held start-up sound goes on

        // Render the UI panel into its OWN floating quad layer (clean, independent of
        // the game frame). Fallback: if we're not displaying in VR (e.g. monitor-only
        // testing) draw it onto the game backbuffer so it's still visible on-screen.
        g_panelDrewThisFrame = false;
        if (g_imguiReady && (g_showOverlay || akvr_gamepad_menu()))
        {
            g_frameCount++;
            ImGuiIO& io = ImGui::GetIO();
            bool toVR = akvr_xr_overlay_ready();
            g_panelDrewThisFrame = true;
            g_panelToVR = toVR;

            ImGui_ImplDX11_NewFrame();
            std::unique_lock<std::recursive_mutex> inputLock(g_imguiInputLock); // until NewFrame took the queued input
            ImGui_ImplWin32_NewFrame();
            // feed the real controller as menu navigation (menu mode only); ONESTEP: left/right
            // held back from ImGui while a slider was controller-active last panel frame
            akvr_gamepad_feed_imgui(g_sliderOwnFrame != 0 && g_sliderOwnFrame + 1 >= g_frameCount);
            if (toVR)   // lay the UI out at the panel's own resolution, not the game window's
            {
                uint32_t ow, oh; akvr_xr_overlay_size(ow, oh);
                io.DisplaySize = ImVec2((float)ow, (float)oh);
                map_mouse_to_panel(ImVec2((float)ow, (float)oh));
            }
            io.MouseDrawCursor = toVR;   // PANELMOUSE: the panel draws its own cursor (the Windows one is on the monitor)
            ImGui::NewFrame();
            inputLock.unlock();
            draw_panel();
            ImGui::Render();

            ID3D11RenderTargetView* ovRtv = nullptr;
            if (toVR && akvr_xr_overlay_begin(&ovRtv))
            {
                const float clear[4] = { 0, 0, 0, 0 };   // transparent â†’ game shows through the margins
                g_context->ClearRenderTargetView(ovRtv, clear);
                g_context->OMSetRenderTargets(1, &ovRtv, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                akvr_xr_overlay_end();
            }
            else if (g_rtv)   // monitor fallback
            {
                g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
        }

        // Hand the finished frame (now WITH the overlay) to the headset. "gameplay"
        // = the 3D camera is being finalized this frame (counter advancing) â€” false
        // during splash/loading/menus, which tells the headset layer to float
        // world-fixed instead of head-pinned.
        {
            static uint64_t lastFin = 0; static int stall = 999;
            uint64_t fin = akvr_camera_finalize_count();
            if (fin != lastFin) { stall = 0; lastFin = fin; } else if (stall < 1000) stall++;

            CameraView cam = akvr_camera_read();

            // The finalize counter alone isn't enough. On AK's loading screens the
            // engine keeps ticking the camera for several seconds behind the 2D
            // image, so the counter advances, we still call it "gameplay", and the
            // head-pinned layer drags the static picture around with your head.
            // Second test: the camera VALUES sitting bit-identical. Live play never
            // does that for long â€” even standing still the game applies its own
            // camera sway (the idle micro-jitter noted in M2) â€” but a loading screen
            // freezes them dead. 30 frames ~= 1/3 s at 90 Hz.
            static CameraView prev{}; static int frozen = 0;
            bool same = cam.valid && prev.valid
                     && cam.x == prev.x && cam.y == prev.y && cam.z == prev.z
                     && cam.yaw == prev.yaw && cam.pitch == prev.pitch && cam.roll == prev.roll
                     && cam.fov == prev.fov;
            if (same) { if (frozen < 1000) frozen++; } else frozen = 0;
            prev = cam;

            // ...and that verdict OSCILLATES on a loading screen: the picture snaps to
            // your face and back out. Both halves of that are this one flag (head-pinned
            // vs world-fixed, AND full FOV vs the smaller screen FOV), so a flag that
            // flickers IS the artefact.
            //
            // FIRST ATTEMPT (build LOADFIX) counted FRAMES and did not fix it. JJ then
            // gave the decisive detail: it snaps **once for each individual loading
            // screen image that appears**. That is the tell â€” every new image is a
            // decode HITCH, and during a hitch Present barely runs, so a threshold of
            // "30 frames" can span several seconds while the counters crawl. The tests
            // were measuring the wrong thing.
            //
            // So: measure TIME, not frames. A hitch no longer flatters the counters, and
            // the thresholds mean what they say regardless of frame rate. Hysteresis is
            // in milliseconds too, and deliberately asymmetric â€” drop to the world-fixed
            // screen quickly (it is the comfortable state and being early costs nothing),
            // return to head-pinned gameplay only after a full second of real camera life.
            const ULONGLONG nowMs = GetTickCount64();
            static ULONGLONG lastFinMs  = 0;   // last time the finalize counter advanced
            static ULONGLONG lastMoveMs = 0;   // last time the camera VALUES changed
            static ULONGLONG lastProjMs = 0;   // last time a 3D projection was built
            if (!lastFinMs) { lastFinMs = lastMoveMs = lastProjMs = nowMs; }
            if (stall == 0) lastFinMs  = nowMs;   // stall is reset above when fin moves
            if (!same)      lastMoveMs = nowMs;

            // MEASURED 2026-08-05 (build MODETRACE, akvr_mode_trace.csv). Camera liveness
            // alone cannot tell a loading screen from gameplay â€” AK ticks its camera for
            // FIVE SECONDS behind a loading image, which is the snap JJ kept seeing. Two
            // signals in that trace separate them completely:
            //
            //   genuine gameplay : a 3D projection is built on 100% of frames (884/888
            //                      and 131/131 across two real gameplay stretches)
            //   the false snap   : only 12% of frames (20/163), AND the camera's own FOV
            //                      reads as garbage throughout â€” 21.8, then -18560, then
            //                      2.4e7, 1.9e9, 4.6e11, -1.7e14 degrees. The camera
            //                      object is valid but its contents are being torn down
            //                      and rebuilt, so `cam.valid` is useless here.
            //
            // So: gameplay requires a recently-built projection AND a sane camera, not
            // just a twitching one. Observe projection builds independently of
            // the optional rewrite: projvr=0 must not force permanent screen mode.
            const uint64_t projTotal = akvr_projection_observation_count();
            static uint64_t prevProjTotal = 0;
            const int projHits = (int)(projTotal - prevProjTotal);
            prevProjTotal = projTotal;
            if (projHits > 0) lastProjMs = nowMs;
            // HUDFIT: is the 3D picture squeezed by projVR right now? Pause and the main
            // menu build projections every frame without a camera finalize (so they are
            // not GAMEPLAY), loading screens only on 3-24% of frames. A 10-frame run each
            // way keeps a loading screen from flickering between the two shapes.
            {
                static int projRun = 0, noProjRun = 0;
                if (projHits > 0) { if (projRun < 10000) projRun++; noProjRun = 0; }
                else              { if (noProjRun < 10000) noProjRun++; projRun = 0; }
                if (!g_lastAnamorphic && projRun >= 10) g_lastAnamorphic = true;
                else if (g_lastAnamorphic && noProjRun >= 10) g_lastAnamorphic = false;
                // PAUSEROLL 2026-09-26: this is "a 3D scene is being drawn", NOT "projVR is
                // on". xr.cpp gates its projVR uses with g_projvrOn itself; the pause hang
                // needs the plain signal, or with projvr=0 every pause hung LEVEL and the
                // frozen picture lost the head roll it was drawn with (JJ).
                akvr_xr_set_anamorphic(g_lastAnamorphic);
            }

            // SECOND MEASUREMENT (build LOADPROJ's trace) â€” the thresholds above were far
            // too lenient, and FOV sanity turned out not to discriminate at all. During
            // the load AK reads its camera from a DIFFERENT object: position marches
            // smoothly from -164000 toward 0 while the FOV blows up (1429, -39937,
            // 240289, 3843208, -1.7e14) and passes through perfectly plausible values
            // (94.70, 94.67, 94.81) purely by coincidence. So "is the FOV sane" is a
            // coin toss there. What separates them is CADENCE, and it is absolute:
            //
            //                        proj built   camera finalized   FOV jumps >2deg
            //   real gameplay x2      100%         every frame (0 ms)      0%
            //   the false snap         19%         63 ms median           11%
            //   loading                3-24%       875 ms median          6-16%
            //
            // In real gameplay BOTH happen on EVERY frame, without exception, across
            // 1253 sampled frames. Nothing else in the trace does that. So require a
            // sustained RUN of perfect frames rather than "something happened recently":
            // 20 consecutive is a certainty in gameplay and ~0.19^20 during the snap.
            //
            // Frames are the right unit HERE (unlike the earlier timing bug) because we
            // are measuring per-frame CADENCE, not elapsed duration â€” and cadence stays
            // 1:1 through a hitch, so a stutter cannot fake it.
            // The run counters ARE the hysteresis, so the millisecond layer is gone:
            // 20 consecutive perfect frames to enter gameplay, 10 consecutive bad ones
            // to leave. Asymmetric on purpose â€” the world-fixed screen is the
            // comfortable state, so we are quick to reach it and slow to leave it.
            const bool camSane   = cam.valid && cam.fov > 10.0f && cam.fov < 170.0f;
            const bool frameGood = (projHits > 0) && (stall == 0) && camSane;
            static int goodRun = 0, badRun = 0;
            if (frameGood) { if (goodRun < 10000) goodRun++; badRun = 0; }
            else           { if (badRun  < 10000) badRun++;  goodRun = 0; }

            static bool verdict = true;
            // ENTRYFIX 2026-09-26 â€” JJ: entering the game it is "briefly a small window
            // and then it pops out". 20 perfect frames = ~0.45 s at the 45 fps lock shown
            // as a floating screen. 10 still cannot happen by chance in a load (measured
            // 19% perfect frames there: 0.19^10 = 6e-8), and halves the window.
            if (!verdict && goodRun >= 10) verdict = true;
            else if (verdict && badRun >= 10) verdict = false;
            const bool raw = frameGood;          // recorded in the trace for the next pass
            bool gameplay = verdict;
            akvr_xr_set_screen_hold(!verdict && goodRun > 0);   // ENTRYHOLD (xr.cpp)
            akvr_xr_set_camera_pos(camSane, cam.x, cam.y, cam.z);   // main-menu recognition (xr.cpp)
            // CLEANUP 2026-10-01: its search copies the game's data three times (a hitch ~3 s into play) - only when
            // the automatic pose pairing that uses it is on (poseauto=1; off on JJ's game).
            if (akvr_xr_pose_auto()) akvr_frameid_tick(gameplay, akvr_camera_finalize_count());   // FRAMEID discovery
            g_lastGameplay = gameplay;           // the HUD tick reads this next frame

            // Record the decision and its inputs (see ModeRec). Projection observations
            // are the activity signal: a 3D scene being drawn
            // produces BuildProjectionMatrix calls continuously, a 2D loading image
            // should produce none.
            mode_record((uint32_t)(nowMs - lastFinMs), (uint32_t)(nowMs - lastMoveMs),
                        raw, verdict, cam, projHits);

            // PANELTIDY5 2026-10-11: the scene TAP (fed only by the test-tools render-target observer) removed.
            submitGameplay = gameplay;
            submitFov = cam.valid ? cam.fov : 0.0f;
            deferNativeSubmit = g_nativeAfterPresent && akvr_xr_native()
                && akvr_xr_kat_feed() && akvr_xr_katanga_texture();
            if (!deferNativeSubmit)
            {
                const double start = akvr_probe_clock_ms();
                akvr_xr_frame_submit(swapChain, submitFov, submitGameplay);
                submitMs = akvr_probe_clock_ms() - start;
            }
        }

        // Mirror the panel onto the monitor so it can be screenshotted. Safe here â€”
        // the headset already has its copy of this frame, so painting the backbuffer
        // now can't reach the eye swapchains. This is the LAST use of the draw data
        // this frame, so we scale it DOWN in place: the panel is 1440 px, and painting
        // it 1:1 makes it huge next to the game (reported 2026-07-26). We shrink it to
        // ~half the monitor height in the top-left corner. Destructive scaling is fine â€”
        // next frame's NewFrame rebuilds the draw data from scratch.
        if (g_panelDrewThisFrame && g_panelToVR && g_rtv)
        {
            ImDrawData* dd = ImGui::GetDrawData();
            if (dd && dd->CmdListsCount > 0 && dd->DisplaySize.y > 1.0f)
            {
                DXGI_SWAP_CHAIN_DESC scd{};
                float bbH = (SUCCEEDED(swapChain->GetDesc(&scd)) && scd.BufferDesc.Height)
                          ? (float)scd.BufferDesc.Height : dd->DisplaySize.y;
                // Target â‰ˆ half the REAL monitor height. With the resolution boost the
                // backbuffer (and often the game window) is g_resScale bigger than the
                // monitor, so divide it back out â€” otherwise the panel scales up with the
                // render and can run off the visible (clipped) desktop, unscreenshottable.
                float ref = bbH * 0.5f / (g_resScale > 1.0f ? g_resScale : 1.0f);
                float k = ref / dd->DisplaySize.y;
                if (k > 0.05f && k < 1.0f)
                {
                    for (int i = 0; i < dd->CmdListsCount; ++i)
                    {
                        ImDrawList* dl = dd->CmdLists[i];
                        for (int v = 0; v < dl->VtxBuffer.Size; ++v)
                        { dl->VtxBuffer[v].pos.x *= k; dl->VtxBuffer[v].pos.y *= k; }
                        for (int c = 0; c < dl->CmdBuffer.Size; ++c)
                        { ImVec4& cr = dl->CmdBuffer[c].ClipRect; cr.x *= k; cr.y *= k; cr.z *= k; cr.w *= k; }
                    }
                    dd->DisplaySize.x *= k; dd->DisplaySize.y *= k;
                }
            }
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(dd);
        }

        // The headset image is pinned to your head each frame; for the world to
        // stay put (not drag along) the game camera must counter-rotate by the
        // same head motion â€” that's the head-track injection. They MUST be on
        // together, so auto-enable head-tracking the moment we start displaying.
        // Runs once; F11 still toggles it manually afterwards.
        {
            static bool s_autoHead = false;
            if (!s_autoHead && akvr_xr_displaying())
            {
                s_autoHead = true;
                if (!akvr_head_state().on) akvr_head_toggle();
            }

            // The auto-recenter above can grab the neutral pose before the Quest's
            // 6DOF position tracking has settled (position still 0) â€” which then
            // reads as a huge phantom "lean" once tracking goes live. Re-capture
            // the zero-point the first frame position becomes valid.
            // ...and "valid" latches on the FIRST frame position reports good, which is
            // too early â€” the runtime can hand back a floor-referenced or not-yet-settled
            // position for a moment, and we'd bake that in as the zero point. Seen
            // in-game 2026-07-25 as a permanent lean of y +1.00 m (i.e. we'd zeroed at
            // the floor and were reading true standing height as lean, pushing the camera
            // ~1 m up). Wait for position to have been valid for a sustained stretch
            // before taking the reference.
            // RECENTERTIME 2026-10-01 â€” JJ: entering the main menu showed "the view of Batman from the top at the bottom
            // of the screen" until the camera "calibrates". MENUSTART capture: the camera was 118 units (lean_y 1.157 m,
            // the head's height above the tracking origin) above the menu camera until 6.07 s, then dropped to 0: the
            // reference was taken after 120 FRAMES, and the start-up / menu-load screens present so few frames that
            // this took ~6 s, into the menu. Now: 1 s of steady tracking by the clock.
            static bool      s_posRecenter = false;
            static ULONGLONG s_posValidSince = 0;
            if (s_autoHead && !s_posRecenter)
            {
                const ULONGLONG tnow = GetTickCount64();
                if (!akvr_xr_head_pos_valid()) s_posValidSince = 0;
                else if (!s_posValidSince)     s_posValidSince = tnow;
                if (s_posValidSince && tnow - s_posValidSince >= 1000)
                {
                    s_posRecenter = true;
                    akvr_head_recenter();
                }
            }
        }
        // No monitor v-sync in VR â€” the headset's compositor is what paces us, and
        // waiting on the desktop's refresh on top of that only adds latency and
        // judder. This is the belt to write_comfort_settings()'s braces: the ini can
        // be reset from the in-game menu, this cannot.
        // Test whether downstream Present produces the Katanga pair that should
        // be submitted now. Sparse before/after samples record the actual boundary.
        ID3D11Texture2D* probeSource = nullptr;   // PANELTIDY5 2026-10-11: the test-tools pixel read-back removed (the timing ring stays)
        akvr_present_probe_before(g_device, g_context, probeSource,
            akvr_camera_finalize_count(), submitGameplay, deferNativeSubmit, xrBeginMs, akvr_xr_wait_ms(),
            akvr_xr_pose_delay_used());
        const double presentStart = akvr_probe_clock_ms();
        const HRESULT result = oPresent(swapChain, 0, flags);
        const double presentMs = akvr_probe_clock_ms() - presentStart;
        if (deferNativeSubmit)
        {
            const double start = akvr_probe_clock_ms();
            akvr_xr_frame_submit(swapChain, submitFov, submitGameplay);
            submitMs = akvr_probe_clock_ms() - start;
        }
        akvr_present_probe_after(g_context, probeSource, presentMs, submitMs);
        return result;
    }

    HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
                                      UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        // The game is resizing the backbuffer â€” drop our view first,
        // recreate lazily on the next Present.
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_resizeCalls++;   // diagnostic: does this game EVER resize? (panel shows it)

        // FORCE SQUARE. The game's video-options menu resizes the backbuffer to a
        // 16:9 monitor mode, which letterboxes our VR view. The window doesn't drive
        // the render on this game, so we override the size the game asks DXGI for â€”
        // whatever it requests, the backbuffer becomes g_targetRes Ã— g_targetRes.
        // (Standard forced-resolution technique; UE3 sizes its scene to the backbuffer.)
        if (g_forceSquare && g_targetRes >= 512)
        { width = (UINT)g_targetRes; height = (UINT)g_targetRes; }
        // WINDOWED 2026-09-26: a window bigger than the monitor can be clamped, and the
        // game may resize its buffer to that clamped window (or pass 0,0 = "window
        // size"). A buffer smaller than the engine render is the left-part-only,
        // shearing picture. While the size is forced, the buffer IS the forced size.
        else if (g_forceRes && akvr_early_width() >= 512 && akvr_early_height() >= 512)
        { width = (UINT)akvr_early_width(); height = (UINT)akvr_early_height(); }

        akvr_xr_notify_resize();   // rebuild the headset eye swapchains at the new size (else black after the menu)
        return oResizeBuffers(swapChain, bufferCount, width, height, format, flags);
    }

    // Three ways a DX11 game can end up with a swapchain; cover all of them rather
    // than assume. Batman imports dxgi.dll directly, so CreateSwapChain is the likely
    // path, but the cost of covering the others is a few lines.
    using CreateSwapChainFn        = HRESULT(__stdcall*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using CreateSwapChainForHwndFn = HRESULT(__stdcall*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    using D3D11CreateDASCFn        = HRESULT(__stdcall*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                                         UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                                         D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    CreateSwapChainFn        oCreateSwapChain        = nullptr;
    CreateSwapChainForHwndFn oCreateSwapChainForHwnd = nullptr;
    D3D11CreateDASCFn        oD3D11CreateDASC        = nullptr;

    // ---- Present without kiero (the geo-11 path) ----------------------------
    // WHY THIS EXISTS, 2026-08-06: with geo-11 installed the game would not launch
    // at all â€” no window, no splash. geo-11's own log tells the story: kiero's
    // startup probe creates a THROWAWAY 100x100 device+swapchain purely to read the
    // Present vtable, that call goes through geo-11's wrapper, and geo-11 fails it
    // (`Unexpected call back into IDXGIFactory::CreateSwapChain` -> E_INVALIDARG).
    // The game never got as far as creating its own device. A stereo injector that
    // owns d3d11.dll and dxgi.dll cannot be probed with a fake device.
    //
    // We do not need one. We already sit on every creation path, so we can take the
    // vtable from the REAL swapchain the moment it is born and detour Present there.
    // That also puts us AFTER geo-11 in the chain, which is where we want to be: by
    // the time our Present runs, geo-11 has drawn both eyes and published them.
    bool g_scHooked = false;

    // STAY WINDOWED â€” 2026-09-26 (build WINDOWED). Forcing Windowed at creation is not
    // enough if the game later calls SetFullscreenState(TRUE): DXGI then switches the
    // GAME MONITOR to the render size (the "display mode ... from dxgi.dll" log line),
    // which only works if the monitor offers that mode â€” so a plain 2K monitor could
    // never render above 2K â€” and a fullscreen window minimizes when focus leaves it,
    // which is the "starts minimized, controller dead" report. Say yes, stay windowed.
    using SetFullscreenStateFn = HRESULT(__stdcall*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
    SetFullscreenStateFn oSetFullscreenState = nullptr;
    // What the game believes. WINDOWED showed it asking again and again, because
    // GetFullscreenState kept telling it the request had not taken â€” and a game that
    // thinks it is fullscreen-but-failed also behaves like a fullscreen game that
    // lost focus (minimizes). Report what it asked for; the swapchain stays windowed.
    bool g_fsBelieved = false;
    HRESULT __stdcall hkSetFullscreenState(IDXGISwapChain* sc, BOOL fullscreen, IDXGIOutput* target)
    {
        if (g_forceWindowed)
        {
            static int s_refused = 0;
            if (fullscreen && ++s_refused <= 3) note("fullscreen request #%d refused - staying windowed", s_refused);
            g_fsBelieved = fullscreen != FALSE;
            if (fullscreen) return S_OK;
        }
        return oSetFullscreenState ? oSetFullscreenState(sc, fullscreen, target) : E_FAIL;
    }
    using GetFullscreenStateFn = HRESULT(__stdcall*)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
    GetFullscreenStateFn oGetFullscreenState = nullptr;
    HRESULT __stdcall hkGetFullscreenState(IDXGISwapChain* sc, BOOL* fullscreen, IDXGIOutput** target)
    {
        HRESULT hr = oGetFullscreenState ? oGetFullscreenState(sc, fullscreen, target) : E_FAIL;
        if (SUCCEEDED(hr) && g_forceWindowed && g_fsBelieved && fullscreen) *fullscreen = TRUE;
        return hr;
    }

    void hook_present_on(IDXGISwapChain* sc)
    {
        if (sc) { ID3D11Device* rd = nullptr; if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&rd)) && rd) { akvr_hudsplit_watch_device(rd); rd->Release(); } }   // VSID: the real device
        // geo-11 only. The AER build's kiero path is known-good and shipping; this
        // is the alternative for the one case where kiero cannot be used at all.
        if (!akvr_early_geo11() || g_scHooked || !sc || !g_presentHook) return;
        void** vt = *(void***)sc;
        // Same slots kiero binds: IDXGISwapChain::Present is 8, ResizeBuffers 13.
        if (MH_CreateHook(vt[8], (void*)&hkPresent, (void**)&oPresent) != MH_OK) return;
        MH_EnableHook(vt[8]);
        if (MH_CreateHook(vt[13], (void*)&hkResizeBuffers, (void**)&oResizeBuffers) == MH_OK)
            MH_EnableHook(vt[13]);
        // IDXGISwapChain::SetFullscreenState is slot 10.
        if (MH_CreateHook(vt[10], (void*)&hkSetFullscreenState, (void**)&oSetFullscreenState) == MH_OK)
            MH_EnableHook(vt[10]);
        // IDXGISwapChain::GetFullscreenState is slot 11.
        if (MH_CreateHook(vt[11], (void*)&hkGetFullscreenState, (void**)&oGetFullscreenState) == MH_OK)
            MH_EnableHook(vt[11]);
        g_scHooked = true;
        akvr_camera_install();   // was kiero's job; F9 still re-runs it if the scan misses
    }

    HRESULT __stdcall hkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
    {
        if (desc)
        {
            apply_forced_size(desc->BufferDesc.Width, desc->BufferDesc.Height, desc->OutputWindow);
            g_createdWindowed = desc->Windowed != FALSE;
            if (g_forceWindowed) desc->Windowed = TRUE;
        }
        akvr_hudsplit_watch_device(dev);   // VSID: before the game creates its shaders
        HRESULT hr = oCreateSwapChain(f, dev, desc, out);
        if (SUCCEEDED(hr) && out && *out) hook_present_on(*out);
        // Alt+Enter is DXGI's own fullscreen switch; stay windowed (see hkSetFullscreenState).
        if (SUCCEEDED(hr) && g_forceWindowed && f && desc && desc->OutputWindow)
            f->MakeWindowAssociation(desc->OutputWindow, DXGI_MWA_NO_ALT_ENTER);
        return hr;
    }

    HRESULT __stdcall hkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* ro, IDXGISwapChain1** out)
    {
        DXGI_SWAP_CHAIN_DESC1 local{};
        const DXGI_SWAP_CHAIN_DESC1* use = desc;
        if (desc) { local = *desc; apply_forced_size(local.Width, local.Height, hwnd); use = &local; }
        // A null fullscreen-desc means windowed, which is the escape hatch we want.
        if (g_forceWindowed && fs) { g_createdWindowed = fs->Windowed != FALSE; fs = nullptr; }
        akvr_hudsplit_watch_device(dev);   // VSID
        HRESULT hr = oCreateSwapChainForHwnd(f, dev, hwnd, use, fs, ro, out);
        if (SUCCEEDED(hr) && out && *out) hook_present_on(*out);
        return hr;
    }

    HRESULT __stdcall hkD3D11CreateDASC(IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl,
                                        UINT nfl, UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** outSc,
                                        ID3D11Device** outDev, D3D_FEATURE_LEVEL* outFl, ID3D11DeviceContext** outCtx)
    {
        DXGI_SWAP_CHAIN_DESC local{};
        const DXGI_SWAP_CHAIN_DESC* use = desc;
        if (desc)
        {
            local = *desc;
            apply_forced_size(local.BufferDesc.Width, local.BufferDesc.Height, desc->OutputWindow);
            g_createdWindowed = local.Windowed != FALSE;
            if (g_forceWindowed) local.Windowed = TRUE;
            use = &local;
        }
        HRESULT hr = oD3D11CreateDASC(ad, dt, sw, flags, fl, nfl, sdk, use, outSc, outDev, outFl, outCtx);
        if (SUCCEEDED(hr) && outDev && *outDev) akvr_hudsplit_watch_device(*outDev);   // VSID
        if (SUCCEEDED(hr) && outSc && *outSc) hook_present_on(*outSc);
        return hr;
    }

    // Install before the game reaches its own creation call. We're a version.dll proxy,
    // so we run first â€” but dxgi/d3d11 may not be loaded yet, hence the explicit
    // LoadLibrary. Grabbing a throwaway factory just to read its vtable is the standard
    // way in: the vtable is per-implementation, not per-instance, so patching that slot
    // catches the factory the game later makes for itself.
    // ---- DXGI display information -------------------------------------------
    // MEASURED 2026-07-27: with the work area faked to 5888x3397 and the game's ini
    // asking for 2880x2880, the render still came out 3840x2160 â€” neither the claim
    // nor the request. 3840x2160 is a real display's resolution, so the size is coming
    // from the one screen-information channel we do NOT intercept: DXGI. user32 lies
    // don't reach it. So: hook IDXGIOutput and (a) report the same enlarged desktop we
    // report through user32, (b) advertise our wanted render size as an available
    // display mode, since a mode-list snap would explain a 4K ceiling exactly.
    // Everything is logged into the same startup transcript â€” if these lines never
    // appear, DXGI isn't the source either and the log says so.
    using OutGetDescFn   = HRESULT(__stdcall*)(IDXGIOutput*, DXGI_OUTPUT_DESC*);
    using OutModeListFn  = HRESULT(__stdcall*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
    using OutClosestFn   = HRESULT(__stdcall*)(IDXGIOutput*, const DXGI_MODE_DESC*, DXGI_MODE_DESC*, IUnknown*);
    OutGetDescFn  oOutGetDesc  = nullptr;
    OutModeListFn oOutModeList = nullptr;
    OutClosestFn  oOutClosest  = nullptr;
    const UINT    kExtraModes  = 2;   // our exact size, plus a 16:9 one at the same height

    void note(const char* fmt, ...)
    {
        char b[192];
        va_list ap; va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        akvr_early_note(b);
    }

    HRESULT __stdcall hkOutGetDesc(IDXGIOutput* self, DXGI_OUTPUT_DESC* d)
    {
        HRESULT hr = oOutGetDesc(self, d);
        if (SUCCEEDED(hr) && d)
        {
            RECT& r = d->DesktopCoordinates;
            int fw = 0, fh = 0; akvr_early_fake_screen(fw, fh);
            note("DXGI Output.GetDesc real=%dx%d -> %dx%d",
                 (int)(r.right - r.left), (int)(r.bottom - r.top), fw, fh);
            if (akvr_early_enabled() && fw && fh)
            { r.left = 0; r.top = 0; r.right = fw; r.bottom = fh; }
        }
        return hr;
    }

    // Two-call API: pDesc == null asks only for the count. We add exactly one mode
    // (our target) at the end, which keeps the list's ascending order intact because
    // ours is the largest.
    HRESULT __stdcall hkOutModeList(IDXGIOutput* self, DXGI_FORMAT fmt, UINT flags,
                                    UINT* pNum, DXGI_MODE_DESC* pDesc)
    {
        if (!pNum || !akvr_early_enabled())
            return oOutModeList(self, fmt, flags, pNum, pDesc);

        // DXGIMODES 2026-09-26 (geo-11): only ADD our exact render size; replacing the
        // monitor's biggest mode with a 16:9 one is the AER-era experiment, and under
        // geo-11 a made-up 16:9 mode is just another size the game might pick.
        const bool exactOnly = akvr_early_geo11();
        const UINT extra = exactOnly ? 1u : kExtraModes;
        if (!pDesc)
        {
            UINT n = 0;
            HRESULT hr = oOutModeList(self, fmt, flags, &n, nullptr);
            if (SUCCEEDED(hr))
            { note("DXGI ModeList count %u -> %u", n, n + extra); *pNum = n + extra; }
            return hr;
        }

        UINT cap = *pNum;
        UINT n   = (cap > extra) ? cap - extra : 0;
        HRESULT hr = oOutModeList(self, fmt, flags, &n, pDesc);
        if (SUCCEEDED(hr) && exactOnly && cap >= n + 1 && n > 0)
        {
            UINT biggest = 0; long long bestA = 0;
            for (UINT i = 0; i < n; ++i)
            {
                long long a = (long long)pDesc[i].Width * pDesc[i].Height;
                if (a > bestA) { bestA = a; biggest = i; }
            }
            DXGI_MODE_DESC mine = pDesc[biggest];       // inherit refresh rate + flags
            mine.Width = (UINT)akvr_early_width(); mine.Height = (UINT)akvr_early_height();
            pDesc[n] = mine;
            *pNum = n + 1;
            note("DXGI ModeList: added %ux%u @%u/%u Hz", mine.Width, mine.Height,
                 mine.RefreshRate.Numerator, mine.RefreshRate.Denominator);
        }
        else if (SUCCEEDED(hr) && cap >= n + kExtraModes && n > 0)
        {
            // The engine picks the biggest 16:9 mode the driver offers that fits the (faked)
            // work area. On a 4K panel run at 1440p that is a genuine 3840x2160 entry â€” not
            // DSR, just the screen's real top mode. Adding modes alongside it was ignored,
            // so REMOVE its favourite: overwrite the biggest real entry with our target,
            // inheriting that entry's refresh rate and flags (a mode advertised at a refresh
            // rate the display never runs at is a plausible reason ours got skipped).
            //
            // Nothing of value is lost â€” the driver offers no custom/DSR mode here to
            // protect (NVIDIA's DSR and custom-resolution pages are both unavailable on
            // this machine), and the desktop is untouched because the game runs windowed.
            //
            // Decisive either way: if the render follows, the mode list is the lever. If the
            // game still produces 3840x2160 with that mode gone from the list, it is not
            // reading its size from here at all and the remaining explanation is internal to
            // the engine â€” a disassembly job.
            UINT biggest = 0; long long bestA = 0;
            for (UINT i = 0; i < n; ++i)
            {
                long long a = (long long)pDesc[i].Width * pDesc[i].Height;
                if (a > bestA) { bestA = a; biggest = i; }
            }
            UINT oldW = pDesc[biggest].Width, oldH = pDesc[biggest].Height;
            UINT h    = (UINT)akvr_early_height();

            DXGI_MODE_DESC tmpl = pDesc[biggest];        // inherit refresh rate + flags
            tmpl.Width = (h * 16) / 9; tmpl.Height = h;  // 16:9 â€” the shape it insists on
            pDesc[biggest] = tmpl;

            DXGI_MODE_DESC sq = tmpl;                    // and the square, as an extra
            sq.Width = (UINT)akvr_early_width(); sq.Height = h;
            pDesc[n]     = sq;
            pDesc[n + 1] = tmpl;
            *pNum = n + kExtraModes;
            note("DXGI ModeList: replaced biggest %ux%u with %ux%u (+square %ux%u) @%u/%u Hz",
                 oldW, oldH, tmpl.Width, tmpl.Height, sq.Width, sq.Height,
                 tmpl.RefreshRate.Numerator, tmpl.RefreshRate.Denominator);
        }
        else *pNum = n;
        return hr;
    }

    HRESULT __stdcall hkOutClosest(IDXGIOutput* self, const DXGI_MODE_DESC* want,
                                   DXGI_MODE_DESC* got, IUnknown* dev)
    {
        HRESULT hr = oOutClosest(self, want, got, dev);
        if (want && got)
        {
            note("DXGI ClosestMode asked %ux%u -> %ux%u",
                 want->Width, want->Height, got->Width, got->Height);
            // DXGIMODES: our exact render size is the closest match to itself (the
            // driver would round 3072x2688 to a real monitor mode, e.g. 2560x1600).
            if (SUCCEEDED(hr) && akvr_early_enabled() &&
                want->Width == (UINT)akvr_early_width() && want->Height == (UINT)akvr_early_height())
            {
                got->Width = want->Width; got->Height = want->Height;
                note("DXGI ClosestMode: kept %ux%u", got->Width, got->Height);
            }
        }
        return hr;
    }

    void install_output_hooks(IDXGIFactory* fac)
    {
        if (!fac) return;
        IDXGIAdapter* ad = nullptr;
        if (FAILED(fac->EnumAdapters(0, &ad)) || !ad) return;
        IDXGIOutput* out = nullptr;
        if (SUCCEEDED(ad->EnumOutputs(0, &out)) && out)
        {
            // IDXGIOutput vtable: IUnknown 0-2, IDXGIObject 3-6, then GetDesc 7,
            // GetDisplayModeList 8, FindClosestMatchingMode 9. All outputs share it.
            void** vt = *(void***)out;
            if (MH_CreateHook(vt[7], (void*)&hkOutGetDesc,  (void**)&oOutGetDesc)  == MH_OK) MH_EnableHook(vt[7]);
            if (MH_CreateHook(vt[8], (void*)&hkOutModeList, (void**)&oOutModeList) == MH_OK) MH_EnableHook(vt[8]);
            if (MH_CreateHook(vt[9], (void*)&hkOutClosest,  (void**)&oOutClosest)  == MH_OK) MH_EnableHook(vt[9]);
            note("DXGI output hooks installed");
            out->Release();
        }
        else note("DXGI: no output 0 to hook");
        ad->Release();
    }

    // DESKTOP GUARD â€” 2026-09-26. After the first run that really rendered 3840x2160
    // (build ENGINE169) JJ saw Windows rescale, and the desktop was left at 3840x2160
    // after the game quit (it had been 2560x1440). BatmanAK.exe does not import any
    // ChangeDisplaySettings* function, so some other module in the process did it.
    // Hook the user32 functions themselves (every caller goes through them) and log
    // WHO asked and for what. Logging only - see note_mode_change for why.
    using CDSExWFn = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
    using CDSExAFn = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
    CDSExWFn oCDSExW = nullptr;
    CDSExAFn oCDSExA = nullptr;

    // â›” LOG ONLY â€” 2026-09-26, build GUARDLOG. Refusing broke the picture: the call
    // is dxgi.dll switching the GAME monitor to the render size for the session
    // (CDS_FULLSCREEN, flags 0x4 â€” temporary, undone at exit). Refused, the game got
    // a 2560x1440 buffer while rendering 3840x2160: left part only, shear on head
    // motion. The "desktop left at 4K" that prompted the guard was a false alarm
    // (JJ's second monitor is native 4K). Never block this call.
    template <class DM>
    void note_mode_change(const char* fn, const DM* dm, DWORD flags, void* ret)
    {
        char mod[MAX_PATH] = "?";
        HMODULE caller = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &caller) && caller)
        {
            GetModuleFileNameA(caller, mod, MAX_PATH);
        }
        const char* base = strrchr(mod, '\\');
        char line[400];
        snprintf(line, sizeof(line), "display mode: %s %ux%u flags 0x%X from %s",
                 fn, dm ? (unsigned)dm->dmPelsWidth : 0u, dm ? (unsigned)dm->dmPelsHeight : 0u,
                 (unsigned)flags, base ? base + 1 : mod);
        akvr_early_note(line);
    }

    LONG WINAPI hkCDSExW(LPCWSTR dev, DEVMODEW* dm, HWND h, DWORD flags, LPVOID p)
    {
        if (dm) note_mode_change("ChangeDisplaySettingsExW", dm, flags, _ReturnAddress());
        return oCDSExW ? oCDSExW(dev, dm, h, flags, p) : DISP_CHANGE_FAILED;
    }

    LONG WINAPI hkCDSExA(LPCSTR dev, DEVMODEA* dm, HWND h, DWORD flags, LPVOID p)
    {
        if (dm) note_mode_change("ChangeDisplaySettingsExA", dm, flags, _ReturnAddress());
        return oCDSExA ? oCDSExA(dev, dm, h, flags, p) : DISP_CHANGE_FAILED;
    }

    using CDSWFn = LONG(WINAPI*)(DEVMODEW*, DWORD);
    using CDSAFn = LONG(WINAPI*)(DEVMODEA*, DWORD);
    CDSWFn oCDSW = nullptr;
    CDSAFn oCDSA = nullptr;
    LONG WINAPI hkCDSW(DEVMODEW* dm, DWORD flags)
    {
        if (dm) note_mode_change("ChangeDisplaySettingsW", dm, flags, _ReturnAddress());
        return oCDSW ? oCDSW(dm, flags) : DISP_CHANGE_FAILED;
    }
    LONG WINAPI hkCDSA(DEVMODEA* dm, DWORD flags)
    {
        if (dm) note_mode_change("ChangeDisplaySettingsA", dm, flags, _ReturnAddress());
        return oCDSA ? oCDSA(dm, flags) : DISP_CHANGE_FAILED;
    }

    void install_desktop_guard()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (!u32) return;
        MH_Initialize();
        void* w = (void*)GetProcAddress(u32, "ChangeDisplaySettingsExW");
        void* a = (void*)GetProcAddress(u32, "ChangeDisplaySettingsExA");
        int ok = 0;
        if (w && MH_CreateHook(w, (void*)&hkCDSExW, (void**)&oCDSExW) == MH_OK && MH_EnableHook(w) == MH_OK) ++ok;
        if (a && MH_CreateHook(a, (void*)&hkCDSExA, (void**)&oCDSExA) == MH_OK && MH_EnableHook(a) == MH_OK) ++ok;
        void* w2 = (void*)GetProcAddress(u32, "ChangeDisplaySettingsW");
        void* a2 = (void*)GetProcAddress(u32, "ChangeDisplaySettingsA");
        if (w2 && MH_CreateHook(w2, (void*)&hkCDSW, (void**)&oCDSW) == MH_OK && MH_EnableHook(w2) == MH_OK) ++ok;
        if (a2 && MH_CreateHook(a2, (void*)&hkCDSA, (void**)&oCDSA) == MH_OK && MH_EnableHook(a2) == MH_OK) ++ok;
        char line[96];
        snprintf(line, sizeof(line), "desktop guard: %d of 4 mode-change functions hooked", ok);
        akvr_early_note(line);
    }

    // DXGI11 2026-10-06 - JJ: through SteamVR "Batman didn't" reach the headset (Battlefront II did). SteamVR's
    // xrclient_BatmanAK.txt: every projection view "failed to submit ... VRCompositorError_SharedTexturesNotSupported"
    // (3814 times; 9008 in the Aug 7 run) after "Failed to create sync texture. Ensure application was built using
    // DXGI 1.1 or later (i.e. Call CreateDXGIFactory1)". Arkham Knight imports the DXGI 1.0 CreateDXGIFactory; a device
    // from a 1.0 factory chain cannot make the keyed-mutex texture SteamVR shares frames with. Same failure and fix as
    // BioShock Infinite VR (references/bioshock-vr-latest/docs/STATUS.md, "install_dxgi11_upgrade"): answer the game's
    // CreateDXGIFactory with CreateDXGIFactory1. Virtual Desktop's runtime never needed it. Setting dxgi11=0 turns it off.
    using CreateFactoryFn = HRESULT(__stdcall*)(REFIID, void**);
    CreateFactoryFn oCreateDXGIFactory = nullptr, g_realCreateFactory1 = nullptr;
    // DXGI11c 2026-10-07: g_realCreateFactory1 is now OUR MinHook trampoline of CreateDXGIFactory1 (Microsoft's own code),
    // not the export address. Through the export, the call went into geo-11's CreateDXGIFactory1 hook as well as its
    // CreateDXGIFactory one: geo-11 then never attached (katanga mapping missing, "wrapper never seen") and the eyes
    // were the plain frame cut in half - JJ: "way too close to my face, and the eyes were too separated".
    HRESULT __stdcall hkCreateDXGIFactory1Pass(REFIID riid, void** pp) { return g_realCreateFactory1(riid, pp); }
    // DXGI11d 2026-10-07: with every call upgraded geo-11 never attached (baseline with dxgi11=0: attached, katanga open;
    // with it on: 2 calls answered, no katanga). Upgrade only calls that come from the GAME (a BatmanAK.exe frame in the
    // first 12 of the call stack); geo-11's own calls go through untouched. dxgi11=2 upgrades every call (old way).
    // Each caller is recorded for the status line.
    HRESULT __stdcall hkCreateDXGIFactory(REFIID riid, void** pp)
    {
        void* fr[12] = {};
        const USHORT n = CaptureStackBackTrace(1, 12, fr, nullptr);
        const HMODULE exe = GetModuleHandleW(nullptr);
        bool fromGame = false;
        char who[64] = "";
        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE m = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fr[i], &m) || !m) continue;
            if (m == exe) fromGame = true;
            if (!who[0] && m != exe)
            {
                char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
                const char* sl = strrchr(path, '\\');
                if (_stricmp(sl ? sl + 1 : path, "dinput8.dll") != 0) strncpy_s(who, sl ? sl + 1 : path, _TRUNCATE);
            }
        }
        const bool upgrade = g_realCreateFactory1 && (fromGame || akvr_early_dxgi11_mode() == 2);
        char add[96];
        snprintf(add, sizeof(add), "%s%s via %s -> %s", g_dxgi11Who[0] ? "; " : "", fromGame ? "game" : "other", who[0] ? who : "?",
                 upgrade ? "1.1" : "left 1.0");
        if (strlen(g_dxgi11Who) + strlen(add) < sizeof(g_dxgi11Who)) strcat_s(g_dxgi11Who, add);
        if (upgrade) { InterlockedIncrement(&g_dxgi11Calls); return g_realCreateFactory1(riid, pp); }
        return oCreateDXGIFactory(riid, pp);
    }

    // DXGI11b 2026-10-07: installed FIRST in init_thread. In JJ's SteamVR run the hook answered 0 calls: it sat in
    // install_creation_hooks, after akvr_xr_try_init, and SteamVR's instance takes seconds to answer - the game had made
    // its factory by then. Under Virtual Desktop (instant) the same build answered 2 calls.
    bool g_dxgi11Hooked = false;
    void install_dxgi11_hook()
    {
        if (g_dxgi11Hooked || !akvr_early_dxgi11()) return;
        g_dxgi11Hooked = true;
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (!dxgi) return;
        MH_Initialize();   // no-op if already done
        void* cf1 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
        void* cf = (void*)GetProcAddress(dxgi, "CreateDXGIFactory");
        // DXGI11c: a pass-through on CreateDXGIFactory1 only to own a trampoline to the untouched original
        const bool ok1 = cf1 && MH_CreateHook(cf1, (void*)&hkCreateDXGIFactory1Pass, (void**)&g_realCreateFactory1) == MH_OK
                         && MH_EnableHook(cf1) == MH_OK;
        if (!ok1) g_realCreateFactory1 = nullptr;
        const bool ok = ok1 && cf && MH_CreateHook(cf, (void*)&hkCreateDXGIFactory, (void**)&oCreateDXGIFactory) == MH_OK
                        && MH_EnableHook(cf) == MH_OK;
        akvr_early_note(ok ? "dxgi11: the game's CreateDXGIFactory answered with CreateDXGIFactory1 (SteamVR frame sharing)"
                           : "dxgi11: could not hook CreateDXGIFactory");
    }

    void install_creation_hooks()
    {
        install_desktop_guard();
        if (g_creationHooked) return;

        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        if (!dxgi) return;
        MH_Initialize();   // no-op if kiero/gamepad already did it
        install_dxgi11_hook();   // DXGI11 (normally already in from the top of init_thread)

        using CreateFactory1Fn = HRESULT(__stdcall*)(REFIID, void**);
        auto cf1 = (CreateFactory1Fn)GetProcAddress(dxgi, "CreateDXGIFactory1");
        if (cf1)
        {
            IDXGIFactory* fac = nullptr;
            if (SUCCEEDED(cf1(__uuidof(IDXGIFactory), (void**)&fac)) && fac)
            {
                void** vt = *(void***)fac;
                // IDXGIFactory::CreateSwapChain is slot 10 (IUnknown 0-2, IDXGIObject
                // 3-6, then EnumAdapters/MakeWindowAssociation/GetWindowAssociation).
                if (MH_CreateHook(vt[10], (void*)&hkCreateSwapChain, (void**)&oCreateSwapChain) == MH_OK)
                    MH_EnableHook(vt[10]);

                IDXGIFactory2* fac2 = nullptr;
                if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&fac2)) && fac2)
                {
                    void** vt2 = *(void***)fac2;
                    // IDXGIFactory2::CreateSwapChainForHwnd is slot 15.
                    if (MH_CreateHook(vt2[15], (void*)&hkCreateSwapChainForHwnd, (void**)&oCreateSwapChainForHwnd) == MH_OK)
                        MH_EnableHook(vt2[15]);
                    fac2->Release();
                }
                // Display lies (fake desktop size, injected mode list) exist only to
                // make the game render BIGGER than the monitor. In geo-11 mode we do
                // not force the render size at all, so they have nothing to do â€” and
                // lying to DXGI underneath a stereo injector that reads the same
                // information is a needless variable in an already-fragile stack.
                // DXGIMODES 2026-09-26: ...but we DO force it now (ENGINE169/GEOSHAPE), and
                // the game validates its size against the DXGI mode list: 3072x2688 was
                // not on it, so it reset itself to fullscreen 2560x1600 and drew the 3D in
                // a corner. So under geo-11 the hooks go in whenever the size is forced.
                if (!akvr_early_geo11() || akvr_engine_res() > 0) install_output_hooks(fac);
                fac->Release();
                g_creationHooked = true;
            }
        }

        if (d3d11)
        {
            void* t = (void*)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
            if (t && MH_CreateHook(t, (void*)&hkD3D11CreateDASC, (void**)&oD3D11CreateDASC) == MH_OK)
            {
                MH_EnableHook(t);
                g_creationHooked = true;
            }
        }
    }

    // ------------------------------------------------------------------
    // geo-11 in HOOK mode â€” loaded under a name that is NOT `d3d11.dll`.
    //
    // WHY. As shipped, geo-11 *is* `d3d11.dll`: it sits next to the exe, Windows
    // binds every d3d11 import to it, and the genuine System32 copy may never be
    // mapped in its own right at all. Virtual Desktop's OpenXR runtime composites
    // INSIDE the game process, and with geo-11 wearing that name it never found the
    // headset â€” XR_ERROR_FORM_FACTOR_UNAVAILABLE for a whole session, bisected to
    // geo-11's d3d11.dll alone. See memory `geo11-blocks-inprocess-openxr`.
    //
    // 3DMigoto (geo-11's base) supports being loaded under any other name, and
    // detects it ITSELF â€” no ini setting is involved:
    //     if (hinstDLL != GetModuleHandleA("d3d11.dll")) HookD3D11(hinstDLL);
    // (`references/3Dmigoto/DirectX11/DLLMainHook.cpp:335`). It then Nektra-hooks
    // `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain` inside the REAL
    // d3d11.dll rather than replacing the module, so the genuine DLL stays present
    // and whole and only those two entry points are intercepted. Everything else
    // â€” D3D11Core*, the D3DKMT* kernel thunks, the DXGI device path â€” reaches
    // Microsoft's code untouched, which is the whole point of the exercise.
    //
    // THREE PRECONDITIONS, or geo-11's DllMain returns FALSE and it silently
    // does not load at all (LoadLibrary just fails):
    //   1. `dxgi.dll` must already be in the process  â€” HookDXGIFactories, and a
    //      failure there is fatal to its DllMain.
    //   2. `d3d11.dll` must already be in the process â€” HookD3D11 resolves it with
    //      GetModuleBaseAddress; it cannot LoadLibrary from DllMain (its own TODO
    //      says as much). BatmanAK.exe imports NEITHER statically â€” it imports
    //      d3d9.dll and loads d3d11/dxgi later â€” so nobody has loaded them yet.
    //      install_creation_hooks() above loads both, which is why we run after it.
    //   3. We may not do this from OUR DllMain either: LoadLibrary under the loader
    //      lock is exactly the deadlock guide 02 Â§3 warns about. Hence this thread.
    //
    // ORDER IS THE OTHER REASON THIS RUNS LAST. On a shared function, whoever hooks
    // SECOND ends up OUTERMOST. We want geo-11 outermost on device/swapchain
    // creation so that it wraps a swapchain we have already detoured:
    //     game -> geo-11 (draws both eyes) -> us (Present; both eyes are done) -> real
    // Load geo-11 before our hooks and the nesting inverts â€” we would be handed its
    // wrapper and run BEFORE its stereo work, which is the wrong half of the frame.
    //
    // NOT to be confused with geo-11's `hook=` ini setting, which makes it patch COM
    // vtables instead of wrapping them. That collides with our MinHook detours on the
    // same swapchain slots and crashed the render thread on 2026-08-07. `hook=` must
    // stay commented out; this mechanism does not need it.
    void load_geo11_hooked()
    {
        if (!akvr_early_geo11()) return;              // no geo-11 installed at all

        std::wstring p = settings_path();
        if (p.empty()) return;
        std::wstring dir = p.substr(0, p.find_last_of(L"\\/") + 1);

        // Installed the classic way? Then it already owns d3d11.dll, it is already
        // loaded, and there is nothing for us to do. Leave the known-good setup be.
        if (GetFileAttributesW((dir + L"d3d11.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            note("geo-11: WRAP mode (d3d11.dll present) - not loading an alias");
            return;
        }

        std::wstring alias = dir + L"geo11.dll";
        if (GetFileAttributesW(alias.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            note("geo-11: neither d3d11.dll nor geo11.dll here - STEREO WILL BE OFF");
            return;
        }

        // The genuine pair, by ABSOLUTE system path, so we cannot accidentally pick
        // up a wrapper sitting next to the game (guide 02 Â§2 â€” the same reason
        // REFramework resolves the real dinput8 through GetSystemDirectory).
        wchar_t sys[MAX_PATH];
        if (GetSystemDirectoryW(sys, MAX_PATH))
        {
            std::wstring s = std::wstring(sys) + L"\\";
            LoadLibraryW((s + L"dxgi.dll").c_str());
            LoadLibraryW((s + L"d3d11.dll").c_str());
        }
        const int haveDxgi  = GetModuleHandleW(L"dxgi.dll")  != nullptr;
        const int haveD3d11 = GetModuleHandleW(L"d3d11.dll") != nullptr;

        SetLastError(0);
        HMODULE g = LoadLibraryW(alias.c_str());
        DWORD   e = g ? 0 : GetLastError();
        g_geo11Hooked = (g != nullptr);
        // If this says FAILED with dxgi/d3d11 both 1, geo-11's DllMain refused us
        // for its own reasons (see verify_intended_target) â€” not a preconditions bug.
        note("geo-11 HOOK mode: geo11.dll %s (dxgi=%d d3d11=%d err=%lu)",
             g ? "LOADED" : "FAILED", haveDxgi, haveD3d11, (unsigned long)e);
    }

    DWORD WINAPI init_thread(LPVOID)
    {
        // FIRST â€” before kiero starts polling for an existing swapchain. Kiero can only
        // find one that already exists, which is far too late to change its size.
        // Note: the render SIZE is no longer decided here. It can't be â€” this thread
        // races the game's startup and loses. earlyres.cpp handles it from DllMain.
        settings_load();          // need g_targetRes before the game creates anything

        // WHICH RUNTIME? 2026-08-07. Virtual Desktop's OpenXR runtime does its
        // compositing INSIDE the game process, so it needs to create its own D3D11
        // device there â€” and geo-11 owns d3d11.dll and refuses exactly that (it
        // killed our own startup probe the same way). Proven by removing geo-11's
        // two halves in turn: with its dxgi.dll gone VR still failed, with its
        // d3d11.dll gone VR worked instantly. geo-11's own `allow_create_device=2`
        // compatibility setting did not help.
        //
        // SteamVR is the natural escape: its compositor lives in ANOTHER process
        // (vrserver/vrcompositor), so the part loaded into the game is far thinner
        // and has much less to lose to a wrapped d3d11.
        //
        // This machine already names SteamVR in the per-user registry key while the
        // loader is using the machine-wide one (Virtual Desktop). Rather than edit
        // the registry â€” which needs admin and would change every VR app on the PC â€”
        // we point THIS PROCESS at it with XR_RUNTIME_JSON, which the loader honours
        // above both keys. Off by default; `xrhkcu=1` in akvr_settings.ini.
        if (g_useHkcuRuntime)
        {
            HKEY k{};
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\OpenXR\\1", 0,
                              KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
            {
                wchar_t path[MAX_PATH]{}; DWORD cb = sizeof(path), type = 0;
                if (RegQueryValueExW(k, L"ActiveRuntime", nullptr, &type, (LPBYTE)path, &cb) == ERROR_SUCCESS
                    && (type == REG_SZ || type == REG_EXPAND_SZ)
                    && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", path);
                RegCloseKey(k);
            }
        }

        // CLAIM THE HEADSET BEFORE geo-11 EXISTS.
        // 2026-08-07: with geo-11 installed, Virtual Desktop answered
        // XR_ERROR_FORM_FACTOR_UNAVAILABLE to every request for the whole session â€”
        // "Virtual Desktop Server is not running" in its own log â€” while the very
        // same machine, headset and connection served a separate viewer process
        // happily, and served THIS game the moment geo-11's DLLs were renamed away.
        // Ruled out by test, in this order: geo-11's LoadLibrary redirection
        // (load_library_redirect=0) and its private nvapi64.dll (renamed away).
        // Neither made any difference, so it is not a DLL-substitution problem.
        //
        // What is left is ORDER. Everything else we do runs at the first Present,
        // which is long after geo-11 has built its device and entered Direct Mode.
        // This runs from our load thread instead, before the game has created
        // anything at all â€” so we ask for the headset while the process is still
        // ordinary. Session creation still happens later (it needs the device);
        // only the instance + system handle are taken early.
        install_dxgi11_hook();   // DXGI11b: before the headset probe, which can wait seconds on SteamVR
        akvr_xr_try_init();

        // Our creation hooks, then geo-11 on top of them. BOTH have moved AHEAD of
        // the headset probe below: the probe can sit here for 40 s, and the game
        // creates its device and swapchain long before that â€” hooking afterwards was
        // a race we were only winning because UE3 spends the time loading shaders.
        // load_geo11_hooked() must stay immediately after install_creation_hooks():
        // it depends on it having loaded the real dxgi/d3d11, and on hooking second
        // so that geo-11 ends up outermost. Read its comment before reordering.
        install_creation_hooks();
        // VSID4: the game's own D3D11CreateDevice slot, so the reticle split can name the HUD shaders.
        akvr_hudsplit_hook_game_device();
        // RE02: patch the game's 16:9 viewport computation to emit a square, before it
        // has had a chance to run. Opt-in via `vpsquare=1` â€” see g_vpSquareWanted.
        if (g_vpSquareWanted) g_vpSquareApplied = akvr_vp_square_install();
        load_geo11_hooked();

        // HEADSET PROBE â€” answers "can we get a headset at all?" WITHOUT needing a
        // single frame to render. 2026-08-07: geo-11's hooking mode crashed the
        // render thread (its vtable hooks and our MinHook detours on the same
        // swapchain slots), which would otherwise hide the only thing we wanted to
        // learn from that run. This writes the OpenXR status to its own file from
        // our own thread, so the answer survives a crash, a black screen, or our
        // Present hook being switched off entirely.
        if (g_xrProbe)
        {
            std::wstring pp = settings_path();
            if (!pp.empty())
            {
                pp = pp.substr(0, pp.find_last_of(L"\\/")) + L"\\akvr_xr_probe.txt";
                for (int i = 0; i < 20 && !akvr_xr_ok(); ++i)
                {
                    Sleep(2000);
                    akvr_xr_try_init();
                    if (FILE* pf = _wfopen(pp.c_str(), L"wb"))
                    {
                        fprintf(pf, "AKVR headset probe (no rendering involved)\n");
                        fprintf(pf, "attempt %d, %d s in\n\n%s\n", i + 1, (i + 1) * 2,
                                akvr_xr_status());
                        fclose(pf);
                    }
                }
            }
        }

        // geo-11 present: DO NOT touch kiero. Its probe creates a throwaway
        // device+swapchain, geo-11 fails that call, and the game dies before it ever
        // opens a window (measured 2026-08-06 â€” see hook_present_on). Our creation
        // hooks take Present off the real swapchain instead, so there is nothing to
        // wait for here.
        if (akvr_early_geo11()) return 0;

        // The game's swapchain may not exist yet at DLL load; kiero
        // retries until d3d11 is up. Cap at ~2 minutes, then give up
        // quietly (game keeps running unmodified).
        for (int i = 0; i < 480; ++i)
        {
            if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success)
            {
                kiero::bind(8,  (void**)&oPresent,       hkPresent);
                kiero::bind(13, (void**)&oResizeBuffers, hkResizeBuffers);
                akvr_camera_install();   // safe to retry via F9 if the scan misses
                return 0;
            }
            Sleep(250);
        }
        return 1;
    }
}

void akvr_start()
{
    HANDLE h = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

void akvr_stop()
{
    akvr_audiosync_shutdown();   // AUDIOSYNC: never leave the sound held
    akvr_camera_shutdown();
    if (!akvr_early_geo11()) kiero::shutdown();   // never initialised on the geo-11 path
}

// PANELTEST 2026-10-07: AKN's test switch "panel 1|0" + its telemetry (engine stereo: JJ could not open the panel)
extern "C" __declspec(dllexport) void akvr_panel_set(int on) { InterlockedExchange(&g_panelCmd, on ? 1 : 2); }
extern "C" __declspec(dllexport) unsigned long long akvr_panel_state()
{   // bits: 1 shown, 2 pad to panel, 4 ImGui ready, 8 drew last frame, 16 drew into the headset layer, 32 pad connected,
    // 64 both stick clicks down now, 128 headset panel layer ready; bits 16..31 = pad buttons; 32.. = chord fires
    const GamepadDiag d = akvr_gamepad_diag();
    unsigned long long v = (g_showOverlay ? 1 : 0) | (akvr_gamepad_menu() ? 2 : 0) | (g_imguiReady ? 4 : 0) | (g_panelDrewThisFrame ? 8 : 0) |
        (g_panelToVR ? 16 : 0) | (d.connected ? 32 : 0) | ((d.buttons & 0xC0) == 0xC0 ? 64 : 0) | (akvr_xr_overlay_ready() ? 128 : 0);
    return v | ((unsigned long long)d.buttons << 16) | ((unsigned long long)(unsigned)g_chordFires << 32);
}

