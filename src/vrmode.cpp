// AKVR — VR or flat (2D), decided at process start (VRLAUNCH, 2026-10-02).
//
// JJ: "The installer also needs to create a way for the game to be launched in VR, leaving launching it from Steam to
// just play normally in 2D mode." The mod's dinput8.dll is always loaded by the game. It runs in VR only when
//   - the VR launcher (AKVR-Launch-VR.bat, desktop shortcut) left akvr_launch_vr.flag in Binaries\Win64 within the
//     last 3 minutes (the note is used up at once, so a later Steam start is 2D again), or
//   - the game was started with -akvr (Steam launch options: always VR).
// Otherwise nothing of AKVR starts: no geo-11, no hooks, no OpenXR - the dinput8 exports still pass through.
//
// The game keeps different settings in VR (windowed at the VR size, frame cap, blur off, OneFrameThreadLag) and saves
// whatever ran last, in two files: BmGame\Config\BmSystemSettings.ini and NVIDIA's settings store
// <Documents>\WB Games\Batman Arkham Knight\GFXSettings.BatmanArkhamKnight.xml. Each mode keeps its own copy in
// Binaries\Win64\akvr_profiles\{2d,vr}\; when the mode changes, the files as the last session left them go to that
// mode's copy and the other mode's copy comes back. A crashed VR session therefore still gives a normal 2D start.
//
// Runs inside DllMain (loader lock): only kernel32 file calls and one registry read (no LoadLibrary, no COM).
#include <windows.h>
#include <cwchar>

namespace {
    wchar_t g_dir[MAX_PATH] = L"";       // Binaries\Win64\ with a trailing backslash
    bool g_vr = false;

    void join(wchar_t* out, const wchar_t* a, const wchar_t* b)
    {
        wcscpy_s(out, MAX_PATH, a);
        wcscat_s(out, MAX_PATH, b);
    }
    bool exists(const wchar_t* p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

    void log_line(const wchar_t* fmt, ...)
    {
        wchar_t p[MAX_PATH]; join(p, g_dir, L"akvr_mode_log.txt");
        // keep it small: start over once it passes 32 KB
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 32768) DeleteFileW(p);
        wchar_t msg[600];
        SYSTEMTIME st{}; GetLocalTime(&st);
        int n = _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond);
        va_list ap; va_start(ap, fmt);
        if (n > 0) _vsnwprintf_s(msg + n, _countof(msg) - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        wcscat_s(msg, L"\r\n");
        HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        char a[1200]; const int len = WideCharToMultiByte(CP_UTF8, 0, msg, -1, a, sizeof(a), nullptr, nullptr);
        DWORD w = 0; if (len > 1) WriteFile(h, a, (DWORD)(len - 1), &w, nullptr);
        CloseHandle(h);
    }

    bool read_small(const wchar_t* p, char* buf, DWORD cap)
    {
        buf[0] = 0;
        HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD r = 0; const BOOL ok = ReadFile(h, buf, cap - 1, &r, nullptr);
        CloseHandle(h);
        buf[ok ? r : 0] = 0;
        return ok != FALSE;
    }
    void write_small(const wchar_t* p, const char* s)
    {
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0; WriteFile(h, s, (DWORD)strlen(s), &w, nullptr);
        CloseHandle(h);
    }

    // <Documents> as Windows has it (the folder can be moved: JJ's is D:\Documents)
    bool documents(wchar_t* out)
    {
        out[0] = 0;
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                          0, KEY_READ, &k) == ERROR_SUCCESS)
        {
            wchar_t raw[MAX_PATH]; DWORD sz = sizeof(raw), type = 0;
            if (RegQueryValueExW(k, L"Personal", nullptr, &type, (BYTE*)raw, &sz) == ERROR_SUCCESS &&
                (type == REG_EXPAND_SZ || type == REG_SZ))
            {
                raw[_countof(raw) - 1] = 0;
                if (!ExpandEnvironmentStringsW(raw, out, MAX_PATH)) out[0] = 0;
            }
            RegCloseKey(k);
        }
        if (!out[0])
        {
            if (!GetEnvironmentVariableW(L"USERPROFILE", out, MAX_PATH)) return false;
            wcscat_s(out, MAX_PATH, L"\\Documents");
        }
        return true;
    }

    struct Pair { wchar_t live[MAX_PATH]; const wchar_t* name; };

    // copy every live file that exists into profiles\<mode>\ (the state that mode's session left)
    void save_to(const Pair* f, int n, const wchar_t* mode)
    {
        wchar_t dir[MAX_PATH]; join(dir, g_dir, L"akvr_profiles\\"); CreateDirectoryW(dir, nullptr);
        wcscat_s(dir, mode); wcscat_s(dir, L"\\"); CreateDirectoryW(dir, nullptr);
        for (int i = 0; i < n; ++i)
        {
            if (!f[i].live[0] || !exists(f[i].live)) continue;
            wchar_t dst[MAX_PATH]; join(dst, dir, f[i].name);
            const BOOL ok = CopyFileW(f[i].live, dst, FALSE);
            log_line(L"  saved %s for %s: %s", f[i].name, mode, ok ? L"ok" : L"FAILED");
        }
    }
    // put profiles\<mode>\ back over the live files (only what that copy holds)
    void restore_from(const Pair* f, int n, const wchar_t* mode)
    {
        wchar_t dir[MAX_PATH]; join(dir, g_dir, L"akvr_profiles\\"); wcscat_s(dir, mode); wcscat_s(dir, L"\\");
        for (int i = 0; i < n; ++i)
        {
            if (!f[i].live[0]) continue;
            wchar_t src[MAX_PATH]; join(src, dir, f[i].name);
            if (!exists(src))
            {
                // 2D with no copy: the player never had this file (game not started before the install, or the
                // install saved none). Leaving VR's would start 2D windowed at the VR size; without it the game
                // makes its own defaults. Only once VR's copy is safe.
                wchar_t vrc[MAX_PATH]; join(vrc, g_dir, L"akvr_profiles\\vr\\"); wcscat_s(vrc, f[i].name);
                if (_wcsicmp(mode, L"2d") == 0 && exists(f[i].live) && exists(vrc))
                {
                    const BOOL ok = DeleteFileW(f[i].live);
                    log_line(L"  no 2d copy of %s - removed VR's, the game makes its defaults: %s", f[i].name, ok ? L"ok" : L"FAILED");
                }
                else log_line(L"  no %s copy of %s - left as it is", mode, f[i].name);
                continue;
            }
            const BOOL ok = CopyFileW(src, f[i].live, FALSE);
            log_line(L"  %s settings back: %s %s", mode, f[i].name, ok ? L"ok" : L"FAILED");
        }
    }
}

// true = run AKVR (VR); false = stay out of the game entirely (2D)
bool akvr_vr_mode_decide(HINSTANCE self)
{
    wchar_t mod[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, mod, MAX_PATH);
    if (!n || n >= MAX_PATH) return true;                       // cannot tell: behave as before (VR)
    wchar_t* slash = wcsrchr(mod, L'\\');
    if (!slash) return true;
    slash[1] = 0;
    wcscpy_s(g_dir, mod);

    // ---- the mode ----
    const wchar_t* why = L"no VR launch note (started from Steam)";
    wchar_t flag[MAX_PATH]; join(flag, g_dir, L"akvr_launch_vr.flag");
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(flag, GetFileExInfoStandard, &fa))
    {
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        const ULONGLONG t = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        const ULONGLONG c = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        const bool fresh = c >= t ? (c - t) < 180ULL * 10000000ULL : true;
        DeleteFileW(flag);                                       // used up either way
        if (fresh) { g_vr = true; why = L"VR launcher"; }
        else why = L"VR launch note older than 3 minutes (ignored)";
    }
    const wchar_t* cmd = GetCommandLineW();
    if (!g_vr && cmd && (wcsstr(cmd, L" -akvr") || wcsstr(cmd, L" -AKVR"))) { g_vr = true; why = L"-akvr on the command line"; }
    log_line(L"start in %s (%s)", g_vr ? L"VR" : L"2D", why);

    // ---- the game's settings for this mode ----
    Pair files[2] = {};
    files[0].name = L"BmSystemSettings.ini";
    files[1].name = L"GFXSettings.BatmanArkhamKnight.xml";
    {
        wchar_t p[MAX_PATH]; join(p, g_dir, L"..\\..\\BmGame\\Config\\BmSystemSettings.ini");
        if (!GetFullPathNameW(p, MAX_PATH, files[0].live, nullptr)) files[0].live[0] = 0;
        wchar_t d[MAX_PATH];
        if (documents(d)) { join(files[1].live, d, L"\\WB Games\\Batman Arkham Knight\\GFXSettings.BatmanArkhamKnight.xml"); }
    }
    wchar_t lastPath[MAX_PATH]; join(lastPath, g_dir, L"akvr_profiles\\last.txt");
    char last[16] = "";
    if (!read_small(lastPath, last, sizeof(last)))
    {
        // An install from before VRLAUNCH ran VR only: the live files are VR's. Its first 2D copy is the player's own
        // settings the installer saved (vrmod_graphics_backup), if there is one.
        strcpy_s(last, "vr");
        wchar_t two[MAX_PATH]; join(two, g_dir, L"akvr_profiles\\2d\\BmSystemSettings.ini");
        if (!exists(two))
        {
            wchar_t dir[MAX_PATH]; join(dir, g_dir, L"akvr_profiles\\"); CreateDirectoryW(dir, nullptr);
            wcscat_s(dir, L"2d\\"); CreateDirectoryW(dir, nullptr);
            for (int i = 0; i < 2; ++i)
            {
                wchar_t src[MAX_PATH]; join(src, g_dir, L"vrmod_graphics_backup\\"); wcscat_s(src, files[i].name);
                wchar_t dst[MAX_PATH]; join(dst, dir, files[i].name);
                if (exists(src)) CopyFileW(src, dst, TRUE);
            }
        }
        log_line(L"first start since VR/2D launching came in: the current settings are taken as VR's");
    }
    const bool lastVr = _strnicmp(last, "vr", 2) == 0;
    if (lastVr != g_vr)
    {
        save_to(files, 2, lastVr ? L"vr" : L"2d");
        restore_from(files, 2, g_vr ? L"vr" : L"2d");
    }
    write_small(lastPath, g_vr ? "vr" : "2d");
    return g_vr;
}

bool akvr_vr_mode() { return g_vr; }
