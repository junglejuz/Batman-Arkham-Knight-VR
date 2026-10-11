#pragma once
// AKN link (M5, 2026-10-07): AKVR in ENGINE STEREO mode. With -akn on the command line the game draws both eyes
// itself (akvr_native/, XINPUT9_1_0.dll in the game folder, active with -akn -aknstereo); geo-11 is not loaded and
// AKVR takes the eye pair and the HUD picture from that DLL. See akvr_native/NOTES.md "M5".
#include <d3d11.h>

bool akvr_engine_stereo();                     // decided once in DllMain from the command line (-akn)
void akvr_engine_stereo_detect();              // DllMain: kernel32 only
bool akn_link();                               // the AKN exports were found (retried until they are)
ID3D11Texture2D* akn_get_pair(long* seq);      // 2W x H, left half = left eye; null before the first 3D frame
ID3D11Texture2D* akn_get_hud(long* seq);       // window size, transparent where there is no HUD; null when off
void akn_send_eyes(const float tan8[8], const float off2[2], int asym);   // lens tangents l,r,u,d per eye; offsets in game units
void akn_send_present();                       // AKVR's Present: the frame and its HUD are complete
void akn_send_hud(int on);
void akn_send_report(long fresh, long flat);
void akn_send_game_device(ID3D11Device* dev);
unsigned long long akn_get_pair_stamp();          // POSESTAMP: camera-finalize count the newest pair was drawn with (0 = none)
void akn_send_pose_report(long matched, long total, long delay);   // the device behind the game's swap chain: AKN hooks only its table     // telemetry: frames shown from the pair vs the flat fallback
const char* akn_link_diag();
void akn_command(const char* text);   // PANELRAIN: the same words as akn\command.txt ("rainview 2", "rainpause 1", ...)
int akn_query(const char* name);      // PANELRAIN: "akn_rain_view" / "akn_far_rain" / "akn_rain_pause" (-1 = not available)
