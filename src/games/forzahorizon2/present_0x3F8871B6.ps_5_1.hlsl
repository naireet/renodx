// Azimuth 0.9.7 present composite (Azimuth-owned shader, not FH2's): display plane -> swapchain through a
// 1025-entry per-channel curve (Xbox display gamma ramp emulation). Vanilla saturates before the curve.
// Uses no injected constants, so Azimuth's root signature for this pass stays untouched.

SamplerState linear_clamp_s : register(s0);
Texture2D<float4> image : register(t0);
Texture1D<float4> curve : register(t1);

void main(
    float4 v0: SV_Position0,
    float2 v1: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float4 color = image.SampleLevel(linear_clamp_s, v1.xy, 0);
  o0.w = color.w;

  float3 clamped = saturate(color.rgb);
  float3 position = clamped * 1024.f;
  float3 weight = frac(position);
  int3 index = (int3)position;
  int3 next_index = min(index + 1, 1024);

  float3 curved = float3(
      lerp(curve.Load(int2(index.x, 0)).x, curve.Load(int2(next_index.x, 0)).x, weight.x),
      lerp(curve.Load(int2(index.y, 0)).y, curve.Load(int2(next_index.y, 0)).y, weight.y),
      lerp(curve.Load(int2(index.z, 0)).z, curve.Load(int2(next_index.z, 0)).z, weight.z));

  // Keep the vanilla curve on [0, 1] and carry HDR/wide-gamut values beyond it unchanged.
  o0.rgb = curved + (color.rgb - clamped);
}
