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

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        akvr_early_init(instance);   // MUST be before the game runs — see above
        akvr_start();
        break;
    case DLL_PROCESS_DETACH:
        akvr_stop();
        break;
    }
    return TRUE;
}
