// AKN bootstrap: XINPUT9_1_0.dll from the game folder, every export forwarded to the system copy.
#include <windows.h>
namespace {
HMODULE real_xinput()
{
    static HMODULE m = [] {
        wchar_t p[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (!n || n >= MAX_PATH || wcscat_s(p, L"\\xinput9_1_0.dll") != 0) return HMODULE{};
        return LoadLibraryW(p);
    }();
    return m;
}
template <typename T> T real(const char* name)
{
    HMODULE m = real_xinput();
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}
}
extern "C" DWORD WINAPI pXInputGetState(DWORD i, void* s)
{
    static auto f = real<DWORD(WINAPI*)(DWORD, void*)>("XInputGetState");
    return f ? f(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" DWORD WINAPI pXInputSetState(DWORD i, void* v)
{
    static auto f = real<DWORD(WINAPI*)(DWORD, void*)>("XInputSetState");
    return f ? f(i, v) : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" DWORD WINAPI pXInputGetCapabilities(DWORD i, DWORD fl, void* c)
{
    static auto f = real<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetCapabilities");
    return f ? f(i, fl, c) : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" DWORD WINAPI pXInputGetDSoundAudioDeviceGuids(DWORD i, GUID* a, GUID* b)
{
    static auto f = real<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>("XInputGetDSoundAudioDeviceGuids");
    return f ? f(i, a, b) : ERROR_DEVICE_NOT_CONNECTED;
}
