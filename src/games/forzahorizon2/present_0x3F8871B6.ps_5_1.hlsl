// Azimuth 0.9.7 present composite (Azimuth-owned shader, not FH2's): display plane -> swapchain through a
// 1025-entry per-channel curve (Xbox display gamma ramp emulation). Replaced as the RenoDX output pass:
// SDR keeps the vanilla curve exactly; HDR10 decodes the RenoDX intermediate and PQ-encodes into the
// R10G10B10A2 swapchain (settings arrive through push-constant injection on this small root signature).
#include "./shared.h"

SamplerState linear_clamp_s : register(s0);
Texture2D<float4> image : register(t0);
Texture1D<float4> curve : register(t1);

void main(
    float4 v0: SV_Position0,
    float2 v1: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float4 color = image.SampleLevel(linear_clamp_s, v1.xy, 0);
  o0.w = color.w;

  if (RENODX_SWAP_CHAIN_OUTPUT_PRESET == renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10) {
    o0.rgb = renodx::draw::SwapChainPass(color.rgb, v0.xy);
    return;
  }

  float3 position = saturate(color.rgb) * 1024.f;
  float3 weight = frac(position);
  int3 index = (int3)position;
  int3 next_index = min(index + 1, 1024);

  o0.rgb = float3(
      lerp(curve.Load(int2(index.x, 0)).x, curve.Load(int2(next_index.x, 0)).x, weight.x),
      lerp(curve.Load(int2(index.y, 0)).y, curve.Load(int2(next_index.y, 0)).y, weight.y),
      lerp(curve.Load(int2(index.z, 0)).z, curve.Load(int2(next_index.z, 0)).z, weight.z));
}
