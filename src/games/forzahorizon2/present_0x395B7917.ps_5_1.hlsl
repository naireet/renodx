// Azimuth 0.9.7 present composite used when the Xbox display gamma ramp is identity (in-game Brightness 50, the
// default): display plane -> swapchain as a plain copy. Replaced as the RenoDX output pass like 0x3F8871B6:
// SDR stays a copy; HDR10 decodes the RenoDX intermediate and PQ-encodes into the R10G10B10A2 swapchain.
#include "./shared.h"

SamplerState linear_clamp_s : register(s0);
Texture2D<float4> image : register(t0);

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

  o0.rgb = color.rgb;
}
