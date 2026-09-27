#include "openvr_beacon.h"
#include <openvr.h>
#include <cstdio>

namespace
{
    vr::IVRSystem* g_sys = nullptr;
    char           g_status[160] = "openvr beacon: not started";
}

void akvr_ovr_beacon_try_init()
{
    if (g_sys) return;   // already connected — never re-init or shut down

    vr::EVRInitError err = vr::VRInitError_None;
    // Overlay app type mirrors what the real Katanga viewer / vrscreencap register
    // as — the closest mimic of the consumer geo-11's katanga_vr path expects.
    g_sys = vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (g_sys && err == vr::VRInitError_None)
    {
        snprintf(g_status, sizeof(g_status), "openvr beacon: connected (overlay app)");
    }
    else
    {
        g_sys = nullptr;
        snprintf(g_status, sizeof(g_status), "openvr beacon: %s",
                 vr::VR_GetVRInitErrorAsEnglishDescription(err));
    }
}

const char* akvr_ovr_beacon_status() { return g_status; }
