#pragma once
// HUDSPLIT 2026-09-28 — step 1 of the HUD on its own headset layer (VR_IMPROVEMENT_BACKLOG item 6).
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

void        akvr_hudsplit_install(ID3D11DeviceContext* ctx);   // once the real context is known
void        akvr_hudsplit_tick();                              // every Present (render thread)
bool        akvr_hudsplit_active();                            // inside the HUD draw, this thread
void        akvr_hudsplit_note_bind(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* const* rtvs, unsigned n);
void        akvr_hudsplit_hide_set(bool on);                   // test: skip the HUD draw entirely
bool        akvr_hudsplit_hide();
const char* akvr_hudsplit_diag();
// HUDLAYER 2026-09-28: the HUD drawn into our own image, shown by the headset as a head-locked layer.
struct ID3D11Texture2D;
void        akvr_hudsplit_layer_set(bool on);                 // panel / settings "hudlayer"
bool        akvr_hudsplit_layer();
void        akvr_hudsplit_layer_gate(bool gameplay);          // xr.cpp: steady gameplay this frame
bool        akvr_hudsplit_layer_live();                       // HUD went to the layer in the last frames
ID3D11Texture2D* akvr_hudsplit_layer_image(unsigned& w, unsigned& h, int& fmt);   // fresh this Present, or null
const char* akvr_hudsplit_layer_diag();
// HUDSPLIT: scene-depth pieces (reticle) stay in the 3D picture (needs the fix-patch script's HUDSPLIT edit).
void        akvr_hudsplit_split_set(bool on);
bool        akvr_hudsplit_split();
const char* akvr_hudsplit_split_diag();
void        akvr_hudsplit_band_set(float pct);   // RETFLAT band: top % of the view where HUD pieces stay flat
float       akvr_hudsplit_band();
void        akvr_hudsplit_bottom_set(float pct); // EDGEBAND: bottom % of the view where HUD pieces stay on the layer
float       akvr_hudsplit_bottom();
void        akvr_hudsplit_layer_shot(const wchar_t* path);   // LAYERSHOT: save the HUD layer image (next readback)
struct IUnknown;
void        akvr_hudsplit_watch_device(IUnknown* dev);   // VSID: recognise the 13 patched HUD vertex shaders
const char* akvr_hudsplit_vs_diag();
void        akvr_hudsplit_hook_game_device();   // VSID4: see the game-facing device at D3D11CreateDevice
void        akvr_hudsplit_squash_set(bool on);   // RETSQUASH: reticle keeps its size near the view edges
bool        akvr_hudsplit_squash();
void        akvr_hudsplit_dump(const wchar_t* path);           // F2: per-frame scope log as CSV
