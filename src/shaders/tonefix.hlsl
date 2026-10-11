// HDRCURVE 2026-10-07 - JJ: with depth of field off the picture is "still clipping like it's low dynamic range compared
// to when the depth of field is turned on". The depth-of-field-off path ends in the game's PS c0af99686f49c503:
//     out = pow(saturate(lerp(scene * cb0[6], cb0[7].rgb, cb0[7].w)), cb0[8].x)
// - the HDR scene times a fixed scale, HARD CLIPPED at 1 (each channel), then gamma. AKVR draws this replacement in its
// place (hudsplit.cpp HDRCURVE).
// GAMELOOK 2026-10-07 - JJ: the after-the-fact stand-ins (curve, table, edge on the finished 8-bit picture) "clip the
// shit out of the highlights" and posterise. This pass now does the game's whole DoF-on look on the full-range scene,
// in the game's own order (CS 679d7ed1): edge darkening on the scene, Hable film curve (A .22 B .30 C .10 D .20 E .01
// F .30, white 11.2), gamma, then the game's colour table (copied while DoF was on), all in float before the output is
// quantised. Exposure 1.87 with the table / 2.51 without (fitted to JJ's F2 pair 214022 -> 214017).
// cb13 (AKVR): g_tone  x = exposure, y = 1 / hable(white), z = curve on (0 = the game's clip, unchanged), w = table strength
//              g_tone2 x = edge darkening (the game's 0.3), y = table size n (n*n x n strip)
cbuffer Game : register(b0) { float4 g_c[9]; };
cbuffer AKVR : register(b13) { float4 g_tone; float4 g_tone2; float4 g_glow; float4 g_src; };   // g_glow.x = glow amount (GLOWFIRST)
Texture2D sceneTex : register(t0);
Texture2D lutTex : register(t8);
Texture2D glowTex : register(t9);   // GLOWFIRST: the glow built from this same scene (toneglow.hlsl)
SamplerState lutSmp : register(s7);

float3 hable(float3 x) { return (x * (0.22 * x + 0.03) + 0.002) / (x * (0.22 * x + 0.3) + 0.06) - 0.0333333; }

float3 game_table(float3 c)
{
    const float n = g_tone2.y;
    c = saturate(c);
    const float b = c.b * (n - 1.0);
    const float s = min(floor(b), n - 2.0);
    const float f = b - s;
    const float2 uv = float2((s * n + c.r * (n - 1.0) + 0.5) / (n * n), (c.g * (n - 1.0) + 0.5) / n);
    const float3 a = lutTex.SampleLevel(lutSmp, uv, 0).rgb;
    const float3 d = lutTex.SampleLevel(lutSmp, uv + float2(1.0 / n, 0.0), 0).rgb;
    return lerp(c, saturate(lerp(a, d, f)), g_tone.w);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    uint w, h;
    sceneTex.GetDimensions(w, h);
    const float4 s = sceneTex.Load(int3(uint2(uv * float2(w, h)), 0));
    float3 a = s.rgb * g_c[6].xyz;
    if (g_tone.z > 0.0)
    {
        if (g_glow.x > 0.0) a += glowTex.SampleLevel(lutSmp, uv, 0).rgb * g_glow.x;   // GLOWFIRST: before everything, as the game
        if (g_tone2.x > 0.0)
        {
            const float2 p = (uv * 2.0 - 1.0) * 0.769231;
            const float v = max(1.0 - dot(p, p), 0.0);
            a *= 1.0 + g_tone2.x * (v * v - 1.0);
        }
        a = hable(max(a, 0.0) * g_tone.x) * g_tone.y;
    }
    float3 r = pow(saturate(lerp(a, g_c[7].xyz, g_c[7].w)), g_c[8].x);
    if (g_tone.z > 0.0 && g_tone.w > 0.0 && g_tone2.y >= 2.0) r = game_table(r);
    return float4(r, s.w);
}
