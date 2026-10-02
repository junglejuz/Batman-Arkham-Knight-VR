// AKVR — early render-size control ("bigres")
// ---------------------------------------------------------------------------
// THE PROBLEM. Arkham Knight decides its render size ONCE, during startup, and
// never revisits it: measured 2423x1363 on a 2560x1440 desktop. That is exactly
// the largest 16:9 that fits the desktop WORK AREA (2560x1392) minus 29px of
// window caption/border. So the size is arithmetic on the work area — the render
// is chained to the monitor, which is precisely what a VR mod must break.
//
// WHY EVERY PREVIOUS ATTEMPT FAILED. hooks.cpp already spoofed the same APIs, but
// it did so from init_thread — a worker thread spawned by DllMain. The game's main
// thread carries on the instant DllMain returns, so it was a footrace against the
// game's own startup, and the game had a head start. The spoof was almost certainly
// installed AFTER the size was already baked. Same for the DPI-awareness call (and
// DPI turned out to be a red herring anyway: the desktop really is 2560x1440, no
// Windows scaling involved).
//
// THE FIX. Do it in DllMain itself, before the exe executes a single instruction.
// We are a version.dll proxy, so the loader runs our DllMain during import
// resolution — earlier than any game code can possibly run. MinHook is NOT safe
// that early (it enumerates and suspends threads, which can deadlock under the
// loader lock), so we don't use it: we rewrite the game's IMPORT TABLE instead.
// That is just a few pointer writes into already-mapped memory — no locks, no
// thread enumeration, no library loading. The loader has already snapped the IAT
// by the time DllMain runs, so the entries are there waiting for us.
//
// Guide reference (references/vrframework/guides/14-engine-tweaks-and-quirks.md
// section 4, "Forcing the window and render resolution to the HMD"): "Two surfaces
// need to agree: the OS window/swapchain and the engine's internal render target."
// Here they are the same surface — AK derives its internal viewport from the window
// it builds out of these numbers — so fooling the numbers moves both together.
//
// WHAT WE CLAIM. The caller picks a render HEIGHT (e.g. 2160). We report a fake
// work area of (H*16/9 + slack) x (H + chrome) and a fake screen slightly larger,
// so the game's own "largest 16:9 that fits" arithmetic lands on exactly HxH*16/9.
// The window ends up bigger than the physical monitor and hangs off the edge —
// harmless, because in VR nobody looks at it.
//
// EVERY intercepted call is logged with its answer, in order. If the render size
// still doesn't move, that log names the question the game asked instead, which is
// the entry point for the static disassembly path.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <MinHook.h>   // only for the HUD viewport hook, installed at first Present
#include <intrin.h>    // _ReturnAddress (TEXDRAW)
#include "camera.h"
#include "xr.h"
#include "hud_projection.h"
void akvr_hud_layers_hook_vtable(void** vt);   // HUDLAYERS, defined below
void akvr_hud_layers_discover();
void akvr_hud_layers_discover_quick();
bool akvr_hud_room_all();   // ROOMALL
const char* akvr_hud_layer_xf_list();
#include <cstddef>

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
namespace {

    // MEASURED 2026-07-27 (build BIGRES-1, JJ's log): the render size is the game's
    // OWN ini resolution (BmSystemSettings.ini ResX/ResY), CLAMPED to fit the desktop
    // work area. Claiming a huge work area removed the clamp and the ini value went
    // straight through — asked 3840x2160, got 3840x2160. So there are two halves and
    // we now hold both: the ini says what we want, this file removes the ceiling.
    bool g_enabled   = true;     // spoof on/off (persisted; restart to apply)
    // ONE size. The two axes are never independently settable — a mismatched frame
    // shape is exactly the bug we are trying to fix, so width is always DERIVED:
    // square when projVR's square view is the target (the normal case), 16:9 only
    // for comparing against the flat view.
    int  g_renderH   = 2880;     // the render size (height); width follows the shape
    bool g_square    = true;     // true -> WxH = HxH; false -> 16:9
    int  g_chrome    = 29;       // caption+border the game subtracts from the work area
    int  g_taskbar   = 48;       // work-area shrink we pretend the taskbar causes

    // When the ENGINE render size is ours, the buffer/window size IS the engine
    // size — see the note in load_settings. 0 = feature off.
    int  g_engW = 0, g_engH = 0;
    int  render_w()  { return g_engH ? g_engW
                            : (g_square ? g_renderH : (g_renderH * 16) / 9); }
    int  render_h()  { return g_engH ? g_engH : g_renderH; }

    int  g_fakeWorkW = 0, g_fakeWorkH = 0;      // what we tell the game the work area is
    int  g_fakeScrW  = 0, g_fakeScrH  = 0;      // ...and the screen
    bool g_installed = false;
    int  g_patched   = 0;        // import entries we successfully redirected
    long g_calls     = 0;        // total intercepted calls

    // --- startup log ---------------------------------------------------------
    // Fixed buffer, no allocation: this runs under the loader lock where the heap
    // is legal but best left alone. First entries only — the interesting traffic is
    // all during startup, and we must not flood once the game is looping.
    char  g_log[7000];
    int   g_logLen  = 0;
    int   g_logHits = 0;
    volatile LONG g_logLock = 0;

    void log_add(const char* fmt, ...)
    {
        if (g_logHits > 120) return;                   // startup only
        while (InterlockedCompareExchange(&g_logLock, 1, 0) != 0) { }
        if (g_logLen < (int)sizeof(g_log) - 200)
        {
            char line[192];
            va_list ap; va_start(ap, fmt);
            int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
            va_end(ap);
            // Collapse an immediate repeat. The window queries fire once per frame
            // forever, so without this the startup transcript is 100 identical
            // "GetWindowRect = 2560x1440" lines and the useful answers above them
            // scroll out of the buffer.
            static char prev[192] = "";
            static int  prevRun   = 0;
            if (n > 0 && strcmp(line, prev) == 0)
            {
                ++prevRun;
                InterlockedExchange(&g_logLock, 0);
                return;
            }
            if (n > 0 && prevRun > 0)
            {
                char rep[64];
                int rn = _snprintf_s(rep, sizeof(rep), _TRUNCATE, "   (repeated %d more times)", prevRun);
                for (int i = 0; i < rn && g_logLen < (int)sizeof(g_log) - 2; ++i)
                    g_log[g_logLen++] = rep[i];
                g_log[g_logLen++] = '\n';
                g_log[g_logLen]   = 0;
                prevRun = 0;
            }
            if (n > 0) strcpy_s(prev, sizeof(prev), line);
            if (n > 0)
            {
                for (int i = 0; i < n && g_logLen < (int)sizeof(g_log) - 2; ++i)
                    g_log[g_logLen++] = line[i];
                g_log[g_logLen++] = '\n';
                g_log[g_logLen]   = 0;
                ++g_logHits;
            }
        }
        InterlockedExchange(&g_logLock, 0);
    }

    // --- originals -----------------------------------------------------------
    using GetSysMetricsFn  = int  (WINAPI*)(int);
    using SPIAFn           = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT);
    using SPIWFn           = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT);
    using GetMonInfoAFn    = BOOL (WINAPI*)(HMONITOR, LPMONITORINFO);
    using GetMonInfoWFn    = BOOL (WINAPI*)(HMONITOR, LPMONITORINFO);
    using GetClientRectFn  = BOOL (WINAPI*)(HWND, LPRECT);
    using GetWindowInfoFn  = BOOL (WINAPI*)(HWND, PWINDOWINFO);
    using AdjWndRectFn     = BOOL (WINAPI*)(LPRECT, DWORD, BOOL);
    using CreateWinExWFn   = HWND (WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                            HWND, HMENU, HINSTANCE, LPVOID);
    using CreateWinExAFn   = HWND (WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                            HWND, HMENU, HINSTANCE, LPVOID);
    using GetWindowRectFn  = BOOL (WINAPI*)(HWND, LPRECT);
    using EnumDispAFn      = BOOL (WINAPI*)(LPCSTR,  DWORD, DEVMODEA*);
    using EnumDispWFn      = BOOL (WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);

    GetSysMetricsFn  oGetSystemMetrics      = nullptr;
    SPIAFn           oSystemParametersInfoA = nullptr;
    SPIWFn           oSystemParametersInfoW = nullptr;
    GetMonInfoAFn    oGetMonitorInfoA       = nullptr;
    GetMonInfoWFn    oGetMonitorInfoW       = nullptr;
    GetClientRectFn  oGetClientRect         = nullptr;
    GetWindowInfoFn  oGetWindowInfo         = nullptr;
    AdjWndRectFn     oAdjustWindowRect      = nullptr;
    CreateWinExWFn   oCreateWindowExW       = nullptr;
    CreateWinExAFn   oCreateWindowExA       = nullptr;

    // JJ 2026-08-05: the logo pop-up on launch lands in the bottom-left of monitor TWO.
    // Centring the main game window (hooks.cpp set_client_size) does not reach it — the
    // splash appears long before the swapchain exists, so by the time our graphics hooks
    // are alive it has already been and gone. Catch it where it is born instead.
    //
    // Only TOP-LEVEL windows of a sensible size are moved: no parent (a child window's
    // coordinates are relative to its parent and moving it would be nonsense) and big
    // enough not to be a tooltip or a hidden message-only window.
    void centre_on_primary(HWND hw)
    {
        if (!hw) return;
        RECT rc{};
        if (!GetWindowRect(hw, &rc)) return;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w < 200 || h < 120) return;

        MONITORINFO mi{ sizeof(MONITORINFO) };
        HMONITOR hm = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        if (!hm || !GetMonitorInfoW(hm, &mi)) return;
        const int x = mi.rcMonitor.left + ((mi.rcMonitor.right  - mi.rcMonitor.left) - w) / 2;
        const int y = mi.rcMonitor.top  + ((mi.rcMonitor.bottom - mi.rcMonitor.top)  - h) / 2;
        SetWindowPos(hw, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HWND WINAPI hkCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style,
                                  int x, int y, int w, int h, HWND parent, HMENU menu,
                                  HINSTANCE inst, LPVOID param)
    {
        HWND hw = oCreateWindowExW ? oCreateWindowExW(ex, cls, name, style, x, y, w, h,
                                                      parent, menu, inst, param) : nullptr;
        if (hw && !parent && !(style & WS_CHILD)) centre_on_primary(hw);
        return hw;
    }

    HWND WINAPI hkCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style,
                                  int x, int y, int w, int h, HWND parent, HMENU menu,
                                  HINSTANCE inst, LPVOID param)
    {
        HWND hw = oCreateWindowExA ? oCreateWindowExA(ex, cls, name, style, x, y, w, h,
                                                      parent, menu, inst, param) : nullptr;
        if (hw && !parent && !(style & WS_CHILD)) centre_on_primary(hw);
        return hw;
    }
    GetWindowRectFn  oGetWindowRect         = nullptr;
    EnumDispAFn      oEnumDisplaySettingsA  = nullptr;
    EnumDispWFn      oEnumDisplaySettingsW  = nullptr;

    // ---------------------------------------------------------------------------
    // the stubs
    // ---------------------------------------------------------------------------
    // Only the metrics that describe "how big is the screen / usable area" are
    // rewritten. Everything else passes through untouched — lying more widely than
    // necessary is how you break mouse mapping and window placement.
    int WINAPI hkGetSystemMetrics(int idx)
    {
        int v = oGetSystemMetrics ? oGetSystemMetrics(idx) : 0;
        int out = v;
        if (g_enabled)
        {
            switch (idx)
            {
            case SM_CXSCREEN:        case SM_CXVIRTUALSCREEN: out = g_fakeScrW;  break;
            case SM_CYSCREEN:        case SM_CYVIRTUALSCREEN: out = g_fakeScrH;  break;
            case SM_CXFULLSCREEN:    case SM_CXMAXIMIZED:     out = g_fakeWorkW; break;
            case SM_CYFULLSCREEN:                             out = g_fakeWorkH - g_chrome; break;
            case SM_CYMAXIMIZED:                              out = g_fakeWorkH; break;
            default: break;
            }
        }
        InterlockedIncrement(&g_calls);
        if (idx <= 1 || idx == SM_CXFULLSCREEN || idx == SM_CYFULLSCREEN ||
            idx == SM_CXMAXIMIZED || idx == SM_CYMAXIMIZED ||
            idx == SM_CXVIRTUALSCREEN || idx == SM_CYVIRTUALSCREEN)
            log_add("GetSystemMetrics(%d) real=%d -> %d", idx, v, out);
        return out;
    }

    void spoof_workarea(RECT* rc)
    {
        rc->left = 0; rc->top = 0; rc->right = g_fakeWorkW; rc->bottom = g_fakeWorkH;
    }

    BOOL WINAPI hkSystemParametersInfoA(UINT act, UINT up, PVOID pp, UINT fw)
    {
        BOOL r = oSystemParametersInfoA ? oSystemParametersInfoA(act, up, pp, fw) : FALSE;
        if (r && act == SPI_GETWORKAREA && pp)
        {
            RECT* rc = (RECT*)pp;
            log_add("SPI_GETWORKAREA(A) real=%dx%d -> %dx%d%s",
                    rc->right - rc->left, rc->bottom - rc->top, g_fakeWorkW, g_fakeWorkH,
                    g_enabled ? "" : "   (NOT APPLIED - spoof off)");
            if (g_enabled) spoof_workarea(rc);
            InterlockedIncrement(&g_calls);
        }
        return r;
    }

    BOOL WINAPI hkSystemParametersInfoW(UINT act, UINT up, PVOID pp, UINT fw)
    {
        BOOL r = oSystemParametersInfoW ? oSystemParametersInfoW(act, up, pp, fw) : FALSE;
        if (r && act == SPI_GETWORKAREA && pp)
        {
            RECT* rc = (RECT*)pp;
            log_add("SPI_GETWORKAREA(W) real=%dx%d -> %dx%d%s",
                    rc->right - rc->left, rc->bottom - rc->top, g_fakeWorkW, g_fakeWorkH,
                    g_enabled ? "" : "   (NOT APPLIED - spoof off)");
            if (g_enabled) spoof_workarea(rc);
            InterlockedIncrement(&g_calls);
        }
        return r;
    }

    void spoof_moninfo(LPMONITORINFO mi, const char* tag)
    {
        log_add("GetMonitorInfo%s real mon=%dx%d work=%dx%d -> %dx%d / %dx%d%s", tag,
                mi->rcMonitor.right - mi->rcMonitor.left, mi->rcMonitor.bottom - mi->rcMonitor.top,
                mi->rcWork.right - mi->rcWork.left,       mi->rcWork.bottom - mi->rcWork.top,
                g_fakeScrW, g_fakeScrH, g_fakeWorkW, g_fakeWorkH,
                g_enabled ? "" : "   (NOT APPLIED - spoof off)");
        if (!g_enabled) return;
        mi->rcMonitor.left = 0; mi->rcMonitor.top = 0;
        mi->rcMonitor.right = g_fakeScrW; mi->rcMonitor.bottom = g_fakeScrH;
        mi->rcWork.left = 0; mi->rcWork.top = 0;
        mi->rcWork.right = g_fakeWorkW; mi->rcWork.bottom = g_fakeWorkH;
    }

    BOOL WINAPI hkGetMonitorInfoA(HMONITOR h, LPMONITORINFO mi)
    {
        BOOL r = oGetMonitorInfoA ? oGetMonitorInfoA(h, mi) : FALSE;
        if (r && mi) { spoof_moninfo(mi, "A"); InterlockedIncrement(&g_calls); }
        return r;
    }

    BOOL WINAPI hkGetMonitorInfoW(HMONITOR h, LPMONITORINFO mi)
    {
        BOOL r = oGetMonitorInfoW ? oGetMonitorInfoW(h, mi) : FALSE;
        if (r && mi) { spoof_moninfo(mi, "W"); InterlockedIncrement(&g_calls); }
        return r;
    }

    // Pass-through + log only. The client rect is the truth about the window we
    // actually got, so faking it would hide whether the plan worked — and it also
    // drives mouse mapping. We want to READ this one, not bend it.
    BOOL WINAPI hkGetClientRect(HWND h, LPRECT rc)
    {
        BOOL r = oGetClientRect ? oGetClientRect(h, rc) : FALSE;
        if (r && rc)
        {
            LONG w = rc->right - rc->left, ht = rc->bottom - rc->top;
            if (w > 800) log_add("GetClientRect = %dx%d", (int)w, (int)ht);
        }
        return r;
    }

    // GetWindowInfo ALSO carries a client rect (WINDOWINFO::rcClient), and the exe
    // imports it (verified in the import table 2026-08-03) while we were only
    // watching GetClientRect. If the engine sizes itself through this call instead,
    // both our spoof and our log have been blind to it the whole time — which would
    // explain a scene size (2496x2688) that matches nothing we claim.
    BOOL WINAPI hkGetWindowInfo(HWND h, PWINDOWINFO wi)
    {
        BOOL r = oGetWindowInfo ? oGetWindowInfo(h, wi) : FALSE;
        if (r && wi)
        {
            LONG cw = wi->rcClient.right - wi->rcClient.left;
            LONG ch = wi->rcClient.bottom - wi->rcClient.top;
            if (cw > 800) log_add("GetWindowInfo client = %dx%d", (int)cw, (int)ch);
        }
        return r;
    }

    // Imported too, and never watched. This is how a window turns a desired CLIENT
    // size into an outer size; a game that computes its own window geometry will go
    // through here, and the numbers it passes reveal the size it is aiming for.
    // THIS IS THE DOOR. Startup log 2026-08-04: we force the client to 3200x3200 and
    // it HOLDS ("GetClientRect = 3200x3200") until "AdjustWindowRect in 3840x2160"
    // fires — then the game rebuilds its window at 3840x2160 and keeps it. Every
    // read-side lie we tell (work area, monitor, DXGI output, mode list) is answered
    // correctly and simply bypassed, because the game is not asking: it is asserting a
    // size it already decided. So stop observing this call and REWRITE it. Turning the
    // requested client rect into our target here means the game computes, and then
    // sets, the window WE want — using its own code path, so nothing fights back.
    BOOL WINAPI hkAdjustWindowRect(LPRECT rc, DWORD style, BOOL menu)
    {
        int inW = 0, inH = 0;
        if (rc) { inW = rc->right - rc->left; inH = rc->bottom - rc->top; }
        bool rewrote = false;
        if (g_enabled && rc && inW > 800 && inH > 400)
        {
            // Only the big, window-sized requests — never a dialog or a control.
            int wantW = render_w(), wantH = g_renderH;
            if (inW != wantW || inH != wantH)
            {
                rc->right  = rc->left + wantW;
                rc->bottom = rc->top  + wantH;
                rewrote = true;
            }
        }
        BOOL r = oAdjustWindowRect ? oAdjustWindowRect(rc, style, menu) : FALSE;
        if (inW > 800)
            log_add("AdjustWindowRect in %dx%d%s", inW, inH,
                    rewrote ? "  -> REWRITTEN" : "");
        return r;
    }

    // THE LAST DOOR. JJ, 2026-08-04: monitor 1 is 1440p, monitor 2 is 4K, and he plays
    // on 1 — but AK opens on 2 and bakes a 3840x2160 render size from it before our
    // window work can land. Every size call we DO intercept answers correctly and gets
    // ignored, because EnumDisplayMonitors hands the monitor rectangle directly to the
    // caller's callback: the game never has to ask a function we can spoof.
    // So we filter the enumeration itself and show the game ONE monitor — the primary,
    // carrying our claimed size. It cannot choose a display it is never told about.
    using EnumDispMonFn = BOOL (WINAPI*)(HDC, LPCRECT, MONITORENUMPROC, LPARAM);
    EnumDispMonFn oEnumDisplayMonitors = nullptr;

    MONITORENUMPROC g_userProc = nullptr;   // startup is single-threaded here
    LPARAM          g_userData = 0;
    int             g_monSeen  = 0;

    BOOL CALLBACK mon_filter(HMONITOR h, HDC dc, LPRECT rc, LPARAM)
    {
        ++g_monSeen;
        MONITORINFO mi{ sizeof(mi) };
        BOOL primary = FALSE;
        if (oGetMonitorInfoW && oGetMonitorInfoW(h, &mi))
            primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        if (!primary) return TRUE;          // skip it, keep enumerating

        RECT fake{ 0, 0, (LONG)g_fakeScrW, (LONG)g_fakeScrH };
        if (rc) log_add("EnumDisplayMonitors: primary %dx%d -> %dx%d (others hidden)",
                        (int)(rc->right - rc->left), (int)(rc->bottom - rc->top),
                        g_fakeScrW, g_fakeScrH);
        return g_userProc ? g_userProc(h, dc, &fake, g_userData) : TRUE;
    }

    BOOL WINAPI hkEnumDisplayMonitors(HDC dc, LPCRECT clip, MONITORENUMPROC proc, LPARAM data)
    {
        if (!g_enabled || !proc)
            return oEnumDisplayMonitors ? oEnumDisplayMonitors(dc, clip, proc, data) : FALSE;
        g_userProc = proc; g_userData = data; g_monSeen = 0;
        BOOL r = oEnumDisplayMonitors ? oEnumDisplayMonitors(dc, clip, &mon_filter, 0) : FALSE;
        g_userProc = nullptr;
        return r;
    }

    BOOL WINAPI hkGetWindowRect(HWND h, LPRECT rc)
    {
        BOOL r = oGetWindowRect ? oGetWindowRect(h, rc) : FALSE;
        if (r && rc)
        {
            LONG w = rc->right - rc->left, ht = rc->bottom - rc->top;
            if (w > 800) log_add("GetWindowRect = %dx%d", (int)w, (int)ht);
        }
        return r;
    }

    // 2026-09-26 (build DISPMODE): with every other size question faked, the game
    // still created 2560x1440 from ini ResX/ResY=3840x2160; the one honest answer
    // left in the log was the CURRENT display mode. So when bigres is on, fake the
    // current/registry mode too, and append 3840x2160 and 5120x2880 to the mode LIST
    // in case the game validates its ini size against it. Every list is logged.
    template <class DM>
    BOOL enum_display_settings(BOOL r, DWORD mode, DM* dm, const char* tag,
                               DM& last, int& realCount, bool& listLogged, int& bigW, int& bigH)
    {
        if (!dm) return r;
        if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS)
        {
            if (!r) return r;
            const unsigned w = dm->dmPelsWidth, h = dm->dmPelsHeight;
            if (g_enabled) { dm->dmPelsWidth = (DWORD)g_fakeScrW; dm->dmPelsHeight = (DWORD)g_fakeScrH; }
            log_add("EnumDisplaySettings(%s,%s) = %ux%u%s", tag,
                    mode == ENUM_CURRENT_SETTINGS ? "current" : "registry", w, h,
                    g_enabled ? " -> REWRITTEN" : "");
            return r;
        }
        if (r)
        {
            last = *dm; realCount = (int)mode + 1;
            if ((int)(dm->dmPelsWidth * dm->dmPelsHeight) > bigW * bigH)
            { bigW = (int)dm->dmPelsWidth; bigH = (int)dm->dmPelsHeight; }
            return r;
        }
        if (!g_enabled || realCount == 0) return r;
        // UNLOCK169 2026-09-26: with a forced 3072x2688 render the game still chose a
        // 2560x1600 view (its camera then built at 0.625 = 16:10) — it validates its
        // size against this list, and 3072x2688 was not in it (3840x2160, which is,
        // went straight through). So the forced render size leads the list.
        const int kExtra[3][2] = { { render_w(), render_h() }, { 3840, 2160 }, { 5120, 2880 } };
        const int k = (int)mode - realCount;
        if (k == 0 && !listLogged)
        {
            listLogged = true;
            log_add("EnumDisplaySettings(%s) list: %d real modes, largest %dx%d; appending %dx%d, 3840x2160, 5120x2880",
                    tag, realCount, bigW, bigH, render_w(), render_h());
        }
        if (k < 0 || k > 2) return r;
        dm->dmBitsPerPel       = last.dmBitsPerPel;
        dm->dmDisplayFrequency = last.dmDisplayFrequency;
        dm->dmDisplayFlags     = last.dmDisplayFlags;
        dm->dmFields           = last.dmFields;
        dm->dmPelsWidth        = (DWORD)kExtra[k][0];
        dm->dmPelsHeight       = (DWORD)kExtra[k][1];
        return TRUE;
    }

    BOOL WINAPI hkEnumDisplaySettingsA(LPCSTR dev, DWORD mode, DEVMODEA* dm)
    {
        static DEVMODEA last{}; static int realCount = 0, bigW = 0, bigH = 0; static bool logged = false;
        BOOL r = oEnumDisplaySettingsA ? oEnumDisplaySettingsA(dev, mode, dm) : FALSE;
        return enum_display_settings(r, mode, dm, "A", last, realCount, logged, bigW, bigH);
    }

    BOOL WINAPI hkEnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW* dm)
    {
        static DEVMODEW last{}; static int realCount = 0, bigW = 0, bigH = 0; static bool logged = false;
        BOOL r = oEnumDisplaySettingsW ? oEnumDisplaySettingsW(dev, mode, dm) : FALSE;
        return enum_display_settings(r, mode, dm, "W", last, realCount, logged, bigW, bigH);
    }

    // ---------------------------------------------------------------------------
    // import-table redirection
    // ---------------------------------------------------------------------------
    bool same_dll(const char* a, const char* b)     // case-insensitive, ASCII
    {
        for (;; ++a, ++b)
        {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) return false;
            if (!ca) return true;
        }
    }

    bool same_fn(const char* a, const char* b)
    {
        for (;; ++a, ++b) { if (*a != *b) return false; if (!*a) return true; }
    }

    // Swap one entry in `mod`'s import table. Returns the address that was there
    // (i.e. the real Windows function) or nullptr if the import wasn't found.
    void* patch_import(HMODULE mod, const char* dll, const char* fn, void* replacement)
    {
        BYTE* base = (BYTE*)mod;
        auto* dos  = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size) return nullptr;

        auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
        for (; imp->Name; ++imp)
        {
            const char* name = (const char*)(base + imp->Name);
            if (!same_dll(name, dll)) continue;

            auto* thunk  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            auto* origin = imp->OriginalFirstThunk
                         ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk)
                         : thunk;

            for (; origin->u1.AddressOfData; ++origin, ++thunk)
            {
                if (IMAGE_SNAP_BY_ORDINAL(origin->u1.Ordinal)) continue;   // imported by number, no name to match
                auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + origin->u1.AddressOfData);
                if (!same_fn((const char*)byName->Name, fn)) continue;

                void* previous = (void*)thunk->u1.Function;
                DWORD old = 0;
                if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                    return nullptr;
                thunk->u1.Function = (ULONGLONG)replacement;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                ++g_patched;
                return previous;
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // settings — read by hand, no CRT file streams, no allocation
    // ---------------------------------------------------------------------------
    int read_int(const char* text, const char* key, int fallback)
    {
        size_t klen = 0; while (key[klen]) ++klen;
        for (const char* p = text; *p; )
        {
            const char* line = p;
            while (*p && *p != '\n') ++p;
            bool match = true;
            for (size_t i = 0; i < klen; ++i)
                if (line[i] != key[i]) { match = false; break; }
            if (match && line[klen] == '=')
            {
                int v = 0; bool any = false;
                for (const char* q = line + klen + 1; *q >= '0' && *q <= '9'; ++q)
                { v = v * 10 + (*q - '0'); any = true; }
                if (any) return v;
            }
            if (*p) ++p;
        }
        return fallback;
    }

    // =======================================================================
    // ENGINE RENDER SIZE — the real lever, found by RE 2026-08-05.
    //
    // Every read-side spoof in this file only ever answered questions the engine
    // wasn't asking. The render size is actually two plain globals that the engine
    // fills from the ini and then hands to its viewport-creation call:
    //
    //   0x140aed8c9  cmp  [flag], r12d
    //   0x140aed8d0  je   +0x1C            ; skip the block below
    //   0x140aed8d2  ResX = ResX * 2       ; 28-byte "double it" block
    //   0x140aed8e0  ResY = ResY * 2
    //   ...
    //   0x140aed9ed  mov r9d, [ResX]       ; the ONLY consumer
    //   0x140aed9f4  call [r10+0x290]      ; CreateViewport(..., ResX, ResY, ...)
    //
    // We don't need a code cave: that doubling block is 28 bytes of straight-line
    // code with nothing branching into it, which is more than enough room to store
    // our own two constants instead. Turn the `je` into two NOPs so the block always
    // runs, then overwrite it with `mov [ResX], W` / `mov [ResY], H` and pad.
    // RIP-relative stores, so it survives ASLR with no fixups.
    //
    // Both global addresses are DECODED FROM THE MATCHED SITE, never hardcoded.
    // Signature is masked over the four displacements and is unique in the image.
    const uint8_t kEngPat[] = {
        0x44,0x39,0x25,0,0,0,0,  0x74,0x1C,
        0x8B,0x05,0,0,0,0,  0x03,0xC0,  0x89,0x05,0,0,0,0,
        0x8B,0x05,0,0,0,0,  0x03,0xC0,  0x89,0x05,0,0,0,0 };
    const bool kEngMask[] = {
        1,1,1,0,0,0,0,  1,1,
        1,1,0,0,0,0,  1,1,  1,1,0,0,0,0,
        1,1,0,0,0,0,  1,1,  1,1,0,0,0,0 };

    // Persisted: ONE size (the height) + the headset's wide:tall shape x1000.
    // 0 height = feature off, the game keeps its own size.
    int       g_engineH     = 0;
    int       g_engineShape = 911;         // tan(42.35)/tan(45) — Quest 3 via VDXR
    int       g_geo11Shape  = 1778;        // GEO11SHAPE: render width/height x1000 under geo-11 (16:9)
    // (g_engW / g_engH are declared up with render_w — the buffer size depends on them.)
    uintptr_t g_engAddr = 0;               // matched signature address
    uintptr_t g_engResX = 0, g_engResY = 0;
    bool      g_engOn = false;

    // =======================================================================
    // COMFORT SETTINGS — motion blur, v-sync and chromatic aberration OFF.
    //
    // All three are actively unpleasant in a headset: blur smears the world whenever
    // you turn your head, v-sync fights the compositor's own pacing, and chromatic
    // aberration paints fake colour fringing on top of the lens's real fringing.
    //
    // This runs in DllMain, NOT at first Present, and that is the whole point: UE3
    // reads its config once at startup, so a write from the render loop would only
    // take effect on the player's SECOND launch. A new user's first launch has to be
    // right, so this has to happen before the engine has read anything.
    //
    // The non-obvious part: setting the top [SystemSettings] block is NOT enough. The
    // quality presets [SystemSettingsBucket4] and [SystemSettingsBucket5] set
    // MotionBlur=True again whenever a graphics preset is applied, and the Default*
    // keys turn blur and chromatic aberration back on whenever the video menu is reset
    // to defaults. So force EVERY line, in every section — seven of them on a stock
    // install had to be flipped.
    //
    // WINDOWED MODE is in the same list, and it is not a preference — it is a hard
    // requirement. We render 3184x3500 on a 2560x1440 desktop; exclusive fullscreen
    // cannot present a surface bigger than the display mode, so the whole render-size
    // solve depends on the game running in a window. JJ's known-good pair is
    // `Fullscreen=False` + `WindowDisplayMode=0`, so that is exactly what we write.
    // (This is the CONFIG route. hooks.cpp also has a `windowed` escape hatch that
    // forces it at swapchain creation — that one stays DEFAULT OFF, because forcing it
    // there pushed the window behind the desktop and killed the controller.)
    // `rescue` keys are NOT forced to our value — they are only RAISED to it when the
    // game has left them below 60. That is the difference between rescuing a player
    // from the engine's unusable 30 fps default and stamping on a frame cap they chose
    // deliberately in the video menu. Never overwrite a considered user setting.
    struct ConfigForce { const char* key; const char* value; bool rescue; };
    const ConfigForce kComfortKeys[] = {
        { "MotionBlur=",               "False" , false },
        { "DefaultMotionBlur=",        "False" , false },
        { "ChromaticAberration=",      "False" , false },
        { "DefaultChromaticAberration=", "False" , false },
        { "FilmGrain=",                "False" , false },
        { "DefaultFilmGrain=",         "False" , false },
        { "UseVsync=",                 "False" , false },
        { "DefaultUseVsync=",          "False" , false },
        { "UseAdaptiveVsync=",         "False" , false },
        { "DefaultUseAdaptiveVsync=",  "False" , false },
        { "Fullscreen=",               "False" , false },
        { "DefaultFullscreen=",        "False" , false },
        { "WindowDisplayMode=",        "0"     , false },
        { "DefaultWindowDisplayMode=", "0"     , false },
        // AER draws the two eyes on CONSECUTIVE frames, so anything that widens the gap
        // between them shows up as the eyes disagreeing — which reads exactly like the
        // old "stereo won't sync" and "head yaw tilts the horizon" bugs. UE3's
        // one-frame thread lag is precisely that: an extra frame between the game
        // thread's pose and what the render thread draws. It is NOT in
        // DefaultSystemSettings.ini, so a fresh install inherits the engine's compiled
        // default of True and a new user would meet both symptoms on day one.
        { "OneFrameThreadLag=",        "False" , false },
        // The game's own default is 30, which is unusable in a headset. See the
        // regeneration note below for why this key is in the list at all.
        { "MaxFPS=",                   "120"   , true },
        { "DefaultMaxFPS=",            "120"   , true },
    };

    int g_comfortChanged = -1;      // -1 = not attempted, else lines flipped

    // Is geo-11 installed alongside us? `d3dxdm.ini` is its stereo config and
    // nothing else ships that name, so its presence in Binaries\Win64 is a
    // reliable "the scene is drawn twice per frame" flag — and we need the answer
    // in DllMain, before geo-11's d3d11.dll is necessarily loaded.
    bool g_geo11 = false;

    void detect_geo11(HINSTANCE self)
    {
        wchar_t p[MAX_PATH];
        if (!GetModuleFileNameW(self, p, MAX_PATH)) return;
        for (int i = (int)wcslen(p) - 1; i >= 0; --i)
            if (p[i] == L'\\' || p[i] == L'/') { p[i + 1] = 0; break; }
        if (wcslen(p) + 16 >= MAX_PATH) return;
        wcscat_s(p, MAX_PATH, L"d3dxdm.ini");
        g_geo11 = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    }

    int force_comfort_in(const wchar_t* path)
    {
        // Keep the player's untouched original the first time we ever edit it.
        wchar_t bak[MAX_PATH];
        wcscpy_s(bak, MAX_PATH, path);
        if (wcslen(bak) + 16 < MAX_PATH)
        {
            wcscat_s(bak, MAX_PATH, L".akvr-original");
            CopyFileW(path, bak, TRUE);          // TRUE = don't overwrite an existing one
        }

        FILE* f = _wfopen(path, L"rb");
        if (!f) return -1;                       // not generated yet — that's expected
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n <= 0 || n > (16 << 20)) { fclose(f); return -1; }
        char* in = (char*)malloc((size_t)n + 1);
        if (!in) { fclose(f); return -1; }
        size_t got = fread(in, 1, (size_t)n, f);
        fclose(f);
        in[got] = 0;

        // Worst case a line grows by 5 bytes ("True" -> "False" plus slack), so double
        // the input and add headroom rather than doing arithmetic we could get wrong.
        size_t cap = got * 2 + 1024;
        char* out = (char*)malloc(cap);
        if (!out) { free(in); return -1; }

        size_t w = 0, i = 0;
        int changed = 0;
        while (i < got)
        {
            size_t ls = i;
            while (i < got && in[i] != '\r' && in[i] != '\n') ++i;
            size_t lineLen = i - ls;
            size_t es = i;                       // the line ending, copied verbatim
            while (i < got && (in[i] == '\r' || in[i] == '\n'))
            {
                ++i;
                if (i < got && in[i - 1] == '\n') break;   // one ending only
            }

            const ConfigForce* force = nullptr;
            for (size_t k = 0; k < sizeof(kComfortKeys) / sizeof(kComfortKeys[0]); ++k)
            {
                size_t klen = strlen(kComfortKeys[k].key);
                if (lineLen >= klen && memcmp(in + ls, kComfortKeys[k].key, klen) == 0)
                { force = &kComfortKeys[k]; break; }
            }

            // Three keys are AER-build measures with no reason to exist on the geo-11
            // build, and the geo-11 install we have is KNOWN-GOOD with the game's own
            // values — so changing them would put an untested variable into the one
            // test that matters. Stand down on all three when geo-11 is present:
            //   OneFrameThreadLag — forced because AER draws the eyes on CONSECUTIVE
            //     frames, so an extra frame of game->render lag made them disagree.
            //     geo-11 draws both eyes in the SAME frame; the reason evaporates, and
            //     a stereo injector is exactly what game/render-thread sync affects.
            //   Fullscreen / WindowDisplayMode — forced windowed ONLY because we
            //     rendered bigger than the desktop and exclusive fullscreen cannot
            //     present that. In geo-11 mode we do not force the render size at all.
            // 2026-09-26 (MODELIST): the window keys are FORCED again under geo-11 when
            // we force the render size (always, since ENGINE169). The game had reset
            // itself to Fullscreen=True / WindowDisplayMode=2, and exclusive fullscreen
            // only accepts monitor modes — it fell back to 2560x1600 and drew the 3D in
            // a corner of the 3072x2688 frame. OneFrameThreadLag still stands down (True
            // measured 44 -> 84 fps under geo-11).
            const bool windowKey =
                strcmp(force ? force->key : "", "Fullscreen=") == 0 ||
                strcmp(force ? force->key : "", "DefaultFullscreen=") == 0 ||
                strcmp(force ? force->key : "", "WindowDisplayMode=") == 0 ||
                strcmp(force ? force->key : "", "DefaultWindowDisplayMode=") == 0;
            // THREADSYNC 2026-10-02: OneFrameThreadLag is forced False under geo-11 too now. With the lag on, the world is
            // drawn one frame behind the camera the game used for the rain and the HUD markers (they needed pose delay 2,
            // the world 3); off, JJ: the reticle and the distance marker "look perfectly stable now. And locked to their
            // targets". It crashed twice with the test tools on, not since they are off (FIX_CHANGES 6).
            if (force && g_geo11 && windowKey && !g_engH)
                force = nullptr;

            // A rescue key whose current value is already sane is left completely
            // alone — copied through as if we had never matched it.
            if (force && force->rescue)
            {
                const size_t klen = strlen(force->key);
                char num[32] = { 0 };
                size_t vn = lineLen - klen;
                if (vn > sizeof(num) - 1) vn = sizeof(num) - 1;
                memcpy(num, in + ls + klen, vn);
                if (atof(num) >= 60.0) force = nullptr;
            }

            if (force)
            {
                const size_t klen = strlen(force->key);
                const size_t vlen = strlen(force->value);
                if (lineLen != klen + vlen || memcmp(in + ls + klen, force->value, vlen) != 0)
                    ++changed;
                if (w + klen + vlen + (i - es) + 1 >= cap) break;
                memcpy(out + w, force->key,   klen);   w += klen;
                memcpy(out + w, force->value, vlen);   w += vlen;
            }
            else
            {
                if (w + lineLen + (i - es) + 1 >= cap) break;
                memcpy(out + w, in + ls, lineLen);     w += lineLen;
            }
            memcpy(out + w, in + es, i - es);          w += (i - es);
        }

        if (changed > 0)
        {
            FILE* o = _wfopen(path, L"wb");
            if (o) { fwrite(out, 1, w, o); fclose(o); }
        }
        free(out);
        free(in);
        return changed;
    }

    // MEASURED 2026-08-05: on a FRESH install `BmSystemSettings.ini` does not exist yet
    // — UE3 generates it from `DefaultSystemSettings.ini` on first launch (the shipped
    // `UserSystemSettings.ini` is UTF-16 and literally says
    // `BasedOn=..\BmGame\Config\DefaultSystemSettings.ini`). Editing only the generated
    // file would therefore do nothing on a new user's first run, which is exactly the
    // run that has to be right. So force BOTH: the template the game builds from, and
    // the generated file if it is already there. Doing both also survives UE3
    // regenerating the config later, e.g. after a game patch bumps [IniVersion].
    // Note DefaultSystemSettings ships `ChromaticAberration=True` and `MotionBlur=True`
    // in buckets 4 and 5, so on a stock install this is doing real work.
    void force_comfort_settings()
    {
        wchar_t dir[MAX_PATH];
        if (!GetModuleFileNameW(GetModuleHandleW(nullptr), dir, MAX_PATH)) return;
        // ...\Binaries\Win64\BatmanAK.exe -> strip exe name, then Win64, then Binaries
        for (int k = 0; k < 3; ++k)
        {
            wchar_t* slash = wcsrchr(dir, L'\\');
            if (!slash) return;
            *slash = 0;
        }
        if (wcslen(dir) + 48 >= MAX_PATH) return;

        // COST JJ A SESSION AT 30 FPS, 2026-08-05 — read this before touching the
        // order below. UE3 REGENERATES BmSystemSettings.ini from
        // DefaultSystemSettings.ini whenever the template is newer. Build WINDOWED
        // edited the template on an install that already had a generated file, so the
        // game threw JJ's whole tuned config away and rebuilt it from stock: MaxFPS
        // went 120 -> 30 (the shipped default), OneFrameThreadLag flipped, draw-distance
        // and mesh-detail scales reset, and the frame rate fell to 30.
        //
        // So: touch the TEMPLATE ONLY when the generated file does not exist yet, which
        // is exactly the fresh-install case it was added for. On an install that already
        // has a config we edit that file and nothing else, so nothing can trigger a
        // regeneration. MaxFPS is in the key list as well, so that even if some other
        // agent regenerates the config, it cannot come back at 30.
        wchar_t gen[MAX_PATH];
        wcscpy_s(gen, MAX_PATH, dir);
        wcscat_s(gen, MAX_PATH, L"\\BmGame\\Config\\BmSystemSettings.ini");

        int changed = force_comfort_in(gen);
        const char* which = "generated config";
        if (changed < 0)
        {
            wchar_t tmpl[MAX_PATH];
            wcscpy_s(tmpl, MAX_PATH, dir);
            wcscat_s(tmpl, MAX_PATH, L"\\BmGame\\Config\\DefaultSystemSettings.ini");
            changed = force_comfort_in(tmpl);
            which = "template (fresh install)";
        }
        g_comfortChanged = changed;
        log_add("comfort: motion blur / v-sync / chromatic aberration off, windowed on, "
                "MaxFPS 120 — %s, %d lines flipped%s", which, changed,
                g_geo11 ? "  [geo-11: OneFrameThreadLag forced False too (THREADSYNC)]" : "");
    }

    bool eng_write(void* at, const void* src, size_t n)
    {
        DWORD old = 0;
        if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(at, src, n);
        VirtualProtect(at, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, n);
        return true;
    }

    void install_engine_res()
    {
        if (g_engOn || g_engW < 64 || g_engH < 64) return;

        // Image extent straight from the PE headers — psapi is another DLL to pull
        // in, and this runs under the loader lock.
        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return;
        uint8_t* base = (uint8_t*)exe;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        size_t size = nt->OptionalHeader.SizeOfImage;

        const size_t n = sizeof(kEngPat);
        uint8_t* hit = nullptr;
        for (size_t i = 0; i + n <= size; ++i)
        {
            if (base[i] != 0x44 || base[i + 1] != 0x39) continue;   // cheap prefilter
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
                if (kEngMask[j] && base[i + j] != kEngPat[j]) { ok = false; break; }
            if (ok) { hit = base + i; break; }
        }
        if (!hit) { log_add("engine-res: signature NOT FOUND — size left to the game"); return; }
        g_engAddr = (uintptr_t)hit;

        // Decode the two globals out of the block's own RIP-relative operands.
        //   hit+9  : 8B 05 disp32   mov eax,[ResX]   (next instruction at hit+15)
        //   hit+23 : 8B 05 disp32   mov eax,[ResY]   (next instruction at hit+29)
        int32_t dX = *(int32_t*)(hit + 11);
        int32_t dY = *(int32_t*)(hit + 25);
        g_engResX = (uintptr_t)(hit + 15) + dX;
        g_engResY = (uintptr_t)(hit + 29) + dY;

        // Always take the block: je -> nop nop.
        const uint8_t nops2[2] = { 0x90, 0x90 };
        if (!eng_write(hit + 7, nops2, 2)) { log_add("engine-res: could not unlock the branch"); return; }

        // 28 bytes: two RIP-relative dword stores (10 each) + 8 NOPs.
        uint8_t blk[28];
        memset(blk, 0x90, sizeof(blk));
        uint8_t* p = blk;
        uintptr_t after1 = (uintptr_t)hit + 9 + 10;    // next instruction after store 1
        uintptr_t after2 = after1 + 10;                // next instruction after store 2
        p[0] = 0xC7; p[1] = 0x05;
        *(int32_t*)(p + 2) = (int32_t)((intptr_t)g_engResX - (intptr_t)after1);
        *(int32_t*)(p + 6) = g_engW;
        p += 10;
        p[0] = 0xC7; p[1] = 0x05;
        *(int32_t*)(p + 2) = (int32_t)((intptr_t)g_engResY - (intptr_t)after2);
        *(int32_t*)(p + 6) = g_engH;

        if (!eng_write(hit + 9, blk, sizeof(blk)))
        { log_add("engine-res: could not write the block"); return; }

        g_engOn = true;
        log_add("engine-res: FORCING %dx%d  (site %p, ResX %p, ResY %p)",
                g_engW, g_engH, (void*)hit, (void*)g_engResX, (void*)g_engResY);
    }

    // =======================================================================
    // HUD SIZE — live, no restart. Found by RE 2026-08-05 (second pass).
    //
    // AK's UI is Scaleform, so there is no UE3 Canvas to shrink and no config key
    // (HudCanvasScale is inert — the name isn't even in the exe). The FIRST attempt
    // chased a "reference resolution" pair; that is DEAD (writes stuck, gate forced,
    // no visual change) — see the ak-hud-too-big-in-vr memory, do not re-walk it.
    //
    // The real chokepoint is the one guide 11 describes: every Scaleform movie's
    // viewport is built from ONE global screen rectangle, L/T/R/B as four ints,
    // shipped as (0, 0, 1280, 720). Three separate call sites do exactly:
    //
    //     movie->GetViewport(&vp);
    //     vp.Left = L;  vp.Top = T;  vp.Width = R - L;  vp.Height = B - T;
    //     movie->SetViewport(&vp);           // vtable+0x70 (GetViewport is +0x78)
    //
    // ...so shrinking that rectangle and re-centring it shrinks ALL UI uniformly.
    // That is Anvil's onCalcUIViewportHook and Starfield's SetViewport rescale,
    // except AK hands us the rectangle as plain data, so no hook is needed at all.
    //
    // The rectangle has exactly ONE writer in the whole image — a
    // SetUIViewport(w, h, mode) that runs on a resolution change and nowhere else:
    //     L = 0, T = 0, R = w, B = h
    //     ...unless the display is WIDER than 16:9, in which case it already
    //        pillarboxes: R = h * 1.7778, L = (w - h*1.7778) * 0.5
    // Our portrait render is far NARROWER than 16:9, so that branch never fires and
    // the UI gets the full frame — which is precisely why it fills the headset.
    //
    // Because the game writes it only on a resolution change, a per-frame store from
    // us wins outright. Addresses are decoded from the matched signature (ASLR-safe),
    // never hardcoded.
    //
    // Byte layout of the matched site — every instruction LENGTH written out first,
    // because getting these off by 1-3 bytes decoded garbage pointers last time and
    // writing through them killed the render thread:
    //   +0   48 C7 05 d32 imm32   mov qword [L], 0   (11)  end +11 -> L = hit+11+d
    //   +11  89 35 d32            mov [R], esi       ( 6)  end +17 -> R = hit+17+d
    //   +17  89 1D d32            mov [B], ebx       ( 6)  end +23 -> B = hit+23+d
    //   +23  F3 48 0F 2A FB       cvtsi2ss xmm7, rbx ( 5)
    //   +28  0F 28 C6             movaps xmm0, xmm6  ( 3)
    //   +31  0F 28 CF             movaps xmm1, xmm7  ( 3)
    // T is L+4 (the qword store above zeroes L and T together), and R/B must land at
    // L+8 / L+12 — a self-check strong enough that a bad decode cannot survive it.
    const uint8_t kHudPat[] = {
        0x48,0xC7,0x05,0,0,0,0, 0,0,0,0,
        0x89,0x35,0,0,0,0,
        0x89,0x1D,0,0,0,0,
        0xF3,0x48,0x0F,0x2A,0xFB,
        0x0F,0x28,0xC6,
        0x0F,0x28,0xCF };
    const bool kHudMask[] = {
        1,1,1,0,0,0,0, 1,1,1,1,
        1,1,0,0,0,0,
        1,1,0,0,0,0,
        1,1,1,1,1,
        1,1,1,
        1,1,1 };

    float  g_hudScale  = 1.0f;      // 1 = the game's own size
    bool   g_hudAspectFix = true;
    float  g_hudAspectApplied = 1.0f;

    // MEASURED 2026-08-05: with `hudscale = 0.60`, an F2 capture of a loading screen
    // showed its 16:9 image occupying exactly **59% of the frame width** — our own HUD
    // shrink, applied to a screen that is nothing BUT UI. Pulling the HUD in from the
    // edges is right when it overlays a 3D scene; on a 2D screen the picture is the
    // whole point, and the shrink only wraps it in black which we then present at the
    // same angular size. Hence: scale during gameplay, hands off otherwise.
    bool   g_hudGameplay = true;
    // MENU3D: the pinned main-menu text is sized by the main-menu screen size.
    // ENTRYFIX 2026-09-26 — JJ: the loading screen after the main menu "is a certain
    // size and then suddenly gets larger". The main-menu phase lasts until 5 s of screen,
    // so the first 5 s of that loading screen got the main-menu text size. The menu size
    // now applies only while the menu's live camera runs (g_hudGameplay).
    float  hud_eff()   { return (akvr_xr_menu3d_active() && g_hudGameplay) ? akvr_xr_menu_zoom()
                                : (g_hudGameplay ? g_hudScale : 1.0f); }
    // UIGATES: separate HEIGHT scale (JJ: "separate the top and bottom HUD elements from
    // the left and right ones ... two separate sliders"). Width is g_hudScale.
    float  g_hudScaleV = 1.0f;
    // Menus/pause/map: let the UI use the FULL frame height (JJ: the map HUD "is still
    // conforming to a sixteen by nine aspect ratio, so the top part needs to go up and
    // the bottom part needs to go down"). The pixel aspect keeps it unstretched.
    bool   g_menuUiFill = true;
    // HUDAREA 2026-09-26: JJ's radar blinks (whole frames, both eyes, worst when a
    // popup appears) with any HUD size/position, and never at the defaults. Besides the
    // per-movie viewports we also rewrite the game's ONE global UI rectangle every
    // Present, from our thread. This switch turns only that write off (A/B), and
    // g_uiStolen counts the frames the game had put its own value back.
    bool   g_hudGlobal = true;
    long   g_uiStolen  = 0;
    // HUDPOS 2026-09-26: a separate HEIGHT scale does not work on this game — JJ
    // measured it: the HUD is laid out as ONE picture (height alone just re-cropped).
    // So height follows width, and the vertical control is a POSITION instead.
    float  hud_eff_v() { return hud_eff(); }
    float  g_hudRaise = 0.0f;   // + = up, as a fraction of the frame height (gameplay HUD)
    void   menu3d_shift(int W, int H, int& dx, int& dy);
    int    hud_up_px(int H);
    // The ONE place the HUD rectangle is computed — for each movie's viewport and for
    // the game's global UI rectangle alike, so the two can never disagree (their
    // disagreement is what re-laid-out movies every frame).
    //   width  = W x width scale
    //   height = H x height scale x f (the projVR squash), or the FULL height on
    //            menu screens (g_menuUiFill), where the pixel aspect alone un-stretches
    //   raised by hud_up_px (HUDUP: centred on straight ahead, not on the off-centre
    //   frame), plus the MENU3D pin shift, then kept inside the frame.
    void hud_rect(int W, int H, float f, bool squash, int& l, int& t, int& w, int& h)
    {
        w = (int)(W * hud_eff());
        h = (int)(H * hud_eff_v());
        if (squash && f < 1.0f)
            h = (!g_hudGameplay && g_menuUiFill) ? H : (int)(h * f);
        else if (squash)
            w = (int)(w / f);
        if (w < 64) w = 64;
        if (h < 64) h = 64;
        int sx = 0, sy = 0; menu3d_shift(W, H, sx, sy);
        l = (W - w) / 2 + sx;
        t = (H - h) / 2 - (squash ? hud_up_px(H) : 0) + sy
            - (g_hudGameplay ? (int)(g_hudRaise * (float)H) : 0);   // HUDPOS
        {
            if (h <= H) t = t < 0 ? 0 : (t > H - h ? H - h : t);
            if (w <= W) l = l < 0 ? 0 : (l > W - w ? W - w : l);
        }
    }
    // MENU3D: pixels to slide a menu so it stays where it was in the world while the
    // camera follows the head (the angle the camera turned, through the projection).
    void menu3d_shift(int W, int H, int& dx, int& dy)
    {
        dx = dy = 0;
        float dyaw = 0.0f, dpitch = 0.0f;
        if (!akvr_xr_menu3d_offset(dyaw, dpitch)) return;
        float hH = 0.8f, hV = 0.8f; akvr_xr_eye_half_fov(hH, hV);
        const float k = 3.14159265f / 180.0f;
        dyaw   = dyaw   < -80.0f ? -80.0f : (dyaw   > 80.0f ? 80.0f : dyaw);
        dpitch = dpitch < -80.0f ? -80.0f : (dpitch > 80.0f ? 80.0f : dpitch);
        dx = (int)(tanf(dyaw * k)   / tanf(hH) * W * 0.5f);   // head left -> text right
        dy = (int)(tanf(dpitch * k) / tanf(hV) * H * 0.5f);   // head up   -> text down
    }
    int*   g_uiL = nullptr;         // the one UI viewport rectangle, as four ints
    int*   g_uiT = nullptr;
    int*   g_uiR = nullptr;
    int*   g_uiB = nullptr;
    bool   g_hudFound  = false;

    // The game's own rectangle, captured before we ever touch it. Everything is
    // scaled relative to THIS, so "back to 1.00" is exact and a resolution change
    // re-baselines itself.
    int    g_uiBaseL = 0, g_uiBaseT = 0, g_uiBaseR = 0, g_uiBaseB = 0;
    bool   g_uiHaveBase = false;
    int    g_uiWroteL = 0, g_uiWroteT = 0, g_uiWroteR = 0, g_uiWroteB = 0;
    bool   g_uiWrote   = false;
    bool   g_hudSticks = true;
    char   g_hudDiag[640] = "hud: not probed yet";

    void find_hud_scale()
    {
        static bool tried = false;
        if (tried) return;
        tried = true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return;
        uint8_t* base = (uint8_t*)exe;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        size_t size = nt->OptionalHeader.SizeOfImage;

        const size_t n = sizeof(kHudPat);
        uint8_t* hit = nullptr;
        for (size_t i = 0; i + n <= size; ++i)
        {
            if (base[i] != 0x48 || base[i + 1] != 0xC7 || base[i + 2] != 0x05) continue;
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
                if (kHudMask[j] && base[i + j] != kHudPat[j]) { ok = false; break; }
            if (ok) { hit = base + i; break; }
        }
        if (!hit) { log_add("hud-rect: signature NOT FOUND"); return; }

        int* L = (int*)((hit + 11) + *(int32_t*)(hit + 3));
        int* R = (int*)((hit + 17) + *(int32_t*)(hit + 13));
        int* B = (int*)((hit + 23) + *(int32_t*)(hit + 19));
        int* T = L + 1;

        auto in_image = [&](void* p) {
            return (uint8_t*)p >= base && (uint8_t*)p < base + size
                && (((uintptr_t)p) & 3) == 0;
        };
        // Fail safe, never crash: both a range check AND the layout self-check.
        if (!in_image(L) || !in_image(R) || !in_image(B) ||
            R != L + 2 || B != L + 3)
        {
            log_add("hud-rect: decode failed — DISABLED (L %p R %p B %p)",
                    (void*)L, (void*)R, (void*)B);
            return;
        }

        g_uiL = L; g_uiT = T; g_uiR = R; g_uiB = B;
        g_hudFound = true;
        log_add("hud-rect: found at %p — game's UI rect is %d,%d..%d,%d",
                (void*)L, *L, *T, *R, *B);
    }

    // -----------------------------------------------------------------------
    // MEASURED 2026-08-05 (build UIRECT): the rectangle above works — our values
    // stick and the LOADING SCREEN shrank — but the gameplay HUD and the menu
    // screens did not move. Reason found: the per-frame loop over active movies at
    // 0x1405cf220 only feeds a movie the global rectangle if it passes three gates,
    // otherwise it hands that movie the WHOLE render target instead:
    //
    //   +17  jne skip   <- movie flag 0x1000 set in [movieDef+0x100]   (per movie)
    //   +29  jne skip   <- display is ULTRAWIDE (aspect > 16:9, < 4:1) (per frame)
    //   +53  jne skip   <- [movieDef+0x110] is non-null                (per movie)
    //
    // Gate 2 already passes for us — our render is far narrower than 16:9, and the
    // loading screen proves the path is reachable — so it is left completely alone
    // (it is genuinely about display aspect). Gates 1 and 3 are the per-movie ones
    // that exclude the HUD and the menus, and neutralising them simply makes every
    // movie use the same rectangle. Two 2-byte NOPs, applied only while the slider
    // is away from 1.00 and restored exactly when it comes back.
    const uint8_t kGatePat[] = {
        0x48,0x8B,0x81,0x80,0x00,0x00,0x00,                     // mov rax,[rcx+0x80]
        0xF7,0x80,0x00,0x01,0x00,0x00,0x00,0x10,0x00,0x00,      // test [rax+0x100],0x1000
        0x75,0x00,                                              // +17 jne  (gate 1)
        0x49,0x8B,0xCD,                                         // mov rcx,r13
        0xE8,0,0,0,0,                                           // call is-ultrawide
        0x85,0xC0,                                              // test eax,eax
        0x75,0x00,                                              // +29 jne  (gate 2)
        0x4C,0x8B,0x43,0x1C,                                    // mov r8,[rbx+0x1c]
        0x49,0x8B,0x04,0xF0,                                    // mov rax,[r8+rsi*8]
        0x48,0x8B,0x88,0x80,0x00,0x00,0x00,                     // mov rcx,[rax+0x80]
        0x48,0x39,0xB9,0x10,0x01,0x00,0x00,                     // cmp [rcx+0x110],rdi
        0x75,0x00 };                                            // +53 jne  (gate 3)
    const bool kGateMask[] = {
        1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,
        1,0,
        1,1,1,
        1,0,0,0,0,
        1,1,
        1,0,
        1,1,1,1,
        1,1,1,1,
        1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,
        1,0 };

    uint8_t* g_gateSite  = nullptr;
    uint8_t  g_gateOrig[4] = { 0, 0, 0, 0 };   // the two jne opcodes+rel8 we replace
    bool     g_gatesOpen = false;

    void find_hud_gates()
    {
        static bool tried = false;
        if (tried) return;
        tried = true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return;
        uint8_t* base = (uint8_t*)exe;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        size_t size = nt->OptionalHeader.SizeOfImage;

        const size_t n = sizeof(kGatePat);
        for (size_t i = 0; i + n <= size; ++i)
        {
            if (base[i] != 0x48 || base[i + 1] != 0x8B || base[i + 2] != 0x81) continue;
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
                if (kGateMask[j] && base[i + j] != kGatePat[j]) { ok = false; break; }
            if (ok) { g_gateSite = base + i; break; }
        }
        if (!g_gateSite) { log_add("hud-gates: signature NOT FOUND (HUD stays full-frame)"); return; }

        memcpy(g_gateOrig,     g_gateSite + 17, 2);
        memcpy(g_gateOrig + 2, g_gateSite + 53, 2);
        log_add("hud-gates: found at %p (gate1 %02X %02X, gate3 %02X %02X)",
                (void*)g_gateSite, g_gateOrig[0], g_gateOrig[1], g_gateOrig[2], g_gateOrig[3]);
    }

    void set_hud_gates(bool open)
    {
        if (!g_gateSite || g_gatesOpen == open) return;
        const uint8_t nops[2] = { 0x90, 0x90 };
        if (open)
        {
            if (!eng_write(g_gateSite + 17, nops, 2)) return;
            if (!eng_write(g_gateSite + 53, nops, 2)) { eng_write(g_gateSite + 17, g_gateOrig, 2); return; }
        }
        else
        {
            eng_write(g_gateSite + 17, g_gateOrig,     2);
            eng_write(g_gateSite + 53, g_gateOrig + 2, 2);
        }
        g_gatesOpen = open;
    }

    // =======================================================================
    // THE HUD, PROPERLY — rewrite the viewport Scaleform is actually handed.
    //
    // MEASURED 2026-08-05 (build UIGATE): the global rectangle sticks, the gates are
    // open, and the gameplay HUD and menus STILL do not move. So they are not fed by
    // that rectangle at all — something else hands them their viewport. Chasing which
    // caller it is has now cost two builds, so stop chasing callers and take the
    // chokepoint every caller must pass through, exactly as guide 11 prescribes:
    // Scaleform's own `GFxMovieView::SetViewport`.
    //
    // We reach it without RTTI (stripped) and without a fixed address, because the
    // per-frame movie loop hands us live objects:
    //     loop object -> [+0x1c] = array of movies, [+0x24] = count
    //     movie       -> [+0x3c] = the GFxMovieView (note: NOT 8-aligned)
    //     view        -> [0]     = vtable, +0x70 = SetViewport, +0x78 = GetViewport
    // So: hook the loop (a clean 2-argument function), let it run, then for every
    // movie re-push a scaled viewport on the game's own thread — and patch the
    // vtable slot once so every LATER SetViewport from any path is scaled too.
    //
    // The rescale is written from the movie's own BUFFER size, never multiplied onto
    // whatever was there, so applying it twice is identical to applying it once. That
    // is what makes it safe to keep the global-rectangle lever above at the same time.
    struct GfxViewport                      // Scaleform Viewport, verified by RE
    {
        int   bufW, bufH;
        int   left, top, width, height;
        int   scLeft, scTop, scWidth, scHeight;
        int   flags;
        float scale, aspect;
    };
    // BatmanAK SetViewport compares/copies exactly 0x34 bytes. Its layout
    // calculation uses +0x2c as uniform scale and +0x30 as pixel aspect.
    static_assert(sizeof(GfxViewport) == 0x34);
    static_assert(offsetof(GfxViewport, aspect) == 0x30);

    struct MovieAspect { void* view = nullptr; AkvrHudAspectState state; };
    MovieAspect g_movieAspects[64];
    SRWLOCK g_aspectLock = SRWLOCK_INIT;

    // 2026-09-26 (build HUDFIT): the squash follows the PICTURE, not the mode. Pause
    // and the main menu are drawn through projVR exactly like gameplay (F2 captures),
    // so their UI needs the same correction; loading screens and splash are flat
    // pictures and must not get it. hooks.cpp decides from projection cadence.
    bool   g_hudAnamorphic = false;

    float hud_aspect_factor(int width, int height)
    {
        if (!g_hudAspectFix || !g_hudAnamorphic || !akvr_projvr()
            || !akvr_xr_fov_measured()) return 1.0f;
        float halfH = 0.0f, halfV = 0.0f;
        akvr_xr_eye_half_fov(halfH, halfV);
        return akvr_hud_pixel_aspect(width, height, halfH, halfV);
    }

    // HUDUP: how far (pixels, of a buffer H tall) to raise a HUD that is squashed for
    // projVR so it is centred on straight ahead rather than on the off-centre frame.
    int hud_up_px(int H)
    {
        const float o = akvr_xr_eye_v_offset();
        return (o > 0.0f && o < 0.9f) ? (int)(o * (float)H * 0.5f) : 0;
    }

    bool correct_movie_aspect(void* view, GfxViewport& vp)
    {
        if (!view) return false;
        const float factor = hud_aspect_factor(vp.bufW, vp.bufH);
        bool changed = false;
        AcquireSRWLockExclusive(&g_aspectLock);
        MovieAspect* slot = nullptr;
        for (auto& entry : g_movieAspects)
            if (entry.view == view) { slot = &entry; break; }
        if (!slot && std::fabs(factor - 1.0f) > 0.00001f)
            for (auto& entry : g_movieAspects)
                if (!entry.view) { entry.view = view; slot = &entry; break; }
        if (slot)
        {
            const float desired = slot->state.apply(vp.aspect, factor);
            changed = std::isfinite(desired) && std::fabs(desired - vp.aspect) > 0.00001f;
            if (changed) vp.aspect = desired;
        }
        ReleaseSRWLockExclusive(&g_aspectLock);
        return changed;
    }

    // VPLOG 2026-09-26: HUD and map icons still flicker after two fixes that were
    // reasoned, not measured. Record every viewport set — who set it (game through
    // our vtable hook 'V', our post-fix 'F', our live re-push 'P'), what came in and
    // what went out — and dump it with F1 (akvr_*_viewports.csv). Fixed ring, no
    // allocation, cheap enough to leave on.
    struct VpRec {
        double t; void* view; DWORD tid; char src;
        int bufW, bufH, inL, inT, inW, inH, outL, outT, outW, outH;
        float inAspect, outAspect;
    };
    VpRec g_vpLog[4096];
    volatile LONG g_vpLogN = 0;
    void vp_log(char src, void* view, const GfxViewport& in, const GfxViewport& out)
    {
        const LONG i = InterlockedIncrement(&g_vpLogN) - 1;
        VpRec& r = g_vpLog[i & 4095];
        LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        r.t = 1000.0 * (double)q.QuadPart / (double)f.QuadPart;
        r.view = view; r.tid = GetCurrentThreadId(); r.src = src;
        r.bufW = in.bufW; r.bufH = in.bufH;
        r.inL = in.left; r.inT = in.top; r.inW = in.width; r.inH = in.height; r.inAspect = in.aspect;
        r.outL = out.left; r.outT = out.top; r.outW = out.width; r.outH = out.height; r.outAspect = out.aspect;
    }

    typedef void (*SetViewportFn)(void* view, const GfxViewport* vp);
    typedef void (*GetViewportFn)(void* view, GfxViewport* vp);
    typedef void* (*MovieLoopFn)(void* thiz, void* arg);

    MovieLoopFn   oMovieLoop = nullptr;
    uint8_t*      g_loopSite = nullptr;
    long          g_loopCalls = 0;
    long          g_scaled    = 0;

    void**        g_vtHooked[8] = { nullptr };
    SetViewportFn g_vtOrigSet[8] = { nullptr };
    int           g_vtCount = 0;

    // Live slider. A movie only re-reads its viewport when the game sets one, so a
    // new slider value would otherwise not show until the next menu or level. We can
    // re-push it ourselves — but ONLY from the thread the game itself uses for GFx,
    // never cross-thread. That thread id is recorded the first time the game calls in;
    // if Present turns out to run somewhere else, the live push simply never happens
    // and the diagnostic says so, instead of us racing the game's UI.
    void*  g_views[32] = { nullptr };
    int    g_nViews    = 0;

    // HUDPIECES 2026-09-26 — JJ's radar vanishes whole frames whenever another HUD
    // element pops up, but ONLY while the HUD is shrunk. Guide 11: tell movies apart
    // by their file URL (Starfield's GetFileURL table) and treat them separately.
    // The movie-view vtable slot 1 is `mov rax,[rcx+0x48]; ret` (BatmanAK.exe
    // 0x1415447f0, vtable 0x142CC3150) = GetMovieDef. The def's URL string is found by
    // a guarded scan of the def (pointers to C strings, or GFx GString data at +0x0C),
    // accepting only text that contains ".swf" / ".gfx". Each piece can be left
    // full-size from the panel; excluded names persist in akvr_settings.ini.
    struct MovieInfo { void* view; char name[96]; int tries; bool shrink; int bufW, bufH; bool screenSeen;
                       int origMode; float stageVisPx;       // SCALEMODE
                       uintptr_t def; float ps, pdx, pdy; };  // PIECEID / PIECEXF
    // RADARTEX 2026-09-26: shrink a screen piece also while it draws into its own texture.
    bool   g_hudDual = true;
    MovieInfo g_movies[64] = {};
    int    g_nMovies = 0;
    int    g_shrinkGen = 0;                 // bumps the live push when a switch changes
    char   g_noShrink[1024] = "";           // ";name;name;" of pieces left full size

    bool looks_like_url(const char* s)
    {
        int n = 0; bool dot = false;
        for (; n < 95 && s[n]; ++n)
        {
            const unsigned char c = (unsigned char)s[n];
            if (c < 0x20 || c > 0x7e) return false;
        }
        if (n < 5 || s[n] != 0) return false;
        for (int i = 0; i + 3 < n; ++i)
        {
            char a = s[i], b = (char)(s[i+1] | 0x20), c = (char)(s[i+2] | 0x20), d = (char)(s[i+3] | 0x20);
            if (a == '.' && ((b == 's' && c == 'w' && d == 'f') || (b == 'g' && c == 'f' && d == 'x'))) dot = true;
        }
        return dot;
    }
    bool readable_ptr(uintptr_t p) { return p > 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 3) == 0; }
    bool try_string(uintptr_t p, char* out)
    {
        const uintptr_t cands[3] = { p, (p & ~(uintptr_t)3) + 12, (p & ~(uintptr_t)3) + 16 };
        for (uintptr_t cand : cands)
        {
            if (!readable_ptr(cand & ~(uintptr_t)3)) continue;
            const char* s = (const char*)cand;
            if (looks_like_url(s)) { strncpy_s(out, 96, s, _TRUNCATE); return true; }
        }
        return false;
    }
    // Name-like text: printable, 6..90 chars, has a letter and one of . _ / \ (a
    // package path "UI_HUD.Radar" or a file name). ASCII or UTF-16 (UE3 FString).
    bool name_like_ascii(const char* s, char* out)
    {
        int n = 0; bool letter = false, sep = false;
        for (; n < 91 && s[n]; ++n)
        {
            const unsigned char c = (unsigned char)s[n];
            if (c < 0x20 || c > 0x7e) return false;
            if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') letter = true;
            if (c == '.' || c == '_' || c == '/' || c == 0x5c) sep = true;
        }
        if (n < 6 || n > 90 || s[n] != 0 || !letter || !sep) return false;
        strncpy_s(out, 96, s, _TRUNCATE); return true;
    }
    bool name_like_wide(const wchar_t* w, char* out)
    {
        char tmp[96]; int n = 0;
        for (; n < 91 && w[n]; ++n)
        {
            if (w[n] < 0x20 || w[n] > 0x7e) return false;
            tmp[n] = (char)w[n];
        }
        if (n > 90) return false;
        tmp[n] = 0;
        return name_like_ascii(tmp, out);
    }
    // Look through obj's first 0x300 bytes (and one level of pointers) for text.
    // pass 0 = only file names (.swf/.gfx), pass 1 = any name-like text.
    bool scan_obj_text(uintptr_t obj, int pass, char* out)
    {
        for (int depth = 0; depth < 2; ++depth)
            for (int i = 0; i < 0x300; i += 8)
            {
                uintptr_t p = *(uintptr_t*)(obj + i);
                if (!readable_ptr(p & ~(uintptr_t)3)) continue;
                uintptr_t tgt[4] = { p, (p & ~(uintptr_t)3) + 12, (p & ~(uintptr_t)3) + 16, 0 };
                if (depth == 1)
                {
                    uintptr_t q = *(uintptr_t*)p;            // one more hop
                    if (!readable_ptr(q & ~(uintptr_t)3)) continue;
                    tgt[0] = q; tgt[1] = (q & ~(uintptr_t)3) + 12; tgt[2] = (q & ~(uintptr_t)3) + 16;
                }
                for (int k = 0; k < 3; ++k)
                {
                    if (!readable_ptr(tgt[k] & ~(uintptr_t)3)) continue;
                    char cand[96];
                    if (pass == 0) { if (looks_like_url((const char*)tgt[k])) { strncpy_s(out, 96, (const char*)tgt[k], _TRUNCATE); return true; } continue; }
                    if (name_like_ascii((const char*)tgt[k], cand) || name_like_wide((const wchar_t*)tgt[k], cand))
                    { strcpy_s(out, 96, cand); return true; }
                }
            }
        return false;
    }
    // PIECEID 2026-09-26 — the pieces had no names ("name not found") because the scan
    // above stops two pointers from the movie, and Scaleform keeps the file name about
    // five away (def -> bind data -> data def -> load data -> URL). Two deeper, fault-
    // guarded searches: (1) text naming a HUD module; (2) the movie's FILE SIZE, which
    // Scaleform keeps in the header data beside the stage rectangle. Every HUD file's
    // exact size is known from the extraction (akvr/diagnostics/hud-scripts-20260926),
    // and a match only counts if a stage width in twips (20480.0f = 1024 px, or 25600.0f
    // = 1280 px) sits within 0x40 bytes, so a random integer cannot name a piece.
    struct KnownMovie { uint32_t len; const char* name; };
    const KnownMovie kKnownMovies[] = {
        { 63903,  "HudModuleAlertsAndSurveillance" }, { 670830, "HudModuleBatmobile" },
        { 137268, "HudModuleBespokeLocalRadar" },     { 60085,  "HudModuleCrimeSceneInfo/StoryHUD" },
        { 58830,  "HudModuleDetectiveModeJamming" },  { 241691, "HudModuleDetectiveMode" },
        { 23645,  "HudModuleDownload_Batman" },       { 71946,  "HudModuleDownloadProgress" },
        { 161722, "HudModuleHealth" },                { 64464,  "HudModuleObjectives" },
        { 29437,  "HudModuleRoomName" },              { 200105, "HudModuleScanner" },
        { 172302, "HudModuleStoryMode" },             { 16086,  "HudModuleSuicideCars" },
        { 618377, "HudModuleTargets" },               { 416553, "MissionWheel" },
        { 23456,  "ModularHudBm3" },                  { 194501, "RadialGadgetSelect" },
        { 56960,  "HudModuleBoss" },                  { 56718,  "HudModuleBoss(old)" },
        { 19017,  "ContentBeacons" },                 { 177998, "FrontMostLayer" },
        { 127368, "FrontendBGFader" },                { 86105,  "WorldMap3D" },
        { 150639, "ProjectorVideoOverlay" },          { 101331, "RemoteHackingDevice" },
        { 81473,  "OmnitronBrainHacking" },           { 69478,  "PauseMenu" },
        { 22251,  "LeaderboardMini" },                { 64520,  "BroadcastAnalyser" },
    };
    bool has_stage_twips(uintptr_t a)
    {
        for (intptr_t d = -0x40; d <= 0x40; d += 4)
        {
            const float f = *(const float*)(a + d);
            if (f == 20480.0f || f == 25600.0f) return true;
        }
        return false;
    }
    bool text_names_module(uintptr_t p, char* out)
    {
        const uintptr_t cands[3] = { p, p + 12, p + 16 };
        for (uintptr_t c : cands)
        {
            char tmp[96] = "";
            if (!name_like_ascii((const char*)c, tmp) && !name_like_wide((const wchar_t*)c, tmp)) continue;
            if (strstr(tmp, "HudModule") || strstr(tmp, "ModularHud") || strstr(tmp, "Radar"))
            { strcpy_s(out, 96, tmp); return true; }
        }
        return false;
    }
    int g_idDepth = -1;                    // diagnostics: how deep the last hit was
    // Only plainly readable, committed memory: never a guard page (touching a thread's
    // stack guard under __try would break that stack's growth later). One VirtualQuery
    // per node, cached by region.
    bool page_ok(uintptr_t p, size_t n)
    {
        static thread_local uintptr_t lo = 0, hi = 0;
        if (p >= lo && p + n <= hi) return true;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)p, &mbi, sizeof(mbi))) return false;
        const DWORD pr = mbi.Protect;
        if (mbi.State != MEM_COMMIT || (pr & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            return false;
        lo = (uintptr_t)mbi.BaseAddress; hi = lo + mbi.RegionSize;
        return p + n <= hi;
    }
    volatile LONG g_scanBusy = 0;          // the live push can call in from another thread
    bool deep_scan(uintptr_t root, char* out)
    {
        if (InterlockedCompareExchange(&g_scanBusy, 1, 0) != 0) return false;   // try next time
        struct Node { uintptr_t p; int depth; };
        static Node q[1536];
        int head = 0, tail = 0;
        q[tail++] = { root, 0 };
        bool found = false;
        while (head < tail && !found)
        {
            const Node n = q[head++];
            if (!page_ok(n.p - 0x40, 0x280)) continue;
            __try
            {
                if (text_names_module(n.p, out)) { g_idDepth = n.depth; found = true; }
                for (int i = 0; i < 0x200 && !found; i += 4)
                {
                    const uintptr_t a = n.p + i;
                    const uint32_t u = *(const uint32_t*)a;
                    if (u >= 16000 && u <= 700000)
                        for (const KnownMovie& k : kKnownMovies)
                            if (u == k.len && has_stage_twips(a))
                            { strcpy_s(out, 96, k.name); g_idDepth = n.depth; found = true; break; }
                    if (found) break;
                    if ((i & 7) != 0) continue;
                    const uintptr_t p = *(const uintptr_t*)a;
                    if (!readable_ptr(p)) continue;
                    // Strings are reached as nodes too (checked above, after page_ok).
                    if (n.depth < 5 && tail < 1536 && (p & 7) == 0)
                    {
                        bool seen = false;
                        for (int k = 0; k < tail && !seen; ++k) seen = q[k].p == p;
                        if (!seen) q[tail++] = { p, n.depth + 1 };
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        InterlockedExchange(&g_scanBusy, 0);
        return found;
    }
    bool movie_url_scan(void* view, char* out)
    {
        __try
        {
            uintptr_t def = *(uintptr_t*)((uint8_t*)view + 0x48);
            if (readable_ptr(def) && scan_obj_text(def, 0, out)) return true;
            if (scan_obj_text((uintptr_t)view, 0, out)) return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        // PIECEID: the deep search, from the definition first, then the movie itself.
        __try
        {
            uintptr_t def = *(uintptr_t*)((uint8_t*)view + 0x48);
            if (readable_ptr(def) && deep_scan(def, out)) return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return deep_scan((uintptr_t)view, out);
    }
    bool name_excluded(const char* name)
    {
        if (!name[0]) return false;
        char key[100]; _snprintf_s(key, sizeof(key), _TRUNCATE, ";%s;", name);
        return strstr(g_noShrink, key) != nullptr;
    }
    MovieInfo* movie_info(void* view)
    {
        for (int i = 0; i < g_nMovies; ++i) if (g_movies[i].view == view) return &g_movies[i];
        if (g_nMovies >= 64) return nullptr;
        MovieInfo& m = g_movies[g_nMovies++];
        m.view = view; m.name[0] = 0; m.tries = 0; m.shrink = true; m.screenSeen = false;
        m.origMode = -1; m.stageVisPx = 0.0f;
        m.def = 0; m.ps = 1.0f; m.pdx = 0.0f; m.pdy = 0.0f;
        return &m;
    }
    // PIECEXF 2026-09-26 — JJ: move and scale the radar on its own. Each HUD piece is its
    // own Scaleform movie and ROOTSHRINK already owns each movie's root matrix, so a piece
    // gets its own size (about the HUD box centre) and shift (fractions of the screen) on
    // top of the HUD size. Saved by piece NAME (PIECEID) as "name:size:x:y;".
    char g_pieceXf[2048] = "";
    void piece_xf_load(MovieInfo& m)
    {
        m.ps = 1.0f; m.pdx = 0.0f; m.pdy = 0.0f;
        if (!m.name[0]) return;
        char key[100]; _snprintf_s(key, sizeof(key), _TRUNCATE, ";%s:", m.name);
        const char* hit = strstr(g_pieceXf, key);
        if (!hit) return;
        float s = 1.0f, x = 0.0f, y = 0.0f;
        if (sscanf_s(hit + strlen(key), "%f:%f:%f", &s, &x, &y) == 3)
        { m.ps = s; m.pdx = x; m.pdy = y; }
    }
    bool piece_custom(const MovieInfo& m)
    {
        return std::fabs(m.ps - 1.0f) > 0.001f || std::fabs(m.pdx) > 0.0005f || std::fabs(m.pdy) > 0.0005f;
    }
    // Should this movie take part in the HUD shrink? Names are resolved lazily (the
    // definition may not be filled in the first time a movie sets its viewport).
    bool movie_shrinks(void* view, int bufW, int bufH)
    {
        MovieInfo* m = view ? movie_info(view) : nullptr;
        if (!m) return true;
        m->bufW = bufW; m->bufH = bufH; m->screenSeen = true;
        // A freed movie's address can be reused by a new one, so the name belongs to the
        // movie's DEFINITION: re-identify whenever that changes. PIECEID's deep search is
        // not free, so a known definition is not searched again (and an unnamed one at
        // most four times - it may not be filled in on the first viewport set).
        uintptr_t def = 0;
        __try { def = *(uintptr_t*)((uint8_t*)view + 0x48); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (def != m->def) { m->def = def; m->tries = 0; m->name[0] = 0; m->shrink = true; }
        if (!m->name[0] && m->tries < 4)
        {
            ++m->tries;
            char nm[96] = "";
            if (movie_url_scan(view, nm))
            {
                strcpy_s(m->name, nm);
                m->shrink = !name_excluded(m->name);
                piece_xf_load(*m);
                log_add("hud-piece: %p = %s (found %d pointers deep)", view, m->name, g_idDepth);
            }
        }
        return m->shrink;
    }
    DWORD  g_gfxThread = 0;
    float  g_pushedScale = -1.0f;
    float  g_pushedAspect = -1.0f;
    bool   g_sameThread  = false;
    int    g_lastSetL = 0, g_lastSetT = 0, g_lastSetW = 0, g_lastSetH = 0;   // MAPFILL

    // Shrink a viewport to g_hudScale of its own buffer and centre it. Idempotent.
    // SCALEMODE 2026-09-26 (experimental, g_hudScaleMode) - the radar flips to full size
    // although its movie never sees anything but the shrunk box (HUDPROBE); suspect: a
    // Scaleform render-to-texture step (filters on the popups) that ignores the box's
    // offset. So shrink WITHOUT an offset box: Scaleform 4 NoScale mode (+0xe8 = 0), centre
    // alignment (+0xec = 0, already), FULL-SCREEN box, and Viewport.Scale = stage pixels per
    // screen pixel. Layout 0x1411ac780, NoScale branch 0x1411acb14: visible frame = viewport
    // size x Scale (x aspect), centred. The HUD movies run ShowAll (mode 1), centre, and show
    // stageVisPx stage pixels across the width (measured from +0xf0/+0xf8 while in ShowAll;
    // the same for any box of the screen's shape). Same size as the box method:
    // Scale = stageVisPx / (hudScale x W). Raise = the box moved up by the same pixels.
    bool   g_hudScaleMode = false;
    int  read_mode(void* view)
    {
        __try { return *(int*)((uint8_t*)view + 0xe8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    bool write_mode(void* view, int mode)
    {
        __try { *(int*)((uint8_t*)view + 0xe8) = mode; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    float read_vis_px(void* view)
    {
        __try
        {
            const float a = *(float*)((uint8_t*)view + 0xf0), b = *(float*)((uint8_t*)view + 0xf8);
            return (b - a) / 20.0f;                          // twips -> stage pixels
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
    }
    MovieInfo* movie_find(void* view)
    {
        for (int i = 0; i < g_nMovies; ++i) if (g_movies[i].view == view) return &g_movies[i];
        return nullptr;
    }
    // Put a movie back to the scale mode it had before SCALEMODE touched it.
    bool restore_mode(void* view, GfxViewport& vp)
    {
        MovieInfo* m = movie_find(view);
        if (!m || m->origMode < 0 || read_mode(view) == m->origMode) return false;
        write_mode(view, m->origMode);
        vp.scale = 1.0f;
        return true;
    }

    bool g_hudRootMode = true;              // ROOTSHRINK, below
    bool root_ready();
    bool call_targets(uintptr_t call, uintptr_t target);

    bool scale_viewport_box(void* view, GfxViewport& vp)
    {
        const int W = vp.bufW, H = vp.bufH;
        if (W < 64 || H < 64) return false;
        // UIGATES 2026-09-26, from the viewport log (VPLOG): movies drawn into their OWN
        // texture (buffers 1024x768, 1024x720, 256x192 — map icons and the like) were
        // being forced into the screen HUD rectangle, then set back by the game, frame
        // after frame: the flicker, and the squished map icons. Only a viewport on the
        // SCREEN (the game's own UI rectangle size) is ours to change.
        // GEOSHAPE: with a forced non-16:9 render the game's own UI rectangle stayed
        // 2560x1600 while the movies' buffers are the render (3072x2688), so compare
        // against the render size when we force it.
        const bool screenSized = g_engH
            ? (W == g_engW && H == g_engH)
            : (g_uiHaveBase && W == g_uiBaseR - g_uiBaseL && H == g_uiBaseB - g_uiBaseT);
        if (!screenSized)
        {
            // RADARTEX 2026-09-26 — JJ: the radar "flickers between the scaled and the
            // non-scaled". Viewport log 20:14: the radar's view (0x40743F60) is set with
            // BOTH a 3120x2730 screen buffer and a 1024x720 buffer of its own; five HUD
            // pieces do this. In its own texture it was left full layout (UIGATES rule),
            // and that texture is then shown over the whole screen - a full-size radar
            // whenever the game switches it to the texture path (popups). So a piece
            // that has been seen on the SCREEN gets the same RELATIVE box inside its own
            // buffer. Own-texture-only movies (map icons 1024x768 / 256x192) are never
            // screen pieces and stay untouched; the old UIGATES bug forced the SCREEN
            // rectangle into textures, which this does not do.
            if (!g_hudDual || !view) return false;
            MovieInfo* m = nullptr;
            for (int i = 0; i < g_nMovies; ++i) if (g_movies[i].view == view) { m = &g_movies[i]; break; }
            if (!m || !m->screenSeen || !m->shrink) return false;
            if (hud_eff() >= 0.999f && hud_eff() <= 1.001f && g_hudRaise == 0.0f) return false;
            int l, t, w, h;
            hud_rect(W, H, 1.0f, false, l, t, w, h);
            if (vp.left == l && vp.top == t && vp.width == w && vp.height == h) return false;
            vp.left = l; vp.top = t; vp.width = w; vp.height = h;
            return true;
        }
        // HUDPIECES: a piece switched off in the panel stays full size; undo our box.
        if (!movie_shrinks(view, W, H))
        {
            const bool restored = restore_mode(view, vp);
            if (g_lastSetW > 0 && vp.left == g_lastSetL && vp.top == g_lastSetT
                && vp.width == g_lastSetW && vp.height == g_lastSetH)
            {
                vp.left = 0; vp.top = 0; vp.width = W; vp.height = H;
                return true;
            }
            return restored;
        }
        const bool aspectChanged = correct_movie_aspect(view, vp);
        // HUDFIT 2026-09-26 — JJ: scaled down, the HUD is "still cropped on the edges,
        // it's scaling its crop as well"; F2 showed the Batmobile gauge sliced at its
        // outer edge. The pixel-aspect correction alone told Scaleform the viewport was
        // ~0.51 as wide, logically, as the buffer, so a 16:9 movie no longer fit its
        // viewport and lost its sides — inside whatever rectangle the size slider made.
        // Squash the RECTANGLE's height by the same factor: logically the viewport is
        // 16:9 again (nothing to crop) and physically it is flattened exactly as much as
        // the display will stretch it back.
        const float f = hud_aspect_factor(W, H);
        const bool squash = f < 0.999f || f > 1.001f;
        // At 1.00 and no squash touch NOTHING. Forcing left/top/width/height back to the
        // full buffer would flatten any sub-viewport a movie legitimately asked for, so
        // "off" has to mean "hands off", not "reset to full frame".
        if (!squash && hud_eff() >= 0.999f && hud_eff() <= 1.001f
            && hud_eff_v() >= 0.999f && hud_eff_v() <= 1.001f)
        {
            const bool restored = restore_mode(view, vp);
            // MAPFILL 2026-09-26: the map's movies are created in the ~10 frames before
            // the detector leaves gameplay, so they got the shrunk, raised HUD box and
            // letterboxed themselves in black inside it (F2, 17:40). "Hands off" must
            // still undo OUR box: a viewport exactly equal to the last one we set goes
            // back to the full frame. Any other sub-viewport is the game's; left alone.
            if (g_lastSetW > 0 && vp.left == g_lastSetL && vp.top == g_lastSetT
                && vp.width == g_lastSetW && vp.height == g_lastSetH
                && (vp.width != W || vp.height != H))
            {
                vp.left = 0; vp.top = 0; vp.width = W; vp.height = H;
                return true;
            }
            return aspectChanged || restored;
        }
        // SCALEMODE: shrink by Scaleform's own scale on a full-screen box.
        {
            MovieInfo* m = movie_find(view);
            const int mode = m ? read_mode(view) : -1;
            if (m && m->origMode < 0 && mode >= 0) m->origMode = mode;
            if (m && mode == 1) { const float vw = read_vis_px(view); if (vw > 1.0f) m->stageVisPx = vw; }
            const bool scaleMode = g_hudScaleMode && !(g_hudRootMode && root_ready());
            if (scaleMode && !squash && m && m->origMode == 1 && m->stageVisPx > 1.0f)
            {
                const float P = hud_eff() * (float)W / m->stageVisPx;   // screen px per stage px
                if (P > 0.01f && write_mode(view, 0))
                {
                    const int raisePx = g_hudGameplay ? (int)(g_hudRaise * (float)H) : 0;
                    vp.left = 0; vp.top = -raisePx; vp.width = W; vp.height = H;
                    vp.scale = 1.0f / P;
                    return true;
                }
            }
            else if (!scaleMode && restore_mode(view, vp))
            {
                int l2, t2, w2, h2;
                hud_rect(W, H, f, squash, l2, t2, w2, h2);
                g_lastSetL = l2; g_lastSetT = t2; g_lastSetW = w2; g_lastSetH = h2;
                vp.left = l2; vp.top = t2; vp.width = w2; vp.height = h2;
                return true;
            }
        }
        int l, t, w, h;
        hud_rect(W, H, f, squash, l, t, w, h);
        g_lastSetL = l; g_lastSetT = t; g_lastSetW = w; g_lastSetH = h;
        if (vp.left == l && vp.top == t && vp.width == w && vp.height == h) return aspectChanged;
        vp.left = l; vp.top = t; vp.width = w; vp.height = h;
        return true;
    }

    // ROOTSHRINK 2026-09-26 - the radar flip, from the game's own HUD scripts.
    // Extracted from BmGame.upk (Gildor extract + JPEXS): the new-style HUD is ONE AS3
    // movie (ModularHudBm3, stage 1024x720) that loads every module (health, Batmobile,
    // targets/compass, surveillance, radar) into itself, and EVERY module is a tilted 3D
    // panel (matrix3D + its own perspectiveProjection, centre 512,384 etc.). No script
    // positions anything from the screen size per frame (only a one-off widescreen shift),
    // and the old-style prompts movie (AS2, no 3D) never jumps. So the jump is Scaleform
    // projecting 3D panels inside an OFFSET, shrunk viewport; at HUD 1.00 the viewport is
    // stock and nothing flickers. This mode keeps the viewport stock (full buffer) and
    // shrinks the movie one level up instead: the render-tree root's 2D matrix, which
    // SetViewport itself fills from the movie's ViewportMatrix (view+0x110, 2x4 floats,
    // stage twips -> viewport pixels) via TreeNode::SetMatrix 0x1411cdc70 on the root at
    // view+0x88 (call 0x1411aeca5). Layout 0x1411ac780 runs only from SetViewport; the
    // per-frame indirect-transform loop in slot 27 stops BELOW the root, so nothing
    // rewrites the root every frame. We compose our rect onto the stock matrix after
    // every SetViewport (the movie has just re-pushed the stock one).
    typedef void (*TreeSetMatrixFn)(void* node, const float* m2x4);
    TreeSetMatrixFn g_treeSetMatrix = nullptr;
    bool   g_rootTried = false;
    long   g_rootApplied = 0, g_rootMovies = 0;
    struct RootState { void* view; bool want; int l, t, w, h, W, H; float last[8]; bool haveLast; };
    RootState g_roots[64] = {};

    // ---- HUDSTEADY 2026-09-28 — JJ: "because they are connected to the head tracking, micro
    // movements of the head cause very minor jittering of all the HUD elements. Can we very
    // slightly dampen it?" The HUD is painted into the picture, and the headset shows each
    // picture at the head pose it was drawn with, so the HUD sits exactly where the head
    // pointed for that frame: every tremor moves it. Here the HUD is drawn instead toward a
    // slightly SMOOTHED head direction s: shifted inside the picture by (s - r), r = the head
    // pose that frame's camera used (akvr_head_frame_quat, exact), so it appears at s.
    // Playbook 04-ui-and-hud.md "Body-locked is the default" (lazy follow), commit b955f224.
    // Capped at a few degrees so a real head turn never leaves the HUD behind; world-attached
    // markers move by the same small amount. 0 = off (the HUD exactly as before).
    float    g_steady = 0.0f;                  // panel 0..1
    float    g_steadyTx = 0.0f, g_steadyTy = 0.0f;   // HUD shift, tangent units (x right, y up)
    float    g_steadyQ[4] = { 0, 0, 0, 1 };    // smoothed head orientation
    bool     g_steadyInit = false;
    uint64_t g_steadyFin = 0;
    LARGE_INTEGER g_steadyT{};
    long     g_steadyPaired = 0, g_steadyMissed = 0;
    float    g_steadyDeg = 0.0f;               // current HUD lag angle
    long     g_steadyAge[4] = {};              // HUDSTEADY2: camera-match age 0 / 1 / 2 / other
    float    g_steadyPxPerDeg = 0.0f;          // last conversion used (diag)

    void q_mul(const float* a, const float* b, float* o)
    {
        o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    }
    // Normalised lerp from a to b by t along the short way.
    void q_nlerp(const float* a, const float* b, float t, float* o)
    {
        const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const float sb = d < 0.0f ? -t : t;
        float n = 0.0f;
        for (int i = 0; i < 4; ++i) { o[i] = a[i] * (1.0f - t) + b[i] * sb; n += o[i] * o[i]; }
        n = n > 0.0f ? 1.0f / sqrtf(n) : 1.0f;
        for (int i = 0; i < 4; ++i) o[i] *= n;
    }
    float q_angle_deg(const float* a, const float* b)
    {
        float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        if (d > 1.0f) d = 1.0f;
        return 2.0f * acosf(d) * 57.29578f;
    }
    // Once per game frame, on the game thread (movie slot 27, where the HUD is captured).
    void steady_update()
    {
        const uint64_t fin = akvr_camera_finalize_count();
        if (fin == g_steadyFin) return;
        g_steadyFin = fin;
        LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
        float dt = g_steadyT.QuadPart ? (float)((double)(now.QuadPart - g_steadyT.QuadPart) / (double)f.QuadPart) : 0.022f;
        g_steadyT = now;
        if (dt > 0.1f) dt = 0.1f;
        float r[4];
        if (g_steady <= 0.001f || !g_hudGameplay) { g_steadyTx = g_steadyTy = 0.0f; g_steadyInit = false; g_steadyDeg = 0.0f; return; }
        // HUDSTEADY2 2026-09-28 — JJ (0.5 and 1.0): "it doesn't seem to be dampening micro
        // movements, it's just making the entire thing lag". That is the one-frame-stale
        // signature: slot 27 runs BEFORE this frame's camera finalize, so the camera rotator
        // read here is the PREVIOUS frame's. The pose this frame's camera will use is the newest
        // one handed to the stub. The matched age is kept as evidence (mostly 1 = before, as
        // assumed now; mostly 0 = after, and then both choices are the same entry).
        float rm[4]; int age = -1;
        if (akvr_head_frame_quat(rm, &age)) { if (age >= 0 && age < 3) ++g_steadyAge[age]; else ++g_steadyAge[3]; }
        if (!akvr_head_newest_quat(r)) { ++g_steadyMissed; g_steadyTx = g_steadyTy = 0.0f; g_steadyInit = false; return; }
        ++g_steadyPaired;
        if (!g_steadyInit) { memcpy(g_steadyQ, r, sizeof(r)); g_steadyInit = true; }
        // Time constant 15..75 ms, cap 0.25..1.0 degree across the panel's range (was 20..120 ms,
        // 0.5..2.5 deg: the lag JJ saw on real head turns).
        const float tau = 0.015f + 0.06f * g_steady;
        const float capDeg = 0.25f + 0.75f * g_steady;
        float s[4];
        q_nlerp(g_steadyQ, r, 1.0f - expf(-dt / tau), s);
        const float ang = q_angle_deg(s, r);
        if (ang > capDeg) { float t[4]; q_nlerp(r, s, capDeg / ang, t); memcpy(s, t, sizeof(s)); }
        memcpy(g_steadyQ, s, sizeof(s));
        g_steadyDeg = q_angle_deg(s, r);
        // s seen from r: rel = conj(r) * s; its forward (OpenXR -Z) in r's view.
        const float rc[4] = { -r[0], -r[1], -r[2], r[3] };
        float rel[4]; q_mul(rc, s, rel);
        const float x = rel[0], y = rel[1], z = rel[2], w = rel[3];
        // rotate (0,0,-1) by rel
        const float vx = -(2.0f * (x * z + w * y));
        const float vy = -(2.0f * (y * z - w * x));
        const float vz = -(1.0f - 2.0f * (x * x + y * y));
        if (vz < -0.5f) { g_steadyTx = vx / -vz; g_steadyTy = vy / -vz; }
    }

    bool root_ready()
    {
        if (g_rootTried) return g_treeSetMatrix != nullptr;
        g_rootTried = true;
        const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        const uintptr_t fn = base + (0x1411cdc70 - 0x140000000);
        const uintptr_t call = base + (0x1411aeca5 - 0x140000000);
        // mov [rsp+8],rbx; push rdi; sub rsp,0x20; mov rbx,rdx; mov edx,1 (Change_Matrix)
        static const uint8_t pro[] = { 0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x20,
                                       0x48,0x8B,0xDA, 0xBA,0x01,0x00,0x00,0x00 };
        __try
        {
            if (memcmp((const void*)fn, pro, sizeof(pro)) != 0 || !call_targets(call, fn))
            { log_add("hud-root: code does not match the RE (other game build?) - box mode kept"); return false; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_add("hud-root: fault while checking - box mode kept"); return false; }
        g_treeSetMatrix = (TreeSetMatrixFn)fn;
        log_add("hud-root: render-root matrix setter found at %p", (void*)fn);
        return true;
    }
    RootState* root_state(void* view, bool create)
    {
        RootState* freeSlot = nullptr;
        for (auto& r : g_roots)
        {
            if (r.view == view) return &r;
            if (!r.view && !freeSlot) freeSlot = &r;
        }
        if (!create || !freeSlot) return nullptr;
        memset(freeSlot, 0, sizeof(*freeSlot));
        freeSlot->view = view;
        return freeSlot;
    }
    // Compose our rect onto the movie's own matrix and set it on the render root.
    // force = the game has just re-pushed its stock matrix, so our record is stale.
    void apply_root(void* view, bool force)
    {
        RootState* r = root_state(view, false);
        if (!r || !g_treeSetMatrix) return;
        __try
        {
            void* node = *(void**)((uint8_t*)view + 0x88);
            if (!node || (uintptr_t)node < 0x10000) return;
            float m[8];
            memcpy(m, (uint8_t*)view + 0x110, sizeof(m));
            if (r->want && r->W > 0 && r->H > 0)
            {
                const float sx = (float)r->w / (float)r->W, sy = (float)r->h / (float)r->H;
                for (int i = 0; i < 4; ++i) { m[i] *= sx; m[4 + i] *= sy; }
                m[3] += (float)r->l;
                m[7] += (float)r->t;
                // PIECEXF: this piece's own size about the HUD box centre, then its shift.
                MovieInfo* mi = movie_find(view);
                if (mi && piece_custom(*mi))
                {
                    const float cx = r->l + 0.5f * r->w, cy = r->t + 0.5f * r->h;
                    for (int i = 0; i < 4; ++i) { m[i] *= mi->ps; m[4 + i] *= mi->ps; }
                    m[3] += cx * (1.0f - mi->ps) + mi->pdx * (float)r->W;
                    m[7] += cy * (1.0f - mi->ps) - mi->pdy * (float)r->H;   // + = up
                }
            }
            // HUDSTEADY: the smoothed-head shift (tangents -> buffer pixels at the camera's FOV).
            if (r->want && (g_steadyTx != 0.0f || g_steadyTy != 0.0f) && r->W > 0)
            {
                const CameraView cv = akvr_camera_read();
                if (cv.valid && cv.fov > 10.0f && cv.fov < 170.0f)
                {
                    const float ppt = 0.5f * (float)r->W / tanf(cv.fov * 0.5f * 0.01745329f);
                    g_steadyPxPerDeg = ppt * 0.01745329f;
                    m[3] += g_steadyTx * ppt;
                    m[7] -= g_steadyTy * ppt;            // + = up
                }
            }
            if (!force && r->haveLast && memcmp(m, r->last, sizeof(m)) == 0) return;
            g_treeSetMatrix(node, m);
            memcpy(r->last, m, sizeof(m));
            r->haveLast = true;
            ++g_rootApplied;
            if (!r->want) r->view = nullptr;           // back to stock: stop tracking
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { r->view = nullptr; }
    }

    // The one entry point: the box logic decides WHETHER and WHERE to shrink; in root
    // mode the box it chose becomes the root matrix and the viewport stays full.
    bool scale_viewport(void* view, GfxViewport& vp)
    {
        const GfxViewport in = vp;
        const bool changed = scale_viewport_box(view, vp);
        const int W = vp.bufW, H = vp.bufH;
        const float f = hud_aspect_factor(W, H);
        const bool squash = f < 0.999f || f > 1.001f;
        const bool full = vp.left == 0 && vp.top == 0 && vp.width == W && vp.height == H;
        if (!view || !g_hudRootMode || squash || !root_ready())
        {
            // Leaving root mode: the next apply puts the stock matrix back.
            if (RootState* r = root_state(view, false)) r->want = false;
            return changed;
        }
        int l, t, w, h;
        hud_rect(W, H, 1.0f, false, l, t, w, h);
        const bool ours = !full && vp.left == l && vp.top == t && vp.width == w && vp.height == h;
        if (ours)
        {
            RootState* r = root_state(view, true);
            if (r)
            {
                if (!r->want) ++g_rootMovies;
                r->want = true; r->l = l; r->t = t; r->w = w; r->h = h; r->W = W; r->H = H;
                vp.left = 0; vp.top = 0; vp.width = W; vp.height = H;
            }
        }
        else if (MovieInfo* mi = movie_find(view);
                 full && mi && piece_custom(*mi) && mi->shrink && g_hudGameplay
                 && (g_engH ? (W == g_engW && H == g_engH) : true))
        {
            // PIECEXF at HUD size 1.00: the box logic keeps its hands off, but a piece the
            // player moved still needs its root matrix (full box, own transform on top).
            RootState* r = root_state(view, true);
            if (r)
            {
                if (!r->want) ++g_rootMovies;
                r->want = true; r->l = 0; r->t = 0; r->w = W; r->h = H; r->W = W; r->H = H;
            }
        }
        else if (RootState* r = root_state(view, false))
            r->want = false;
        return memcmp(&vp, &in, sizeof(vp)) != 0;
    }


    // TEXDRAW 2026-09-26 - the radar's flip, found by static RE. Scaleform 4's render-tree
    // viewport setter (BatmanAK.exe 0x1411cd700, TreeRoot::SetViewport) has three callers:
    // the movie's own SetViewport (0x1411aec7f, the one our box already reaches) and two
    // render-to-texture paths (calls at 0x141207d73 and 0x1412081c1) that build a FRESH
    // viewport from the target picture's own size (full rect, scale 1) - the draw popups
    // trigger. Those always laid the HUD out full size: the radar's "full size" position in
    // box mode, and "above it" in SCALEMODE (scale 1 = half size about the centre). Here
    // only calls from those two sites, during gameplay with the HUD shrunk, and only a
    // full-rect viewport, get the same RELATIVE box as the screen movies. Counted in the
    // HUD diag line. Prologue and both call sites are verified before hooking.
    typedef void (*TreeSetVpFn)(void* root, const GfxViewport* vp);
    TreeSetVpFn   g_origTreeVp = nullptr;
    uintptr_t     g_texRet1 = 0, g_texRet2 = 0;
    volatile LONG g_texCalls = 0, g_texScaled = 0;
    int           g_texBufW = 0, g_texBufH = 0;
    bool          g_texTried = false, g_texHooked = false;
    bool          g_hudTexFix = true;

    void hkTreeSetViewport(void* root, const GfxViewport* vp)
    {
        const uintptr_t ret = (uintptr_t)_ReturnAddress();
        if (vp && (ret == g_texRet1 || ret == g_texRet2))
        {
            InterlockedIncrement(&g_texCalls);
            g_texBufW = vp->bufW; g_texBufH = vp->bufH;
            const bool shrunk = hud_eff() < 0.999f || hud_eff() > 1.001f || g_hudRaise != 0.0f;
            if (g_hudTexFix && !(g_hudRootMode && g_treeSetMatrix) && g_hudGameplay && shrunk && vp->bufW >= 64 && vp->bufH >= 64 &&
                vp->left == 0 && vp->top == 0 && vp->width == vp->bufW && vp->height == vp->bufH)
            {
                GfxViewport v = *vp;
                int l, t, w, h;
                hud_rect(v.bufW, v.bufH, 1.0f, false, l, t, w, h);
                v.left = l; v.top = t; v.width = w; v.height = h;
                InterlockedIncrement(&g_texScaled);
                g_origTreeVp(root, &v);
                return;
            }
        }
        g_origTreeVp(root, vp);
    }

    bool call_targets(uintptr_t call, uintptr_t target)
    {
        const uint8_t* c = (const uint8_t*)call;
        if (c[0] != 0xE8) return false;
        return call + 5 + (intptr_t)*(const int32_t*)(c + 1) == target;
    }

    void install_texdraw_hook()
    {
        if (g_texTried) return;
        g_texTried = true;
        const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        const uintptr_t target = base + (0x1411cd700 - 0x140000000);
        const uintptr_t call1 = base + (0x141207d73 - 0x140000000);
        const uintptr_t call2 = base + (0x1412081c1 - 0x140000000);
        static const uint8_t prologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };   // push rbx; sub rsp,0x20
        __try
        {
            if (memcmp((const void*)target, prologue, sizeof(prologue)) != 0 ||
                !call_targets(call1, target) || !call_targets(call2, target))
            { log_add("tex-draw: code does not match the RE (other game build?) - not hooked"); return; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_add("tex-draw: fault while checking - not hooked"); return; }
        g_texRet1 = call1 + 5;
        g_texRet2 = call2 + 5;
        MH_Initialize();
        if (MH_CreateHook((void*)target, (void*)&hkTreeSetViewport, (void**)&g_origTreeVp) != MH_OK ||
            MH_EnableHook((void*)target) != MH_OK)
        { log_add("tex-draw: MinHook failed - not hooked"); return; }
        g_texHooked = true;
        log_add("tex-draw: render-tree viewport hooked at %p", (void*)target);
    }

    void hkSetViewport(void* view, const GfxViewport* vp)
    {
        SetViewportFn orig = nullptr;
        if (view)
        {
            void** vt = *(void***)view;
            for (int i = 0; i < g_vtCount; ++i)
                if (g_vtHooked[i] == vt) { orig = g_vtOrigSet[i]; break; }

            // Every movie that passes through here is one the slider can re-push
            // later — this is how the list grows past the handful seen at startup.
            bool seen = false;
            for (int i = 0; i < g_nViews; ++i) if (g_views[i] == view) { seen = true; break; }
            if (!seen && g_nViews < 32) g_views[g_nViews++] = view;
            g_gfxThread = GetCurrentThreadId();
        }
        if (!orig) orig = g_vtOrigSet[0];      // cannot happen, but never call null
        if (!orig) return;

        if (vp)
        {
            GfxViewport local = *vp;
            if (scale_viewport(view, local)) { ++g_scaled; vp_log('V', view, *vp, local); orig(view, &local); apply_root(view, true); return; }
            vp_log('V', view, *vp, *vp);
        }
        orig(view, vp);
        if (view) apply_root(view, true);      // ROOTSHRINK: the movie re-pushed its stock matrix
    }

    bool in_exe_image(void* p)
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe || !p) return false;
        uint8_t* base = (uint8_t*)exe;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        return (uint8_t*)p >= base && (uint8_t*)p < base + nt->OptionalHeader.SizeOfImage;
    }

    // Re-push a scaled viewport for one movie, and make sure its class is hooked.
    void fix_one_movie(void* movie)
    {
        if (!movie || (uintptr_t)movie < 0x10000) return;
        void* view = nullptr;
        memcpy(&view, (uint8_t*)movie + 0x3c, sizeof(view));     // deliberately unaligned
        if (!view || (uintptr_t)view < 0x10000) return;

        void** vt = *(void***)view;
        if (!in_exe_image(vt)) return;
        void* setp = vt[14];                                     // +0x70
        void* getp = vt[15];                                     // +0x78
        if (!in_exe_image(setp) || !in_exe_image(getp)) return;

        // Patch this class's SetViewport once, so every later caller is covered too.
        bool known = false;
        for (int i = 0; i < g_vtCount; ++i) if (g_vtHooked[i] == vt) { known = true; break; }
        if (!known && g_vtCount < 8 && setp != (void*)&hkSetViewport)
        {
            DWORD old = 0;
            if (VirtualProtect(&vt[14], sizeof(void*), PAGE_READWRITE, &old))
            {
                g_vtHooked[g_vtCount]  = vt;
                g_vtOrigSet[g_vtCount] = (SetViewportFn)setp;
                vt[14] = (void*)&hkSetViewport;
                VirtualProtect(&vt[14], sizeof(void*), old, &old);
                akvr_hud_layers_hook_vtable(vt);                 // HUDLAYERS: per-frame layer moves
                log_add("hud-view: hooked SetViewport for movie class %p (was %p)",
                        (void*)vt, setp);
                ++g_vtCount;
                known = true;
            }
        }

        bool seen = false;
        for (int i = 0; i < g_nViews; ++i) if (g_views[i] == view) { seen = true; break; }
        if (!seen && g_nViews < 32) g_views[g_nViews++] = view;

        // And fix the viewport this movie already has, right now, on this thread.
        SetViewportFn setFn = nullptr;
        for (int i = 0; i < g_vtCount; ++i) if (g_vtHooked[i] == vt) setFn = g_vtOrigSet[i];
        if (!setFn) setFn = (SetViewportFn)setp;

        GfxViewport vp;
        memset(&vp, 0, sizeof(vp));
        ((GetViewportFn)getp)(view, &vp);
        const GfxViewport got = vp;
        if (scale_viewport(view, vp)) { vp_log('F', view, got, vp); setFn(view, &vp); ++g_scaled; apply_root(view, true); }
        else apply_root(view, false);
    }

    // CRASHED IN JJ'S HANDS 2026-08-05 (build WINDOWED): "Rendering thread exception",
    // three frames in VERSION.dll, while dragging the HUD slider in-game. Cause: this
    // list is append-only, but MOVIES ARE DESTROYED when a menu closes — so after any
    // menu had been opened and closed, re-pushing walked into freed memory. A live
    // slider that reaches back into objects it does not own has to assume every entry
    // may already be dead. Three defences, all cheap:
    //   1. the vtable must be one we recognise — freed memory almost never still holds
    //      a pointer we have on file;
    //   2. the whole call is inside __try, and anything that faults is DROPPED from the
    //      list instead of taking the game down;
    //   3. we only call SetViewport when the rect would actually CHANGE. Scaleform
    //      re-lays-out the movie on every viewport set, which is why the HUD and menus
    //      crawled — we were re-pushing every single frame of a slider drag.
    // Entry is void* only, no C++ objects, so __try is legal here.
    // VPWATCH 2026-09-26 — the radar flips between its shrunk and full-size position
    // on popups, yet no SetViewport call shows it (all resizes go through the one
    // hooked vtable slot; the setter has no direct callers). Read every known movie's
    // stored viewport (+0xa0, the 0x34 bytes GetViewport copies) once per Present and
    // log any change as a 'W' row, on the same clock as the V/P rows and the radar
    // recorder, to tell a direct write from a separate drawing path.
    struct VpWatch { void* view; GfxViewport last; };
    VpWatch g_watch[64] = {};
    int     g_nWatch = 0;
    long    g_watchChanges = 0;
    void watch_one(void* view)
    {
        GfxViewport now;
        __try { memcpy(&now, (uint8_t*)view + 0xa0, sizeof(now)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        int i = 0;
        for (; i < g_nWatch; ++i) if (g_watch[i].view == view) break;
        if (i == g_nWatch)
        {
            if (g_nWatch >= 64) return;
            g_watch[g_nWatch].view = view; g_watch[g_nWatch].last = now; ++g_nWatch;
            return;
        }
        if (memcmp(&now, &g_watch[i].last, sizeof(now)) != 0)
        {
            vp_log('W', view, g_watch[i].last, now);
            g_watch[i].last = now;
            ++g_watchChanges;
        }
    }
    void watch_views()
    {
        for (int i = 0; i < g_nViews; ++i)
        {
            void* view = g_views[i];
            if (!view) continue;
            void** vt = nullptr;
            __try { vt = *(void***)view; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            bool ours = false;
            for (int k = 0; k < g_vtCount; ++k) if (g_vtHooked[k] == vt) { ours = true; break; }
            if (ours) watch_one(view);
        }
    }

    bool repush_one(void* view, SetViewportFn setFn, GetViewportFn getFn)
    {
        __try
        {
            GfxViewport vp;
            memset(&vp, 0, sizeof(vp));
            getFn(view, &vp);
            GfxViewport want = vp;
            if (!scale_viewport(view, want)) { apply_root(view, false); return true; }   // viewport fine; root may differ
            if (want.left == vp.left && want.top == vp.top &&
                want.width == vp.width && want.height == vp.height && want.aspect == vp.aspect &&
                want.scale == vp.scale)
            { apply_root(view, false); return true; }        // already correct — do NOT re-set
            vp_log('P', view, vp, want);
            setFn(view, &want);
            ++g_scaled;
            apply_root(view, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;                                    // dead movie — forget it
        }
    }

    void repush_views()
    {
        int keep = 0;
        for (int i = 0; i < g_nViews; ++i)
        {
            void* view = g_views[i];
            if (!view) continue;

            bool alive = false;
            void** vt = nullptr;
            __try { vt = *(void***)view; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = nullptr; }

            if (vt && in_exe_image(vt))
            {
                SetViewportFn setFn = nullptr;
                for (int k = 0; k < g_vtCount; ++k)
                    if (g_vtHooked[k] == vt) { setFn = g_vtOrigSet[k]; break; }
                void* getp = vt[15];
                // Only a vtable we hooked ourselves is trusted. An unknown one means
                // this is not (any longer) a movie we put in the list.
                if (setFn && in_exe_image(getp))
                    alive = repush_one(view, setFn, (GetViewportFn)getp);
            }
            if (alive) g_views[keep++] = view;
        }
        g_nViews = keep;
    }

    void* hkMovieLoop(void* thiz, void* arg)
    {
        void* r = oMovieLoop ? oMovieLoop(thiz, arg) : nullptr;
        ++g_loopCalls;
        g_gfxThread = GetCurrentThreadId();
        if (thiz && (uintptr_t)thiz > 0x10000)
        {
            void** movies = *(void***)((uint8_t*)thiz + 0x1c);
            int    count  = *(int*)((uint8_t*)thiz + 0x24);
            if (movies && count > 0 && count < 256)
                for (int i = 0; i < count; ++i) fix_one_movie(movies[i]);
        }
        return r;
    }

    // MEASURED 2026-08-05 (build GFXVIEW): the movie LOOP never runs — `movie loop
    // hooked x0`, so nothing was captured and nothing was scaled. The site that does
    // run is the PER-MOVIE one at 0x1405d9ea0, which sets one movie's viewport
    // (Get -> overwrite from the global rect -> Set) with no gates at all.
    //
    // Its argument count is not a guess: the body reads incoming stack slots at
    // rbp+0x80 and rbp+0x88, so it takes SIX arguments (rcx, edx, r8, r9, then two on
    // the stack). Arg 4 (r9) is the object whose [+0x3c] is the GFxMovieView; arg 3
    // (r8) is the one whose [+0x80] is the movie definition, so both are worth trying.
    typedef void* (*MovieVpFn)(void* a1, uintptr_t a2, void* a3, void* a4,
                               uintptr_t a5, uintptr_t a6);
    MovieVpFn oMovieVp = nullptr;
    uint8_t*  g_movieVpSite = nullptr;
    long      g_movieVpCalls = 0;

    void* hkMovieVp(void* a1, uintptr_t a2, void* a3, void* a4,
                    uintptr_t a5, uintptr_t a6)
    {
        void* r = oMovieVp ? oMovieVp(a1, a2, a3, a4, a5, a6) : nullptr;
        ++g_movieVpCalls;
        g_gfxThread = GetCurrentThreadId();
        fix_one_movie(a4);
        if (a3 != a4) fix_one_movie(a3);
        return r;
    }

    const uint8_t kMovieVpPat[] = {
        0x48,0x8B,0xC4, 0x4C,0x89,0x48,0x20, 0x4C,0x89,0x40,0x18,
        0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
        0x48,0x8D,0x68,0xA8, 0x48,0x81,0xEC,0x20,0x01,0x00,0x00 };

    // The loop's prologue — `mov rax,rsp / push rbp / push r12 / push r13 /
    // lea rbp,[rax-0x5f] / sub rsp,0xa0`, then `mov r13,rdx / mov rbx,rcx`. Only rcx
    // and rdx are consumed, and r8/r9 are never homed, so it is exactly 2 arguments.
    const uint8_t kLoopPat[] = {
        0x48,0x8B,0xC4, 0x55, 0x41,0x54, 0x41,0x55,
        0x48,0x8D,0x68,0xA1, 0x48,0x81,0xEC,0xA0,0x00,0x00,0x00 };

    void install_movie_hook()
    {
        static bool tried = false;
        if (tried) return;
        tried = true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return;
        uint8_t* base = (uint8_t*)exe;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        size_t size = nt->OptionalHeader.SizeOfImage;

        // Anchor on the gate block (already known unique) and walk back to the
        // function start via the prologue — far safer than a generic prologue scan,
        // which would match hundreds of functions.
        find_hud_gates();
        if (!g_gateSite) { log_add("hud-view: no anchor, movie hook NOT installed"); return; }

        uint8_t* fn = nullptr;
        for (size_t back = 0; back < 0x400 && !fn; ++back)
        {
            uint8_t* p = g_gateSite - back;
            if (p < base) break;
            bool ok = true;
            for (size_t j = 0; j < sizeof(kLoopPat); ++j)
                if (p[j] != kLoopPat[j]) { ok = false; break; }
            if (ok) fn = p;
        }
        if (!fn || !in_exe_image(fn) || (size_t)(fn - base) >= size)
        {
            log_add("hud-view: movie-loop prologue not found, hook NOT installed");
            return;
        }

        MH_Initialize();     // idempotent — kiero/hooks.cpp has already done this
        if (MH_CreateHook(fn, (void*)&hkMovieLoop, (void**)&oMovieLoop) == MH_OK &&
            MH_EnableHook(fn) == MH_OK)
        {
            g_loopSite = fn;
            log_add("hud-view: movie loop hooked at %p", (void*)fn);
        }
        else
        {
            log_add("hud-view: MinHook FAILED on movie loop at %p", (void*)fn);
        }
    }

    void install_movie_vp_hook()
    {
        static bool tried = false;
        if (tried) return;
        tried = true;

        HMODULE exe = GetModuleHandleW(nullptr);
        if (!exe) return;
        uint8_t* base = (uint8_t*)exe;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        size_t size = nt->OptionalHeader.SizeOfImage;

        const size_t n = sizeof(kMovieVpPat);
        uint8_t* fn = nullptr;
        for (size_t i = 0; i + n <= size; ++i)
        {
            if (base[i] != 0x48 || base[i + 1] != 0x8B || base[i + 2] != 0xC4) continue;
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
                if (base[i + j] != kMovieVpPat[j]) { ok = false; break; }
            if (ok) { fn = base + i; break; }
        }
        if (!fn) { log_add("hud-view: per-movie viewport signature NOT FOUND"); return; }

        MH_Initialize();
        if (MH_CreateHook(fn, (void*)&hkMovieVp, (void**)&oMovieVp) == MH_OK &&
            MH_EnableHook(fn) == MH_OK)
        {
            g_movieVpSite = fn;
            log_add("hud-view: per-movie viewport hooked at %p", (void*)fn);
        }
        else
        {
            log_add("hud-view: MinHook FAILED on per-movie viewport at %p", (void*)fn);
        }
    }

    void load_settings(HINSTANCE self)
    {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(self, path, MAX_PATH)) return;
        for (int i = (int)wcslen(path) - 1; i >= 0; --i)
            if (path[i] == L'\\' || path[i] == L'/') { path[i + 1] = 0; break; }
        wcscat_s(path, L"akvr_settings.ini");

        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        char buf[4096]; DWORD got = 0;
        if (ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got)
        {
            buf[got] = 0;
            g_enabled = read_int(buf, "bigres",       g_enabled ? 1 : 0) != 0;
            g_renderH = read_int(buf, "rendersize",   g_renderH);
            g_square  = read_int(buf, "rendersquare", g_square ? 1 : 0) != 0;
            g_chrome  = read_int(buf, "bigreschrome", g_chrome);
            // Engine render size. ONE number (the height) — the width is DERIVED
            // from the headset's own angular shape, so the two axes can never drift
            // apart (JJ's rule: expose the constraint, not two controls). The shape
            // is written back each session by hooks.cpp from the measured frusta;
            // 911 = tan(42.35)/tan(45) x1000, the Quest 3 through VDXR.
            g_engineH     = read_int(buf, "engineres",   g_engineH);
            g_engineShape = read_int(buf, "engineshape", g_engineShape);
            g_geo11Shape  = read_int(buf, "geo11shape",  g_geo11Shape);
            g_hudScale    = read_int(buf, "hudscale", (int)(g_hudScale * 1000)) / 1000.0f;
            g_hudScaleV   = read_int(buf, "hudscalev", (int)(g_hudScale * 1000)) / 1000.0f;   // absent: follow width
            g_menuUiFill  = read_int(buf, "menuuifill", 1) != 0;
            g_hudRaise    = read_int(buf, "hudraise", 0) / 1000.0f;
            g_hudAspectFix = read_int(buf, "hudaspect", 1) != 0;
        }
        CloseHandle(h);

        if (g_renderH < 720)  g_renderH = 720;
        if (g_renderH > 4320) g_renderH = 4320;
        if (g_chrome  < 0)    g_chrome  = 0;
        if (g_chrome  > 200)  g_chrome  = 200;
        if (g_engineH && g_engineH < 720)  g_engineH = 720;
        if (g_engineH > 4320) g_engineH = 4320;
        if (g_engineShape < 300)  g_engineShape = 300;    // 0.30 .. 3.00 wide:tall
        if (g_engineShape > 3000) g_engineShape = 3000;

        // Derive the engine render size here, before anything reads render_w():
        // once the ENGINE size is ours, the buffer and window must be exactly it.
        // A buffer bigger than the render leaves black margins; smaller CROPS the
        // render (measured 2026-08-04 — that is all buffer-forcing ever did). So the
        // two are the same number by construction, never two settings.
        if (g_engineH >= 720)
        {
            g_engH = g_engineH;
            g_engW = (int)(((long long)g_engineH * g_engineShape) / 1000);
            g_engW &= ~7;                                 // keep it 8-aligned
        }
        else { g_engW = g_engH = 0; }
    }

    // GSAHOLD 2026-09-27 — the graphics-menu reset, found. Leaving the menu runs
    // URGFxMovieUI_OptionsGraphics::execApplySelectedOptions (0x140498500) -> vtable+0x770 =
    // 0x14001F2D0, which reads "Display_Mode" (2 = fullscreen), "ResolutionX", "ResolutionY" through
    // getters 0x140C67370/390/3B0 = GFSDK_GSA_GetOptionValue (NvGsa.x64.dll, NVIDIA GeForce Experience
    // game-settings store) and applies them: exclusive fullscreen 2560x1440 -> the view shrinks/clips.
    // (The earlier MENURES/MENUMODE holds were on the GFE registration copies, which apply never reads.)
    // Answer those three with windowed + the forced engine size whenever the engine size is forced.
    // GFSDK_GSA_GetOptionValue(OptionValueAndType* out, const wchar_t* name): value at out+8.
    typedef int (*GsaGetFn)(void* out, const wchar_t* name);
    GsaGetFn g_oGsaGet = nullptr;
    int      g_gsaMode = 0, g_gsaResX = 0, g_gsaResY = 0, g_gsaHits = 0;
    int      g_gsaComfort = 0, g_gsaBlurWas = -1;
    int hkGsaGetOptionValue(void* out, const wchar_t* name)
    {
        const int r = g_oGsaGet ? g_oGsaGet(out, name) : 0;
        // GSACOMFORT 2026-09-29 — JJ's fresh install: the config had MotionBlur=True, the mod flipped it,
        // and by the next start the game had put 5 comfort lines back (it keeps these options in this
        // store too and saves them back); head movement blurred. The store's names (wide strings in
        // BatmanAK.exe next to the GFSDK_GSA_RegisterOption calls): MotionBlur, Vsync, AdaptiveVsync.
        // Answer all three with 0 (off; bool or int, value at out+8), whatever the render size.
        if (out && name)
        {
            int32_t* v = (int32_t*)((uint8_t*)out + 8);
            if (wcscmp(name, L"MotionBlur") == 0) { if (g_gsaBlurWas < 0) g_gsaBlurWas = *v; *v = 0; ++g_gsaComfort; }
            else if (wcscmp(name, L"Vsync") == 0 || wcscmp(name, L"AdaptiveVsync") == 0) { *v = 0; ++g_gsaComfort; }
        }
        if (g_engOn && out && name && g_engW >= 64 && g_engH >= 64)
        {
            int32_t* v = (int32_t*)((uint8_t*)out + 8);
            if      (wcscmp(name, L"Display_Mode") == 0) { g_gsaMode = *v; *v = 0; ++g_gsaHits; }
            else if (wcscmp(name, L"ResolutionX")  == 0) { g_gsaResX = *v; *v = g_engW; ++g_gsaHits; }
            else if (wcscmp(name, L"ResolutionY")  == 0) { g_gsaResY = *v; *v = g_engH; ++g_gsaHits; }
        }
        return r;
    }

} // namespace

// ---------------------------------------------------------------------------
// entry point — called from DllMain, before the game runs
// ---------------------------------------------------------------------------
void akvr_early_init(HINSTANCE self)
{
    if (g_installed) return;
    g_installed = true;

    load_settings(self);

    // Must run before force_comfort_settings() — it decides one of the keys.
    detect_geo11(self);

    // ⛔ MEASURED 2026-08-06: a SQUARE render destroys geo-11's depth on Arkham.
    // At 2688x2688 a foreground car read as a flat card in front of everything and
    // the background trees sat in the wrong plane — separation and convergence
    // could not touch it, correctly so, because those scale depth GLOBALLY and
    // cannot misplace one object relative to another. 2560x1440 fixed it outright.
    // The fix's shader corrections assume 16:9, and AK constrains its camera to
    // 16:9 anyway. So when geo-11 is present, our render-shape forcing stands down
    // completely and the game keeps the size in its own ini (honoured on RESTART).
    // 2026-09-26: the fake monitor size stays available (bigres), but only as a 16:9
    // CEILING. Standing it down too left the game clamping its ini size to the
    // 2560x1440 monitor: ResX/ResY=3840x2160 was written, 2560x1440 came out
    // (startup log, build POSEDELAY). The ceiling never changes the shape — the ini
    // still decides — so it cannot reintroduce the square that broke the depth.
    // 2026-09-26 (build ENGINERES169): every Win32 size answer was faked (monitor,
    // work area, display mode and mode list) and the game STILL asked for 2560x1440,
    // so it sizes itself from somewhere we cannot reach by import patching. The
    // engine-res patch (writes AK's own ResX/ResY globals) is the lever that worked
    // on the AER build; its signature still matches this exe once. Under geo-11 it
    // now runs too, but always 16:9 from the height, never the square/portrait shape.
    if (g_geo11)
    {
        g_square = false;         // 16:9 ceiling only
        if (g_engineH >= 720)
        {
            // GEO11SHAPE 2026-09-26: the width/height ratio (x1000) is now a setting,
            // default 16:9. JJ added a geo-11 override forcing every non-square render
            // target to stereo (3Dmigoto treats SQUARE targets specially, the likely
            // reason the 2688x2688 test lost its depth). The headset's own shape under
            // FULLVIEW is ~1.14:1 in tan space, so equal detail both ways wants ~1.14.
            // EYEVIEW 2026-09-27: down to 0.70 wide:tall. Each eye's own view renders only
            // ~0.92:1 (SKVR runs 2528x2736 under geo-11). What broke depth here was exactly
            // SQUARE targets (3Dmigoto's square-RT rule), so an exact square is nudged off.
            int shape = g_geo11Shape < 700 ? 700 : (g_geo11Shape > 2400 ? 2400 : g_geo11Shape);
            if (shape == 1000) shape = 992;
            g_engH = g_engineH;
            g_engW = (int)(((long long)g_engineH * shape) / 1000) & ~7;
        }
    }

    // Before the engine reads a single config line — so a brand-new install is right
    // on its FIRST launch, not its second.
    force_comfort_settings();

    // We are the CEILING, not the request — the request is the game's own ini
    // resolution. So claim a work area with generous headroom on BOTH axes and let
    // the ini value through untouched. Width also allows for the game deriving a
    // 16:9 default from our claim, so the claim never becomes the binding limit.
    int wideW   = (g_renderH * 16) / 9;          // the 16:9 the game may derive from our claim
    int wantW   = render_w();
    g_fakeWorkW = (wantW > wideW ? wantW : wideW) + 128;
    g_fakeWorkH = g_renderH + g_chrome + 128;
    g_fakeScrW  = g_fakeWorkW;
    g_fakeScrH  = g_fakeWorkH + g_taskbar;

    HMODULE exe = GetModuleHandleW(nullptr);        // BatmanAK.exe
    if (!exe) return;

    const char* U = "USER32.dll";
    oGetSystemMetrics      = (GetSysMetricsFn) patch_import(exe, U, "GetSystemMetrics",      (void*)&hkGetSystemMetrics);
    oSystemParametersInfoA = (SPIAFn)          patch_import(exe, U, "SystemParametersInfoA", (void*)&hkSystemParametersInfoA);
    oSystemParametersInfoW = (SPIWFn)          patch_import(exe, U, "SystemParametersInfoW", (void*)&hkSystemParametersInfoW);
    oGetMonitorInfoA       = (GetMonInfoAFn)   patch_import(exe, U, "GetMonitorInfoA",       (void*)&hkGetMonitorInfoA);
    oGetMonitorInfoW       = (GetMonInfoWFn)   patch_import(exe, U, "GetMonitorInfoW",       (void*)&hkGetMonitorInfoW);
    oGetClientRect         = (GetClientRectFn) patch_import(exe, U, "GetClientRect",         (void*)&hkGetClientRect);
    oGetWindowInfo         = (GetWindowInfoFn) patch_import(exe, U, "GetWindowInfo",         (void*)&hkGetWindowInfo);
    oEnumDisplayMonitors   = (EnumDispMonFn)   patch_import(exe, U, "EnumDisplayMonitors",   (void*)&hkEnumDisplayMonitors);
    oAdjustWindowRect      = (AdjWndRectFn)    patch_import(exe, U, "AdjustWindowRect",      (void*)&hkAdjustWindowRect);
    oCreateWindowExW       = (CreateWinExWFn)  patch_import(exe, U, "CreateWindowExW",       (void*)&hkCreateWindowExW);
    oCreateWindowExA       = (CreateWinExAFn)  patch_import(exe, U, "CreateWindowExA",       (void*)&hkCreateWindowExA);
    oGetWindowRect         = (GetWindowRectFn) patch_import(exe, U, "GetWindowRect",         (void*)&hkGetWindowRect);
    oEnumDisplaySettingsA  = (EnumDispAFn)     patch_import(exe, U, "EnumDisplaySettingsA",  (void*)&hkEnumDisplaySettingsA);
    oEnumDisplaySettingsW  = (EnumDispWFn)     patch_import(exe, U, "EnumDisplaySettingsW",  (void*)&hkEnumDisplaySettingsW);

    // A patched-but-unresolved original would be a null call. Belt and braces:
    // anything we failed to capture falls back to the real export.
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32)
    {
        if (!oGetSystemMetrics)      oGetSystemMetrics      = (GetSysMetricsFn) GetProcAddress(u32, "GetSystemMetrics");
        if (!oSystemParametersInfoA) oSystemParametersInfoA = (SPIAFn)          GetProcAddress(u32, "SystemParametersInfoA");
        if (!oSystemParametersInfoW) oSystemParametersInfoW = (SPIWFn)          GetProcAddress(u32, "SystemParametersInfoW");
        if (!oGetMonitorInfoA)       oGetMonitorInfoA       = (GetMonInfoAFn)   GetProcAddress(u32, "GetMonitorInfoA");
        if (!oGetMonitorInfoW)       oGetMonitorInfoW       = (GetMonInfoWFn)   GetProcAddress(u32, "GetMonitorInfoW");
        if (!oGetClientRect)         oGetClientRect         = (GetClientRectFn) GetProcAddress(u32, "GetClientRect");
        if (!oGetWindowRect)         oGetWindowRect         = (GetWindowRectFn) GetProcAddress(u32, "GetWindowRect");
        if (!oCreateWindowExW)       oCreateWindowExW       = (CreateWinExWFn)  GetProcAddress(u32, "CreateWindowExW");
        if (!oCreateWindowExA)       oCreateWindowExA       = (CreateWinExAFn)  GetProcAddress(u32, "CreateWindowExA");
        if (!oEnumDisplaySettingsA)  oEnumDisplaySettingsA  = (EnumDispAFn)     GetProcAddress(u32, "EnumDisplaySettingsA");
        if (!oEnumDisplaySettingsW)  oEnumDisplaySettingsW  = (EnumDispWFn)     GetProcAddress(u32, "EnumDisplaySettingsW");
    }

    // The engine render size, patched into the game's own code before it runs.
    // (g_engW/g_engH were derived in load_settings — render_w() depends on them.)
    if (g_engH) install_engine_res();
    // GSAHOLD: the graphics menu's apply reads its values from NVIDIA's settings store.
    // Always installed since GSACOMFORT (motion blur / v-sync off); the size answers still need g_engH.
    g_oGsaGet = (GsaGetFn)patch_import(exe, "NvGsa.x64.dll", "GFSDK_GSA_GetOptionValue", (void*)&hkGsaGetOptionValue);

    log_add("bigres %s: want %dx%d (%s), claiming screen %dx%d work %dx%d, %d imports redirected",
            g_enabled ? "ON" : "OFF (logging only)",
            render_w(), g_renderH, g_square ? "square" : "16:9",
            g_fakeScrW, g_fakeScrH, g_fakeWorkW, g_fakeWorkH, g_patched);
}

// Let the DXGI-side hooks (hooks.cpp, installed later once dxgi.dll exists) write
// into the same startup transcript, so the panel shows one ordered story.
void akvr_early_note(const char* s) { log_add("%s", s); }
void akvr_early_fake_screen(int& w, int& h) { w = g_fakeScrW; h = g_fakeScrH; }

// ---- readouts for the overlay / persistence -------------------------------
// There is deliberately NO width setter: width is always derived from the size and
// the shape, so the two axes cannot drift apart.
const char* akvr_early_log()        { return g_log[0] ? g_log : "bigres: no calls recorded"; }
bool        akvr_early_enabled()    { return g_enabled; }
int         akvr_early_width()      { return render_w(); }
int         akvr_early_height()     { return render_h(); }
bool        akvr_early_geo11()      { return g_geo11; }
bool        akvr_early_square()     { return g_square; }
int         akvr_early_chrome()     { return g_chrome; }
int         akvr_early_patched()    { return g_patched; }
long        akvr_early_calls()      { return g_calls; }
void        akvr_early_enable(bool on)   { g_enabled = on; }       // takes effect next launch
void        akvr_early_size_set(int h)   { if (h >= 720 && h <= 4320) g_renderH = h; }
void        akvr_early_square_set(bool s){ g_square = s; }

// ---- HUD size — LIVE. Call akvr_hud_tick() once per frame from Present.
// Shrink the one global UI viewport rectangle and re-centre it; see find_hud_scale().
// This is guide 11's arithmetic verbatim: reconstruct the full extent, multiply by the
// scale, and split the leftover slack in half so the smaller box lands in the middle.
void akvr_hud_gameplay(bool on) { g_hudGameplay = on; }
void akvr_hud_anamorphic(bool on) { g_hudAnamorphic = on; }

// F1: dump the viewport log (see VpRec). Same clock as the frame log (QPC ms).
void akvr_hud_dump_viewports(const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) return;
    fprintf(f, "t_ms,src,view,tid,bufW,bufH,inL,inT,inW,inH,inAspect,outL,outT,outW,outH,outAspect\n");
    const LONG n = g_vpLogN;
    const LONG first = n > 4096 ? n - 4096 : 0;
    for (LONG i = first; i < n; ++i)
    {
        const VpRec& r = g_vpLog[i & 4095];
        fprintf(f, "%.3f,%c,%p,%lu,%d,%d,%d,%d,%d,%d,%.5f,%d,%d,%d,%d,%.5f\n",
                r.t, r.src, r.view, (unsigned long)r.tid, r.bufW, r.bufH,
                r.inL, r.inT, r.inW, r.inH, r.inAspect,
                r.outL, r.outT, r.outW, r.outH, r.outAspect);
    }
    fclose(f);
}

// MARKFIRST 2026-10-02 — JJ: "when first entering gameplay ... some HUD elements appear attached to the face and then
// quickly snap out to hang in space correctly". Until the first HUD-part read of a gameplay stretch (0.5 s) nothing
// carries its room mark, so the pieces the fix gives scene depth (top band, tips) are drawn into the head-locked
// picture. hudsplit sends the whole HUD to the room layer until the marks have had 200 ms to land.
volatile ULONGLONG g_hudMarksAt = 0;
bool akvr_hud_marks_ready()
{
    const ULONGLONG at = g_hudMarksAt;
    return at == 1 || (at != 0 && GetTickCount64() - at > 200);
}

void akvr_hud_tick()
{
    find_hud_scale();                      // one-shot; safe to call every frame
    find_hud_gates();                      // one-shot
    install_movie_hook();                  // one-shot
    install_movie_vp_hook();               // one-shot
    install_texdraw_hook();                // one-shot: TEXDRAW (radar flip)
    if (!g_hudFound) return;

    const int L = *g_uiL, T = *g_uiT, R = *g_uiR, B = *g_uiB;

    // Is this still our rectangle, or has the game written its own? Anything that is
    // not ours becomes the new baseline, so a resolution change re-baselines for free.
    const bool ours = g_uiWrote && L == g_uiWroteL && T == g_uiWroteT
                                && R == g_uiWroteR && B == g_uiWroteB;
    if (!ours)
    {
        if (R - L >= 64 && B - T >= 64)
        {
            g_uiBaseL = L; g_uiBaseT = T; g_uiBaseR = R; g_uiBaseB = B;
            g_uiHaveBase = true;
        }
        if (g_uiWrote) { g_hudSticks = false; ++g_uiStolen; }
    }

    // Report the raw state every frame, whatever happens next.
    _snprintf_s(g_hudDiag, sizeof(g_hudDiag), _TRUNCATE,
                "hud: rect %d,%d..%d,%d of %d,%d..%d,%d %s | gates %s | "
                "loop %s x%ld, per-movie %s x%ld, classes %d, viewports scaled %ld",
                L, T, R, B, g_uiBaseL, g_uiBaseT, g_uiBaseR, g_uiBaseB,
                g_uiWrote ? (g_hudSticks ? "STICKS" : "OVERWRITTEN") : "unscaled",
                !g_gateSite ? "NOT FOUND" : (g_gatesOpen ? "OPEN" : "stock"),
                g_loopSite ? "hooked" : "NO", g_loopCalls,
                g_movieVpSite ? "hooked" : "NO", g_movieVpCalls, g_vtCount, g_scaled);
    if (g_nViews)
    {
        const size_t len = strlen(g_hudDiag);
        _snprintf_s(g_hudDiag + len, sizeof(g_hudDiag) - len, _TRUNCATE,
                    " | %d movies, live push on (%s thread)", g_nViews,
                    g_sameThread ? "same" : "other");
    }
    {
        const size_t len = strlen(g_hudDiag);
        _snprintf_s(g_hudDiag + len, sizeof(g_hudDiag) - len, _TRUNCATE,
                    " | HUD area write %s, game overwrote it %ld times | texture draws %s: %ld, shrunk %ld, last %dx%d"
                    " | shrink method: %s (root sets %ld, movies %ld)",
                    g_hudGlobal ? "ON" : "OFF", g_uiStolen, g_texHooked ? "hooked" : "NOT hooked",
                    (long)g_texCalls, (long)g_texScaled, g_texBufW, g_texBufH,
                    !g_hudRootMode ? "screen box" : (g_treeSetMatrix ? "INSIDE the movie" : "screen box (root setter not found)"),
                    g_rootApplied, g_rootMovies);
    }

    watch_views();                         // VPWATCH: log viewport changes nobody announced
    // ROOTSHRINK: re-apply only if the movie's own matrix moved under us (slot 68 rewrites
    // it in place); a no-op compare otherwise.
    for (auto& r : g_roots) if (r.view && r.want) apply_root(r.view, false);
    // HUDLAYERS: find the layers again 3 s into every gameplay stretch (after loads the
    // movies are new), but only when the player has saved layer adjustments.
    // ROOMALL 2026-09-30: always with the whole-HUD rule on, and a first pass at 0.5 s so the HUD reaches the room
    // almost at once (JJ: "enters with the HUD elements attached to the face very briefly").
    {
        static ULONGLONG since = 0; static bool done = false, early = false;
        if (!g_hudGameplay) { since = 0; done = false; early = false; g_hudMarksAt = 0; }
        else
        {
            if (!since) since = GetTickCount64();
            const bool want = akvr_hud_layer_xf_list()[0] || akvr_hud_room_all();
            if (!early && GetTickCount64() - since > 500)
            { early = true; if (want) akvr_hud_layers_discover_quick(); g_hudMarksAt = GetTickCount64(); }
            if (!want && !g_hudMarksAt) g_hudMarksAt = 1;   // MARKFIRST: nothing to mark, nothing to wait for
            if (!done && GetTickCount64() - since > 3000)
            { done = true; if (want) akvr_hud_layers_discover_quick(); }
        }
    }
    if (!g_uiHaveBase) return;             // not initialised yet

    const int fullW = g_uiBaseR - g_uiBaseL;
    const int fullH = g_uiBaseB - g_uiBaseT;

    // Re-push only when size or projection aspect changes, including restoration
    // when projVR/screen mode changes. Never re-layout every frame for a stable HUD.
    g_hudAspectApplied = hud_aspect_factor(fullW, fullH);
    g_sameThread = (g_gfxThread == GetCurrentThreadId());
    static ULONGLONG lastPush = 0;
    const ULONGLONG now = GetTickCount64();
    // UIGATES: height scale and the menu full-height mode are part of the key too.
    const float pushKey = hud_eff() + 7.0f * hud_eff_v() + 3.0f * g_hudRaise + (g_menuUiFill ? 50.0f : 0.0f)
                        + (g_hudGameplay ? 100.0f : 0.0f) + 1000.0f * (float)g_shrinkGen;
    if (g_nViews > 0 && (g_pushedScale != pushKey
        || std::fabs(g_pushedAspect - g_hudAspectApplied) > 0.0001f) && now - lastPush >= 200)
    {
        repush_views();
        g_pushedScale = pushKey;
        g_pushedAspect = g_hudAspectApplied;
        lastPush = now;
    }
    // MENU3D (experimental): the pinned menu moves every frame the head moves, so it
    // is re-pushed every frame; repush_one still skips any movie already in place.
    // This is the per-frame re-layout that made menus crawl on 2026-08-05 — which is
    // exactly what this switch is here to find out about.
    // (MENU3D's every-frame push is REMOVED: it crashed the game. MENULIVE needs none.)

    // At 1.0 we write NOTHING — the default state must not touch the game's memory at
    // all. Only if we have actually scaled during this session do we restore the
    // game's own rectangle once, so dragging back to 1 is a genuine "off".
    // HUDFLICK 2026-09-26: the squash (see scale_viewport) goes into this rectangle
    // too. JJ saw HUD elements flicker after HUDFIT: the game re-sets movies from THIS
    // rectangle every frame and we then squashed them in the hook, so an unsquashed
    // layout existed for a moment each frame. Now the game's own set is already right.
    const float squashF = g_hudAspectApplied;
    const bool  squashing = squashF < 0.999f || squashF > 1.001f;
    if (!squashing && hud_eff() >= 0.999f && hud_eff() <= 1.001f
        && hud_eff_v() >= 0.999f && hud_eff_v() <= 1.001f)
    {
        if (g_uiWrote)
        {
            *g_uiL = g_uiBaseL; *g_uiT = g_uiBaseT;
            *g_uiR = g_uiBaseR; *g_uiB = g_uiBaseB;
            g_uiWrote = false; g_hudSticks = true;
        }
        set_hud_gates(false);              // and put the game's own routing back
        return;
    }

    // UIGATES 2026-09-26: the gates stay SHUT. Opening them routed movies that draw
    // into their own texture (map icons etc.) through the screen rectangle, and the
    // game put them back every frame — the flicker, measured in the viewport log. The
    // SetViewport hook already reaches every screen movie without them.
    set_hud_gates(false);

    // Live slider. MEASURED 2026-08-05 (build GFXVIEW2): Present is NOT the thread the
    // game set the viewports on, so the same-thread guard meant the slider never moved
    // anything and JJ had to relaunch. Relaxed deliberately: Get/SetViewport are plain
    // accessors on the movie object — no allocation, no list surgery — and we only call
    // them on the frame the VALUE ACTUALLY CHANGES, i.e. once per drag, not per frame.
    // That is a far smaller window than anything else already running here.
    // ...but RATE-LIMITED. Scaleform re-lays-out a movie every time its viewport is
    // set, so pushing on every frame of a slider drag is what made the HUD and the
    // menus crawl (measured in JJ's headset 2026-08-05). Five pushes a second still
    // reads as live under the hand, and the final value always lands within 200 ms of
    // letting go because the pending value is retried until it has been pushed.

    // HUDAREA: area write off -> give the game its own rectangle back once, then
    // leave it alone. The per-movie viewports above still carry the HUD size.
    if (!g_hudGlobal)
    {
        if (g_uiWrote)
        {
            *g_uiL = g_uiBaseL; *g_uiT = g_uiBaseT;
            *g_uiR = g_uiBaseR; *g_uiB = g_uiBaseB;
            g_uiWrote = false; g_hudSticks = true;
        }
        return;
    }

    int l, t, w, h;
    hud_rect(fullW, fullH, squashF, squashing, l, t, w, h);   // same maths as every movie
    g_uiWroteL = g_uiBaseL + l;
    g_uiWroteT = g_uiBaseT + t;
    g_uiWroteR = g_uiWroteL + w;
    g_uiWroteB = g_uiWroteT + h;

    *g_uiL = g_uiWroteL; *g_uiT = g_uiWroteT;
    *g_uiR = g_uiWroteR; *g_uiB = g_uiWroteB;
    g_uiWrote = true;
}
float akvr_hud_scale()          { return g_hudScale; }
float akvr_hud_scale_v()        { return g_hudScaleV; }
void  akvr_hud_scale_v_set(float s) { g_hudScaleV = s < 0.25f ? 0.25f : (s > 2.0f ? 2.0f : s); }
float akvr_hud_raise()          { return g_hudRaise; }
void  akvr_hud_raise_set(float r) { g_hudRaise = r < -0.3f ? -0.3f : (r > 0.3f ? 0.3f : r); }
bool  akvr_hud_menu_fill()      { return g_menuUiFill; }
int   akvr_hud_piece_count()     { return g_nMovies; }
const char* akvr_hud_piece_name(int i, int& bufW, int& bufH, void*& view)
{
    if (i < 0 || i >= g_nMovies) return "";
    bufW = g_movies[i].bufW; bufH = g_movies[i].bufH; view = g_movies[i].view;
    return g_movies[i].name;
}
void  akvr_hud_piece_xf(int i, float& s, float& x, float& y)
{
    s = 1.0f; x = y = 0.0f;
    if (i < 0 || i >= g_nMovies) return;
    s = g_movies[i].ps; x = g_movies[i].pdx; y = g_movies[i].pdy;
}
void  akvr_hud_piece_xf_set(int i, float s, float x, float y)
{
    if (i < 0 || i >= g_nMovies) return;
    s = s < 0.2f ? 0.2f : (s > 3.0f ? 3.0f : s);
    x = x < -0.6f ? -0.6f : (x > 0.6f ? 0.6f : x);
    y = y < -0.6f ? -0.6f : (y > 0.6f ? 0.6f : y);
    MovieInfo& m = g_movies[i];
    m.ps = s; m.pdx = x; m.pdy = y;
    if (m.name[0])
    {
        for (int k = 0; k < g_nMovies; ++k)                 // every copy of the same file
            if (strcmp(g_movies[k].name, m.name) == 0)
            { g_movies[k].ps = s; g_movies[k].pdx = x; g_movies[k].pdy = y; }
        // Rewrite the saved list without this name, then append it if not default.
        char key[100]; _snprintf_s(key, sizeof(key), _TRUNCATE, ";%s:", m.name);
        char out[2048] = "";
        const char* p = g_pieceXf;
        while (*p)
        {
            const char* next = strchr(p + 1, ';');
            const size_t len = next ? (size_t)(next - p) : strlen(p);
            if (!(strncmp(p, key, strlen(key)) == 0)) strncat_s(out, p, len);
            p += len;
        }
        if (piece_custom(m))
        {
            char add[160];
            _snprintf_s(add, sizeof(add), _TRUNCATE, ";%s:%.3f:%.3f:%.3f", m.name, s, x, y);
            strncat_s(out, add, _TRUNCATE);
        }
        strcpy_s(g_pieceXf, out);
    }
    ++g_shrinkGen;
}
const char* akvr_hud_piece_xf_list() { return g_pieceXf; }
void  akvr_hud_piece_xf_list_set(const char* list)
{
    strncpy_s(g_pieceXf, list ? list : "", _TRUNCATE);
    for (int k = 0; k < g_nMovies; ++k) piece_xf_load(g_movies[k]);
    ++g_shrinkGen;
}
int   akvr_hud_id_depth() { return g_idDepth; }
bool  akvr_hud_piece_shrink(int i) { return i >= 0 && i < g_nMovies ? g_movies[i].shrink : true; }
void  akvr_hud_piece_shrink_set(int i, bool on)
{
    if (i < 0 || i >= g_nMovies) return;
    MovieInfo& m = g_movies[i];
    m.shrink = on;
    // Every movie with the same file shares the switch, now and in later sessions.
    for (int k = 0; k < g_nMovies; ++k)
        if (m.name[0] && strcmp(g_movies[k].name, m.name) == 0) g_movies[k].shrink = on;
    if (m.name[0])
    {
        char key[100]; _snprintf_s(key, sizeof(key), _TRUNCATE, ";%s;", m.name);
        char* hit = strstr(g_noShrink, key);
        if (!on && !hit)
        {
            if (!g_noShrink[0]) strcpy_s(g_noShrink, ";");
            strncat_s(g_noShrink, key + 1, _TRUNCATE);
        }
        else if (on && hit)
            memmove(hit, hit + strlen(key) - 1, strlen(hit + strlen(key) - 1) + 1);
    }
    ++g_shrinkGen;
}
const char* akvr_hud_noshrink()    { return g_noShrink; }
void  akvr_hud_noshrink_set(const char* list)
{
    strncpy_s(g_noShrink, list ? list : "", _TRUNCATE);
    for (int k = 0; k < g_nMovies; ++k) g_movies[k].shrink = !name_excluded(g_movies[k].name);
    ++g_shrinkGen;
}
// ============================== HUDLAYERS ==============================
// 2026-09-27 — JJ wants the radar moved/scaled apart from the compass. Both live in the
// same ModularHudBm3 movie (PIECES: four copies of that container, each hosting several
// modules), so the split happens INSIDE a movie, on its render-tree nodes.
// Static RE (BatmanAK.exe, Scaleform 4 render tree):
//   node      0x38 bytes in a 4 KB page, first at page+0x38; parent at node+0x20
//   data      [[page+0x20] + idx*8 + 0x28], idx = (node-page-0x38)/0x38 (slot-27 loop)
//             data+0x0a flags (0x200 = 3D), data+0x10 the 2x4 matrix (stage twips)
//   children  TreeContainer::Insert 0x1411cd480 (Change 0x100) / Remove 0x1411cd4f0
//             (0x200) keep them at data+0x90: 0 = none; untagged = up to two children
//             inline at +0x90/+0x98; low bit set = array (count +0x08, items from +0x10)
// Phase 1 (build LAYERS) guessed the list and found none; the F2 dump still showed nodes
// whose parent was the root, which is what led to the insert function.
// Moving a layer = TreeNode::SetMatrix on its node, on the game's GFx thread, every frame
// just before the movie's slot-27 call (measured once per movie per frame, arguments
// rcx/dl only). The game rewrites a node's matrix only when its display object moves, so
// the game's own value is taken as the new base whenever the node no longer holds ours.
namespace
{
    bool node_data(uintptr_t node, uintptr_t& data)
    {
        const uintptr_t page = node & ~(uintptr_t)0xFFF;
        if (node < page + 0x38 || !page_ok(page, 0x30)) return false;
        const uintptr_t idx = (node - page - 0x38) / 0x38;
        uintptr_t tbl = 0;
        __try { tbl = *(uintptr_t*)(page + 0x20); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!readable_ptr(tbl) || !page_ok(tbl + 0x28 + idx * 8, 8)) return false;
        __try { data = *(uintptr_t*)(tbl + 0x28 + idx * 8); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return readable_ptr(data) && page_ok(data, 0xa0);
    }
    bool is_child_of(uintptr_t c, uintptr_t parent)
    {
        if (!readable_ptr(c) || (c & 7) || !page_ok(c, 0x38)) return false;
        __try { return *(uintptr_t*)(c + 0x20) == parent; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int node_children(uintptr_t node, uintptr_t* out, int max)
    {
        uintptr_t d = 0;
        if (!node_data(node, d)) return 0;
        uintptr_t s0 = 0, s1 = 0;
        __try { s0 = *(uintptr_t*)(d + 0x90); s1 = *(uintptr_t*)(d + 0x98); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (!s0) return 0;
        int n = 0;
        if (s0 & 1)
        {
            const uintptr_t a = s0 & ~(uintptr_t)1;
            if (!page_ok(a, 0x10)) return 0;
            uintptr_t cnt = 0;
            __try { cnt = *(uintptr_t*)(a + 8); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
            if (cnt > 512 || !page_ok(a + 0x10, (size_t)cnt * 8)) return 0;
            for (uintptr_t i = 0; i < cnt && n < max; ++i)
            {
                uintptr_t c = 0;
                __try { c = *(uintptr_t*)(a + 0x10 + i * 8); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                if (is_child_of(c, node)) out[n++] = c;
            }
        }
        else
        {
            if (is_child_of(s0, node)) out[n++] = s0;
            if (s1 && n < max && is_child_of(s1, node)) out[n++] = s1;
        }
        return n;
    }
    bool node_matrix(uintptr_t node, float* m, int* flags)
    {
        uintptr_t d = 0;
        if (!node_data(node, d)) return false;
        __try { memcpy(m, (void*)(d + 0x10), 12 * sizeof(float)); if (flags) *flags = *(uint16_t*)(d + 0x0a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ---- the layer list (rebuilt by discovery; settings survive by key) ----
    struct Layer
    {
        void* view; uintptr_t node, parentNode; int depth, parent, kids, flags;
        char name[48]; char key[96]; char path[40];
        float s, dx, dy; bool hide;             // the player's adjustment
        float base[12], last[12]; bool haveLast;   // 2D uses the first 8 (2x4), 3D all 12 (3x4)
        bool hidByUs, baseVisible;                  // HIDE: our visible=false and the game's own state
        bool room, taggedByUs;                      // PARTTAG: "hang in the room" (colour mark) and ours applied
        bool zoomHide;                              // ZOOMHIDE: hidden only while the zoom vignette is on
        bool world, autoRoom;                       // ROOMALL: "stays on its target"; marked by the whole-HUD rule
    };
    Layer   g_layers[2048];
    int     g_nLayers = 0;
    SRWLOCK g_layerLock = SRWLOCK_INIT;
    char    g_layerXf[4096] = "";               // ";key:size:x:y:hide:room:zoomhide:world" per adjusted layer
    // ROOMALL 2026-09-30 — JJ: ticking parts one by one "is pretty hard to sift through"; "have everything hang in the
    // room automatically, and just the ones that shouldn't be unticked". With g_roomAll the mod marks every MAXIMAL
    // branch of the HUD tree that holds no "stays on its target" part: a branch is marked when it contains no world
    // part but its parent does (or it is a container root). Marks never stack or need cancelling, and anything the
    // game adds under a marked branch later (pop-ups, tips) inherits the mark.
    bool    g_roomAll = true;
    volatile bool g_autoDirty = true;
    int     g_layerAdjusted = 0;                // layers with an adjustment (fast skip)
    long    g_layerSets = 0;
    char    g_layerDiag[224] = "HUD layers: not searched yet";

    bool layer_custom(const Layer& L)
    {
        return L.hide || L.room || L.zoomHide || L.world || std::fabs(L.s - 1.0f) > 0.001f || std::fabs(L.dx) > 0.0005f || std::fabs(L.dy) > 0.0005f;
    }
    void layer_load(Layer& L)
    {
        L.s = 1.0f; L.dx = L.dy = 0.0f; L.hide = false; L.room = false; L.zoomHide = false; L.world = false;
        char k[112]; _snprintf_s(k, sizeof(k), _TRUNCATE, ";%s:", L.key);
        const char* hit = strstr(g_layerXf, k);
        if (!hit) return;
        float s = 1, x = 0, y = 0; int h = 0, rm = 0, zh = 0, wd = 0;
        if (sscanf_s(hit + strlen(k), "%f:%f:%f:%d:%d:%d:%d", &s, &x, &y, &h, &rm, &zh, &wd) >= 3)
        { L.s = s; L.dx = x; L.dy = y; L.hide = h != 0; L.room = rm != 0; L.zoomHide = zh != 0; L.world = wd != 0; }
    }
    void layer_save(const Layer& L)
    {
        char k[112]; _snprintf_s(k, sizeof(k), _TRUNCATE, ";%s:", L.key);
        char out[4096] = "";
        for (const char* p = g_layerXf; *p; )
        {
            const char* next = strchr(p + 1, ';');
            const size_t len = next ? (size_t)(next - p) : strlen(p);
            if (strncmp(p, k, strlen(k)) != 0) strncat_s(out, p, len);
            p += len;
        }
        if (layer_custom(L))
        {
            char add[160];
            _snprintf_s(add, sizeof(add), _TRUNCATE, ";%s:%.3f:%.4f:%.4f:%d:%d:%d:%d", L.key, L.s, L.dx, L.dy, L.hide ? 1 : 0, L.room ? 1 : 0, L.zoomHide ? 1 : 0, L.world ? 1 : 0);
            strncat_s(out, add, _TRUNCATE);
        }
        strcpy_s(g_layerXf, out);
    }

    // ---- names: display objects hold a pointer to their render node and an ASString
    // name (ASString -> node -> char data). A breadth-first pointer search from the movie's
    // main sprite (view+0x50) pairs them. Heuristic: the first identifier-like string in
    // the object that points at the node. Run only on discovery.
    bool as_name(uintptr_t p, char* out)
    {
        if (!readable_ptr(p) || (p & 7) || !page_ok(p, 8)) return false;
        uintptr_t s = 0;
        __try { s = *(uintptr_t*)p; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!readable_ptr(s) || !page_ok(s, 48)) return false;
        __try
        {
            const char* c = (const char*)s;
            if (!((c[0] >= 'A' && c[0] <= 'Z') || (c[0] >= 'a' && c[0] <= 'z'))) return false;
            int n = 0;
            for (; n < 40 && c[n]; ++n)
                if (!((c[n] >= 'A' && c[n] <= 'Z') || (c[n] >= 'a' && c[n] <= 'z') ||
                      (c[n] >= '0' && c[n] <= '9') || c[n] == '_')) return false;
            if (n < 3 || c[n]) return false;
            memcpy(out, c, n); out[n] = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    const int kSeenBits = 16;
    uintptr_t g_seen[1 << kSeenBits];
    bool seen_insert(uintptr_t p)
    {
        uint32_t h = (uint32_t)((p >> 3) * 2654435761u) >> (32 - kSeenBits);
        for (int i = 0; i < 64; ++i, h = (h + 1) & ((1 << kSeenBits) - 1))
        {
            if (g_seen[h] == p) return false;
            if (!g_seen[h]) { g_seen[h] = p; return true; }
        }
        return false;                            // table crowded: treat as seen
    }
    // Node -> layer index, hashed (the name search tests ~1M pointers per container).
    uintptr_t g_lhNode[4096]; int g_lhIdx[4096];
    uint32_t lh_slot(uintptr_t p) { return (uint32_t)((p >> 3) * 2654435761u) >> 20; }
    void layer_hash_build(int first, int last)
    {
        memset(g_lhNode, 0, sizeof(g_lhNode));
        for (int i = first; i < last; ++i)
            for (uint32_t h = lh_slot(g_layers[i].node), n = 0; n < 4096; ++n, h = (h + 1) & 4095)
                if (!g_lhNode[h]) { g_lhNode[h] = g_layers[i].node; g_lhIdx[h] = i; break; }
    }
    int layer_of(uintptr_t node, int, int)
    {
        if (!node) return -1;
        for (uint32_t h = lh_slot(node), n = 0; n < 4096; ++n, h = (h + 1) & 4095)
        {
            if (!g_lhNode[h]) return -1;
            if (g_lhNode[h] == node) return g_lhIdx[h];
        }
        return -1;
    }
    void name_layers(uintptr_t sprite, int first, int last)
    {
        static uintptr_t q[24000]; static uint8_t qd[24000];
        memset(g_seen, 0, sizeof(g_seen));
        layer_hash_build(first, last);
        int head = 0, tail = 0;
        q[tail] = sprite; qd[tail++] = 0; seen_insert(sprite);
        while (head < tail)
        {
            const uintptr_t o = q[head]; const int d = qd[head++];
            if (!page_ok(o, 0x180)) continue;
            int li = -1;
            for (int off = 0; off < 0x180 && li < 0; off += 8)
            {
                uintptr_t p = 0;
                __try { p = *(uintptr_t*)(o + off); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                li = layer_of(p, first, last);
            }
            if (li >= 0 && !g_layers[li].name[0])
                for (int off = 0; off < 0x180; off += 8)
                {
                    uintptr_t p = 0;
                    __try { p = *(uintptr_t*)(o + off); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                    char nm[48];
                    if (as_name(p, nm) && strncmp(nm, "instance", 8) != 0 && strncmp(nm, "added", 5) != 0 &&
                        strncmp(nm, "removed", 7) != 0 && strcmp(nm, "enterFrame") != 0 && strcmp(nm, "render") != 0)
                    { strcpy_s(g_layers[li].name, nm); break; }
                }
            if (d >= 12) continue;
            for (int off = 0; off < 0x180 && tail < 24000; off += 8)
            {
                uintptr_t p = 0;
                __try { p = *(uintptr_t*)(o + off); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (!readable_ptr(p) || (p & 7)) continue;
                if (seen_insert(p)) { q[tail] = p; qd[tail++] = (uint8_t)(d + 1); }
            }
        }
    }

    // ---- CONTAINERFP 2026-09-27 — JJ: "the parent nodes were named differently the second
    // time around, and my settings were lost". The name search is a heuristic (it saved one
    // container as "antiAliasType", a TextField property), and container numbers follow
    // creation order, so neither can carry a setting across launches. A container is now
    // recognised by a FINGERPRINT: the set of its parts' design positions (depth, x, y in
    // 50-twip steps, has-children), each hashed to 16 bits. On discovery each container
    // takes the saved fingerprint it overlaps most (Jaccard >= 0.35, one-to-one); animated
    // or runtime-added parts only lower the score. Unmatched containers get a new id.
    // Saved as hudcontainers=K1=hhhh,hhhh,...;K2=...
    struct ContFp { char id[8]; uint16_t t[160]; int n; bool taken; };
    ContFp g_contFps[16];
    int    g_nContFps = 0;
    char   g_contFpList[4600] = "";
    void cont_fp_parse()
    {
        g_nContFps = 0;
        const char* p = g_contFpList;
        while (*p && g_nContFps < 16)
        {
            while (*p == ';') ++p;
            const char* eq = strchr(p, '=');
            if (!eq) break;
            ContFp& c = g_contFps[g_nContFps];
            memset(&c, 0, sizeof(c));
            const size_t idLen = (size_t)(eq - p) < sizeof(c.id) - 1 ? (size_t)(eq - p) : sizeof(c.id) - 1;
            memcpy(c.id, p, idLen);
            p = eq + 1;
            while (*p && *p != ';' && c.n < 160)
            {
                unsigned v = 0;
                if (sscanf_s(p, "%4x", &v) == 1) c.t[c.n++] = (uint16_t)v;
                while (*p && *p != ',' && *p != ';') ++p;
                if (*p == ',') ++p;
            }
            while (*p && *p != ';') ++p;
            if (c.id[0]) ++g_nContFps;
        }
    }
    void cont_fp_serialize()
    {
        char out[4600] = "";
        for (int i = 0; i < g_nContFps; ++i)
        {
            char head[16]; _snprintf_s(head, sizeof(head), _TRUNCATE, ";%s=", g_contFps[i].id);
            strncat_s(out, head, _TRUNCATE);
            for (int k = 0; k < g_contFps[i].n; ++k)
            {
                char t[8]; _snprintf_s(t, sizeof(t), _TRUNCATE, k ? ",%04x" : "%04x", g_contFps[i].t[k]);
                strncat_s(out, t, _TRUNCATE);
            }
        }
        strcpy_s(g_contFpList, out);
    }
    int cont_tokens(int first, int last, uint16_t* out)
    {
        int n = 0;
        for (int k = first + 1; k < last && n < 160; ++k)
        {
            const Layer& L = g_layers[k];
            if (L.depth < 6 || L.depth > 11) continue;          // skip the shared container frame
            const int x = (int)floorf(L.base[3] / 50.0f + 0.5f), y = (int)floorf(L.base[7] / 50.0f + 0.5f);
            uint32_t h = (uint32_t)L.depth * 73856093u ^ (uint32_t)x * 19349663u ^ (uint32_t)y * 83492791u
                       ^ (L.kids ? 2654435761u : 0u);
            h ^= h >> 16;
            const uint16_t t = (uint16_t)h;
            bool dup = false;
            for (int j = 0; j < n && !dup; ++j) dup = out[j] == t;
            if (!dup) out[n++] = t;
        }
        return n;
    }
    float cont_similarity(const uint16_t* a, int na, const uint16_t* b, int nb)
    {
        if (!na || !nb) return 0.0f;
        int common = 0;
        for (int i = 0; i < na; ++i)
            for (int j = 0; j < nb; ++j) if (a[i] == b[j]) { ++common; break; }
        return (float)common / (float)(na + nb - common);
    }
    // Assign ids to all containers at once, best-matching pairs first (Jaccard >= 0.35);
    // unmatched containers get a new id. ids[i] receives an index into g_contFps.
    void cont_assign(const int* firsts, const int* lasts, int nc, int* ids)
    {
        static uint16_t tok[16][160]; int ntok[16];
        for (int i = 0; i < nc; ++i) { ids[i] = -1; ntok[i] = cont_tokens(firsts[i], lasts[i], tok[i]); }
        for (;;)
        {
            int bi = -1, bj = -1; float best = 0.35f;
            for (int i = 0; i < nc; ++i)
            {
                if (ids[i] >= 0) continue;
                for (int j = 0; j < g_nContFps; ++j)
                {
                    if (g_contFps[j].taken) continue;
                    const float sim = cont_similarity(tok[i], ntok[i], g_contFps[j].t, g_contFps[j].n);
                    if (sim >= best) { best = sim; bi = i; bj = j; }
                }
            }
            if (bi < 0) break;
            ids[bi] = bj; g_contFps[bj].taken = true;
        }
        for (int i = 0; i < nc; ++i)
        {
            if (ids[i] < 0)   // ROOMALL2: an unused (empty) slot keeps its id, so the known ids never move
                for (int j = 0; j < g_nContFps; ++j)
                    if (!g_contFps[j].taken && g_contFps[j].n == 0) { ids[i] = j; g_contFps[j].taken = true; break; }
            if (ids[i] < 0 && g_nContFps < 16)
            {
                int maxId = 0;
                for (int j = 0; j < g_nContFps; ++j) { int v = 0; if (sscanf_s(g_contFps[j].id, "K%d", &v) == 1 && v > maxId) maxId = v; }
                ids[i] = g_nContFps++;
                memset(&g_contFps[ids[i]], 0, sizeof(ContFp));
                _snprintf_s(g_contFps[ids[i]].id, sizeof(g_contFps[ids[i]].id), _TRUNCATE, "K%d", maxId + 1);
                g_contFps[ids[i]].taken = true;
            }
            // refresh: follows slow drift. ROOMALL2: never from a half-built container (fewer than 80% of the saved
            // parts - the 0.5 s pass can run while the game is still building its HUD), so saved ids stay stable.
            if (ids[i] >= 0 && ntok[i] * 5 >= g_contFps[ids[i]].n * 4)
            { memcpy(g_contFps[ids[i]].t, tok[i], sizeof(uint16_t) * ntok[i]); g_contFps[ids[i]].n = ntok[i]; }
        }
    }

    // ---- discovery: walk every ModularHud movie's tree (depth <= 7) ----
    // PAUSESTUTTER 2026-09-27 — JJ: "a severe stutter of one to two seconds, about three seconds
    // after returning from a pause screen or the map". That is the automatic re-discovery 3 s into
    // every gameplay stretch: its name search (name_layers) walks ~24k objects per container and
    // tests ~1M pointers with VirtualQuery, on the game thread. Names are panel labels only (keys
    // are fingerprints + paths), so the automatic pass reuses the last names by key and skips the
    // search; the panel button and the F2 dump still search. Last pass timing in the diag.
    struct NameByKey { char key[96]; char name[48]; };
    NameByKey g_nameCache[2048];
    int       g_nNameCache = 0;
    double    g_discoverMs = 0.0;
    void tag_after_discover();                  // MARKCARRY: below, beside the colour guard's list
    void discover_layers(bool withNames = true)
    {
        LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
        root_ready();                            // the matrix setter (ROOTSHRINK)
        AcquireSRWLockExclusive(&g_layerLock);
        // Names found so far, by key (skips container rows, whose name is the id).
        for (int i = 0; i < g_nLayers; ++i)
        {
            const Layer& L = g_layers[i];
            if (!L.name[0] || L.depth == 0 || !L.key[0]) continue;
            int at = -1;
            for (int c = 0; c < g_nNameCache && at < 0; ++c) if (strcmp(g_nameCache[c].key, L.key) == 0) at = c;
            if (at < 0 && g_nNameCache < 2048) at = g_nNameCache++;
            if (at >= 0) { strcpy_s(g_nameCache[at].key, L.key); strcpy_s(g_nameCache[at].name, L.name); }
        }
        // Keep what was applied to nodes that are still there: without the game's own
        // matrix (base) and ours (last), a moved part would be moved again on top.
        // MARKCARRY 2026-09-30 — JJ: the reticle and target distance sat at HUD depth "until you actually hit R1".
        // A re-read forgot which nodes carried OUR colour mark (taggedByUs), so a mark set by an earlier read (the
        // 0.5 s pass, while the HUD is half-built) was never taken off when the next read found a world part under it,
        // and the colour guard kept putting it back. The mark state now travels with the node.
        struct Carry { uintptr_t node; float base[12], last[12]; bool have, hid, vis, tagged; };
        static Carry carry[2048];
        int nCarry = 0;
        for (int i = 0; i < g_nLayers; ++i)
            if ((g_layers[i].haveLast || g_layers[i].hidByUs || g_layers[i].taggedByUs) && g_layers[i].node)
            {
                carry[nCarry].node = g_layers[i].node;
                carry[nCarry].tagged = g_layers[i].taggedByUs; carry[nCarry].have = g_layers[i].haveLast;
                carry[nCarry].hid = g_layers[i].hidByUs; carry[nCarry].vis = g_layers[i].baseVisible;
                memcpy(carry[nCarry].base, g_layers[i].base, sizeof(carry[0].base));
                memcpy(carry[nCarry].last, g_layers[i].last, sizeof(carry[0].last));
                ++nCarry;
            }
        g_nLayers = 0; g_layerAdjusted = 0; g_autoDirty = true;   // ROOMALL
        int containers = 0, named = 0;
        int cFirst[16] = {}, cLast[16] = {};
        for (int i = 0; i < g_nContFps; ++i) g_contFps[i].taken = false;
        // ROOMALL2: the main HUD (ModularHud) first, so the other movies can never crowd its parts out of the list.
        int mainContainers = 0;
        for (int pass = 0; pass < 2; ++pass, mainContainers = pass == 1 ? containers : mainContainers)
        for (int i = 0; i < g_nMovies && g_nLayers < 2040; ++i)
        {
            const MovieInfo& m = g_movies[i];
            if ((pass == 0) != (strstr(m.name, "ModularHud") != nullptr)) continue;
            // ROOMALL2 2026-09-30 — JJ: the launch objective text and a small arrow by Local Surveillance stayed on the
            // face: they are in other HUD movies (FrontMostLayer, unnamed ones, ...). Every movie seen drawing on the
            // screen joins the tree now, except the front-end background fader.
            if (!m.view || !m.screenSeen || strstr(m.name, "FrontendBGFader")) continue;
            uintptr_t root = 0, sprite = 0;
            __try { root = *(uintptr_t*)((uint8_t*)m.view + 0x88); sprite = *(uintptr_t*)((uint8_t*)m.view + 0x50); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!readable_ptr(root)) continue;
            const int first = g_nLayers;
            Layer& R = g_layers[g_nLayers++];
            memset(&R, 0, sizeof(R));
            R.view = m.view; R.node = root; R.depth = 0; R.parent = -1; R.s = 1.0f;
            _snprintf_s(R.path, sizeof(R.path), _TRUNCATE, "c%d", containers);
            for (int k = first; k < g_nLayers; ++k)
            {
                Layer& L = g_layers[k];
                L.flags = 0;
                node_matrix(L.node, L.base, &L.flags);
                if (L.depth >= 12) continue;          // compass/radar sit ~9 deep
                uintptr_t kids[96];
                const int n = node_children(L.node, kids, 96);
                L.kids = n;
                for (int c = 0; c < n && g_nLayers < 2040; ++c)
                {
                    Layer& C = g_layers[g_nLayers++];
                    memset(&C, 0, sizeof(C));
                    C.view = m.view; C.node = kids[c]; C.parentNode = L.node;
                    C.depth = L.depth + 1; C.parent = k; C.s = 1.0f;
                    _snprintf_s(C.path, sizeof(C.path), _TRUNCATE, "%s.%d", L.path, c);
                }
            }
            if (withNames && readable_ptr(sprite)) name_layers(sprite, first, g_nLayers);
            if (containers < 16) { cFirst[containers] = first; cLast[containers] = g_nLayers; }
            ++containers;
        }
        // CONTAINERFP: ids from fingerprints (all containers at once, best pairs first);
        // a part's key is the id + its path inside the container. Names stay labels only.
        int cIds[16];
        const int nc = containers < 16 ? containers : 16;
        // ROOMALL2: the main HUD's containers claim their saved ids first; the other movies only get what is left
        // (several saved fingerprints are one part long, and a new container must never take their id).
        const int nMain = mainContainers < nc ? mainContainers : nc;
        cont_assign(cFirst, cLast, nMain, cIds);
        if (nc > nMain) cont_assign(cFirst + nMain, cLast + nMain, nc - nMain, cIds + nMain);
        for (int ci = 0; ci < nc; ++ci)
        {
            const int first = cFirst[ci];
            char ck[16]; strcpy_s(ck, cIds[ci] >= 0 ? g_contFps[cIds[ci]].id : "K?");
            strcpy_s(g_layers[first].name, ck);                  // shown on the container row
            for (int k = first; k < cLast[ci]; ++k)
            {
                Layer& L = g_layers[k];
                const char* rel = strchr(L.path, '.');
                if (L.name[0] && k != first) ++named;
                _snprintf_s(L.key, sizeof(L.key), _TRUNCATE, "%s/%s", ck, rel ? rel + 1 : "root");
                if (!withNames && k != first)
                    for (int c = 0; c < g_nNameCache; ++c)
                        if (strcmp(g_nameCache[c].key, L.key) == 0) { strcpy_s(L.name, g_nameCache[c].name); ++named; break; }
                for (int c = 0; c < nCarry; ++c)
                    if (carry[c].node == L.node)
                    {
                        memcpy(L.base, carry[c].base, sizeof(L.base));
                        memcpy(L.last, carry[c].last, sizeof(L.last));
                        L.haveLast = carry[c].have;
                        L.hidByUs = carry[c].hid; L.baseVisible = carry[c].vis;
                        L.taggedByUs = carry[c].tagged;   // MARKCARRY
                        break;
                    }
                layer_load(L);
                if (layer_custom(L)) ++g_layerAdjusted;
            }
        }
        cont_fp_serialize();
        tag_after_discover();                    // MARKCARRY
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
        g_discoverMs = 1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
        _snprintf_s(g_layerDiag, sizeof(g_layerDiag), _TRUNCATE,
                    "HUD layers: %d in %d containers, %d named, %d adjusted, matrix sets %ld, last search %.1f ms (%s)",
                    g_nLayers, containers, named, g_layerAdjusted, g_layerSets, g_discoverMs,
                    withNames ? "with names" : "quick");
        ReleaseSRWLockExclusive(&g_layerLock);
        log_add("hud-layers: search %.1f ms (%s), %d parts, %d named, %d adjusted",
                g_discoverMs, withNames ? "with names" : "quick", g_nLayers, named, g_layerAdjusted);
    }

    // ---- per frame, game thread, one movie ----
    volatile LONG g_layerRestores = 0;          // parts reset in the panel, restored here
    // TreeNode::SetVisible 0x1411ccd80 (flags bit 0, Change 4) and SetMatrix3D 0x1411cdce0
    // (copies 0x30 bytes = 3x4 to data+0x10, sets flag 0x200), found 2026-09-27 (HIDE3D).
    typedef void (*TreeSetVisibleFn)(void* node, bool visible);
    typedef void (*TreeSetMatrix3Fn)(void* node, const float* m3x4);
    typedef void* (*TreeWritableFn)(void* node, unsigned change);   // PARTTAG: TreeNode::GetWritableData
    TreeWritableFn g_treeWritable = nullptr;
    constexpr float kPartTag = -1.0f / 512.0f;   // add blue: below one 8-bit step; shaders look for -0.003..-0.001
    long g_tagSets = 0;
    // PARTTAG2 2026-09-30 — JJ: ticking "hang in the room" changed nothing and "marks set" kept rising: the game
    // rewrote the part's colour every frame after our mark. DisplayObjectBase::SetCxform 0x1411eedf0 (obj, cx)
    // copies the object's own colour transform (all 8 floats) onto its render node [obj+0x48]; the alpha setter
    // 0x1411efe60 keeps the node's add row, so only SetCxform wipes the mark. Hooked: after the game's write to a
    // marked node, the mark goes straight back, before the frame is captured.
    typedef void (*ObjSetCxformFn)(void* obj, const float* cx);
    ObjSetCxformFn g_origObjCx = nullptr;
    uintptr_t volatile g_tagNodes[1024] = {};   // ROOMALL: many branches
    volatile LONG g_tagNodeN = 0;
    long g_tagRewrites = 0, g_tagReadOk = 0, g_tagReadBad = 0;
    bool tag_node(uintptr_t node)
    {
        const LONG n = g_tagNodeN;
        for (LONG i = 0; i < n; ++i) if (g_tagNodes[i] == node) return true;
        return false;
    }
    // MARKCARRY 2026-09-30 (called by discover_layers, layer lock held): the guard list keeps only nodes the new
    // tree still holds with our mark, so a node that left the tree is never written again. (The per-part mark state
    // is in the F2 hudlayers.txt: the startup log stops recording long before gameplay.)
    void tag_after_discover()
    {
        LONG kept = 0;
        const LONG n = g_tagNodeN;
        for (LONG k = 0; k < n; ++k)
        {
            bool keep = false;
            for (int i = 0; i < g_nLayers && !keep; ++i) keep = g_layers[i].node == g_tagNodes[k] && g_layers[i].taggedByUs;
            if (keep) g_tagNodes[kept++] = g_tagNodes[k];
        }
        InterlockedExchange(&g_tagNodeN, kept);
    }
    // PARTTAG4/5: the mark goes into add red, green, blue AND alpha (+0x60..+0x6C). CBDUMP showed pieces coloured by
    // their own add (multiply 0), where only the alpha add (0 for such a child) still shows the mark.
    // KEEPGLOW 2026-09-30 — JJ: two compass pieces he had ticked "hang in the room" (K2/...2.0.6, .2.0.7) "should be a
    // glow but it looks like a darkening effect". Writing the mark REPLACED the part's own colour add, and Flash glows and
    // brightness tints are made of add: only the dimming multiply was left. Those two sit under the compass, which is
    // ticked too, so their own mark was redundant (a parent's mark reaches every descendant).
    void mark_rgb(uint8_t* data, float v)
    {
        float* add = (float*)(data + 0x60);
        if (v == 0.0f) { for (int c = 0; c < 4; ++c) if (add[c] == kPartTag) add[c] = 0.0f; return; }
        // KEEPGLOW3 — JJ: with KEEPGLOW/KEEPGLOW2 the launch objective stayed on the head; it hung in the room when the
        // mark overwrote all four adds (WORLDKIDS). Back to that; the glow is kept instead by never marking a part
        // under an already marked one (apply_layers), which is where the two compass pieces lost it.
        for (int c = 0; c < 4; ++c) add[c] = v;
    }
    bool has_mark(uintptr_t data)
    {
        const float* add = (const float*)(data + 0x60);
        return add[0] == kPartTag || add[1] == kPartTag || add[2] == kPartTag || add[3] == kPartTag;
    }
    void hkObjSetCxform(void* obj, const float* cx)
    {
        g_origObjCx(obj, cx);
        __try
        {
            const uintptr_t node = *(const uintptr_t*)((const uint8_t*)obj + 0x48);
            if (node && g_treeWritable && tag_node(node))
                if (uint8_t* w = (uint8_t*)g_treeWritable((void*)node, 2)) { mark_rgb(w, kPartTag); ++g_tagRewrites; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    TreeSetVisibleFn g_treeSetVisible = nullptr;
    TreeSetMatrix3Fn g_treeSetMatrix3 = nullptr;
    bool layer_fns_ready()
    {
        static int state = 0;                   // 0 untested, 1 ok, -1 bad
        if (state) return state > 0;
        const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        const uint8_t* vis = (const uint8_t*)(base + (0x1411ccd80 - 0x140000000));
        const uint8_t* m3  = (const uint8_t*)(base + (0x1411cdce0 - 0x140000000));
        static const uint8_t visPro[] = { 0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x20, 0x4C,0x8B,0xC9 };
        static const uint8_t m3Pro[]  = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10 };
        // PARTTAG: TreeNode::GetWritableData(node, change) 0x1411c96f0 - every setter calls it (1 matrix, 2 colour
        // transform, 4 visible) and writes the returned data; the colour transform is data+0x50 (multiply RGBA)
        // and data+0x60 (add RGBA), as the game's own code at 0x1411f53bd writes it.
        const uint8_t* gwd = (const uint8_t*)(base + (0x1411c96f0 - 0x140000000));
        static const uint8_t gwdPro[] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18,
                                          0x48,0x89,0x7C,0x24,0x20, 0x41,0x54, 0x48,0x83,0xEC,0x20, 0x8B,0xEA };
        __try
        {
            if (memcmp(vis, visPro, sizeof(visPro)) == 0 && memcmp(m3, m3Pro, sizeof(m3Pro)) == 0)
            { g_treeSetVisible = (TreeSetVisibleFn)vis; g_treeSetMatrix3 = (TreeSetMatrix3Fn)m3; state = 1; }
            else state = -1;
            if (memcmp(gwd, gwdPro, sizeof(gwdPro)) == 0) g_treeWritable = (TreeWritableFn)gwd;
            // PARTTAG2: the game's SetCxform (40 53 48 83 EC 20 48 8B DA E8 = push rbx; sub rsp,20; mov rbx,rdx; call)
            const uint8_t* scx = (const uint8_t*)(base + (0x1411eedf0 - 0x140000000));
            static const uint8_t scxPro[] = { 0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xDA, 0xE8 };
            if (g_treeWritable && memcmp(scx, scxPro, sizeof(scxPro)) == 0)
            {
                MH_Initialize();
                if (MH_CreateHook((void*)scx, (void*)&hkObjSetCxform, (void**)&g_origObjCx) != MH_OK ||
                    MH_EnableHook((void*)scx) != MH_OK) g_origObjCx = nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { state = -1; }
        log_add("hud-layers: visible/3D setters %s; colour mark %s; colour guard %s", state > 0 ? "found" : "NOT matched (hide/3D moves off)",
                g_treeWritable ? "found" : "NOT matched (hang in the room off)", g_origObjCx ? "hooked" : "NOT hooked");
        return state > 0;
    }
    bool layer_moved(const Layer& L)
    {
        return std::fabs(L.s - 1.0f) > 0.001f || std::fabs(L.dx) > 0.0005f || std::fabs(L.dy) > 0.0005f;
    }
    void apply_layers(void* view)
    {
        if ((!g_layerAdjusted && !g_layerRestores && !g_roomAll && g_tagNodeN == 0) || !g_treeSetMatrix || !layer_fns_ready()) return;
        if (!TryAcquireSRWLockShared(&g_layerLock)) return;
        if (g_autoDirty)
        {   // ROOMALL: which branches hold a world part (children follow their parent in the list)
            static bool s_holds[2048];
            for (int i = 0; i < g_nLayers; ++i) s_holds[i] = g_layers[i].world;
            // ROOMALL2 2026-09-30 — JJ: the reticle and distance sat in the room for the first 10-20 s: their parts are
            // created later than the tree is read, so their branch was marked and they inherited it. Every saved
            // "stays on its target" key protects its deepest EXISTING ancestor, so the branch stays unmarked until the
            // part appears (and after).
            for (const char* p = strchr(g_layerXf, ';'); p; p = strchr(p + 1, ';'))
            {
                char key[96] = ""; float s, x, y; int h = 0, rm = 0, zh = 0, wd = 0;
                const char* colon = strchr(p + 1, ':');
                if (!colon || colon - (p + 1) >= (int)sizeof(key)) continue;
                memcpy(key, p + 1, colon - (p + 1)); key[colon - (p + 1)] = 0;
                if (sscanf_s(colon + 1, "%f:%f:%f:%d:%d:%d:%d", &s, &x, &y, &h, &rm, &zh, &wd) < 7 || !wd) continue;
                for (;;)
                {
                    int hit = -1;
                    for (int i = 0; i < g_nLayers && hit < 0; ++i) if (strcmp(g_layers[i].key, key) == 0) hit = i;
                    if (hit >= 0) { s_holds[hit] = true; break; }
                    char* dot = strrchr(key, '.');
                    if (dot) { *dot = 0; continue; }
                    char* slash = strchr(key, '/');                  // top level: the container root
                    if (slash && strcmp(slash + 1, "root") != 0) { strcpy_s(slash + 1, sizeof(key) - (slash + 1 - key), "root"); continue; }
                    break;
                }
            }
            for (int i = g_nLayers - 1; i > 0; --i)
                if (s_holds[i] && g_layers[i].parent >= 0 && g_layers[i].parent < i) s_holds[g_layers[i].parent] = true;
            // WORLDKIDS 2026-09-30 — JJ: the reticle hung in space, and on the first grapple it "split in half: half at
            // the depth of the object and half hanging in space". The reticle part has 4 children (two rotated halves
            // among them), and each held no world part while its parent did, so the rule above marked EVERY piece
            // inside the world part. Nothing inside a "stays on its target" part is ever marked now.
            static bool s_inWorld[2048];
            for (int i = 0; i < g_nLayers; ++i)
            {
                const int p = g_layers[i].parent;
                s_inWorld[i] = g_layers[i].world || (p >= 0 && p < i && s_inWorld[p]);
                // ROOTKIDS 2026-09-30 — JJ: the launch objective stays on the head. A movie with no world part marked
                // only its ROOT, and roots are never written (apply_layers skips depth 0), so nothing in it was marked.
                // A root is never marked; its children are the top branches.
                const bool topBranch = p >= 0 && p < i && (s_holds[p] || g_layers[p].depth == 0);
                g_layers[i].autoRoom = p >= 0 && !s_holds[i] && !s_inWorld[i] && topBranch;
            }
            g_autoDirty = false;
        }
        static bool s_markAbove[2048];              // KEEPGLOW3: this part or one above it is marked
        for (int i = 0; i < g_nLayers; ++i)
        {
            Layer& L = g_layers[i];
            // KEEPGLOW3: a part under a marked part already carries the mark; marking it too only wipes its own colour
            // add (JJ's compass glow pieces). Parents come before their children in the list.
            const bool roomWanted = !L.world && (L.room || (g_roomAll && L.autoRoom));   // ROOMALL
            const bool above = L.parent >= 0 && L.parent < i && s_markAbove[L.parent];
            s_markAbove[i] = roomWanted || above;
            if (L.view != view || L.depth == 0) continue;
            const bool moved = layer_moved(L);
            // ZOOMHIDE 2026-09-30 — JJ: the game's own 2D zoom vignette is the same part as the gameplay tips, so a
            // plain hide took the tips too. "hide while zoomed" hides it only while our zoom vignette is on.
            const bool hideNow = L.hide || (L.zoomHide && akvr_xr_vig_active());
            const bool roomNow = roomWanted && !above;
            if (!moved && !hideNow && !L.haveLast && !L.hidByUs && !roomNow && !L.taggedByUs) continue;
            __try
            {
                if (!is_child_of(L.node, L.parentNode)) continue;          // node gone or moved
                float cur[12]; int flags = 0;
                if (!node_matrix(L.node, cur, &flags)) continue;
                const bool is3d = (flags & 0x200) != 0;
                const int  nf = is3d ? 12 : 8;
                const bool wasPending = L.haveLast || L.hidByUs || L.taggedByUs;
                // --- PARTTAG: the colour mark (add blue = kPartTag) the 7 colour HUD shaders look for
                if ((roomNow || L.taggedByUs) && g_treeWritable)
                {
                    uintptr_t d = 0;
                    if (node_data(L.node, d))
                    {
                        const bool b = has_mark(d);   // KEEPGLOW: any channel (the mark avoids the game's own adds)
                        if (roomNow && b) ++g_tagReadOk; else if (roomNow) ++g_tagReadBad;   // PARTTAG2 diag
                        if (roomNow && !tag_node(L.node) && g_tagNodeN < 1024)
                        { g_tagNodes[g_tagNodeN] = L.node; InterlockedIncrement(&g_tagNodeN); }
                        if (roomNow && !b)
                        {
                            if (uint8_t* w = (uint8_t*)g_treeWritable((void*)L.node, 2)) { mark_rgb(w, kPartTag); ++g_tagSets; }
                        }
                        else if (!roomNow && b)
                        {
                            if (uint8_t* w = (uint8_t*)g_treeWritable((void*)L.node, 2)) mark_rgb(w, 0.0f);
                        }
                        L.taggedByUs = roomNow;
                        if (!roomNow)   // PARTTAG2: off the guard's list, so the game's own colour stays
                            for (LONG k = 0; k < g_tagNodeN; ++k)
                                if (g_tagNodes[k] == L.node) { g_tagNodes[k] = g_tagNodes[g_tagNodeN - 1]; InterlockedDecrement(&g_tagNodeN); break; }
                    }
                }
                // --- hide / show through the node's own visible bit
                if (hideNow)
                {
                    if (flags & 1)
                    {
                        if (!L.hidByUs) L.baseVisible = true;
                        g_treeSetVisible((void*)L.node, false);
                        ++g_layerSets;
                    }
                    else if (!L.hidByUs) L.baseVisible = false;
                    L.hidByUs = true;
                }
                else if (L.hidByUs)
                {
                    if (L.baseVisible && !(flags & 1)) g_treeSetVisible((void*)L.node, true);
                    L.hidByUs = false;
                }
                // --- move / resize (2D: 2x4, 3D: 3x4; translation at [3], [7])
                if (moved)
                {
                    if (!(L.haveLast && memcmp(cur, L.last, nf * sizeof(float)) == 0))
                        memcpy(L.base, cur, sizeof(cur));                  // the game's own value
                    float m[12];
                    memcpy(m, L.base, sizeof(m));
                    const float cx = 10240.0f, cy = 7680.0f;              // stage centre, twips
                    const int rows = is3d ? 3 : 2;
                    for (int r = 0; r < rows; ++r)
                        for (int c = 0; c < (is3d ? 3 : 2); ++c) m[r * 4 + c] *= L.s;
                    m[3] = cx + L.s * (L.base[3] - cx) + L.dx * 20480.0f;
                    m[7] = cy + L.s * (L.base[7] - cy) - L.dy * 15360.0f;
                    if (!(L.haveLast && memcmp(m, cur, nf * sizeof(float)) == 0))
                    {
                        if (is3d) g_treeSetMatrix3((void*)L.node, m);
                        else      g_treeSetMatrix((void*)L.node, m);
                        ++g_layerSets;
                    }
                    memcpy(L.last, m, sizeof(m)); L.haveLast = true;
                }
                else if (L.haveLast)
                {
                    // Reset: give the game its own matrix back if the node still holds ours.
                    if (memcmp(cur, L.last, nf * sizeof(float)) == 0)
                    {
                        if (is3d) g_treeSetMatrix3((void*)L.node, L.base);
                        else      g_treeSetMatrix((void*)L.node, L.base);
                    }
                    L.haveLast = false;
                }
                if (wasPending && !L.haveLast && !L.hidByUs && !L.taggedByUs && !layer_custom(L) && g_layerRestores > 0)
                    InterlockedDecrement(&g_layerRestores);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { L.node = 0; }
        }
        ReleaseSRWLockShared(&g_layerLock);
    }

    typedef uintptr_t (*MovieSlot27Fn)(void* view, uintptr_t flag);
    MovieSlot27Fn g_orig27 = nullptr;
    void**        g_vt27 = nullptr;
    volatile LONG g_slot27Calls = 0;
    uintptr_t hkMovieSlot27(void* view, uintptr_t flag)
    {
        InterlockedIncrement(&g_slot27Calls);
        steady_update();                         // HUDSTEADY: once per game frame
        if (g_steady > 0.001f || g_steadyTx != 0.0f || g_steadyTy != 0.0f)
            if (RootState* rs = root_state(view, false)) if (rs->want) apply_root(view, false);
        apply_layers(view);                      // before the frame is captured
        return g_orig27 ? g_orig27(view, flag) : 0;
    }
}
// Called from fix_one_movie's vtable patch (earlyres.cpp): slot 27 = 0x1411ad270.
void akvr_hud_layers_hook_vtable(void** vt)
{
    if (g_vt27 || !vt) return;
    void* cur = vt[27];
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if ((uintptr_t)cur != base + (0x1411ad270 - 0x140000000)) { log_add("hud-layers: slot 27 is not the RE'd function - not hooked"); return; }
    // TIDY4: a vtable swap never ran (0 sets) - the game calls this function directly,
    // as HUDPROBE's code-patch counters showed. Patch the function itself (MinHook).
    g_vt27 = vt;
    MH_Initialize();
    if (MH_CreateHook(cur, (void*)&hkMovieSlot27, (void**)&g_orig27) != MH_OK || MH_EnableHook(cur) != MH_OK)
    { g_orig27 = nullptr; log_add("hud-layers: MinHook on slot 27 FAILED - parts cannot move"); return; }
    log_add("hud-layers: per-frame hook on movie slot 27 (%p, code patch)", cur);
}
void  akvr_hud_layers_discover() { discover_layers(true); }
void  akvr_hud_layers_discover_quick() { discover_layers(false); }   // PAUSESTUTTER: no name search
void  akvr_hud_steady_set(float v) { g_steady = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float akvr_hud_steady() { return g_steady; }
const char* akvr_hud_steady_diag()
{
    static char d[320];
    if (g_steady <= 0.001f) _snprintf_s(d, sizeof(d), _TRUNCATE, "HUD steadiness: off");
    else _snprintf_s(d, sizeof(d), _TRUNCATE,
                     "HUD steadiness: %.2f deg behind the head, frames %ld (missed %ld), camera match age 0/1/2/other %ld/%ld/%ld/%ld, %.1f px/deg%s",
                     g_steadyDeg, g_steadyPaired, g_steadyMissed, g_steadyAge[0], g_steadyAge[1], g_steadyAge[2], g_steadyAge[3],
                     g_steadyPxPerDeg, g_orig27 ? "" : " - per-frame HUD hook OFF");
    return d;
}
const char* akvr_hud_layers_diag()
{
    static char live[240];
    _snprintf_s(live, sizeof(live), _TRUNCATE, "%s | live: frame hook %s, calls %ld, adjusted %d, matrix sets %ld",
                g_layerDiag, g_orig27 ? "ON" : "OFF", (long)g_slot27Calls, g_layerAdjusted, g_layerSets);
    return live;
}
int   akvr_hud_layer_count() { return g_nLayers; }
// Read one layer for the panel. Returns false past the end.
bool  akvr_hud_layer_get(int i, int& depth, int& parent, int& kids, const char*& label, float& s, float& x, float& y, bool& hide, bool& is3d)
{
    if (i < 0 || i >= g_nLayers) return false;
    const Layer& L = g_layers[i];
    depth = L.depth; parent = L.parent; kids = L.kids; label = L.name[0] ? L.name : L.path;
    s = L.s; x = L.dx; y = L.dy; hide = L.hide; is3d = (L.flags & 0x200) != 0;
    return true;
}
bool  akvr_hud_layer_room(int i) { return i >= 0 && i < g_nLayers && g_layers[i].room; }
bool  akvr_hud_layer_zoomhide(int i) { return i >= 0 && i < g_nLayers && g_layers[i].zoomHide; }
bool  akvr_hud_layer_world(int i) { return i >= 0 && i < g_nLayers && g_layers[i].world; }
bool  akvr_hud_layer_autoroom(int i) { return i >= 0 && i < g_nLayers && g_roomAll && g_layers[i].autoRoom && !g_layers[i].world; }
void  akvr_hud_layer_world_set(int i, bool on)
{
    AcquireSRWLockExclusive(&g_layerLock);
    if (i >= 0 && i < g_nLayers)
    {
        Layer& L = g_layers[i];
        const bool was = layer_custom(L);
        L.world = on;
        const bool now = layer_custom(L);
        if (was && !now && (L.haveLast || L.hidByUs || L.taggedByUs)) InterlockedIncrement(&g_layerRestores);
        g_layerAdjusted += (now ? 1 : 0) - (was ? 1 : 0);
        layer_save(L);
        g_autoDirty = true;
    }
    ReleaseSRWLockExclusive(&g_layerLock);
}
bool  akvr_hud_room_all() { return g_roomAll; }
void  akvr_hud_room_all_set(bool on) { g_roomAll = on; g_autoDirty = true; }
// TARGETDEPTH 2026-10-02 — JJ: the target-distance widget is "a combination of both sticking to its target and
// sticking to my face"; it should be "at the depth of what's behind it, what it's pointing to". Each of its ~20
// pieces searched scene depth at its own spot. This gives the fix's HUD shaders (step 1k) one point per "stays on
// its target" part: from the part, down the live tree while a node has exactly one child (804 -> 813 for the
// widget; the reticle 803 has several), then that node's origin up through its parents' matrices to the movie
// root, whose matrix maps stage twips to pixels of the game target. Checked on JJ's F2 (2026-10-01 19:55): widget
// predicted at 0.530 / 0.667 of the eye, its "51m" drawn at 0.530 / 0.651; reticle 0.501 / 0.243 vs 0.52 / 0.248.
// Read on the render thread just before the HUD is drawn; a part with nothing under it gives no point.
// TARGETSTOCK 2026-10-02 — JJ: "when my head is still yes the marker sits still but it doesn't mean they're at the
// location that the game has put them"; before TARGETMOVE head up carried the markers up, TARGETMOVE (the whole head
// turn undone) swung them "the opposite and more exaggerated". Cause: ROOTSHRINK draws the whole HUD movie inside the
// HUD box (hudscale, ~60% of the view), and the game places its world markers in stage space as if the movie filled
// the screen - so every marker is pulled toward the box centre. off (optional) = where the movie's OWN matrix
// (view+0x110, stage twips -> buffer pixels, what the game laid it out for) puts the point, minus where it is drawn.
// TARGETSCALE 2026-10-02 (JJ: with TARGETSTOCK the markers "are jittering when you move your head" - the per-frame
// offset was read from the live tree while the game may lay out the next frame): xf (optional) = the fixed map from
// AKVR's drawn HUD box back to the movie's own layout, in clip units, for the shaders to apply per vertex:
// x' = x + x * xf[0] + xf[2], y' = y + y * xf[1] + xf[3]. Taken from the first on-target part's movie; 0 = none.
// TARGETANCHOR 2026-10-02 — JJ after TARGETFACE: the distance marker "is now flying off its fixed point in object space
// when turning the head" and does not face him. F2 x5: its point was (0.000, 0.080) in every capture = the screen centre.
// The widget part K2/...1 now has TWO children (.0 and .1), so the one-child walk stopped at the part, which never moves;
// the game moves .1.1 (local translation (5605,-59), (307,436), (-4037,-1599), (-256,-5432) across the captures). The
// "115m" label sat where .1.1 puts it in the SHRUNK HUD box, i.e. its pieces were not taken as on-target (too far from
// the centre point) except when they passed near the screen centre - in and out as the head turns. The anchor of a part
// is now the node the game MOVES: the part itself if its translation changes (the reticle), else the one child whose
// subtree moves (down to the node that does); two or more moving children = their parent (the reticle's pulsing pieces).
// Remembered per part while nothing moves; before anything has moved, the old one-child walk.
namespace {
    struct MoveRec { uintptr_t node; float tx, ty; DWORD moved; };
    MoveRec g_mvRec[256]; int g_mvN = 0, g_mvNext = 0;
    struct AnchorRec { uintptr_t part, anchor; };
    AnchorRec g_anchor[8] = {};
    // true when this node's local translation changed by more than 20 units within the last 3 s
    bool node_moved_recently(uintptr_t node, const float* m, DWORD now)
    {
        for (int i = 0; i < g_mvN; ++i)
        {
            MoveRec& r = g_mvRec[i];
            if (r.node != node) continue;
            if (fabsf(m[3] - r.tx) > 20.0f || fabsf(m[7] - r.ty) > 20.0f) { r.tx = m[3]; r.ty = m[7]; r.moved = now; }
            return r.moved != 0 && now - r.moved < 3000;
        }
        MoveRec& r = g_mvN < 256 ? g_mvRec[g_mvN++] : g_mvRec[g_mvNext++ & 255];
        r.node = node; r.tx = m[3]; r.ty = m[7]; r.moved = 0;
        return false;
    }
    // the moving node under `node` (inclusive), 0 = nothing in this visible 2D subtree moved
    uintptr_t moving_anchor(uintptr_t node, int depth, DWORD now, int& budget)
    {
        if (--budget < 0) return 0;
        float m[12]; int fl = 0;
        if (!node_matrix(node, m, &fl) || !(fl & 1) || (fl & 0x200)) return 0;   // hidden or 3D
        if (node_moved_recently(node, m, now)) return node;
        if (depth >= 6) return 0;
        uintptr_t kids[32];
        const int k = node_children(node, kids, 32);
        uintptr_t found = 0; int nf = 0;
        for (int c = 0; c < k; ++c)
        {
            const uintptr_t a = moving_anchor(kids[c], depth + 1, now, budget);
            if (a) { found = a; ++nf; }
        }
        return nf == 1 ? found : (nf >= 2 ? node : 0);
    }
    bool is_under(uintptr_t node, uintptr_t part)
    {
        for (int up = 0; up < 12 && node; ++up)
        {
            if (node == part) return true;
            uintptr_t p = 0;
            __try { p = *(uintptr_t*)(node + 0x20); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (!readable_ptr(p) || (p & 7)) return false;
            node = p;
        }
        return false;
    }
    uintptr_t part_anchor(uintptr_t part, uintptr_t fallback, DWORD now)
    {
        int budget = 96;
        const uintptr_t a = moving_anchor(part, 0, now, budget);
        int slot = -1;
        for (int i = 0; i < 8; ++i) if (g_anchor[i].part == part) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < 8; ++i) if (!g_anchor[i].part) { slot = i; break; }
        if (slot < 0) slot = (int)(part >> 4) & 7;
        if (a) { g_anchor[slot] = { part, a }; return a; }
        if (g_anchor[slot].part == part && g_anchor[slot].anchor && is_under(g_anchor[slot].anchor, part))
        {
            float m[12]; int fl = 0;
            if (node_matrix(g_anchor[slot].anchor, m, &fl) && (fl & 1)) return g_anchor[slot].anchor;
        }
        return fallback;
    }
}

int akvr_hud_target_points(float* xy, int max, int rtW, int rtH, float* off, float* xf)
{
    if (xf) xf[0] = xf[1] = xf[2] = xf[3] = 0.0f;
    if (!xy || max <= 0 || rtW < 16 || rtH < 16) return 0;
    struct Want { uintptr_t node, root; void* view; };
    Want want[8]; int nWant = 0;
    if (!TryAcquireSRWLockShared(&g_layerLock)) return 0;   // a re-read is running: skip this frame
    for (int i = 0; i < g_nLayers && nWant < 8; ++i)
    {
        const Layer& L = g_layers[i];
        if (!L.world || !L.node) continue;
        bool top = true; int r = L.parent, guard = 0;
        while (r >= 0 && r < i && guard++ < 32) { if (g_layers[r].world) { top = false; break; } if (g_layers[r].depth == 0) break; r = g_layers[r].parent; }
        if (!top || r < 0 || r >= i || g_layers[r].depth != 0) continue;
        want[nWant++] = { L.node, g_layers[r].node, g_layers[r].view };
    }
    ReleaseSRWLockShared(&g_layerLock);
    // TARGETMULTI 2026-10-02 — JJ: the Batmobile marker (one of the 74-marker list K4/0.0.0.0.1.0.0.0) "was moving around
    // with head movement ... more like moving in the opposite direction": a world marker hung in the room. A part whose
    // node (after the one-child chain) holds 6 or more children is a LIST: each VISIBLE child on screen is its own point
    // (the reticle has 4-5 pieces and the distance widget 2, so they stay one point each). Up to max points.
    int n = 0;
    // one node's origin, through its parents to the movie root, in clip units; the layout map from the first root
    auto point_of = [&](uintptr_t node, const Want& wt, float& cx, float& cy) -> bool
    {
        float x = 0.0f, y = 0.0f; bool ok = false;
        for (int up = 0; up < 40; ++up)
        {
            float m[12]; int fl = 0;
            if (!node_matrix(node, m, &fl) || (fl & 0x200)) break;   // 3D nodes: no 2D point
            if (node == wt.root && wt.view && xf && xf[0] == 0.0f && xf[1] == 0.0f)
            {   // TARGETSCALE: drawn px = m q, the movie's own px = sm q (scale + shift) -> own = k * drawn + c
                float sm[8]; bool stock = false;
                __try { memcpy(sm, (uint8_t*)wt.view + 0x110, sizeof(sm)); stock = true; }
                __except (EXCEPTION_EXECUTE_HANDLER) { stock = false; }
                if (stock && sm[0] > 1e-4f && sm[5] > 1e-4f && m[0] > 1e-4f && m[5] > 1e-4f && fabsf(m[1]) < 1e-5f &&
                    fabsf(m[4]) < 1e-5f && fabsf(sm[1]) < 1e-5f && fabsf(sm[4]) < 1e-5f)
                {
                    const float kx = sm[0] / m[0], ky = sm[5] / m[5];
                    const float ccx = sm[3] - kx * m[3], ccy = sm[7] - ky * m[7];
                    xf[0] = kx - 1.0f; xf[1] = ky - 1.0f;
                    xf[2] = kx - 1.0f + 2.0f * ccx / (float)rtW;     // clip x = 2 px / W - 1
                    xf[3] = 1.0f - ky - 2.0f * ccy / (float)rtH;     // clip y = 1 - 2 py / H
                }
            }
            const float nx = m[0] * x + m[1] * y + m[3], ny = m[4] * x + m[5] * y + m[7];
            x = nx; y = ny;
            if (node == wt.root) { ok = true; break; }
            uintptr_t p = 0;
            __try { p = *(uintptr_t*)(node + 0x20); } __except (EXCEPTION_EXECUTE_HANDLER) { p = 0; }
            if (!readable_ptr(p) || (p & 7)) break;
            node = p;
        }
        if (!ok || !(x == x) || !(y == y)) return false;
        cx = x / (float)rtW * 2.0f - 1.0f;
        cy = 1.0f - y / (float)rtH * 2.0f;
        return true;
    };
    // TARGETANCHOR: single parts (reticle, distance widget) are collected apart and written LAST, so the shaders' "last
    // match wins" gives their pieces their own point over a nearby world-list marker's (mixed points = a marker's pieces
    // turned about two pivots by TARGETFACE). List points fill the room the single parts leave.
    float sxy[16]; int ns = 0;
    float lxy[16]; int nl = 0;
    const DWORD now = GetTickCount();
    for (int w = 0; w < nWant; ++w)
    {
        uintptr_t node = want[w].node, kids[96];
        int k = node_children(node, kids, 96);
        for (int d = 0; d < 8 && k == 1; ++d) { node = kids[0]; k = node_children(node, kids, 96); }
        if (k == 0) continue;                                // nothing drawn under this part
        if (k >= 6)
        {   // a list: one point per visible child on screen
            for (int c = 0; c < k && nl < 8; ++c)
            {
                float m[12]; int fl = 0;
                if (!node_matrix(kids[c], m, &fl) || !(fl & 1)) continue;   // hidden marker
                float cx = 0.0f, cy = 0.0f;
                if (!point_of(kids[c], want[w], cx, cy) || fabsf(cx) > 1.1f || fabsf(cy) > 1.1f) continue;
                lxy[nl * 2] = cx; lxy[nl * 2 + 1] = cy; ++nl;
            }
            continue;
        }
        if (ns >= 8) continue;
        const uintptr_t anchor = part_anchor(want[w].node, node, now);   // TARGETANCHOR
        float cx = 0.0f, cy = 0.0f;
        if (!point_of(anchor, want[w], cx, cy)) continue;
        sxy[ns * 2] = cx; sxy[ns * 2 + 1] = cy; ++ns;
    }
    if (ns > max) ns = max;
    const int nList = nl < max - ns ? nl : max - ns;
    for (int i = 0; i < nList; ++i) { xy[n * 2] = lxy[i * 2]; xy[n * 2 + 1] = lxy[i * 2 + 1]; ++n; }
    for (int i = 0; i < ns; ++i) { xy[n * 2] = sxy[i * 2]; xy[n * 2 + 1] = sxy[i * 2 + 1]; ++n; }
    if (off) for (int i = 0; i < n * 2; ++i) off[i] = 0.0f;
    return n;
}
void  akvr_hud_layer_zoomhide_set(int i, bool on)
{
    AcquireSRWLockExclusive(&g_layerLock);
    if (i >= 0 && i < g_nLayers)
    {
        Layer& L = g_layers[i];
        const bool was = layer_custom(L);
        L.zoomHide = on;
        const bool now = layer_custom(L);
        if (was && !now && (L.haveLast || L.hidByUs || L.taggedByUs)) InterlockedIncrement(&g_layerRestores);
        g_layerAdjusted += (now ? 1 : 0) - (was ? 1 : 0);
        layer_save(L);
    }
    ReleaseSRWLockExclusive(&g_layerLock);
}
// PARTTAG: "hang in the room" = mark the whole part so the HUD shaders never put it at scene depth.
void  akvr_hud_layer_room_set(int i, bool on)
{
    AcquireSRWLockExclusive(&g_layerLock);
    if (i >= 0 && i < g_nLayers)
    {
        Layer& L = g_layers[i];
        const bool was = layer_custom(L);
        L.room = on;
        const bool now = layer_custom(L);
        if (was && !now && (L.haveLast || L.hidByUs || L.taggedByUs)) InterlockedIncrement(&g_layerRestores);
        g_layerAdjusted += (now ? 1 : 0) - (was ? 1 : 0);
        layer_save(L);
    }
    ReleaseSRWLockExclusive(&g_layerLock);
}
long  akvr_hud_layer_tag_sets() { return g_tagSets; }
const char* akvr_hud_layer_tag_diag()
{
    static char d[160];
    _snprintf_s(d, sizeof(d), _TRUNCATE, "marks set %ld, kept after the game's own colour writes %ld, found in place %ld / missing %ld, guard %s",
                g_tagSets, g_tagRewrites, g_tagReadOk, g_tagReadBad, g_origObjCx ? "on" : "OFF");
    return d;
}
void  akvr_hud_layer_set(int i, float s, float x, float y, bool hide)
{
    AcquireSRWLockExclusive(&g_layerLock);
    if (i >= 0 && i < g_nLayers)
    {
        Layer& L = g_layers[i];
        const bool was = layer_custom(L);
        L.s = s < 0.2f ? 0.2f : (s > 3.0f ? 3.0f : s);
        L.dx = x < -0.6f ? -0.6f : (x > 0.6f ? 0.6f : x);
        L.dy = y < -0.6f ? -0.6f : (y > 0.6f ? 0.6f : y);
        L.hide = hide;
        const bool now = layer_custom(L);
        if (was && !now && (L.haveLast || L.hidByUs || L.taggedByUs)) InterlockedIncrement(&g_layerRestores);   // game thread does it
        g_layerAdjusted += (now ? 1 : 0) - (was ? 1 : 0);
        layer_save(L);
    }
    ReleaseSRWLockExclusive(&g_layerLock);
}
const char* akvr_hud_layer_xf_list() { return g_layerXf; }
const char* akvr_hud_containers_list() { return g_contFpList; }
void  akvr_hud_containers_list_set(const char* list) { strncpy_s(g_contFpList, list ? list : "", _TRUNCATE); cont_fp_parse(); }
void  akvr_hud_layer_xf_list_set(const char* list) { strncpy_s(g_layerXf, list ? list : "", _TRUNCATE); }
void  akvr_hud_layers_dump(const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    discover_layers(false);                       // F2 was slow: no name search here
    fprintf(f, "AKVR HUD layers (HUDLAYERS phase 2)\n%s\n"
               "index depth parent kids 3D  key  |  a b tx / c d ty (twips)  |  adjustment\n", g_layerDiag);
    for (int i = 0; i < g_nContFps; ++i)
        fprintf(f, "container id %s: %d fingerprint parts\n", g_contFps[i].id, g_contFps[i].n);
    fprintf(f, "\n");
    AcquireSRWLockShared(&g_layerLock);
    for (int i = 0; i < g_nLayers; ++i)
    {
        const Layer& L = g_layers[i];
        // WORLDKIDS: the colour mark as it sits in the node now (MARK = flat, in the layer; "not ours" = no part of
        // the mod put it there), and the part's own ticks.
        const char* mark = "";
        uintptr_t d = 0;
        if (node_data(L.node, d))
            __try { if (has_mark(d)) mark = L.taggedByUs ? " MARK" : " MARK(not ours)"; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        fprintf(f, "%4d %*sd%d p%-3d kids %-3d %s %-40s | %.3f %.3f %.0f / %.3f %.3f %.0f | s %.2f x %.3f y %.3f %s%s%s%s\n",
                i, L.depth * 2, "", L.depth, L.parent, L.kids, (L.flags & 0x200) ? "3D" : "  ", L.key,
                L.base[0], L.base[1], L.base[3], L.base[4], L.base[5], L.base[7],
                L.s, L.dx, L.dy, L.hide ? "HIDDEN" : "", L.world ? " ON-TARGET" : "", L.room ? " ROOM" : "", mark);
    }
    ReleaseSRWLockShared(&g_layerLock);
    fclose(f);
}
void* akvr_hud_view_vtable()    { return g_vtCount > 0 ? (void*)g_vtHooked[0] : nullptr; }   // HUDPROBE
bool  akvr_hud_tex_fix()        { return g_hudTexFix; }
void  akvr_hud_tex_fix_set(bool on) { g_hudTexFix = on; }
bool  akvr_hud_root_mode()      { return g_hudRootMode; }
void  akvr_hud_root_mode_set(bool on) { g_hudRootMode = on; ++g_shrinkGen; }
bool  akvr_hud_scale_mode()     { return g_hudScaleMode; }
void  akvr_hud_scale_mode_set(bool on) { g_hudScaleMode = on; ++g_shrinkGen; }
bool  akvr_hud_dual()           { return g_hudDual; }
void  akvr_hud_dual_set(bool on) { g_hudDual = on; ++g_shrinkGen; }
bool  akvr_hud_global()         { return g_hudGlobal; }
void  akvr_hud_global_set(bool on) { g_hudGlobal = on; }
void  akvr_hud_menu_fill_set(bool on) { g_menuUiFill = on; }
bool akvr_hud_aspect_fix() { return g_hudAspectFix; }
void akvr_hud_aspect_fix_set(bool on) { g_hudAspectFix = on; }
float akvr_hud_aspect_factor() { return g_hudAspectApplied; }
void  akvr_hud_scale_set(float s)
{
    g_hudScale = s < 0.25f ? 0.25f : (s > 2.0f ? 2.0f : s);
}
bool  akvr_hud_scale_found()    { return g_hudFound; }
const char* akvr_hud_diag()     { return g_hudFound ? g_hudDiag : "hud: signature not found"; }

// ---- engine render size (the RE lever) — all "next launch", it patches at startup
int   akvr_engine_res()            { return g_engineH; }
int   akvr_geo11_shape()           { return g_geo11Shape; }   // GEO11SHAPE, persisted by hooks.cpp
void  akvr_geo11_shape_set(int s)  { g_geo11Shape = s < 700 ? 700 : (s > 2400 ? 2400 : s); }   // next launch
void  akvr_engine_res_set(int h)   { g_engineH = (h && h < 720) ? 720 : (h > 4320 ? 4320 : h); }
int   akvr_engine_shape()          { return g_engineShape; }
void  akvr_engine_shape_set(int s) { g_engineShape = s < 300 ? 300 : (s > 3000 ? 3000 : s); }
bool  akvr_engine_res_on()         { return g_engOn; }
int   akvr_engine_res_w()          { return g_engW; }
int   akvr_engine_res_h()          { return g_engH; }
// MENURES 2026-09-27 — JJ: leaving the in-game GRAPHICS menu (no change) turns the view into a
// small window high in a corner, with or without the eye view, until restart; the game rewrote
// BmSystemSettings.ini to ResX 2560 / ResY 1440. Static RE (capstone, BatmanAK.exe): the menu's
// native URGFxMovieUI::execSetResolution (0x14051AE60) takes a list index; the menu's own
// "current resolution" lives in two ints (VA 0x143123620 X / 0x143123624 Y, read by the list lookup
// at 0x14001E6A0 against the game's mode list [0x143100158], count [0x143100160]) - separate from
// the startup ResX/ResY (0x143123CF0/CF4) that install_engine_res forces. Experiment: keep the
// menu's pair at the forced engine size, so leaving the menu re-applies OUR size; log what the game
// had and whether our size is in its mode list. The addresses are decoded from the lookup's own
// instructions, and only if its bytes match exactly.
namespace {
    int32_t* g_menuResX = nullptr; int32_t* g_menuResY = nullptr;
    int32_t** g_modeList = nullptr; int32_t* g_modeCount = nullptr;
    bool     g_menuResTried = false, g_menuResOk = false;
    int      g_menuResFixes = 0, g_menuResWasX = 0, g_menuResWasY = 0, g_menuResInList = -1;
    int      g_menuResLastX = 0, g_menuResLastY = 0;
    // MENUMODE 2026-09-27: the hold above worked (menu pair 5120x2880 -> ours, our size is in the
    // game's list) and the reset still happened, because leaving the menu also switches to EXCLUSIVE
    // fullscreen (ini rewritten Fullscreen=True / WindowDisplayMode=2), which only accepts real
    // monitor modes -> 2560x1440 (the MODELIST mechanism). The menu's display mode is the int right
    // after the resolution pair (VA 0x143123628, "Display_Mode" in the GeForce Experience option
    // registration at 0x140C6F04D: cmp dword [rip+d],1). Held at the launch value, which the
    // launch-time comfort pass made windowed.
    int32_t* g_menuMode = nullptr;
    bool     g_menuModeHave = false;
    int      g_menuModeLaunch = 0, g_menuModeFixes = 0, g_menuModeSeen = 0;

    void menu_res_find()
    {
        g_menuResTried = true;
        uint8_t* b = (uint8_t*)GetModuleHandleW(nullptr);
        if (!b) return;
        const uint8_t* p = b + 0x1e6a0;
        static const uint8_t want[] = { 0x8b,0x15, 0,0,0,0, 0x33,0xc9, 0x85,0xd2, 0x7e,0x2a, 0x48,0x8b,0x05, 0,0,0,0,
                                        0x44,0x8b,0x05, 0,0,0,0, 0x44,0x8b,0x0d, 0,0,0,0 };
        __try {
            for (size_t i = 0; i < sizeof(want); ++i)
            {
                const bool wild = (i >= 2 && i < 6) || (i >= 15 && i < 19) || (i >= 22 && i < 26) || (i >= 29 && i < 33);
                if (!wild && p[i] != want[i]) return;
            }
            auto rel = [&](size_t dispAt, size_t insEnd) { int32_t d; memcpy(&d, p + dispAt, 4); return (uint8_t*)(p + insEnd) + d; };
            g_modeCount = (int32_t*)rel(2, 6);
            g_modeList  = (int32_t**)rel(15, 19);
            g_menuResY  = (int32_t*)rel(22, 26);
            g_menuResX  = (int32_t*)rel(29, 33);
            g_menuResOk = true;
            // MENUMODE: the GFE registration reads the mode with `83 3D d32 01`; it must point at X+8.
            const uint8_t* q = b + 0xc6f04d;
            if (q[0] == 0x83 && q[1] == 0x3d && q[6] == 0x01)
            {
                int32_t d; memcpy(&d, q + 2, 4);
                int32_t* m = (int32_t*)(q + 7 + d);
                if ((uint8_t*)m == (uint8_t*)g_menuResX + 8) g_menuMode = nullptr;   // GSAHOLD: hold retired
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_menuResOk = false; }
    }
}

// Every Present while the engine size is forced.
void akvr_menu_res_tick()
{
    if (!g_engOn || g_engW < 64 || g_engH < 64) return;
    if (!g_menuResTried) menu_res_find();
    if (!g_menuResOk) return;
    __try {
        if (g_menuMode)
        {   // MENUMODE: remember the launch display mode, put it back whenever the menu changes it
            const int m = *g_menuMode;
            if (!g_menuModeHave) { g_menuModeHave = true; g_menuModeLaunch = m; }
            else if (m != g_menuModeLaunch) { g_menuModeSeen = m; *g_menuMode = g_menuModeLaunch; ++g_menuModeFixes; }
        }
        const int x = *g_menuResX, y = *g_menuResY;
        if (x != g_engW || y != g_engH)
        {
            if (!g_menuResFixes) { g_menuResWasX = x; g_menuResWasY = y; }
            g_menuResLastX = x; g_menuResLastY = y;
            *g_menuResX = g_engW; *g_menuResY = g_engH;
            ++g_menuResFixes;
        }
        if (g_menuResInList < 0 && *g_modeList && *g_modeCount > 0 && *g_modeCount < 512)
        {
            g_menuResInList = 0;
            for (int i = 0; i < *g_modeCount; ++i)
                if ((*g_modeList)[i * 2] == g_engW && (*g_modeList)[i * 2 + 1] == g_engH) { g_menuResInList = 1; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_menuResOk = false; }
}
const char* akvr_menu_res_diag()
{
    static char s[900];
    if (!g_menuResTried) snprintf(s, sizeof(s), "menu resolution hold: not started");
    else if (!g_menuResOk) snprintf(s, sizeof(s), "menu resolution hold: code NOT matched (other exe version?) - off");
    else
    {
        char modes[200] = ""; size_t n = 0;
        __try {
            const int c = *g_modeCount;
            for (int i = 0; i < c && i < 24 && *g_modeList && n + 16 < sizeof(modes); ++i)
                n += snprintf(modes + n, sizeof(modes) - n, "%dx%d ", (*g_modeList)[i * 2], (*g_modeList)[i * 2 + 1]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        snprintf(s, sizeof(s), "graphics menu: NvGsa answer %s, %d lookups (store held mode %d, %dx%d), blur/vsync off %d (store held blur %d) | GFE copy hold: reset %d time(s) (first %dx%d), our size in the game's list: %s | list: %s",
                 g_oGsaGet ? "ON" : "NOT HOOKED", g_gsaHits, g_gsaMode, g_gsaResX, g_gsaResY, g_gsaComfort, g_gsaBlurWas,
                 g_menuResFixes, g_menuResWasX, g_menuResWasY,
                 g_menuResInList < 0 ? "?" : (g_menuResInList ? "yes" : "NO"), modes);
    }
    return s;
}
const char* akvr_engine_res_diag()
{
    static char s[160];
    if (!g_engineH)      snprintf(s, sizeof(s), "engine render: OFF (game picks its own size)");
    else if (!g_engAddr) snprintf(s, sizeof(s), "engine render: SIGNATURE NOT FOUND — wanted %dx%d", g_engW, g_engH);
    else if (!g_engOn)   snprintf(s, sizeof(s), "engine render: found but PATCH FAILED — wanted %dx%d", g_engW, g_engH);
    else                 snprintf(s, sizeof(s), "engine render: FORCED %d x %d", g_engW, g_engH);
    return s;
}
