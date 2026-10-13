// Copies an owned texture into the bound render target (DLAA output -> FH2's resolved scene colour).
Texture2D<float4> source : register(t0);

float4 main(float4 position: SV_Position, float2 uv: TEXCOORD0) : SV_Target0 {
  return source.Load(int3(position.xy, 0));
}
