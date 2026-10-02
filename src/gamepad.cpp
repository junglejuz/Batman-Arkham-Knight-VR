#include "gamepad.h"
#include <windows.h>
#include <xinput.h>
#include <MinHook.h>
#include "imgui.h"

namespace
{
    using XIGS_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    XIGS_t oXIGS = nullptr;          // original (un-intercepted) XInputGetState, via MinHook trampoline
    bool   g_hooked = false;
    bool   g_menu   = false;
    volatile ULONGLONG g_rsAt = 0;   // SWINGEASE: the last game read with the right stick off centre
    volatile ULONGLONG g_ltAt = 0;   // AIMTRIGGER: the last game read with the left trigger (aim / battle mode) held

    // Our interception: pass through normally, but while menu mode is on, blank out
    // controller 0 for the CALLER (the game) so it acts as if no buttons/sticks are
    // touched. We still report success so the game keeps thinking a pad is plugged in.
    // 2026-09-26: moved from Menu+View (Start+Back) to BOTH STICK CLICKS. JJ: the old
    // chord "interferes with opening the map" — View alone opens AK's map the moment
    // it is pressed, before Menu can join it, and no amount of hiding the finished
    // chord can take that press back. Clicking both sticks at once is not a game
    // action, so the pair is hidden from the game as soon as both are down.
    const WORD MENU_CHORD = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;

    DWORD WINAPI hkXIGS(DWORD idx, XINPUT_STATE* st)
    {
        DWORD r = oXIGS ? oXIGS(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
        if (r == ERROR_SUCCESS && st && idx == 0)
        {
            if (abs(st->Gamepad.sThumbRX) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE ||
                abs(st->Gamepad.sThumbRY) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) g_rsAt = GetTickCount64();   // SWINGEASE
            if (st->Gamepad.bLeftTrigger > 64) g_ltAt = GetTickCount64();   // AIMTRIGGER
            if (g_menu)
                ZeroMemory(&st->Gamepad, sizeof(st->Gamepad));
            // Hide the chord from the game while it's being held.
            else if ((st->Gamepad.wButtons & MENU_CHORD) == MENU_CHORD)
                st->Gamepad.wButtons &= ~MENU_CHORD;
        }
        return r;
    }

    // Read the REAL controller (bypasses our own interception via the trampoline, so
    // it works even in menu mode). Falls back to a direct call before the hook exists.
    bool read_real(XINPUT_STATE& st)
    {
        st = XINPUT_STATE{};
        DWORD r = oXIGS ? oXIGS(0, &st) : XInputGetState(0, &st);
        return r == ERROR_SUCCESS;
    }
}

void akvr_gamepad_install()
{
    if (g_hooked) return;
    // Batman AK (UE3) uses xinput9_1_0.dll — the same one we statically link, so it's
    // already loaded. If a future title uses a different XInput version, add it here.
    HMODULE m = GetModuleHandleW(L"xinput9_1_0.dll");
    if (!m) return;
    void* target = (void*)GetProcAddress(m, "XInputGetState");
    if (!target) return;
    MH_Initialize();   // harmless if kiero already initialized MinHook
    if (MH_CreateHook(target, (void*)&hkXIGS, (void**)&oXIGS) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        g_hooked = true;
}

void akvr_gamepad_set_menu(bool on) { g_menu = on; }
bool akvr_gamepad_menu()            { return g_menu; }

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
    else { downSince = 0; fired = false; }
    return false;
}

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
// AIMTRIGGER: ms since the game last read the left trigger held (huge = never)
unsigned long long akvr_gamepad_left_trigger_ms() { return g_ltAt ? GetTickCount64() - g_ltAt : ~0ull; }
