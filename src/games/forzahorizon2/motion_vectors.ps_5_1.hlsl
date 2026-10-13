// Full-resolution, unclamped motion vectors and a depth copy for DLSS (frame generation / DLAA).
// Recorded right after the tonemap draw on Azimuth's root signature, reusing that draw's bindings:
// t11 txWorldDepth, t6 txWorldStencil (UNORM view of the object id), cb1 ImageProcPostProcessConstBuffer,
// cb13 Azimuth's per-draw block offset. Same math as FH2's half-res velocity shader 0xF2A8B736
// (VelocityTransformArray: per-stencil-id clip -> previous clip, rows at cb1[30 + 4 * id]), without its
// 0.1 NDC length clamp, at native resolution.

cbuffer cb13 : register(b13) {
  float4 cb13[1];
}

cbuffer cb1 : register(b1) {
  float4 cb1[569];
}

Texture2D<float4> t11 : register(t11);
Texture2D<float4> t6 : register(t6);

static const uint VELOCITY_TRANSFORM_ROW = 30u;
static const uint VELOCITY_TRANSFORM_ROW_END = 542u;

void main(
    float4 position: SV_Position,
    float2 uv: TEXCOORD0,
    out float2 motion_vector: SV_Target0,
    out float depth: SV_Target1) {
  const uint block = (asuint(cb13[0].x) >> 4) & 0xF;
  const int3 pixel = int3(position.xy, 0);
  depth = t11.Load(pixel).x;
  uint object_row = VELOCITY_TRANSFORM_ROW + 4u * (uint)floor(t6.Load(pixel).x * 255.f + 0.5f);
  if (object_row + 3u >= VELOCITY_TRANSFORM_ROW_END) {
    object_row = VELOCITY_TRANSFORM_ROW;
  }

  const float2 ndc = uv * float2(2.f, -2.f) + float2(-1.f, 1.f);
  const float3 previous_clip = cb1[block + object_row].xyw * ndc.x
                               + cb1[block + object_row + 1u].xyw * ndc.y
                               + cb1[block + object_row + 2u].xyw * depth
                               + cb1[block + object_row + 3u].xyw;
  const float2 previous_ndc = previous_clip.z != 0.f ? previous_clip.xy / previous_clip.z : ndc;
  // Streamline: normalized screen (UV) offset from this pixel to its position in the previous frame.
  motion_vector = (previous_ndc - ndc) * float2(0.5f, -0.5f);
}
