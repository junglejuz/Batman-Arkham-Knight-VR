// AKVR — version.dll proxy layer
// ------------------------------------------------------------------
// The game loads us thinking we're Windows' version.dll (because we
// sit next to BatmanAK.exe and version.dll is not a "KnownDLL", so the
// exe directory wins the search order). We forward every real
// version.dll call to the genuine one in System32, and use the free
// ride into the process to start our hook thread (see dllmain.cpp).
//
// We deliberately do NOT proxy d3d11.dll so we can coexist with
// SheriefFarouk's frame-pacing fix (which ships as a d3d11.dll).
// ------------------------------------------------------------------
#include <windows.h>

static HMODULE g_real = nullptr;

static FARPROC real_proc(const char* name)
{
    if (!g_real)
    {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\version.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

// One typed forwarder per export. Verbose but boring-and-safe: no
// naked jumps (not allowed on x64 MSVC), no forwarder-path hacks.
extern "C" {

BOOL WINAPI pGetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d)
{ using F = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID); static auto f = (F)real_proc("GetFileVersionInfoA"); return f ? f(a, b, c, d) : FALSE; }

BOOL WINAPI pGetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d)
{ using F = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID); static auto f = (F)real_proc("GetFileVersionInfoW"); return f ? f(a, b, c, d) : FALSE; }

BOOL WINAPI pGetFileVersionInfoExA(DWORD fl, LPCSTR a, DWORD b, DWORD c, LPVOID d)
{ using F = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID); static auto f = (F)real_proc("GetFileVersionInfoExA"); return f ? f(fl, a, b, c, d) : FALSE; }

BOOL WINAPI pGetFileVersionInfoExW(DWORD fl, LPCWSTR a, DWORD b, DWORD c, LPVOID d)
{ using F = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID); static auto f = (F)real_proc("GetFileVersionInfoExW"); return f ? f(fl, a, b, c, d) : FALSE; }

DWORD WINAPI pGetFileVersionInfoSizeA(LPCSTR a, LPDWORD b)
{ using F = DWORD(WINAPI*)(LPCSTR, LPDWORD); static auto f = (F)real_proc("GetFileVersionInfoSizeA"); return f ? f(a, b) : 0; }

DWORD WINAPI pGetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b)
{ using F = DWORD(WINAPI*)(LPCWSTR, LPDWORD); static auto f = (F)real_proc("GetFileVersionInfoSizeW"); return f ? f(a, b) : 0; }

DWORD WINAPI pGetFileVersionInfoSizeExA(DWORD fl, LPCSTR a, LPDWORD b)
{ using F = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD); static auto f = (F)real_proc("GetFileVersionInfoSizeExA"); return f ? f(fl, a, b) : 0; }

DWORD WINAPI pGetFileVersionInfoSizeExW(DWORD fl, LPCWSTR a, LPDWORD b)
{ using F = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD); static auto f = (F)real_proc("GetFileVersionInfoSizeExW"); return f ? f(fl, a, b) : 0; }

DWORD WINAPI pVerFindFileA(DWORD fl, LPCSTR a, LPCSTR b, LPCSTR c, LPSTR d, PUINT e, LPSTR g, PUINT h)
{ using F = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT); static auto f = (F)real_proc("VerFindFileA"); return f ? f(fl, a, b, c, d, e, g, h) : 0; }

DWORD WINAPI pVerFindFileW(DWORD fl, LPCWSTR a, LPCWSTR b, LPCWSTR c, LPWSTR d, PUINT e, LPWSTR g, PUINT h)
{ using F = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT); static auto f = (F)real_proc("VerFindFileW"); return f ? f(fl, a, b, c, d, e, g, h) : 0; }

DWORD WINAPI pVerInstallFileA(DWORD fl, LPCSTR a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPSTR g, PUINT h)
{ using F = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT); static auto f = (F)real_proc("VerInstallFileA"); return f ? f(fl, a, b, c, d, e, g, h) : 0; }

DWORD WINAPI pVerInstallFileW(DWORD fl, LPCWSTR a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPWSTR g, PUINT h)
{ using F = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT); static auto f = (F)real_proc("VerInstallFileW"); return f ? f(fl, a, b, c, d, e, g, h) : 0; }

DWORD WINAPI pVerLanguageNameA(DWORD a, LPSTR b, DWORD c)
{ using F = DWORD(WINAPI*)(DWORD, LPSTR, DWORD); static auto f = (F)real_proc("VerLanguageNameA"); return f ? f(a, b, c) : 0; }

DWORD WINAPI pVerLanguageNameW(DWORD a, LPWSTR b, DWORD c)
{ using F = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD); static auto f = (F)real_proc("VerLanguageNameW"); return f ? f(a, b, c) : 0; }

BOOL WINAPI pVerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d)
{ using F = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT); static auto f = (F)real_proc("VerQueryValueA"); return f ? f(a, b, c, d) : FALSE; }

BOOL WINAPI pVerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d)
{ using F = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT); static auto f = (F)real_proc("VerQueryValueW"); return f ? f(a, b, c, d) : FALSE; }

} // extern "C"
