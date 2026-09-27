#pragma once
// HUDPROBE 2026-09-26 — pass-through watcher on the HUD movie-view vtable functions.
struct ID3D11DeviceContext;

void        akvr_hudprobe_tick(ID3D11DeviceContext* ctx);   // every Present (render thread)
void        akvr_hudprobe_dump(const wchar_t* path);        // F2: the call ring as CSV
const char* akvr_hudprobe_diag();
