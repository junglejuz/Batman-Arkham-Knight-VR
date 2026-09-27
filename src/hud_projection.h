#pragma once
#include <cmath>

// Ratio of horizontal/vertical tangent distance per source pixel. Multiplying
// Scaleform's pixel aspect by this keeps authored circles circular in the lens.
inline float akvr_hud_pixel_aspect(int width, int height, float halfH, float halfV)
{
    if (width <= 0 || height <= 0 || !std::isfinite(halfH) || !std::isfinite(halfV)
        || halfH <= 0.01f || halfV <= 0.01f || halfH >= 1.55f || halfV >= 1.55f)
        return 1.0f;
    const float ratio = std::tan(halfH) * height / (std::tan(halfV) * width);
    return std::isfinite(ratio) && ratio >= 0.1f && ratio <= 10.0f ? ratio : 1.0f;
}

// GetViewport can return our previous override. Preserve the authored value
// rather than multiplying the correction into itself on each call.
struct AkvrHudAspectState
{
    float original = 1.0f;
    float lastApplied = 1.0f;
    bool active = false;

    float apply(float incoming, float correction)
    {
        if (!std::isfinite(incoming) || incoming <= 0.0f) return incoming;
        if (!active || std::fabs(incoming - lastApplied) > 0.00001f)
            original = incoming;
        lastApplied = original * correction;
        active = std::fabs(correction - 1.0f) > 0.00001f;
        return lastApplied;
    }
};
