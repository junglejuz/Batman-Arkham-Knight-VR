// EYEAA 2026-10-06 - JJ: "can we turn off DOF while keeping AA?" With DepthOfField=False the game also skips its SMAA
// (SHCENSUS: the SMAA edge / blend-weight passes only run with DoF on). AKVR smooths each eye's finished picture itself,
// between geo-11's eye surface and the headset image (xr.cpp eye copy). Input = the eye picture as stored (sRGB-encoded
// bytes read through a UNORM view); the headset image is an sRGB render target, so the last pass outputs linear values.
//
// SMAA 1x: Jorge Jimenez et al., MIT licence (vendor/smaa/SMAA.hlsl, AreaTex.h, SearchTex.h), luma edge detection.
// "Soft" = AKVR's own FXAA-style edge blend (written from the published idea, not NVIDIA's FXAA code).
// Compiled at build time by fxc (tools/build_eyeaa_shaders.bat -> src/shaders/eyeaa_*.h).

cbuffer EyeAA : register(b0)
{
    float4 g_rt;      // 1/w, 1/h, w, h
    float4 g_grade;   // GRADING: lift, gamma, gain, 0 (neutral 0, 1, 1)
    float4 g_bloom;   // MODGLOW: intensity, threshold (display 0..1), knee, scatter (size)
    float4 g_src;     // MODGLOW: 1/w, 1/h of the pass's source level; z = glow balance (final pass)
    float4 g_lut;     // GAMEGRADE: strength (0 = off), table size n (n*n x n strip), 0, 0
    float4 g_post0;   // POSTFX: tone curve strength (0 = off), 0, exposure multiplier, edge darkening
    float4 g_post1;   // POSTFX: auto exposure strength (0 = off), ln(target), range (ln), adaptation rate
};

SamplerState LinearSampler : register(s0);   // linear, clamp
SamplerState PointSampler  : register(s1);   // point, clamp

#define SMAA_RT_METRICS g_rt
#define SMAA_PRESET_HIGH
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#include "../../vendor/smaa/SMAA.hlsl"

Texture2D colorTex  : register(t0);
Texture2D edgesTex  : register(t1);
Texture2D blendTex  : register(t2);
Texture2D areaTex   : register(t3);
Texture2D searchTex : register(t4);
Texture2D bloomTex  : register(t5);   // MODGLOW: the finished glow (half size)
Texture2D srcTex    : register(t6);   // MODGLOW: the level a down/up pass reads
Texture2D lutTex    : register(t7);   // GAMEGRADE: the game's colour table, copied while depth of field was on
Texture2D adaptTex  : register(t8);   // POSTFX: 1x1, the adapted ln(average brightness) shared by both eyes
Texture2D curveTex  : register(t9);   // CURVE2: 256x1 tone table on the display value

float3 to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 to_srgb(float3 c) { c = max(c, 0.0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
// GRADING 2026-10-06 - JJ: "a new drawer for grading ... gain, gamma, and lift, similar to a traditional editing suite".
// On the display-referred picture (0..1): lift moves the blacks (white stays put), gain scales, gamma bends the mids.
float3 grade(float3 c)
{
    c = saturate(c);
    c = c * (1.0 - g_grade.x) + g_grade.x;
    c = saturate(c * g_grade.z);
    return pow(c, 1.0 / max(g_grade.y, 0.01));
}

// GAMEGRADE 2026-10-06 - JJ: with depth of field off the picture is "decidedly flatter"; with it on "a bit brighter, a
// bit more blue, and just generally a bit more interesting". The game's last post step (CS 679d7ed1) ends with a colour
// table lookup (n*n x n strip, slices along blue), which the depth-of-field-off path never runs. Same addressing as the
// game's shader: u = (slice*n + r*(n-1) + 0.5) / (n*n), v = (g*(n-1) + 0.5) / n, blend of the two nearest slices.
float3 game_grade(float3 c)
{
    if (g_lut.x <= 0.0) return c;
    const float n = g_lut.y;
    c = saturate(c);
    const float b = c.b * (n - 1.0);
    const float s = min(floor(b), n - 2.0);
    const float f = b - s;
    const float2 uv = float2((s * n + c.r * (n - 1.0) + 0.5) / (n * n), (c.g * (n - 1.0) + 0.5) / n);
    const float3 a = lutTex.SampleLevel(LinearSampler, uv, 0).rgb;
    const float3 d = lutTex.SampleLevel(LinearSampler, uv + float2(1.0 / n, 0.0), 0).rgb;
    return lerp(c, saturate(lerp(a, d, f)), g_lut.x);
}

// POSTFX 2026-10-06 - JJ: "do the auto exposure, edge darkening and contrast curve" - the rest of the game's last post
// step (CS 679d7ed1), which the depth-of-field-off path skips, in the game's order on the display picture taken back
// to linear light: + glow (MODGLOW), x auto exposure (ours: the shared log-average brightness against a target, the
// game divides by its adapted brightness), x edge darkening (the game's own: 1 + a((max(1 - |p 0.769|^2, 0))^2 - 1),
// a = 0.3), film curve (the game's Hable curve, A .22 B .30 C .10 D .20 E .01 F .30, white 11.2, on
// exposure * (linear^contrast + 0.0005 haze); contrast 1.68 / exposure 20.34 fitted, with the edge darkening, to JJ's DoF
// on/off F2 pair 214017/214022 by brightness quantiles, rms 0.013 of display range),
// back to display, then the game's colour table (GAMEGRADE). JJ's lift / gamma / gain come after (grade()).
// CURVE2 2026-10-06 - JJ: the film curve "is clipping values, both brightnesses and colours, especially in the
// highlights"; more contrast "brings back the details in the highlights". The game's Hable curve expects HDR light (white
// = 11.2); on our finished 0..1 picture the fitted exposure pushed everything above ~70% past white. Replaced by a tone
// table on the display value (curveTex, 256 entries, monotone cubic through points read off JJ's on/off F2 pair:
// 0.08->0.062, 0.166->0.163, 0.277->0.36, 0.45->0.64, 0.7->0.85, 1->1; slope still 0.48 at white, no clipping).
// Strength 1 = matched, 0 = off, up to 1.5 = more. The glow is now added AFTER the curve so it cannot push into white.
float3 tone(float3 d)
{
    const float3 t = saturate(d) * (255.0 / 256.0) + 0.5 / 256.0;
    return float3(curveTex.SampleLevel(LinearSampler, float2(t.r, 0.5), 0).r,
                  curveTex.SampleLevel(LinearSampler, float2(t.g, 0.5), 0).r,
                  curveTex.SampleLevel(LinearSampler, float2(t.b, 0.5), 0).r);
}
float3 post(float3 c, float2 uv)
{
    if (g_bloom.x <= 0.0 && g_lut.x <= 0.0 && g_post0.x <= 0.0 && g_post0.w <= 0.0 && g_post1.x <= 0.0) return c;
    float3 l = to_linear(saturate(c)) * g_post0.z;   // exposure (stops, with the curve)
    if (g_post1.x > 0.0) l *= exp(clamp(g_post1.x * (g_post1.y - adaptTex.Load(int3(0, 0, 0)).r), -g_post1.z, g_post1.z));
    if (g_post0.w > 0.0)
    {
        const float2 p = (uv * 2.0 - 1.0) * 0.769231;
        const float v = max(1.0 - dot(p, p), 0.0);
        l *= 1.0 + g_post0.w * (v * v - 1.0);
    }
    float3 d = saturate(to_srgb(l));
    if (g_post0.x > 0.0) d = saturate(lerp(d, tone(d), g_post0.x));
    if (g_bloom.x > 0.0) d = to_srgb(to_linear(d) + bloomTex.SampleLevel(LinearSampler, uv, 0).rgb * (g_bloom.x * g_src.z));   // g_src.z: MODGLOW2 balance
    return game_grade(saturate(d));
}

void fullscreen(uint id, out float4 pos, out float2 uv)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// ---- SMAA pass 1: edges ----
void VS_Edge(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0, out float4 off[3] : TEXCOORD1)
{
    fullscreen(id, pos, uv);
    SMAAEdgeDetectionVS(uv, off);
}
float2 PS_Edge(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 off[3] : TEXCOORD1) : SV_Target
{
    return SMAALumaEdgeDetectionPS(uv, off, colorTex);
}

// ---- SMAA pass 2: blending weights ----
void VS_Blend(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0, out float2 pix : TEXCOORD1, out float4 off[3] : TEXCOORD2)
{
    fullscreen(id, pos, uv);
    SMAABlendingWeightCalculationVS(uv, pix, off);
}
float4 PS_Blend(float4 pos : SV_Position, float2 uv : TEXCOORD0, float2 pix : TEXCOORD1, float4 off[3] : TEXCOORD2) : SV_Target
{
    return SMAABlendingWeightCalculationPS(uv, pix, off, edgesTex, areaTex, searchTex, 0);
}

// ---- SMAA pass 3: neighbourhood blending -> headset image (linear out) ----
void VS_Neigh(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0, out float4 off : TEXCOORD1)
{
    fullscreen(id, pos, uv);
    SMAANeighborhoodBlendingVS(uv, off);
}
float4 PS_Neigh(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 off : TEXCOORD1) : SV_Target
{
    float4 c = SMAANeighborhoodBlendingPS(uv, off, colorTex, blendTex);
    return float4(to_linear(grade(post(c.rgb, uv))), 1);
}

// ---- plain copy (mode off is a raw copy in xr.cpp; this is for checks) ----
void VS_Full(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    fullscreen(id, pos, uv);
}

// ---- soft: AKVR's FXAA-style edge blend -> headset image (linear out) ----
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float4 PS_Soft(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float2 px = g_rt.xy;
    const float3 cM = colorTex.SampleLevel(PointSampler, uv, 0).rgb;
    const float lM = luma(cM);
    const float lN = luma(colorTex.SampleLevel(PointSampler, uv + float2(0, -px.y), 0).rgb);
    const float lS = luma(colorTex.SampleLevel(PointSampler, uv + float2(0,  px.y), 0).rgb);
    const float lW = luma(colorTex.SampleLevel(PointSampler, uv + float2(-px.x, 0), 0).rgb);
    const float lE = luma(colorTex.SampleLevel(PointSampler, uv + float2( px.x, 0), 0).rgb);
    const float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    const float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    const float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125))   // flat area: leave it
        return float4(to_linear(grade(post(cM, uv))), 1);
    const float lNW = luma(colorTex.SampleLevel(PointSampler, uv + float2(-px.x, -px.y), 0).rgb);
    const float lNE = luma(colorTex.SampleLevel(PointSampler, uv + float2( px.x, -px.y), 0).rgb);
    const float lSW = luma(colorTex.SampleLevel(PointSampler, uv + float2(-px.x,  px.y), 0).rgb);
    const float lSE = luma(colorTex.SampleLevel(PointSampler, uv + float2( px.x,  px.y), 0).rgb);
    // edge direction: across the strongest gradient
    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    const float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    const float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, -8.0, 8.0) * px;
    const float3 a = 0.5 * (colorTex.SampleLevel(LinearSampler, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
                            colorTex.SampleLevel(LinearSampler, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    const float3 b = a * 0.5 + 0.25 * (colorTex.SampleLevel(LinearSampler, uv + dir * -0.5, 0).rgb +
                                       colorTex.SampleLevel(LinearSampler, uv + dir *  0.5, 0).rgb);
    const float lb = luma(b);
    const float3 o = (lb < lMin || lb > lMax) ? a : b;
    return float4(to_linear(grade(post(o, uv))), 1);
}

// ---- grading only (edge smoothing off) -> headset image (linear out) ----
float4 PS_Copy(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(to_linear(grade(post(colorTex.SampleLevel(PointSampler, uv, 0).rgb, uv))), 1);
}

// ---- MODGLOW: bright parts at half size (13-ish taps: 4 bilinear = a 4x4 box), soft threshold, in linear light ----
float4 PS_GlowPre(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float2 h = g_src.xy;   // one full-size pixel
    float3 c = 0.25 * (to_linear(colorTex.SampleLevel(LinearSampler, uv + float2(-h.x, -h.y), 0).rgb) +
                       to_linear(colorTex.SampleLevel(LinearSampler, uv + float2( h.x, -h.y), 0).rgb) +
                       to_linear(colorTex.SampleLevel(LinearSampler, uv + float2(-h.x,  h.y), 0).rgb) +
                       to_linear(colorTex.SampleLevel(LinearSampler, uv + float2( h.x,  h.y), 0).rgb));
    const float br = max(c.r, max(c.g, c.b));
    const float t = to_linear(g_bloom.yyy).x;            // threshold, given as display brightness
    const float k = max(t * g_bloom.z, 1e-4);            // soft knee
    float soft = clamp(br - t + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k);
    const float w = max(soft, br - t) / max(br, 1e-4);
    return float4(c * w, 1);
}
// ---- MODGLOW: down (dual-filter) ----
float4 PS_GlowDown(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float2 h = g_src.xy * 0.5;
    float3 s = srcTex.SampleLevel(LinearSampler, uv, 0).rgb * 4.0;
    s += srcTex.SampleLevel(LinearSampler, uv - h, 0).rgb;
    s += srcTex.SampleLevel(LinearSampler, uv + h, 0).rgb;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(h.x, -h.y), 0).rgb;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(-h.x, h.y), 0).rgb;
    return float4(s * 0.125, 1);
}
// ---- MODGLOW: up (tent), added onto the next larger level, weighted by the scatter (size) ----
float4 PS_GlowUp(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float2 h = g_src.xy * 0.5;
    float3 s = srcTex.SampleLevel(LinearSampler, uv + float2(-h.x * 2.0, 0), 0).rgb;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(-h.x, h.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(0, h.y * 2.0), 0).rgb;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(h.x, h.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(h.x * 2.0, 0), 0).rgb;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(h.x, -h.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(0, -h.y * 2.0), 0).rgb;
    s += srcTex.SampleLevel(LinearSampler, uv + float2(-h.x, -h.y), 0).rgb * 2.0;
    return float4(s * (g_bloom.w / 12.0), 1);
}

// POSTFX auto exposure: ln(brightness) of the eye picture into a 256x256 target (mips average it to 1x1), then the
// shared 1x1 value eases toward it (g_post1.w = 1 - exp(-dt / time)).
float4 PS_LumLog(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float3 l = to_linear(saturate(colorTex.SampleLevel(LinearSampler, uv, 0).rgb));
    return float4(log(max(dot(l, float3(0.2126, 0.7152, 0.0722)), 1e-4)), 0, 0, 1);
}
float4 PS_Adapt(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float cur = srcTex.SampleLevel(PointSampler, float2(0.5, 0.5), 8).r;
    const float prev = adaptTex.Load(int3(0, 0, 0)).r;
    return float4(lerp(prev, cur, g_post1.w), 0, 0, 1);
}
