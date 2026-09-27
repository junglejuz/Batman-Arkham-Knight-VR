// AKVR — OpenVR presence beacon (M5 native-stereo diagnostic)
// ------------------------------------------------------------------
// We display to the Quest 3 purely via OpenXR/VDXR — we never use OpenVR/SteamVR
// for actual rendering. But geo-11's katanga_vr shared-texture output (needed for
// native full-rate stereo) may only activate once it sees an OpenVR "VR consumer"
// registered, the way the real Katanga viewer / vrscreencap do (both are OpenVR
// overlay apps). This makes a minimal vr::VR_Init() call purely to be visible to
// geo-11 — nothing else in the mod depends on OpenVR, and if the Quest isn't
// registered with SteamVR at all this will simply fail to connect (harmless).
// ------------------------------------------------------------------
#pragma once

// Idempotent, retries lazily (SteamVR may not be running yet). Safe to call every
// frame — a no-op once it has either succeeded or SteamVR's runtime isn't present.
void        akvr_ovr_beacon_try_init();
const char* akvr_ovr_beacon_status();   // for the overlay
