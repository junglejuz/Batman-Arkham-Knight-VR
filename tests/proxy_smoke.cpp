#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy) { std::printf("Load failed: %lu\n", GetLastError()); return 2; }
    const char* names[] = {"DirectInput8Create", "DllCanUnloadNow", "DllGetClassObject",
                          "DllRegisterServer", "DllUnregisterServer", "GetdfDIJoystick"};
    for (int i = 0; i < 6; ++i) {
        FARPROC named = GetProcAddress(proxy, names[i]);
        if (!named || named != GetProcAddress(proxy, MAKEINTRESOURCEA(i + 1))) return 3;
    }
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = reinterpret_cast<Create>(GetProcAddress(proxy, names[0]));
    IDirectInput8W* input = nullptr;
    HRESULT result = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                            IID_IDirectInput8W, reinterpret_cast<void**>(&input), nullptr);
    if (FAILED(result) || !input) {
        std::printf("DirectInput creation failed: %08lx\n", result); return 4;
    }
    input->Release();
    using GetFormat = LPCDIDATAFORMAT(WINAPI*)();
    auto format = reinterpret_cast<GetFormat>(GetProcAddress(proxy, names[5]))();
    if (!format || format->dwSize != sizeof(DIDATAFORMAT)) return 5;
    using CanUnload = HRESULT(WINAPI*)();
    if (reinterpret_cast<CanUnload>(GetProcAddress(proxy, names[1]))() != S_FALSE) return 6;
    // The genuine API must still reject an unsupported version.
    input = nullptr;
    result = create(GetModuleHandleW(nullptr), 0xffffffff, IID_IDirectInput8W,
                    reinterpret_cast<void**>(&input), nullptr);
    if (input) input->Release();
    if (SUCCEEDED(result)) return 7;
    std::puts("PASS: exports/ordinals, real DirectInput object, joystick format, invalid-version rejection.");
    return 0;
}
