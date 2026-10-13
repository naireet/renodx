// FH2 tonemap/composite (Azimuth 0.9.7 translation). Original resource names from DXBC reflection:
// t0 txWorldRender, t1 txCombinedBloom, t2 txLightRays, t3 txVelocity, t5 txLensFlare, t6 txWorldStencil,
// t7 sunOcclusion, t8/t9 txAlphaBuffer0/1, t10 txAlphaDepthBuffer, t11 txWorldDepth, t12 txColourGrading,
// cb1 ImageProcPostProcessConstBuffer (indexed by Azimuth's per-draw offset in cb13).
#define FH2_SETTINGS_IN_GAME_ROOT_CBV
#include "./common.hlsli"

// Azimuth per-draw root constants (b13 space0).
cbuffer cb13 : register(b13) {
  float4 cb13[1];
}

Texture3D<float4> t12 : register(t12);
Texture2D<float4> t11 : register(t11);
Texture2D<float4> t10 : register(t10);
Texture2D<float4> t9 : register(t9);
Texture2D<float4> t8 : register(t8);

struct t7_t {
  float val[3];
};
StructuredBuffer<t7_t> t7 : register(t7);

Texture2D<float4> t6 : register(t6);
Texture2D<float4> t5 : register(t5);
Texture2D<float4> t3 : register(t3);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);

SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb1 : register(b1) {
  float4 cb1[569];
}

#define cmp -

void main(
    float4 v0: SV_POSITION0,
    float4 v1: TexCoord0,
    float2 v2: TexCoord4,
    float4 v3: TexCoord1,
    float4 v4: TexCoord2,
    float4 v5: TexCoord3,
    out float4 o0: SV_TARGET0) {
  float4 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15;
  uint4 bitmask;
  r2 = 0;  // Only read after assignment; silences fxc's conservative uninitialized-use warning.

  r15.y = (asuint(cb13[0].x) >> 4) & 0xF;  // Azimuth bitfield: cb1 block offset for this draw.
  r0.x = cmp(asint(cb1[r15.y + 543].y) != 0);
  if (asint(cb1[r15.y + 553].x) != 0) {
    r1.xy = (int2)v0.xy;
    r1.zw = float2(0, 0);
    r0.y = t10.Load(r1.xyw).x;
    r0.y = 1 + -r0.y;
    r0.z = t6.Load(r1.xyz).x;
    r0.z = 255 * r0.z;
    r0.z = floor(r0.z);
    r0.w = cmp(r0.y == 1.000000);
    r0.z = cmp(r0.z == cb1[r15.y + 553].y);
    r0.z = r0.z ? r0.w : 0;
    r0.x = r0.z ? 0 : r0.x;
  } else {
    r0.y = 0;
  }
  r11.xzw = t0.Sample(s1_s, v1.xy).xyz;
  // Motion blur along txVelocity, stencil-masked.
  if (r0.x != 0) {
    r0.xz = t3.SampleLevel(s0_s, v1.xy, 0).xy;
    r1.xy = float2(1920, 1080) * r0.xz;
    r12.xy = r1.xy * r1.xy;
    r13.xy = min(abs(r1.xy), abs(r1.xy));
    r13.xy = cmp(r13.xy == float2(0, 0));
    r12.xy = r13.xy ? float2(0, 0) : r12.xy;
    r0.w = dot(r12.xy, float2(1, 1));
    r0.w = sqrt(r0.w);
    r12.xzw = cb1[r15.y + 542].zzz * r0.xzw;
    r13.xzw = min(abs(cb1[r15.y + 542].zzz), abs(r0.xzw));
    r13.xzw = cmp(r13.xzw == float3(0, 0, 0));
    r0.xzw = r13.xzw ? float3(0, 0, 0) : r12.xzw;
    r0.w = (uint)r0.w;
    r0.w = max(2, (uint)r0.w);
    r0.w = min(asuint(cb1[r15.y + 543].z), (uint)r0.w);
    r0.w = (uint)r0.w >> 1;
    r0.w = (int)r0.w + 1;
    bitmask.x = ((~(-1 << 31)) << 1) & 0xffffffff;
    r1.x = (((uint)r0.w << 1) & bitmask.x) | ((uint)1 & ~bitmask.x);
    r1.x = (uint)r1.x;
    r1.yzw = t0.SampleLevel(s1_s, v1.xy, 0).xyz;
    r2.x = t6.SampleLevel(s1_s, v1.xy, 0).x;
    r1.x = -1 + r1.x;
    r14.xz = rcp(r1.xx);
    r12.xz = r14.xz * r0.xz;
    r13.xz = min(abs(r14.xz), abs(r0.xz));
    r13.xz = cmp(r13.xz == float2(0, 0));
    r0.xz = r13.xz ? float2(0, 0) : r12.xz;
    r2.yz = v1.xy + r0.xz;
    r3.xy = v1.xy + -r0.xz;
    r1.x = t6.SampleLevel(s1_s, r2.yz, 0).x;
    r2.w = t6.SampleLevel(s1_s, r3.xy, 0).x;
    r4.xyz = t0.SampleLevel(s0_s, r2.yz, 0).xyz;
    r5.xyz = t0.SampleLevel(s0_s, r3.xy, 0).xyz;
    r0.w = max(1, (uint)r0.w);
    r0.w = (int)r0.w + -1;
    r1.x = cmp(r2.x == r1.x);
    r1.x = r1.x ? r2.x : -1;
    r2.w = cmp(r2.x == r2.w);
    r2.x = r2.w ? r2.x : -1;
    r6.xyz = r1.yzw;
    r7.xyz = r4.xyz;
    r8.xyz = r5.xyz;
    r3.zw = r2.yz;
    r9.xy = r3.xy;
    r10.x = r1.x;
    r10.y = r2.x;
    r2.w = 1;
    r4.w = r0.w;
    while (true) {
      r5.w = cmp(0 >= (int)r4.w);
      if (r5.w != 0) break;
      r4.w = (int)r4.w + -1;
      r3.zw = r3.zw + r0.xz;
      r9.xy = r9.xy + -r0.xz;
      r5.w = t6.SampleLevel(s1_s, r3.zw, 0).x;
      r6.w = t6.SampleLevel(s1_s, r9.xy, 0).x;
      r5.w = cmp(r10.x == r5.w);
      r6.w = cmp(r10.y == r6.w);
      r10.x = r5.w ? r10.x : -1;
      r10.y = r6.w ? r10.y : -1;
      if (r5.w != 0) {
        r6.xyz = r7.xyz + r6.xyz;
        r2.w = 1 + r2.w;
        r7.xyz = t0.SampleLevel(s0_s, r3.zw, 0).xyz;
      }
      if (r6.w != 0) {
        r6.xyz = r6.xyz + r8.xyz;
        r2.w = 1 + r2.w;
        r8.xyz = t0.SampleLevel(s0_s, r9.xy, 0).xyz;
      }
    }
    r14.xzw = rcp(r2.www);
    r12.xzw = r14.xzw * r6.xyz;
    r13.xzw = min(abs(r14.xzw), abs(r6.xyz));
    r13.xzw = cmp(r13.xzw == float3(0, 0, 0));
    r0.xzw = r13.xzw ? float3(0, 0, 0) : r12.xzw;
  } else {
    r0.xzw = r11.xzw;
  }
  // Transparent (alpha buffer) composite, with stencil edge handling.
  r1.x = cmp(asint(cb1[r15.y + 553].x) != 0);
  r1.y = cmp(r0.y != 1.000000);
  r1.x = r1.y ? r1.x : 0;
  if (r1.x != 0) {
    r1.xy = (int2)v0.xy;
    r1.zw = float2(0, 0);
    r2.x = t11.Load(r1.xyw).x;
    r0.y = 0.000500000024 + r0.y;
    r0.y = cmp(r0.y < r2.x);
    if (r0.y != 0) {
      r2.xyz = t8.Load(r1.xyw).xyz;
      r3.xyz = t9.Load(r1.xyw).xyz;
      r12.xzw = r3.xyz * r0.xzw;
      r13.xzw = min(abs(r3.xyz), abs(r0.xzw));
      r13.xzw = cmp(r13.xzw == float3(0, 0, 0));
      r12.xzw = r13.xzw ? float3(0, 0, 0) : r12.xzw;
      r0.xzw = r2.xyz + r12.xzw;
    } else {
      r0.y = t6.Load(r1.xyw).x;
      r0.y = 255 * r0.y;
      r0.y = floor(r0.y);
      r0.y = cmp(r0.y != cb1[r15.y + 553].y);
      if (r0.y != 0) {
        r2.xyz = t8.Load(r1.xyw).xyz;
        r3.xyz = t9.Load(r1.xyw).xyz;
        r12.xyz = r3.xyz * r0.xzw;
        r13.xyz = min(abs(r3.xyz), abs(r0.xzw));
        r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
        r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
        r2.xyz = r2.xyz + r12.xyz;
      }
      if (r0.y == 0) {
        r3.x = t6.Load(r1.xyw, int2(-1, 0)).x;
        r3.y = t6.Load(r1.xyw, int2(1, 0)).x;
        r3.z = t6.Load(r1.xyw, int2(0, 1)).x;
        r3.w = t6.Load(r1.xyw, int2(0, -1)).x;
        r3.xyzw = float4(255, 255, 255, 255) * r3.xyzw;
        r3.xyzw = floor(r3.xyzw);
        r3.xyzw = cmp(r3.xyzw != cb1[r15.y + 553].yyyy);
        r3.xy = (int2)r3.zw | (int2)r3.xy;
        r0.y = (int)r3.y | (int)r3.x;
        if (r0.y != 0) {
          r3.xyz = t8.Load(r1.xyw).xyz;
          r4.xyz = t9.Load(r1.xyw).xyz;
          r12.xyz = r4.xyz * r0.xzw;
          r13.xyz = min(abs(r4.xyz), abs(r0.xzw));
          r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
          r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
          r2.xyz = r3.xyz + r12.xyz;
        }
        if (r0.y == 0) {
          r3.x = t6.Load(r1.xyw, int2(-1, 1)).x;
          r3.y = t6.Load(r1.xyw, int2(1, 1)).x;
          r3.z = t6.Load(r1.xyw, int2(-1, -1)).x;
          r3.w = t6.Load(r1.xyw, int2(1, -1)).x;
          r3.xyzw = float4(255, 255, 255, 255) * r3.xyzw;
          r3.xyzw = floor(r3.xyzw);
          r3.xyzw = cmp(r3.xyzw != cb1[r15.y + 553].yyyy);
          r3.xy = (int2)r3.zw | (int2)r3.xy;
          r0.y = (int)r3.y | (int)r3.x;
          if (r0.y != 0) {
            r3.xyz = t8.Load(r1.xyw).xyz;
            r1.xyz = t9.Load(r1.xyz).xyz;
            r12.xyz = r1.xyz * r0.xzw;
            r13.xyz = min(abs(r1.xyz), abs(r0.xzw));
            r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
            r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
            r2.xyz = r3.xyz + r12.xyz;
          }
          r0.xzw = r0.yyy ? r2.xyz : r0.xzw;
        } else {
          r0.xzw = r2.xyz;
        }
      } else {
        r0.xzw = r2.xyz;
      }
    }
  }
  // Bloom.
  r1.xyz = t1.Sample(s0_s, v2.xy).xyz;
  r0.xyz = r1.xyz + r0.xzw;
  // Light rays, scaled by sun occlusion.
  r0.w = cmp(0 < cb1[r15.y + 28].w);
  if (r0.w != 0) {
    r12.xy = cb1[r15.y + 550].xy * v1.xy;
    r13.xy = min(abs(cb1[r15.y + 550].xy), abs(v1.xy));
    r13.xy = cmp(r13.xy == float2(0, 0));
    r12.xy = r13.xy ? float2(0, 0) : r12.xy;
    r1.xy = cb1[r15.y + 550].zw + r12.xy;
    r0.w = t2.Sample(s0_s, r1.xy).x;
    r1.x = t7[0].val[2];
    r1.x = -1 + r1.x;
    r12.x = r1.x * cb1[r15.y + 29].x;
    r13.x = min(abs(r1.x), abs(cb1[r15.y + 29].x));
    r13.x = cmp(r13.x == 0);
    r12.x = r13.x ? 0 : r12.x;
    r1.x = 1 + r12.x;
    r0.w = log2(abs(r0.w));
    r12.w = cb1[r15.y + 28].w * r0.w;
    r13.w = min(abs(cb1[r15.y + 28].w), abs(r0.w));
    r13.w = cmp(r13.w == 0);
    r0.w = r13.w ? 0 : r12.w;
    r0.w = exp2(r0.w);
    r12.yzw = cb1[r15.y + 28].xyz * r0.www;
    r13.yzw = min(abs(cb1[r15.y + 28].xyz), abs(r0.www));
    r13.yzw = cmp(r13.yzw == float3(0, 0, 0));
    r1.yzw = r13.yzw ? float3(0, 0, 0) : r12.yzw;
    r12.xyz = r1.xxx * r1.yzw;
    r13.xyz = min(abs(r1.xxx), abs(r1.yzw));
    r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
    r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
    r0.xyz = r0.xyz + r12.xyz;
  }
  // Lens flare.
  r0.w = cmp(0 < cb1[r15.y + 548].x);
  if (r0.w != 0) {
    r1.xyz = t5.Sample(s0_s, v1.xy).xyz;
    r0.xyz = r1.xyz + r0.xyz;
  }
  // Exposure (v3.w) and Hable filmic (FilmicParams1/FilmicParamsE, white scale 1 / v3.z),
  // enabled by TonemapAdaptiveParams.y.
  r0.w = cmp(0 < cb1[r15.y + 7].y);
  r12.xyz = v3.www * r0.xyz;
  r13.xyz = min(abs(v3.www), abs(r0.xyz));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r1.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;

  const bool hable_enabled = r0.w != 0;
  const float3 untonemapped = hable_enabled ? r1.xyz : r0.xyz;  // RenoDX: exposed scene before the filmic curve

  r12.xyz = r1.xyz * cb1[r15.y + 2].yyy;
  r13.xyz = min(abs(r1.xyz), abs(cb1[r15.y + 2].yyy));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r2.xyz = cb1[r15.y + 4].xxx + r12.xyz;
  r12.xyz = r2.xyz * r1.xyz;
  r13.xyz = min(abs(r2.xyz), abs(r1.xyz));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r2.xyz = cb1[r15.y + 4].zzz + r12.xyz;
  r12.xyz = r1.xyz * cb1[r15.y + 2].yyy;
  r13.xyz = min(abs(r1.xyz), abs(cb1[r15.y + 2].yyy));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r3.xyz = cb1[r15.y + 2].zzz + r12.xyz;
  r12.xyz = r3.xyz * r1.xyz;
  r13.xyz = min(abs(r3.xyz), abs(r1.xyz));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r1.xyz = cb1[r15.y + 4].yyy + r12.xyz;
  r14.xyz = rcp(r1.xyz);
  r12.xyz = r14.xyz * r2.xyz;
  r13.xyz = min(abs(r14.xyz), abs(r2.xyz));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r1.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r1.xyz = -cb1[r15.y + 4].www + r1.xyz;
  r14.xyz = rcp(v3.zzz);
  r12.xyz = r14.xyz * r1.xyz;
  r13.xyz = min(abs(r14.xyz), abs(r1.xyz));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r1.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r0.xyz = r0.www ? r1.xyz : r0.xyz;
  const float3 vanilla_tonemapped = r0.xyz;  // RenoDX: vanilla filmic result before the LUT
  // Colour grading LUT (16^3, linear in/out) with ColourGrade scale/offset.
  r0.w = cmp(0 < cb1[r15.y + 552].z);
  if (r0.w != 0) {
    r12.xyz = r0.xyz * cb1[r15.y + 552].yyy;
    r13.xyz = min(abs(r0.xyz), abs(cb1[r15.y + 552].yyy));
    r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
    r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
    r1.xyz = cb1[r15.y + 552].xxx + r12.xyz;
    r0.xyz = t12.SampleLevel(s0_s, r1.xyz, 0).xyz;
  }
  // Vignette (top/bottom blend).
  r0.w = 0.5 + v3.y;
  r1.xyzw = cb1[r15.y + 11].xyzw + -cb1[r15.y + 8].xyzw;
  r12.xyzw = r1.xyzw * r0.wwww;
  r13.xyzw = min(abs(r1.xyzw), abs(r0.wwww));
  r13.xyzw = cmp(r13.xyzw == float4(0, 0, 0, 0));
  r12.xyzw = r13.xyzw ? float4(0, 0, 0, 0) : r12.xyzw;
  r1.xyzw = cb1[r15.y + 8].xyzw + r12.xyzw;
  r2.xyz = cb1[r15.y + 12].xyw + -cb1[r15.y + 9].xyw;
  r12.xyz = r2.xyz * r0.www;
  r13.xyz = min(abs(r2.xyz), abs(r0.www));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r2.xyz = cb1[r15.y + 9].xyw + r12.xyz;
  r3.xyzw = cb1[r15.y + 13].xyzw + -cb1[r15.y + 10].xyzw;
  r12.xyzw = r3.xyzw * r0.wwww;
  r13.xyzw = min(abs(r3.xyzw), abs(r0.wwww));
  r13.xyzw = cmp(r13.xyzw == float4(0, 0, 0, 0));
  r12.xyzw = r13.xyzw ? float4(0, 0, 0, 0) : r12.xyzw;
  r3.xyzw = cb1[r15.y + 10].xyzw + r12.xyzw;
  r12.xy = v3.xy * r1.xy;
  r13.xy = min(abs(v3.xy), abs(r1.xy));
  r13.xy = cmp(r13.xy == float2(0, 0));
  r12.xy = r13.xy ? float2(0, 0) : r12.xy;
  r1.x = dot(r12.xy, float2(1, 1));
  r12.xy = v3.xy * r1.zw;
  r13.xy = min(abs(v3.xy), abs(r1.zw));
  r13.xy = cmp(r13.xy == float2(0, 0));
  r12.xy = r13.xy ? float2(0, 0) : r12.xy;
  r1.y = dot(r12.xy, float2(1, 1));
  r1.xy = r1.xy + r2.xy;
  r12.xy = r1.xy * r1.xy;
  r13.xy = min(abs(r1.xy), abs(r1.xy));
  r13.xy = cmp(r13.xy == float2(0, 0));
  r12.xy = r13.xy ? float2(0, 0) : r12.xy;
  r0.w = dot(r12.xy, float2(1, 1));
  r0.w = log2(r0.w);
  r12.w = r2.z * r0.w;
  r13.w = min(abs(r2.z), abs(r0.w));
  r13.w = cmp(r13.w == 0);
  r0.w = r13.w ? 0 : r12.w;
  r0.w = exp2(r0.w);
  r0.w = min(1, r0.w);
  r12.w = r0.w * r3.w;
  r13.w = min(abs(r0.w), abs(r3.w));
  r13.w = cmp(r13.w == 0);
  r0.w = r13.w ? 0 : r12.w;
  r1.xyz = r3.xyz + r0.xyz;
  r1.xyz = saturate(float3(-1, -1, -1) + r1.xyz);
  r1.xyz = r1.xyz + -r0.xyz;
  r12.xyz = r1.xyz * r0.www;
  r13.xyz = min(abs(r1.xyz), abs(r0.www));
  r13.xyz = cmp(r13.xyz == float3(0, 0, 0));
  r12.xyz = r13.xyz ? float3(0, 0, 0) : r12.xyz;
  r0.xyz = r0.xyz + r12.xyz;
  // ColorTransform 4x4.
  r0.w = 1;
  r12.xyzw = cb1[r15.y + 14].xyzw * r0.xyzw;
  r13.xyzw = min(abs(cb1[r15.y + 14].xyzw), abs(r0.xyzw));
  r13.xyzw = cmp(r13.xyzw == float4(0, 0, 0, 0));
  r12.xyzw = r13.xyzw ? float4(0, 0, 0, 0) : r12.xyzw;
  r1.x = dot(r12.xyzw, float4(1, 1, 1, 1));
  r12.xyzw = cb1[r15.y + 15].xyzw * r0.xyzw;
  r13.xyzw = min(abs(cb1[r15.y + 15].xyzw), abs(r0.xyzw));
  r13.xyzw = cmp(r13.xyzw == float4(0, 0, 0, 0));
  r12.xyzw = r13.xyzw ? float4(0, 0, 0, 0) : r12.xyzw;
  r1.y = dot(r12.xyzw, float4(1, 1, 1, 1));
  r12.xyzw = cb1[r15.y + 16].xyzw * r0.xyzw;
  r13.xyzw = min(abs(cb1[r15.y + 16].xyzw), abs(r0.xyzw));
  r13.xyzw = cmp(r13.xyzw == float4(0, 0, 0, 0));
  r12.xyzw = r13.xyzw ? float4(0, 0, 0, 0) : r12.xyzw;
  r1.z = dot(r12.xyzw, float4(1, 1, 1, 1));

  o0.w = 0;  // HUD writes coverage into alpha later in the frame.

  if (RENODX_TONE_MAP_TYPE != RENODX_TONE_MAP_TYPE_VANILLA) {
    HableParams hable_params = {
      cb1[r15.y + 2].y,  // a
      cb1[r15.y + 2].z,  // b
      cb1[r15.y + 4].x,  // c_times_b
      cb1[r15.y + 4].z,  // d_times_e
      cb1[r15.y + 4].y,  // d_times_f
      cb1[r15.y + 4].w,  // e_over_f
      1.f / v3.z,        // white_scale
    };
    o0.xyz = renodx::draw::RenderIntermediatePass(
        ApplyFH2ToneMap(untonemapped, vanilla_tonemapped, r1.xyz, hable_params, hable_enabled));
    return;
  }

  // Vanilla sRGB approximation; saturate emulates the original R10G10B10A2_UNORM target after the upgrade.
  r0.xyz = cmp(r1.xyz < float3(0.00313080009, 0.00313080009, 0.00313080009));
  r2.xyz = float3(12.9200001, 12.9200001, 12.9200001) * r1.xyz;
  r3.xyz = float3(-0.0022799999, -0.0022799999, -0.0022799999) + r1.xyz;
  r3.xyz = sqrt(r3.xyz);
  r1.xyz = float3(0.13448, 0.13448, 0.13448) * r1.xyz;
  r1.xyz = r3.xyz * float3(1.13004994, 1.13004994, 1.13004994) + -r1.xyz;
  r1.xyz = float3(0.00571900001, 0.00571900001, 0.00571900001) + r1.xyz;
  o0.xyz = saturate(r0.xyz ? r2.xyz : r1.xyz);
}
