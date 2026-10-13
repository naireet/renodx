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
  const float4 display = display_plane.Load(pixel);
  const float coverage = saturate(display.a);
  // Solid: visible HUD pixels (including translucent panels and anti-aliased edges) come from the real frame.
  // FH2's HUD also leaves near-transparent backing panels (minimap backing, bottom strip) in the coverage; they
  // barely change the image, so pixels with tiny coverage or no visible HUD contribution stay interpolated.
  // The display plane and the HUD-less scene share the tonemap target's encoding, so they compare directly.
  static const float SOLID_COVERAGE_MIN = 0.03f;
  static const float SOLID_COVERAGE_FULL = 0.06f;
  static const float SOLID_DIFFERENCE_MIN = 0.004f;
  static const float SOLID_DIFFERENCE_FULL = 0.012f;
  const float3 hud_difference = abs(display.rgb - scene);
  const float visible_hud = smoothstep(SOLID_DIFFERENCE_MIN, SOLID_DIFFERENCE_FULL, max(hud_difference.r, max(hud_difference.g, hud_difference.b)));
  const float solid = smoothstep(SOLID_COVERAGE_MIN, SOLID_COVERAGE_FULL, coverage) * visible_hud;
  ui_alpha = (CUSTOM_FRAME_GENERATION_UI_MASK != 0.f ? solid : coverage);
}
