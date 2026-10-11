#pragma once
// AKN DLSS (stage D1) - see dlss.cpp and akvr_native/DLSS_PLAN.md
#include <d3d11.h>
int   akn_dlss_mode();                                   // akvr_settings.ini dlss= (0 off, 1 DLAA .. 5 performance)
float akn_dlss_scale();                                  // render size / headset size per side
void  akn_dlss_out_size(UINT& w, UINT& h);               // the headset eye size DLSS writes
void  akn_dlss_jitter(unsigned long long frame, float& jpx, float& jpy);   // this frame's sub-pixel offset (render pixels)
void  akn_dlss_note_mv(int eye, ID3D11Resource* r);      // the eye's motion vectors (velocity R16G16F)
void  akn_dlss_note_depth(int eye, ID3D11Resource* r);   // the eye's depth
ID3D11Texture2D* akn_dlss_eval(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* src, float jpx, float jpy, const float* V, const float* P, bool vpOk);   // full-size eye or null (V, P: this eye's view and lens, row-vector, P with the jitter)
void  akn_dlss_cmd(const char* what, int v);             // test switches: preset / depthinv / mvjit / mvsigny / mvmode
const char* akn_dlss_diag();
void  akn_dlss_reset();
void  akn_dlss_render_size(int eye, UINT& w, UINT& h);   // AFW: the eye's drawn area (DLSS input size; the depth buffer may be bigger)
ID3D11Resource* akn_dlss_depth(int eye);                 // AFW: this eye's depth (render size), noted in its render
ID3D11ShaderResourceView* akn_dlss_srv(ID3D11Resource* r);   // AFW: a shader view on a game texture (depth formats handled)

// AFW - alternate frame warping (afw.cpp, akvr_native/NOTES.md "AFW PLAN 2026-10-10")
bool  akn_afw_on();                                      // AFW chosen (akvr_settings.ini afw=, command "afw N")
bool  akn_afw_cmd(const char* line);                     // afw / afwmode / afwwin / afwlum / afwdbg / afwlog; true = handled
int   akn_afw_lum();                                     // 1 = copy the drawn eye's exposure into the other eye's view state
int   akn_afw_mode();                                    // 1 this frame, 2 previous frame, 3 combined
// the drawn eye's finished picture (headset size) -> the OTHER eye's picture, or null (then show the drawn eye in both).
// V, P: drawn eye's view + lens (P with the jitter jx, jy in render pixels); dV, dP: the other eye's view + lens, no jitter.
ID3D11Texture2D* akn_afw_warp(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* pic, const float* V, const float* P, float jx, float jy, const float* dV, const float* dP);
void  akn_afw_reset();                                   // forget both eyes' last pictures (mode switch, level change)
const char* akn_afw_diag();
