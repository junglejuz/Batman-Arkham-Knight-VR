// FLAREOFF 2026-10-10 - Arkham Knight's lens flare + lens dirt, switched off the way Luma's AK mod does it.
// The two shaders below are copied unchanged from Luma Framework (Filippo Tarpini, Garamond; Custom MIT licence:
// reuse with the authors' names; references/luma-framework-2026-10/Shaders/Batman Arkham Knight/). Compiled by AKN with
// DISABLE_LENS_FLARE_AND_LENS_DIRT=1 and bound in place of the game's own (Luma names = CRC32 of the game's bytecode).
// Generated from those files - do not edit by hand.
#pragma once
static const char kFlarePsHlsl[] = R"AKNHLSL(Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb1 : register(b1)
{
  float4 cb1[21];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[28];
}

// 3Dmigoto declarations
#define cmp -

#ifndef DISABLE_LENS_FLARE_AND_LENS_DIRT
#define DISABLE_LENS_FLARE_AND_LENS_DIRT 0
#endif

void main(
  float4 v0 : TEXCOORD0,
  float4 v1 : TEXCOORD1,
  float4 v2 : TEXCOORD2,
  float4 v3 : TEXCOORD5,
  float4 v4 : TEXCOORD6,
  float4 v5 : TEXCOORD8,
  out float4 o0 : SV_TARGET0,
  out float4 o1 : SV_TARGET1)
{
  float4 r0,r1,r2;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = t1.SampleBias(s1_s, v0.xy, 0).x;
  r0.xyz = v4.xyz * r0.xxx;
  r0.xyz = v4.www * r0.xyz;
  r1.xy = v3.xy / v3.ww;
  r1.xy = r1.xy * cb1[19].xy + cb1[19].wz;
  r2.xyz = t0.SampleBias(s0_s, r1.xy, 0).xyz;
  r2.xyz = r2.xyz * float3(0.5,0.5,0.5) + float3(0.5,0.5,0.5);
  r0.xyz = r2.xyz * r0.xyz;
  r0.xyz = r0.xyz * v4.xyz + cb0[23].xyz;
  r1.z = 1 + -v5.w;
  r0.w = 1;
  r0.xyzw = r1.zzzz * r0.xyzw;
  r1.z = saturate(v4.w);
  r0.xyzw = r1.zzzz * r0.xyzw;
  t2.GetDimensions(0, uiDest.x, uiDest.y, uiDest.z);
  r1.zw = uiDest.xy;
  r1.zw = (uint2)r1.zw;
  r1.xy = r1.xy * r1.zw;
  r1.xy = (uint2)r1.xy;
  r1.zw = float2(0,0);
  r1.x = t2.Load(r1.xyz).x;
  r1.x = min(0.999999881, r1.x);
  r1.x = r1.x * cb1[20].z + -cb1[20].w;
  r1.x = 1 / r1.x;
  r1.x = cmp(v3.z < r1.x);
  r1.x = r1.x ? 1.000000 : 0;
  r1.y = -cb0[27].x * r1.x + 1;
  r1.x = cb0[27].x * r1.x;
  o1.xyzw = r1.xxxx * r0.xyzw; // What is this used for?

  #if DISABLE_LENS_FLARE_AND_LENS_DIRT
  o0 = 0.0;
  #else
  o0.xyzw = r1.yyyy * r0.xyzw;
  #endif

  return;
})AKNHLSL";
static const char kBloomCsHlsl[] = R"AKNHLSL(Texture2D<float4> t3 : register(t3);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
RWTexture2D<float4> u0 : register(u0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0)
{
  float4 cb0[10];
}

// 3Dmigoto declarations
#define cmp -

#ifndef DISABLE_LENS_FLARE_AND_LENS_DIRT
#define DISABLE_LENS_FLARE_AND_LENS_DIRT 0
#endif

[numthreads(16, 16, 1)]
void main(uint3 vThreadID : SV_DispatchThreadID)
{
  const float4 icb[] = { { 0.200000, 0, 0, 0},
                              { 0.420000, 0, 0, 0},
                              { 0.520000, 0, 0, 0},
                              { 0.580000, 0, 0, 0},
                              { 0.810000, 0, 0, 0},
                              { 0.900000, 0, 0, 0} };
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = (int2)vThreadID.xy;
  r0.xy = r0.xy / cb0[6].xy;
  r0.zw = float2(1,1) / cb0[6].xy;
  r0.xy = r0.zw * float2(0.5,0.5) + r0.xy;
  r1.xz = float2(0,0);
  r1.yw = float2(0.5,2.5) * cb0[7].yy;
  r2.xy = r1.zy + r0.xy;
  r2.xyz = t0.SampleLevel(s0_s, r2.xy, 0).xyz;
  r3.xz = float2(0,0);
  r3.yw = float2(2.5,6.5) * cb0[7].yy;
  r3.xyzw = r3.xyzw + r0.xyxy;
  r4.xyz = t0.SampleLevel(s0_s, r3.xy, 0).xyz;
  r4.xyzw = float4(0.129032254,0.129032254,0.129032254,0.129032254) * r4.xyzx;
  r2.xyzw = r2.xyzx * float4(0.309677422,0.309677422,0.309677422,0.309677422) + r4.xyzw;
  r4.xz = float2(0,0);
  r4.yw = float2(4.5,6.5) * cb0[7].yy;
  r3.xy = r4.zy + r0.xy;
  r5.xyz = t0.SampleLevel(s0_s, r3.xy, 0).xyz;
  r2.xyzw = r5.xyzx * float4(0.0580645166,0.0580645166,0.0580645166,0.0580645166) + r2.xyzw;
  r3.xyz = t0.SampleLevel(s0_s, r3.zw, 0).xyz;
  r2.xyzw = r3.xyzx * float4(0.0032258064,0.0032258064,0.0032258064,0.0032258064) + r2.xyzw;
  r1.xyzw = -r1.xyzw + r0.xyxy;
  r3.xyz = t0.SampleLevel(s0_s, r1.xy, 0).xyz;
  r2.xyzw = r3.xyzx * float4(0.309677422,0.309677422,0.309677422,0.309677422) + r2.xyzw;
  r1.xyz = t0.SampleLevel(s0_s, r1.zw, 0).xyz;
  r1.xyzw = r1.xyzx * float4(0.129032254,0.129032254,0.129032254,0.129032254) + r2.xyzw;
  r2.xyzw = -r4.xyzw + r0.xyxy;
  r3.xyz = t0.SampleLevel(s0_s, r2.xy, 0).xyz;
  r1.xyzw = r3.xyzx * float4(0.0580645166,0.0580645166,0.0580645166,0.0580645166) + r1.xyzw;
  r2.xyz = t0.SampleLevel(s0_s, r2.zw, 0).xyz;
  r1.xyzw = r2.xyzx * float4(0.0032258064,0.0032258064,0.0032258064,0.0032258064) + r1.xyzw;
  r2.xyzw = float4(1,1,1,1) + -r0.xyxy;
  r3.xy = -cb0[9].yz * float2(0.5,0.5) + r0.xy;
  r3.z = dot(r3.xy, r3.xy);
  r3.z = rsqrt(r3.z);
  r3.xy = r3.xy * r3.zz;
  r3.zw = float2(0.0700000003,0.0700000003) * r3.xy;
  r4.xy = r3.xy * float2(0.48999998,0.48999998) + r2.zw;
  r5.x = t0.SampleLevel(s0_s, r4.xy, 0).x;
  r4.zw = r3.xy * float2(0.0139999995,0.0139999995) + r4.xy;
  r5.y = t0.SampleLevel(s0_s, r4.zw, 0).y;
  r4.xy = r3.xy * float2(0.027999999,0.027999999) + r4.xy;
  r5.z = t0.SampleLevel(s0_s, r4.xy, 0).z;
  r4.xy = -r3.xy * float2(0.200000003,0.200000003) + r0.xy;
  r6.x = t0.SampleLevel(s0_s, r4.xy, 0).x;
  r4.zw = r3.xy * float2(0.0160000008,0.0160000008) + r4.xy;
  r6.y = t0.SampleLevel(s0_s, r4.zw, 0).y;
  r3.xy = r3.xy * float2(0.0320000015,0.0320000015) + r4.xy;
  r6.z = t0.SampleLevel(s0_s, r3.xy, 0).z;
  r4.xyz = r6.xyz + r5.xyz;
  r2.xyzw = r2.xyzw + -r0.xyxy;
  r5.xyz = r4.xyz;
  r3.x = 0;
  while (true) {
    r3.y = cmp((int)r3.x >= 6);
    if (r3.y != 0) break;
    r6.xyzw = icb[r3.x+0].xxxx * r2.xyzw + r0.xyxy;
    r3.y = icb[r3.x+0].x * 2 + -1;
    r6.xyzw = r3.zwzw * r3.yyyy + r6.xyzw;
    r7.xyzw = r6.zwzw + -r0.xyxy;
    r8.x = t0.SampleLevel(s0_s, r6.zw, 0).x;
    r6.xyzw = r7.xyzw * float4(0.0199999996,0.0199999996,0.0399999991,0.0399999991) + r6.xyzw;
    r8.y = t0.SampleLevel(s0_s, r6.xy, 0).y;
    r8.z = t0.SampleLevel(s0_s, r6.zw, 0).z;
    r5.xyz = r8.xyz + r5.xyz;
    r3.x = (int)r3.x + 1;
  }
  r2.xyzw = r5.xyzx;

  #if DISABLE_LENS_FLARE_AND_LENS_DIRT
  r2.xyzw = 0.0;
  r3.xyz = 0.0;
  #else
  r2.xyzw = float4(0.166666672,0.166666672,0.166666672,0.166666672) * r2.xyzw;
  r3.xyz = t3.SampleLevel(s0_s, r0.xy, 0).xyz; // t3 is some kind lens dirt.
  #endif

  r3.xyzw = r3.xyzx * float4(8,8,8,8) + float4(0.75,0.75,0.75,0.75);
  r2.xyzw = r3.xyzw * r2.xyzw;
  r2.xyzw = cb0[9].xxxx * r2.xyzw;
  r1.xyzw = r1.xyzw * float4(2.75,2.75,2.75,2.75) + r2.xyzw;
  r2.xyz = t1.SampleLevel(s0_s, r0.xy, 0).xyz;
  r1.xyzw = r1.xyzw * cb0[8].xxxx + r2.xyzx;
  r2.xy = -r0.zw * float2(1,0) + r0.xy;
  r2.xyz = t1.SampleLevel(s0_s, r2.xy, 0).xyz;
  r2.x = dot(r2.xyz, float3(1,1,1));
  r2.yz = r0.zw * float2(1,0) + r0.xy;
  r2.yzw = t1.SampleLevel(s0_s, r2.yz, 0).xyz;
  r2.y = dot(r2.yzw, float3(1,1,1));
  r2.zw = -r0.zw * float2(0,1) + r0.xy;
  r3.xyz = t1.SampleLevel(s0_s, r2.zw, 0).xyz;
  r2.z = dot(r3.xyz, float3(1,1,1));
  r0.zw = r0.zw * float2(0,1) + r0.xy;
  r3.xyz = t1.SampleLevel(s0_s, r0.zw, 0).xyz;
  r0.z = dot(r3.xyz, float3(1,1,1));
  r3.x = r2.x + -r2.y;
  r3.y = r2.z + -r0.z;
  r3.z = 0.0500000007;
  r0.z = dot(r3.xyz, r3.xyz);
  r0.z = rsqrt(r0.z);
  r0.zw = r3.xy * r0.zz;
  r0.zw = float2(0.125,0.125) * r0.zw;
  r2.x = -r0.z * r0.z + 1;
  r2.x = r0.w * r0.w + r2.x;
  r2.x = 3 * r2.x;
  r2.y = saturate(dot(r1.wyz, float3(0.333000004,0.333000004,0.333000004)));
  r0.xy = r0.xy * float2(3,3) + r0.zw;
  r0.xyz = t3.SampleLevel(s1_s, r0.xy, r2.x).xyz;
  r0.xyzw = r0.xyzx * r0.xyzx;
  r0.xyzw = r0.xyzw * r2.yyyy;
  r0.xyzw = r0.xyzw * float4(10,10,10,10) + float4(1,1,1,1);
  r2.xy = vThreadID.xy;
  r2.zw = float2(0,0);
  r2.xyz = t2.Load(r2.xyz).xyz;
  r0.xyzw = r1.xyzw * r0.xyzw + r2.xyzx;
  u0[vThreadID.xy] = r0.xyzw;
  return;
})AKNHLSL";
