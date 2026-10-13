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
  // Solid: clearly visible HUD (glyphs, minimap content, anti-aliased edges) comes from the real frame entirely.
  // FH2's HUD also draws large near-transparent backing panels (minimap backing, bottom strip). Treating them as
  // solid UI pastes the scene behind them at 60 Hz, and treating them as no UI drops their tint from generated
  // frames; both outline the panels in motion. They keep their exact coverage instead, so DLSS-G keeps the tint
  // and interpolates the scene behind them. The display plane and the HUD-less scene share the tonemap target's
  // encoding, so they compare directly.
  static const float SOLID_COVERAGE_MIN = 0.03f;
  static const float SOLID_COVERAGE_FULL = 0.06f;
  static const float SOLID_DIFFERENCE_MIN = 0.004f;
  static const float SOLID_DIFFERENCE_FULL = 0.012f;
  const float3 hud_difference = abs(display.rgb - scene);
  const float visible_hud = smoothstep(SOLID_DIFFERENCE_MIN, SOLID_DIFFERENCE_FULL, max(hud_difference.r, max(hud_difference.g, hud_difference.b)));
  const float solid = lerp(coverage, 1.f, smoothstep(SOLID_COVERAGE_MIN, SOLID_COVERAGE_FULL, coverage) * visible_hud);  ui_alpha = (CUSTOM_FRAME_GENERATION_UI_MASK != 0.f ? solid : coverage);
}
