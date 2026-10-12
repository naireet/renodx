# RenoDX: Forza Horizon 2 (Azimuth)

HDR10 output for the Xbox One version of Forza Horizon 2 running through Azimuth.

- Pinned to **Azimuth 0.9.7**. Shader hashes belong to Azimuth's runtime DXBC translations and change when Azimuth's translator changes.
- Load `renodx-forzahorizon2.addon64` next to `azimuth.exe` with ReShade (addon build, as `dxgi.dll`).
- Hooks: FH2 tonemap/composite `0x362A82D8` (Hable filmic + zone/weather LUT) and Azimuth's present composite `0x3F8871B6`.
- The two 3840x2160-class R10G10B10A2 render targets (tonemap output with HUD, display plane) are upgraded to RGBA16F through clones because Azimuth places game textures over title-memory heaps.
- Settings `ToneMapPeakNits`, `ToneMapGameNits`, `ToneMapUINits` follow standard RenoDX names.
