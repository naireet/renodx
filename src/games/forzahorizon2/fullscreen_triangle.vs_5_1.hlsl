// Fullscreen triangle for RenoDX passes recorded on Azimuth's own root signature.
void main(uint vertex_id: SV_VertexID, out float4 position: SV_Position, out float2 uv: TEXCOORD0) {
  uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  position = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
}
