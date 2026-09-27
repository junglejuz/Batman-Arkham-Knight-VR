#pragma once
#include <stdint.h>

// Engine frame identity (FRAMEID): finds UE3's game- and render-thread frame
// counters at runtime so each presented picture can be paired with the exact head
// pose its camera was given. See frameid.cpp.

// Call once per Present (render thread), with the gameplay verdict and the camera
// finalize count at that moment. Runs the one-time discovery; cheap once done.
void        akvr_frameid_tick(bool gameplay, uint64_t finalizeCount);
// The camera-finalize index whose camera drew the picture being presented now.
bool        akvr_frameid_presented_finalize(uint64_t& finalizeIndex);
const char* akvr_frameid_diag();
