// GLOWFIRST 2026-10-07 - JJ: "I think the glow should go before any of it in terms of the sequence of being applied".
// The game adds its bloom to the full-range scene before exposure, film curve and colour table (CS 679d7ed1). These
// passes build AKVR's glow from the same full-range scene the DoF-off final pass reads (hudsplit.cpp tone_pre), so
// tonefix.hlsl can add it before the curve: bright parts (soft threshold on the curve-mapped brightness) at half size,
// shrunk 7 more times, then added back up level by level (radius = how much the wider levels add). Same dual-filter
// mip chain as MODGLOW in eyeaa.hlsl, in full-range float.
cbuffer Game : register(b0) { float4 g_c[9]; };
cbuffer AKVR : register(b13)
{
    float4 g_tone;    // x exposure, y 1/hable(white), z curve on, w table strength (tonefix.hlsl)
    float4 g_tone2;   // x edge darkening, y table size
    float4 g_glow;    // x intensity x balance, y threshold (display brightness), z knee, w radius
    float4 g_src;     // xy = 1/w, 1/h of the level a pass reads
};
Texture2D sceneTex : register(t0);
Texture2D srcTex   : register(t1);
SamplerState smp   : register(s7);

float3 hable(float3 x) { return (x * (0.22 * x + 0.03) + 0.002) / (x * (0.22 * x + 0.3) + 0.06) - 0.0333333; }

void VS_Full(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PS_HPre(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float2 h = g_src.xy;   // one full-size pixel
    float3 a = 0.25 * (sceneTex.SampleLevel(smp, uv + float2(-h.x, -h.y), 0).rgb + sceneTex.SampleLevel(smp, uv + float2(h.x, -h.y), 0).rgb +
                       sceneTex.SampleLevel(smp, uv + float2(-h.x, h.y), 0).rgb + sceneTex.SampleLevel(smp, uv + float2(h.x, h.y), 0).rgb);
    a = max(a * g_c[6].xyz, 0.0);
    const float3 m = hable(a * g_tone.x) * g_tone.y;              // how bright it will look after the curve
    const float br = max(m.r, max(m.g, m.b));
    const float t = g_glow.y;
    const float k = max(t * g_glow.z, 1e-4);
    float soft = clamp(br - t + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k);
    const float w = max(soft, br - t) / max(br, 1e-4);
    return float4(a * w, 1);
}
float4 PS_HDown(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float2 h = g_src.xy * 0.5;
    float3 s = srcTex.SampleLevel(smp, uv, 0).rgb * 4.0;
    s += srcTex.SampleLevel(smp, uv - h, 0).rgb;
    s += srcTex.SampleLevel(smp, uv + h, 0).rgb;
    s += srcTex.SampleLevel(smp, uv + float2(h.x, -h.y), 0).rgb;
    s += srcTex.SampleLevel(smp, uv + float2(-h.x, h.y), 0).rgb;
    return float4(s * 0.125, 1);
}
float4 PS_HUp(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float2 h = g_src.xy * 0.5;
    float3 s = srcTex.SampleLevel(smp, uv + float2(-h.x * 2.0, 0), 0).rgb;
    s += srcTex.SampleLevel(smp, uv + float2(-h.x, h.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(smp, uv + float2(0, h.y * 2.0), 0).rgb;
    s += srcTex.SampleLevel(smp, uv + float2(h.x, h.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(smp, uv + float2(h.x * 2.0, 0), 0).rgb;
    s += srcTex.SampleLevel(smp, uv + float2(h.x, -h.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(smp, uv + float2(0, -h.y * 2.0), 0).rgb;
    s += srcTex.SampleLevel(smp, uv + float2(-h.x, -h.y), 0).rgb * 2.0;
    return float4(s * (g_glow.w / 12.0), 1);
}
