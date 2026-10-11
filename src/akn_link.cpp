// AKN link: see akn_link.h.
#include "akn_link.h"
#include "camera.h"   // POSESTAMP: akvr_camera_finalize_count
#include <windows.h>
#include <cwchar>
#include <cstdio>

namespace {
bool g_eng = false;
using VerFn = int(*)();
using TexFn = ID3D11Texture2D*(*)(LONG*);
using EyesFn = void(*)(const float*, const float*, int);
using VoidFn = void(*)();
using HudFn = void(*)(int);
using RepFn = void(*)(LONG, LONG);
RepFn p_report = nullptr;
using DevFn = void(*)(ID3D11Device*);
DevFn p_setdev = nullptr;
using StampFn = unsigned long long(*)();
StampFn p_stamp = nullptr;
using PoseRepFn = void(*)(LONG, LONG, LONG);
PoseRepFn p_poserep = nullptr;
VerFn p_ver = nullptr; TexFn p_pair = nullptr, p_hud = nullptr; EyesFn p_eyes = nullptr; VoidFn p_present = nullptr; HudFn p_sethud = nullptr;
bool g_linked = false;
char g_diag[160] = "engine stereo: off";
}

bool akvr_engine_stereo() { return g_eng; }
void akvr_engine_stereo_detect()
{
    const wchar_t* c = GetCommandLineW();
    g_eng = c && (wcsstr(c, L" -akn") != nullptr);
    if (g_eng) snprintf(g_diag, sizeof(g_diag), "engine stereo: on, waiting for the AKN DLL");
}
bool akn_link()
{
    if (g_linked || !g_eng) return g_linked;
    // the game folder's XINPUT9_1_0.dll is AKN (it loads the System32 copy itself under the same name, later)
    HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll");
    if (!m) return false;
    p_ver = (VerFn)GetProcAddress(m, "akn_api_version");
    if (!p_ver || p_ver() < 1) { snprintf(g_diag, sizeof(g_diag), "engine stereo: XINPUT9_1_0.dll has no AKN exports"); return false; }
    p_pair = (TexFn)GetProcAddress(m, "akn_pair");
    p_hud = (TexFn)GetProcAddress(m, "akn_hud");
    p_eyes = (EyesFn)GetProcAddress(m, "akn_set_eyes");
    p_present = (VoidFn)GetProcAddress(m, "akn_present");
    p_sethud = (HudFn)GetProcAddress(m, "akn_set_hud");
    p_report = (RepFn)GetProcAddress(m, "akn_report");   // optional (older AKN builds lack it)
    p_setdev = (DevFn)GetProcAddress(m, "akn_set_game_device");
    p_stamp = (StampFn)GetProcAddress(m, "akn_pair_stamp");
    p_poserep = (PoseRepFn)GetProcAddress(m, "akn_report_pose");
    g_linked = p_pair && p_hud && p_eyes && p_present && p_sethud;
    snprintf(g_diag, sizeof(g_diag), g_linked ? "engine stereo: AKN linked (api %d)" : "engine stereo: AKN exports incomplete (api %d)", p_ver());
    if (g_linked) p_sethud(1);
    return g_linked;
}
ID3D11Texture2D* akn_get_pair(long* seq) { LONG s = 0; ID3D11Texture2D* t = akn_link() ? p_pair(&s) : nullptr; if (seq) *seq = s; return t; }
ID3D11Texture2D* akn_get_hud(long* seq) { LONG s = 0; ID3D11Texture2D* t = akn_link() ? p_hud(&s) : nullptr; if (seq) *seq = s; return t; }
void akn_send_eyes(const float tan8[8], const float off2[2], int asym) { if (akn_link()) p_eyes(tan8, off2, asym); }
void akn_send_present() { if (akn_link()) p_present(); }
void akn_send_hud(int on) { if (akn_link()) p_sethud(on); }
void akn_send_report(long fresh, long flat) { if (akn_link() && p_report) p_report(fresh, flat); }
void akn_send_game_device(ID3D11Device* dev) { if (akn_link() && p_setdev) p_setdev(dev); }
unsigned long long akn_get_pair_stamp() { return (akn_link() && p_stamp) ? p_stamp() : 0; }
void akn_send_pose_report(long matched, long total, long delay) { if (akn_link() && p_poserep) p_poserep(matched, total, delay); }
const char* akn_link_diag() { return g_diag; }
// POSESTAMP: AKN reads the camera-finalize count when it builds a frame's eye views (exported from dinput8.dll)
extern "C" __declspec(dllexport) unsigned long long akvr_finalize_now() { return akvr_camera_write_count(); }   // PAUSESTAMP: + pause-look writes

// PANELRAIN 2026-10-08 (JJ: the panel's rain options + "pause the rain"): commands and state from AKN
void akn_command(const char* text)
{
    if (!akn_link()) return;
    using CmdFn = void(*)(const char*);
    static CmdFn f = (CmdFn)GetProcAddress(GetModuleHandleW(L"XINPUT9_1_0.dll"), "akn_cmd");
    if (f) f(text);
}
int akn_query(const char* name)
{
    if (!akn_link()) return -1;
    using QFn = int(*)();
    QFn f = (QFn)GetProcAddress(GetModuleHandleW(L"XINPUT9_1_0.dll"), name);
    return f ? f() : -1;
}
bool akvr_camera_axes_ago(int k, float bf[3], float br[3], float bu[3], float ff[3], float fr[3], float fu[3]);   // camera.cpp
// FARRAIN2: AKN asks for the game camera's forward (no head) and the head-included one, UE3 world axes
extern "C" __declspec(dllexport) int akvr_rain_axes(float base[3], float full[3])
{
    float br[3], bu[3], fr[3], fu[3];
    return akvr_camera_axes_ago(0, base, br, bu, full, fr, fu) ? 1 : 0;
}
// HUDINEYE test switch (AKN command "hudineye N")
void akvr_xr_hud_in_eye_set(int v);
extern "C" __declspec(dllexport) void akvr_set_hud_in_eye(int v) { akvr_xr_hud_in_eye_set(v); }
// SVRSPACE test switch (AKN command "xrspace N": 0 auto, 1 LOCAL, 2 STAGE)
void akvr_xr_space_mode_set(int v);
extern "C" __declspec(dllexport) void akvr_set_xr_space(int v) { akvr_xr_space_mode_set(v); }
// SVRSTRAIGHT test switch (AKN command "svrstraight N": 0 off, 1 auto SteamVR, 2 always)
void akvr_xr_svr_straight_set(int v);
extern "C" __declspec(dllexport) void akvr_set_svr_straight(int v) { akvr_xr_svr_straight_set(v); }
const char* akvr_xr_hie_diag();
extern "C" __declspec(dllexport) const char* akvr_hie_diag_c() { return akvr_xr_hie_diag(); }   // MARKERCUT: AKN logs it
extern "C" __declspec(dllexport) void akvr_set_markercut(int v);   // MARKERCUT test switch (xr.cpp)
// MENUCHECK (test harness): AKVR's main-menu detection, so AKN's status line shows whether the game left the menu
bool akvr_xr_main_menu_detected();
extern "C" __declspec(dllexport) int akvr_in_main_menu() { return akvr_xr_main_menu_detected() ? 1 : 0; }
// MARKERBASE (AKN world markers): all three axes of the game camera without the head (b*) and with it (f*)
extern "C" __declspec(dllexport) int akvr_marker_axes(float bf[3], float br[3], float bu[3], float ff[3], float fr[3], float fu[3])
{
    return akvr_camera_axes_ago(0, bf, br, bu, ff, fr, fu) ? 1 : 0;
}
// NEARRAIN2: the position offset AKVR adds to the game camera for the head (lean, height, shoulder), world units
bool akvr_camera_pos_delta_ago(int k, float d[3]);   // camera.cpp
extern "C" __declspec(dllexport) int akvr_rain_pos_delta(float d[3]) { return akvr_camera_pos_delta_ago(0, d) ? 1 : 0; }
