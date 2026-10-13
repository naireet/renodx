// Dev-only: shows what DLSS-G treats as UI by tinting the presented frame red.
// Mode 1: tagged UI alpha. Mode 2: HUD coverage (display plane alpha). Mode 3: visible HUD difference.
#include "./shared.h"

Texture2D<float> ui_alpha : register(t0);
Texture2D<float4> display_plane : register(t1);
Texture2D<float4> hudless_scene : register(t2);

float4 main(float4 position: SV_Position, float2 uv: TEXCOORD0) : SV_Target0 {
  const int3 pixel = int3(position.xy, 0);
  float value = 0.f;
  if (CUSTOM_FRAME_GENERATION_UI_DEBUG == 1.f) {
    value = ui_alpha.Load(pixel);
  } else if (CUSTOM_FRAME_GENERATION_UI_DEBUG == 2.f) {
    value = saturate(display_plane.Load(pixel).a);
  } else {
    const float3 difference = abs(display_plane.Load(pixel).rgb - hudless_scene.Load(pixel).rgb);
    value = saturate(max(difference.r, max(difference.g, difference.b)) * 20.f);
  }
  // Square root makes faint values visible; blended with source alpha over the backbuffer.
  return float4(0.5f, 0.f, 0.f, sqrt(saturate(value)) * 0.8f);
}
