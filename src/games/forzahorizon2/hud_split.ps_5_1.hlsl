// DLSS-G UI separation, recorded right after Azimuth's present composite. The HUD-less scene (tonemap output
// copied before the HUD draws) is encoded like the output pass encodes the backbuffer, and the display plane's
// alpha (HUD coverage; the tonemap writes 0) becomes the UI alpha. Matches the backbuffer exactly at in-game
// Brightness 50, where Azimuth's display ramp is identity.
#include "./shared.h"

Texture2D<float4> hudless_scene : register(t0);
Texture2D<float4> display_plane : register(t1);

void main(
    float4 position: SV_Position,
    float2 uv: TEXCOORD0,
    out float4 hudless: SV_Target0,
    out float ui_alpha: SV_Target1) {
  const int3 pixel = int3(position.xy, 0);
  const float3 scene = hudless_scene.Load(pixel).rgb;
  hudless.rgb = (RENODX_SWAP_CHAIN_OUTPUT_PRESET == renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10
                     ? renodx::draw::SwapChainPass(scene, position.xy)
                     : saturate(scene));
  hudless.a = 1.f;
  const float coverage = saturate(display_plane.Load(pixel).a);
  // Solid: translucent HUD panels and anti-aliased edges are composited from the real frame entirely.
  ui_alpha = (CUSTOM_FRAME_GENERATION_UI_MASK != 0.f ? (coverage > (1.f / 255.f) ? 1.f : 0.f) : coverage);
}
