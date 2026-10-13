#include "gamepad.h"
#include <windows.h>
#include <xinput.h>
#include <MinHook.h>
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include "akn_link.h"
int  akvr_xr_menu_phase();   // xr.cpp (HUDRING)
bool akvr_xr_pause_live();   // xr.cpp (HUDRING)
void akvr_ring_layers_request(int ms);   // hooks.cpp (HUDRING2)

namespace
{
    using XIGS_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    XIGS_t oXIGS = nullptr;          // original (un-intercepted) XInputGetState, via MinHook trampoline
    bool   g_hooked = false;
    bool   g_menu   = false;
    volatile ULONGLONG g_rsAt = 0;   // SWINGEASE: the last game read with the right stick off centre
    volatile ULONGLONG g_r3At = 0;   // ZOOMPRESS: the last right-stick click (press edge) the game read
    volatile bool g_r3Was = false;
    volatile ULONGLONG g_ltAt = 0;   // AIMTRIGGER: the last game read with the left trigger (aim / battle mode) held
    volatile int g_ltIdx = -1, g_ltMax = 0, g_ltRealIdx = -1;   // AIMTRIGGER2 diag: which pad, highest value
    volatile ULONGLONG g_ltRealAt = 0;   // AIMTRIGGER2: our own poll of every pad slot found it held

    // Our interception: pass through normally, but while menu mode is on, blank out
    // controller 0 for the CALLER (the game) so it acts as if no buttons/sticks are
    // touched. We still report success so the game keeps thinking a pad is plugged in.
    // 2026-09-26: moved from Menu+View (Start+Back) to BOTH STICK CLICKS. JJ: the old
    // chord "interferes with opening the map" — View alone opens AK's map the moment
    // it is pressed, before Menu can join it, and no amount of hiding the finished
    // chord can take that press back. Clicking both sticks at once is not a game
    // action, so the pair is hidden from the game as soon as both are down.
    const WORD MENU_CHORD = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;

    thread_local bool tl_ourRead = false;   // GAMEROUTE: our own read through the game's route - no blanking, no counting
    void game_read(DWORD idx, XINPUT_STATE* st, DWORD r);
    DWORD WINAPI hkXIGS(DWORD idx, XINPUT_STATE* st)
    {
        DWORD r = oXIGS ? oXIGS(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
        if (!tl_ourRead) game_read(idx, st, r);
        return r;
    }
    // GAMEROUTE 2026-10-07 (JJ, engine stereo, 6th Quest test: the double stick click still does not open the panel;
    // the panel itself opens fine by F8 / AKN's switch). JJ plays with Quest Touch controllers that Virtual Desktop turns
    // into a pad INSIDE the game (Batman City: "his pad input never crossed our XInput seam"). With AKN's XINPUT9_1_0.dll
    // forwarder in the game folder, our read of Windows' own copy never sees that pad. Engine mode now wraps the game's
    // OWN import of XInputGetState (BatmanAK.exe's import table): the game's reads pass our filter, and our chord read
    // calls the top of that same chain, so it sees exactly what the game sees.
    // HUDRING 2026-10-13 (JJ: bat transitions between menus / into gameplay; too short to capture by hand): an A or B press in a
    // menu (start-up / main menu, or the pause screen) asks AKN to write its HUD-sheet ring 0.9 s later (the bats' second)
    void ring_on_press(WORD b)
    {
        static WORD s_prev = 0; const WORD down = (WORD)(b & ~s_prev); s_prev = b;
        if (!(down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B))) return;
        if (akvr_xr_menu_phase() == 2 && !akvr_xr_pause_live()) return;
        using DumpFn = void (*)(const char*, int); static DumpFn f = nullptr; static bool t = false;
        if (!t) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); f = m ? (DumpFn)GetProcAddress(m, "akn_hudring_dump") : nullptr; t = m != nullptr; }
        if (f) f((down & XINPUT_GAMEPAD_A) ? "padA" : "padB", 900);
        akvr_ring_layers_request(600);   // HUDRING2: the HUD tree mid-bats (hooks.cpp)
        { using RsFn = void (*)(const char*); static RsFn r = nullptr; static bool rt = false;   // BATREC: draw kinds new again
          if (!rt) { HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll"); r = m ? (RsFn)GetProcAddress(m, "akn_hudsig_reset") : nullptr; rt = m != nullptr; }
          if (r) r((down & XINPUT_GAMEPAD_A) ? "padA" : "padB"); }
    }
    using XIGS2_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    void** g_iatSlot = nullptr;
    XIGS2_t g_iatNext = nullptr;
    void* g_iatFirst = nullptr;
    volatile LONG g_iatReads = 0, g_ourTopOk = 0, g_ourTopTries = 0;
    // PADINJECT 2026-10-09 (fake-headset tests: the wheels and detective mode are pad-only): buttons held for the game's
    // reads of pad 0 until a deadline (export akvr_pad_inject, AKN command "padbtn <hex mask> <ms>")
    volatile LONG g_injBtns = 0; volatile ULONGLONG g_injUntil = 0; volatile DWORD g_injPkt = 0;
    DWORD WINAPI hkIat(DWORD idx, XINPUT_STATE* st)
    {
        const XIGS2_t n = g_iatNext;
        DWORD r = n ? n(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
        if (!tl_ourRead && idx == 0 && st && g_injUntil && GetTickCount64() < g_injUntil)
        {
            if (r != ERROR_SUCCESS) { ZeroMemory(st, sizeof(*st)); r = ERROR_SUCCESS; }
            st->dwPacketNumber = ++g_injPkt;
            st->Gamepad.wButtons |= (WORD)g_injBtns;
        }
        if (!tl_ourRead) { InterlockedIncrement(&g_iatReads); game_read(idx, st, r); }
        // (HUDRING menu-press dumps removed 2026-10-14 - release tidy; the panel capture covers it)
        return r;
    }
    void** find_iat(const char* dll, const char* fn)
    {
        BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
        auto dos = (IMAGE_DOS_HEADER*)base; auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return nullptr;
        HMODULE gm = GetModuleHandleA(dll);
        void* byName = gm ? (void*)GetProcAddress(gm, fn) : nullptr;
        for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d)
        {
            if (_stricmp((const char*)(base + d->Name), dll) != 0) continue;
            auto names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            auto addrs = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
            for (; addrs->u1.Function; ++names, ++addrs)
            {
                if (d->OriginalFirstThunk && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                {
                    auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
                    if (strcmp((const char*)ibn->Name, fn) == 0) return (void**)&addrs->u1.Function;
                }
                if (byName && (void*)addrs->u1.Function == byName) return (void**)&addrs->u1.Function;   // by ordinal
            }
        }
        return nullptr;
    }
    void iat_wrap()
    {
        if (g_iatSlot) return;
        void** slot = find_iat("XINPUT9_1_0.dll", "XInputGetState");
        if (!slot) return;
        g_iatFirst = *slot;
        g_iatNext = (XIGS2_t)*slot;
        DWORD old = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        {
            InterlockedExchangePointer(slot, (void*)&hkIat);
            VirtualProtect(slot, sizeof(void*), old, &old);
            g_iatSlot = slot;
        }
    }
    void mod_of(const void* p, wchar_t* out, size_t n)
    {
        HMODULE m = nullptr; wchar_t full[MAX_PATH] = L"?";
        if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m) && m)
            GetModuleFileNameW(m, full, MAX_PATH);
        const wchar_t* b = wcsrchr(full, L'\\'); wcsncpy_s(out, n, b ? b + 1 : full, _TRUNCATE);
    }
    void game_read(DWORD idx, XINPUT_STATE* st, DWORD r)
    {
        // AIMTRIGGER2 2026-10-03 - JJ holds the left trigger but head aiming was lost: the trigger was only looked
        // for on pad slot 0. Any slot the game reads counts for the trigger now (menu blanking stays slot 0).
        if (r == ERROR_SUCCESS && st && idx != 0 && idx < 4 && st->Gamepad.bLeftTrigger > 64) { g_ltAt = GetTickCount64(); g_ltIdx = (int)idx; }
        if (r == ERROR_SUCCESS && st && idx == 0)
        {
            if (abs(st->Gamepad.sThumbRX) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE ||
                abs(st->Gamepad.sThumbRY) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) g_rsAt = GetTickCount64();   // SWINGEASE
            if (st->Gamepad.bLeftTrigger > 64) { g_ltAt = GetTickCount64(); g_ltIdx = 0; }   // AIMTRIGGER
            {   // ZOOMPRESS: right-stick click alone (not the menu chord) = the game's zoom button
                const bool r3 = (st->Gamepad.wButtons & MENU_CHORD) == XINPUT_GAMEPAD_RIGHT_THUMB;
                if (r3 && !g_r3Was) g_r3At = GetTickCount64();
                g_r3Was = r3;
            }
            if (st->Gamepad.bLeftTrigger > g_ltMax) g_ltMax = st->Gamepad.bLeftTrigger;
            if (g_menu)
                ZeroMemory(&st->Gamepad, sizeof(st->Gamepad));
            // Hide the chord from the game while it's being held.
            else if ((st->Gamepad.wButtons & MENU_CHORD) == MENU_CHORD)
                st->Gamepad.wButtons &= ~MENU_CHORD;
        }
    }

    // Read the REAL controller (bypasses our own interception via the trampoline, so
    // it works even in menu mode). Falls back to a direct call before the hook exists.
    bool read_real(XINPUT_STATE& st)
    {
        st = XINPUT_STATE{};
        if (g_iatSlot)
        {   // GAMEROUTE: the top of the game's own chain (whoever sits there: us, Virtual Desktop, ...), unfiltered
            const XIGS2_t top = (XIGS2_t)*g_iatSlot;
            tl_ourRead = true; const DWORD r = top ? top(0, &st) : ERROR_DEVICE_NOT_CONNECTED; tl_ourRead = false;
            InterlockedIncrement(&g_ourTopTries); if (r == ERROR_SUCCESS) InterlockedIncrement(&g_ourTopOk);
            return r == ERROR_SUCCESS;
        }
        tl_ourRead = true;
        DWORD r = oXIGS ? oXIGS(0, &st) : XInputGetState(0, &st);
        tl_ourRead = false;
        return r == ERROR_SUCCESS;
    }
}

static wchar_t g_padDiag[400] = L"controller hook: not installed";
void akvr_gamepad_install()
{
    if (akvr_engine_stereo() && !g_iatSlot) iat_wrap();   // GAMEROUTE: engine mode reads the pad the game's way
    if (g_hooked) return;
    // Batman AK (UE3) uses xinput9_1_0.dll — the same one we statically link, so it's
    // already loaded. If a future title uses a different XInput version, add it here.
    // AKN 2026-10-07 (JJ: the double stick click no longer opens the panel): the game folder's XINPUT9_1_0.dll is now
    // AKN, a forwarder - hooking its tiny stub missed. Hook Windows' own copy (the forwarder calls it for every read).
    HMODULE m = nullptr;
    {
        wchar_t sys[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n && n < MAX_PATH - 20 && wcscat_s(sys, L"\\xinput9_1_0.dll") == 0) m = LoadLibraryW(sys);
    }
    if (!m) m = GetModuleHandleW(L"xinput9_1_0.dll");
    if (!m) return;
    void* target = (void*)GetProcAddress(m, "XInputGetState");
    if (!target) return;
    MH_Initialize();   // harmless if kiero already initialized MinHook
    if (MH_CreateHook(target, (void*)&hkXIGS, (void**)&oXIGS) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        g_hooked = true;
    wchar_t mp[MAX_PATH] = L"?"; GetModuleFileNameW(m, mp, MAX_PATH);
    _snwprintf_s(g_padDiag, _countof(g_padDiag), _TRUNCATE, L"controller hook: %s XInputGetState in %s", g_hooked ? L"ON" : L"FAILED", mp);
}
const wchar_t* akvr_gamepad_hook_diag() { return g_padDiag; }
// GAMEROUTE diag (AKN logs it when it changes): the game's import, what sits on top now, and which route sees a pad
extern "C" __declspec(dllexport) const char* akvr_pad_route()
{
    static char s[400];
    wchar_t a[64], b[64], c[64];
    mod_of(g_iatFirst, a, 64); mod_of(g_iatSlot ? *g_iatSlot : nullptr, b, 64); mod_of((void*)g_iatNext, c, 64);
    XINPUT_STATE t1{}, t2{};
    tl_ourRead = true;
    const DWORD rTop = g_iatSlot && *g_iatSlot ? ((XIGS2_t)*g_iatSlot)(0, &t1) : 9999;
    const DWORD rSys = oXIGS ? oXIGS(0, &t2) : 9999;
    tl_ourRead = false;
    snprintf(s, sizeof(s), "pad route: game import %s (was %ls, now on top %ls, under us %ls), game reads through us %ld | pad 0 via the game's route: %s buttons %04x, via Windows' copy: %s buttons %04x | system hook %s",
             g_iatSlot ? "wrapped" : "NOT wrapped", a, b, c, (long)g_iatReads,
             rTop == ERROR_SUCCESS ? "connected" : "none", t1.Gamepad.wButtons, rSys == ERROR_SUCCESS ? "connected" : "none", t2.Gamepad.wButtons, g_hooked ? "on" : "off");
    return s;
}

void akvr_gamepad_set_menu(bool on) { g_menu = on; }
bool akvr_gamepad_menu()            { return g_menu; }

static bool g_chordDoubleTap = false;   // FIRSTPERSON (below)
bool akvr_gamepad_menu_chord_pressed()
{
    // Both stick clicks held together for ~0.6s (see MENU_CHORD for why it moved off
    // Menu+View on 2026-09-26). The hold is longer than the old 0.45s because the
    // 2026-07-25 stick-click version was too easy to trigger by accident. Fires ONCE
    // per hold.
    const ULONGLONG HOLD_MS = 600;
    static ULONGLONG downSince = 0;
    static bool      fired     = false;
    XINPUT_STATE st;
    bool both = read_real(st)
        && (st.Gamepad.wButtons & MENU_CHORD) == MENU_CHORD;
    ULONGLONG now = GetTickCount64();
    if (both)
    {
        if (downSince == 0) downSince = now;
        if (!fired && now - downSince >= HOLD_MS) { fired = true; return true; }
    }
    else
    {
        // FIRSTPERSON 2026-10-11 (JJ, like Batman City overlay.cpp): two quick clicks of both sticks (each under 0.35 s,
        // the second within 0.6 s of the first) = the double tap; the 0.6 s hold stays the menu
        static ULONGLONG lastTap = 0;
        if (downSince && !fired && now - downSince < 350)
        {
            if (lastTap && now - lastTap < 600) { lastTap = 0; g_chordDoubleTap = true; }
            else lastTap = now;
        }
        downSince = 0; fired = false;
    }
    return false;
}
bool akvr_gamepad_chord_double_tap() { const bool v = g_chordDoubleTap; g_chordDoubleTap = false; return v; }

GamepadDiag akvr_gamepad_diag()
{
    GamepadDiag d{};
    d.hooked   = g_hooked;
    d.blanking = g_menu;
    XINPUT_STATE st;
    d.connected = read_real(st);
    if (d.connected)
    {
        d.buttons = st.Gamepad.wButtons;
        d.lx = st.Gamepad.sThumbLX;
        d.ly = st.Gamepad.sThumbLY;
    }
    return d;
}

void akvr_gamepad_feed_imgui(bool muteLeftRight)
{
    if (!g_menu) return;
    XINPUT_STATE st;
    if (!read_real(st)) return;
    XINPUT_GAMEPAD gp = st.Gamepad;
    if (muteLeftRight)
    {   // ONESTEP: a slider owns left/right (hooks.cpp SliderStep); ImGui must not tweak too
        gp.wButtons &= ~(XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
        gp.sThumbLX = 0;
    }
    ImGuiIO& io = ImGui::GetIO();

    auto btn = [&](ImGuiKey k, WORD mask) { io.AddKeyEvent(k, (gp.wButtons & mask) != 0); };
    btn(ImGuiKey_GamepadFaceDown,  XINPUT_GAMEPAD_A);            // activate / edit
    btn(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);            // cancel / back
    btn(ImGuiKey_GamepadFaceLeft,  XINPUT_GAMEPAD_X);
    btn(ImGuiKey_GamepadFaceUp,    XINPUT_GAMEPAD_Y);
    btn(ImGuiKey_GamepadDpadUp,    XINPUT_GAMEPAD_DPAD_UP);
    btn(ImGuiKey_GamepadDpadDown,  XINPUT_GAMEPAD_DPAD_DOWN);
    btn(ImGuiKey_GamepadDpadLeft,  XINPUT_GAMEPAD_DPAD_LEFT);
    btn(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    btn(ImGuiKey_GamepadL1,        XINPUT_GAMEPAD_LEFT_SHOULDER);
    btn(ImGuiKey_GamepadR1,        XINPUT_GAMEPAD_RIGHT_SHOULDER);

    // Left stick → analog nav (deadzone then normalized magnitude, like the stock backend).
    auto axis = [&](ImGuiKey k, int v, int lo, int hi) {
        float f = (float)(v - lo) / (float)(hi - lo);
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        io.AddKeyAnalogEvent(k, f > 0.10f, f);
    };
    axis(ImGuiKey_GamepadLStickRight,  gp.sThumbLX, 7000, 30000);
    axis(ImGuiKey_GamepadLStickLeft,  -gp.sThumbLX, 7000, 30000);
    axis(ImGuiKey_GamepadLStickUp,     gp.sThumbLY, 7000, 30000);
    axis(ImGuiKey_GamepadLStickDown,  -gp.sThumbLY, 7000, 30000);
}
// SWINGEASE: ms since the game last read the right stick off centre (huge = never)
unsigned long long akvr_gamepad_right_stick_ms() { return g_rsAt ? GetTickCount64() - g_rsAt : ~0ull; }
unsigned long long akvr_gamepad_zoom_press_ms() { return g_r3At ? GetTickCount64() - g_r3At : ~0ull; }   // ZOOMPRESS
// AIMTRIGGER: ms since the game last read the left trigger held (huge = never)
unsigned long long akvr_gamepad_left_trigger_ms()
{   // AIMTRIGGER2: the game's reads OR our own poll of all four pad slots (polled at most every 30 ms)
    static ULONGLONG s_poll = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_poll >= 30)
    {
        s_poll = now;
        static bool s_conn[4] = { true, true, true, true };   // an empty slot is slow to ask: every 2 s only
        static ULONGLONG s_try[4] = {};
        for (DWORD i = 0; i < 4; ++i)
        {
            if (!s_conn[i] && now - s_try[i] < 2000) continue;
            s_try[i] = now;
            XINPUT_STATE st{};
            const DWORD r = oXIGS ? oXIGS(i, &st) : XInputGetState(i, &st);
            s_conn[i] = r == ERROR_SUCCESS;
            if (r != ERROR_SUCCESS) continue;
            if (st.Gamepad.bLeftTrigger > g_ltMax) g_ltMax = st.Gamepad.bLeftTrigger;
            if (st.Gamepad.bLeftTrigger > 64) { g_ltRealAt = now; g_ltRealIdx = (int)i; }
        }
    }
    const ULONGLONG a = g_ltAt > g_ltRealAt ? g_ltAt : g_ltRealAt;
    return a ? now - a : ~0ull;
}
const char* akvr_gamepad_trigger_diag()
{
    static char s[160];
    const ULONGLONG now = GetTickCount64();
    snprintf(s, sizeof(s), "left trigger: game read %lld ms ago (pad %d), own poll %lld ms ago (pad %d), highest %d",
             g_ltAt ? (long long)(now - g_ltAt) : -1ll, g_ltIdx, g_ltRealAt ? (long long)(now - g_ltRealAt) : -1ll, g_ltRealIdx, g_ltMax);
    return s;
}
extern "C" __declspec(dllexport) void akvr_pad_inject(int buttons, int ms) { g_injBtns = buttons; g_injUntil = ms > 0 ? GetTickCount64() + (ULONGLONG)ms : 0; }   // PADINJECT
