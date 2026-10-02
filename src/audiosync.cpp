// AUDIOSYNC 2026-10-02 — JJ: from the start-up screens into the first menu "there's a pause of a few seconds where
// everything's black, but the audio continues (which is like a sound effect), and then music starts. The sound effect
// should be synced up with the menu appearing". The mode timeline (MENUGAP) showed the game itself stopping its frames
// (0.8 s, then 1.7 s) while it loads the menu. JJ: "maybe we need to pause the audio so it syncs up".
//
// Arkham Knight's sound (Wwise) plays through XAudio2 2.7, which Wwise creates with CoCreateInstance. We keep that
// IXAudio2 and, only before the main menu has been confirmed (start-up), a watcher thread pauses the audio engine
// (IXAudio2::StopEngine) when no frame has come for 50 ms (was 150: AUDIOSYNC2), and the next frame resumes it (StartEngine): the sound
// carries on from where it stopped, in step with the picture. Resumed after 8 s whatever happens; never in gameplay.
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <MinHook.h>

int akvr_xr_menu_phase();   // xr.cpp: 0 = start-up (before the main menu is confirmed)
void akvr_xr_mode_note(const char* what);           // xr.cpp: a line in the mode timeline
void akvr_hudsplit_ui_now(int& movies, int& draws); // hudsplit.cpp: the screen UI drawn just now
bool akvr_xr_startup_3d();                         // xr.cpp: start-up, the 3D menu scene has begun
float akvr_xr_startup_bright();                    // xr.cpp: the picture's brightness in start-up (-1 = unknown)
// STARTBRIGHT 2026-10-02 — JJ: with the UI-draw rule "the audio started after the main menu had appeared ... We're
// syncing it up with the appearance of the first menu": the menu has appeared when the picture is no longer black
// (mean brightness 0.03+); the UI-draw count only when the brightness is unknown.
// AUDIOSYNC3 2026-10-02 — JJ: "the music still starts very briefly before it pauses"; "There is music that goes together
// with the title screens. And then that stops and then another music starts briefly, but that's the music that should
// appear with the menu." The menu's music starts while the game still draws black frames between its two loading
// pauses, so releasing on the next frame let it through. From the first frame of the 3D menu scene (still black) the
// sound stays held until the menu is really being drawn (its UI call draws 10+ pieces), at most 6 s; every hold and
// release goes into the mode timeline with the UI counts. The logos before it are untouched by this rule.

namespace {
    // XAudio2 2.7 (DirectX June 2010): CLSID_XAudio2 / CLSID_XAudio2_Debug, IID_IXAudio2
    const GUID kClsXA27  = { 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
    const GUID kClsXA27D = { 0xdb05ea35, 0x0329, 0x4d4b, { 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52 } };
    const GUID kIidXA27  = { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };

    // IXAudio2 (2.7) vtable: 0 QueryInterface, 1 AddRef, 2 Release, 3 GetDeviceCount, 4 GetDeviceDetails, 5 Initialize,
    // 6 RegisterForCallbacks, 7 UnregisterForCallbacks, 8 CreateSourceVoice, 9 CreateSubmixVoice, 10 CreateMasteringVoice,
    // 11 StartEngine, 12 StopEngine, ...
    typedef HRESULT (STDMETHODCALLTYPE *StartFn)(void* self);
    typedef void    (STDMETHODCALLTYPE *StopFn)(void* self);

    typedef HRESULT (WINAPI *CoCreateFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    CoCreateFn g_origCoCreate = nullptr;

    void* volatile g_xa = nullptr;            // the game's IXAudio2 (owned by Wwise)
    volatile LONG  g_created = 0;
    bool           g_on = true;               // setting audiosync
    volatile ULONGLONG g_lastFrame = 0;
    volatile LONG  g_stopped = 0;
    volatile ULONGLONG g_stoppedAt = 0;
    volatile LONG  g_holds = 0;
    volatile LONG  g_holdMs = 0;
    volatile LONG  g_holdDone = 0;            // AUDIOSYNC3: the menu-scene hold has ended once (never again)
    bool menu_not_drawn()
    {
        if (g_holdDone || !akvr_xr_startup_3d()) return false;
        const float b = akvr_xr_startup_bright();
        if (b >= 0.0f) return b < 0.03f;
        int mv = 0, dr = 0; akvr_hudsplit_ui_now(mv, dr);
        return dr < 10;
    }
    volatile LONG  g_notes = 0;               // timeline lines written (max 10)
    char g_pending[2][64] = {}; int g_pendN = 0;   // notes for the timeline, written from the Present thread (under g_lock)
    void note(const char* t)
    {
        if (g_notes >= 10 || g_pendN >= 2) return;
        ++g_notes;
        strncpy_s(g_pending[g_pendN++], t, _TRUNCATE);
    }
    CRITICAL_SECTION g_lock;
    bool g_lockReady = false;
    char g_diag[160] = "";

    void engine_start_locked()
    {
        void* xa = g_xa;
        if (g_stopped && xa)
        {
            __try { ((StartFn)(*(void***)xa)[11])(xa); } __except (EXCEPTION_EXECUTE_HANDLER) { g_xa = nullptr; }
            const ULONGLONG held = GetTickCount64() - g_stoppedAt;
            InterlockedExchange(&g_holdMs, g_holdMs + (LONG)held);
            int mv = 0, dr = 0; akvr_hudsplit_ui_now(mv, dr);
            char n[64]; snprintf(n, sizeof(n), "sound on after %llu ms (ui %d/%d, picture %.3f)", (unsigned long long)held, mv, dr,
                                 akvr_xr_startup_bright());
            note(n);
        }
        if (akvr_xr_startup_3d()) InterlockedExchange(&g_holdDone, 1);   // AUDIOSYNC3: the menu-scene hold happens once
        InterlockedExchange(&g_stopped, 0);
    }
    void engine_stop_locked()
    {
        void* xa = g_xa;
        if (g_stopped || !xa) return;
        __try { ((StopFn)(*(void***)xa)[12])(xa); } __except (EXCEPTION_EXECUTE_HANDLER) { g_xa = nullptr; return; }
        g_stoppedAt = GetTickCount64();
        InterlockedExchange(&g_stopped, 1);
        InterlockedIncrement(&g_holds);
    }

    HRESULT WINAPI CoCreateHook(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out)
    {
        const HRESULT hr = g_origCoCreate(clsid, outer, ctx, iid, out);
        if (SUCCEEDED(hr) && out && *out && (IsEqualGUID(clsid, kClsXA27) || IsEqualGUID(clsid, kClsXA27D)) &&
            IsEqualGUID(iid, kIidXA27))
        {
            g_xa = *out;
            InterlockedIncrement(&g_created);
        }
        return hr;
    }

    DWORD WINAPI watcher(LPVOID)
    {
        for (;;)
        {
            Sleep(5);
            if (!g_lockReady) continue;
            const ULONGLONG last = g_lastFrame;
            const ULONGLONG now = GetTickCount64();
            EnterCriticalSection(&g_lock);
            if (g_stopped)
            {
                // never longer than 6 s, and never past the start-up
                if (now - g_stoppedAt > 6000 || !g_on || akvr_xr_menu_phase() != 0) engine_start_locked();
            }
            // AUDIOSYNC2: 50 ms (JJ: "the music starts briefly just before it pauses" - the 150 ms wait let it through)
            else if (g_on && g_xa && last && now - last > 50 && akvr_xr_menu_phase() == 0)
            {
                engine_stop_locked();
                if (g_stopped) note("sound held");
            }
            LeaveCriticalSection(&g_lock);
        }
    }
}

// From akvr_early_init (DllMain): before Wwise makes its audio engine.
void akvr_audiosync_install()
{
    static bool done = false;
    if (done) return;
    done = true;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
    MH_Initialize();   // idempotent
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"combase", "CoCreateInstance", (LPVOID)&CoCreateHook, (LPVOID*)&g_origCoCreate, &target) != MH_OK &&
        MH_CreateHookApiEx(L"ole32", "CoCreateInstance", (LPVOID)&CoCreateHook, (LPVOID*)&g_origCoCreate, &target) != MH_OK)
    {
        snprintf(g_diag, sizeof(g_diag), "start-up sound hold: not installed (no CoCreateInstance)");
        return;
    }
    MH_EnableHook(target);
    HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// Every Present: a frame came, the sound goes on.
void akvr_audiosync_frame()
{
    g_lastFrame = GetTickCount64();
    if (g_pendN && g_lockReady)   // AUDIOSYNC3: the timeline is written on this thread only
    {
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_pendN; ++i) akvr_xr_mode_note(g_pending[i]);
        g_pendN = 0;
        LeaveCriticalSection(&g_lock);
    }
    // AUDIOSYNC3: the 3D menu scene has begun but the menu itself is not drawn yet: hold (at most 6 s from the hold)
    if (!g_stopped && g_lockReady && g_on && g_xa && !g_holdDone && menu_not_drawn())
    {
        EnterCriticalSection(&g_lock);
        engine_stop_locked();
        if (g_stopped) note("sound held (menu scene, no menu yet)");
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (g_stopped && g_lockReady)
    {
        if (menu_not_drawn()) return;   // AUDIOSYNC3: the menu scene is up but the menu is not drawn yet
        EnterCriticalSection(&g_lock);
        engine_start_locked();
        LeaveCriticalSection(&g_lock);
    }
}

void akvr_audiosync_shutdown()
{
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    engine_start_locked();
    LeaveCriticalSection(&g_lock);
}

bool akvr_audiosync() { return g_on; }
void akvr_audiosync_set(bool on) { g_on = on; }
const char* akvr_audiosync_diag()
{
    snprintf(g_diag, sizeof(g_diag), "start-up sound hold: %s, game audio engine %s, held %ld time(s), %ld ms in all%s",
             g_on ? "ON" : "off", g_xa ? "found (XAudio2 2.7)" : (g_created ? "lost" : "not seen"),
             g_holds, g_holdMs, g_stopped ? " (holding now)" : "");
    return g_diag;
}
