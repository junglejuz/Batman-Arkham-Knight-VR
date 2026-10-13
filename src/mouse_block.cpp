// AKVR - the game's DirectInput MOUSE is still while the VR panel is open (PANELMOUSE, ported from Batman City
// 2026-10-13, JJ: "Arkham City's mouse works now in the overlay, port it to ... Arkham Knight"). The game reads the mouse
// through DirectInput, so moving it over the open panel also turned the camera. While the panel is open a mouse read
// returns no movement and no buttons and buffered mouse events are dropped; keyboards and pads are never touched (the
// pad has its own block in gamepad.cpp). The panel itself reads the cursor and the buttons through the window.
#include "mouse_block.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <MinHook.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "dxguid.lib")

void akvr_early_note(const char* s); // earlyres.cpp: one line into the startup transcript

namespace {
void note(const char* fmt, ...) {
    char b[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    akvr_early_note(b);
}
} // namespace

namespace {

using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
GetDeviceStateFn g_stateOrig = nullptr;
GetDeviceDataFn g_dataOrig = nullptr;
bool (*g_blocked)() = nullptr;

struct Dev { void* ptr = nullptr; bool mouse = false; };
Dev g_devs[16];
std::mutex g_mutex;
std::atomic<unsigned> g_blockedReads{0};

bool is_mouse(void* dev) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (Dev& d : g_devs)
        if (d.ptr == dev) return d.mouse;
    for (Dev& d : g_devs) {
        if (d.ptr) continue;
        d.ptr = dev;
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        d.mouse = SUCCEEDED(static_cast<IDirectInputDevice8W*>(dev)->GetCapabilities(&caps)) &&
                  GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE;
        if (d.mouse) note("PANELMOUSE: the game reads the mouse through DirectInput - still while the VR panel is open");
        return d.mouse;
    }
    return false;
}

HRESULT STDMETHODCALLTYPE state_detour(IDirectInputDevice8W* self, DWORD cb, LPVOID data) {
    const HRESULT hr = g_stateOrig(self, cb, data);
    if (SUCCEEDED(hr) && data && cb && g_blocked && g_blocked() && is_mouse(self)) {
        memset(data, 0, cb); // relative movement + buttons: all zero = still and released
        g_blockedReads.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE data_detour(IDirectInputDevice8W* self, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut,
                                      DWORD flags) {
    const HRESULT hr = g_dataOrig(self, cbObj, rg, inOut, flags);
    if (SUCCEEDED(hr) && inOut && *inOut && g_blocked && g_blocked() && is_mouse(self)) {
        *inOut = 0; // events while the panel is open are the panel's
        g_blockedReads.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

} // namespace

void akvr_mouse_block_install(bool (*blocked)()) {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    g_blocked = blocked;
    // The SYSTEM dinput8 (this DLL may itself be the dinput8 proxy): a throwaway keyboard device yields the shared
    // device implementation (vtable slot 9 = GetDeviceState, 10 = GetDeviceData).
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dinput8.dll");
    HMODULE di8 = LoadLibraryW(path);
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = di8 ? reinterpret_cast<CreateFn>(GetProcAddress(di8, "DirectInput8Create")) : nullptr;
    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* dev = nullptr;
    if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                 reinterpret_cast<void**>(&di), nullptr)) || !di ||
        FAILED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) || !dev) {
        if (di) di->Release();
        note("PANELMOUSE: DirectInput not reachable - the game's mouse is not blocked under the panel");
        return;
    }
    void** vt = *reinterpret_cast<void***>(dev);
    MH_Initialize(); // idempotent
    int hooked = 0;
    for (int i = 0; i < 2; ++i) {
        void* target = vt[9 + i];
        void* detour = i == 0 ? reinterpret_cast<void*>(&state_detour) : reinterpret_cast<void*>(&data_detour);
        void** orig = i == 0 ? reinterpret_cast<void**>(&g_stateOrig) : reinterpret_cast<void**>(&g_dataOrig);
        if (MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK) ++hooked;
    }
    dev->Release();
    di->Release();
    note("PANELMOUSE: DirectInput mouse block armed (%d of 2 hooks)", hooked);
}

unsigned akvr_mouse_block_reads() { return g_blockedReads.load(std::memory_order_relaxed); }
