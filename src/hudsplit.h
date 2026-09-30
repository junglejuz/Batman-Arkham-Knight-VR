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
void        akvr_hudsplit_layer_gate(bool gameplay, bool menu = false);   // xr.cpp: steady gameplay / live main menu (MENUONE: no split)
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
void        akvr_hudsplit_layer_shot(const wchar_t* path);
void        akvr_hudsplit_marks_dump(const wchar_t* path);    // MARKREC: F2, what the HUD shaders get   // LAYERSHOT: save the HUD layer image (next readback)
struct IUnknown;
void        akvr_hudsplit_watch_device(IUnknown* dev);   // VSID: recognise the 13 patched HUD vertex shaders
// DRAWPROBE2: draw kinds (pixel + vertex shader pair) around the rain particles. tags: 1 the rain particles,
// 2 uses a rain texture, 4 drawn just before the rain, 8 drawn just after it
int         akvr_probe_kind_count();
bool        akvr_probe_kind(int i, unsigned long long& ps, unsigned long long& vs, int& tags, long& draws, unsigned& count,
                            unsigned& inst, bool& seenNow, bool& hide);
void        akvr_probe_kind_hide(int i, bool on);
// DRAWPROBE3: tag 16 = a see-through (blended) draw anywhere in the frame; hides by vertex-shader group, or all
bool        akvr_probe_group_hidden(unsigned long long vs);
void        akvr_probe_group_hide(unsigned long long vs, bool on);
bool        akvr_probe_all_but_rain();
void        akvr_probe_all_but_rain_set(bool on);
// DRAWPROBE4: every game draw off except the world rain (and the HUD); compute shaders listed and hideable;
// tag 64 = an indirect draw (the GPU supplies the count)
void        akvr_rainwriter_dump(struct _iobuf* f); // RAINWRITER: who writes the rain streak buffer (F2 status)
int         akvr_near_rain_lag();                  // NEARRAIN2: camera frames back (0 = the draw's own view)
void        akvr_near_rain_lag_set(int k);
int         akvr_near_rain_mode();                 // NEARRAIN: 0 as the game draws it, 1 hang in the room, 2 hidden
void        akvr_near_rain_mode_set(int m);
const char* akvr_near_rain_diag();
int         akvr_probe_rain_parts();               // RAINPARTS: draw only the first N streaks of the world rain (-1 all)
void        akvr_probe_rain_parts_set(int v);
bool        akvr_probe_every_draw();
void        akvr_probe_every_draw_set(bool on);
bool        akvr_probe_all_cs();
void        akvr_probe_all_cs_set(bool on);
int         akvr_probe_cs_count();
bool        akvr_probe_cs(int i, unsigned long long& h, long& perFrame, bool& seenNow, bool& hide);
void        akvr_probe_cs_hide(int i, bool on);
const char* akvr_probe_diag();
const char* akvr_hudsplit_vs_diag();
void        akvr_hudsplit_hook_game_device();   // VSID4: see the game-facing device at D3D11CreateDevice
void        akvr_hudsplit_squash_set(bool on);   // RETSQUASH: reticle keeps its size near the view edges
bool        akvr_hudsplit_squash();
void        akvr_hudsplit_dump(const wchar_t* path);           // F2: per-frame scope log as CSV
