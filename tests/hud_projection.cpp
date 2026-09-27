#include "../src/hud_projection.h"
#include <cstdio>
#include <limits>

int main()
{
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::fprintf(stderr, "%s\n", label); ++failed; }
    };
    auto near = [](float a, float b) { return std::fabs(a-b) < 0.0001f; };
    const float h = 42.35f * 3.14159265f / 180.0f;
    const float v = 45.0f * 3.14159265f / 180.0f;
    const float correction = akvr_hud_pixel_aspect(2560, 1440, h, v);
    // A 100x100 authored element: corrected vertical extent and horizontal
    // extent must subtend the same tangent distance after lens projection.
    check(near(100.0f * correction / 1440 * std::tan(v),
               100.0f / 2560 * std::tan(h)), "projected circle stays circular");
    check(near(akvr_hud_pixel_aspect(1024,1024,v,v),1), "square projection identity");
    check(near(akvr_hud_pixel_aspect(0,1440,h,v),1), "invalid dimensions");
    check(near(akvr_hud_pixel_aspect(2560,1440,std::numeric_limits<float>::quiet_NaN(),v),1), "invalid FOV");
    AkvrHudAspectState state;
    float aspect = state.apply(1.2f, correction);
    for (int i=0;i<1000;++i) aspect=state.apply(aspect,correction);
    check(near(aspect,1.2f*correction), "repeated Get/Set does not compound");
    check(near(state.apply(aspect,1),1.2f), "disabling restores authored aspect");
    aspect=state.apply(1.2f,correction);
    aspect=state.apply(0.9f,correction);
    check(near(state.apply(aspect,1),0.9f), "new authored aspect is retained");
    return failed ? 1 : 0;
}
