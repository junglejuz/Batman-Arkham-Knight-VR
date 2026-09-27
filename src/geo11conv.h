#pragma once
// GEOSCALE 2026-09-26 — the panel's world scale drives geo-11's convergence.
struct IUnknown;

void        akvr_geo11conv_tick(IUnknown* device);   // every Present; lazy init, writes on change
float       akvr_geo11conv_scale();                  // 1.00 = the convergence geo-11 started with
void        akvr_geo11conv_scale_set(float s);
bool        akvr_geo11conv_ready();                  // d3dxdm.ini dm_convergence found
bool        akvr_geo11conv_live();                   // GEOLIVE: writing geo-11's value directly
// GEOSEP 2026-09-27 — geo-11 separation (Ctrl+F3/F4), same live route; saved as dm_separation.
float       akvr_geo11sep_value();
void        akvr_geo11sep_set(float v);
bool        akvr_geo11sep_available();              // live, or at least in d3dxdm.ini
bool        akvr_geo11sep_live();
// EYEVIEW 2026-09-27: hold geo-11 at separation S / negative convergence (each eye's own view).
void        akvr_geo11_eyeview(bool on, float S);
bool        akvr_geo11_eyeview_applied();
const char* akvr_geo11_eyeview_diag();
// HUDDIST 2026-09-27: HUD distance in metres (0 = far away), live through geo-11's HUD depth.
void        akvr_geo11_hud_dist_set(float m);
float       akvr_geo11_hud_dist();
bool        akvr_geo11_hud_dist_ok();
float       akvr_geo11_hud_depth_now();
// HUDLIVE 2026-09-27: the HUD distance moves geo-11's HUD shift in its stereo object, found
// through the swapchain the game presents with (passed every Present, before the tick).
void        akvr_geo11_swapchain(void* sc);
const char* akvr_geo11_hud_diag();
const char* akvr_geo11conv_diag();
void        akvr_geo11conv_commit();                // write dm_convergence for the next launch
