// AKVR — entry point
// DllMain must stay minimal (loader lock): we only spawn the init
// thread; all real work happens in hooks.cpp once D3D11 is alive.
//
// ONE exception, and it has to be here: the render-size spoof (earlyres.cpp).
// Arkham Knight sizes its render from the desktop work area during startup and
// never revisits it, so the spoof must already be in place before the game's
// first instruction. The init thread below cannot promise that — it races the
// game's main thread and has been losing. earlyres does no allocation, no
// LoadLibrary and no thread enumeration; it only rewrites entries in the
// already-mapped import table, which is safe under the loader lock.
#include <windows.h>

void akvr_early_init(HINSTANCE self);
void akvr_start();
void akvr_stop();
bool akvr_vr_mode_decide(HINSTANCE self);   // vrmode.cpp: VR launcher note / -akvr, and the per-mode game settings

static bool g_vrRun = false;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        // VRLAUNCH 2026-10-02 — JJ: started from Steam the game plays normally in 2D; only the VR launcher starts VR.
        // In 2D nothing of AKVR runs (the dinput8 exports still pass through to Windows' own).
        g_vrRun = akvr_vr_mode_decide(instance);
        if (!g_vrRun) break;
        akvr_early_init(instance);   // MUST be before the game runs — see above
        akvr_start();
        break;
    case DLL_PROCESS_DETACH:
        if (g_vrRun) akvr_stop();
        break;
    }
    return TRUE;
}
