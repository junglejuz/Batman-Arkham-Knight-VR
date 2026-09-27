#pragma once
#include <d3d11.h>
#include <cstdint>

// Sparse, nonblocking GPU samples. No pixel writes to the game or XR image.
void akvr_present_probe_begin(ID3D11DeviceContext* context);
void akvr_present_probe_before(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* source, uint64_t cameraFrame, bool gameplay, bool afterPresent,
    double xrBeginMs, double xrWaitMs, int poseDelay = -1);
void akvr_present_probe_after(ID3D11DeviceContext* context, ID3D11Texture2D* source,
    double presentMs, double submitMs);
void akvr_present_probe_dump(const wchar_t* path);
void akvr_present_probe_dump_frames(const wchar_t* path);   // every-frame CPU timing
double akvr_probe_clock_ms();
