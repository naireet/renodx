# RenoDX: Forza Horizon 2 (Azimuth)

HDR10 output for the Xbox One version of Forza Horizon 2 running through Azimuth.

- Pinned to **Azimuth 0.9.7**. Shader hashes belong to Azimuth's runtime DXBC translations and change when Azimuth's translator changes.
- Load `renodx-forzahorizon2.addon64` next to `azimuth.exe` with ReShade (addon build, as `dxgi.dll`).
- Hooks: FH2 tonemap/composite `0x362A82D8` (Hable filmic + zone/weather LUT) and Azimuth's present composite `0x3F8871B6`, which doubles as the HDR10 output pass into the existing R10G10B10A2 swapchain (no swapchain proxy).
- The tonemap target and display plane (backbuffer-sized R10G10B10A2 render targets without UAV usage) are upgraded to RGBA16F through clones because Azimuth places game textures over title-memory heaps.
- Azimuth's main graphics root signature is full (64 DWORDs): the tonemap reads its settings from Azimuth's pixel root CBV `b10`, borrowed around that one draw. Only tiny layouts (the output pass) get push-constant injection.
- HDR output and the HDR10 color space only activate once the output-pass replacement is confirmed running; otherwise the mod stays in SDR.
- Settings `ToneMapPeakNits`, `ToneMapGameNits`, `ToneMapUINits` follow standard RenoDX names.
