# RenoDX: Forza Horizon 2 (Azimuth)

HDR10 output for the Xbox One version of Forza Horizon 2 running through Azimuth.

- Pinned to **Azimuth 0.9.7**. Shader hashes belong to Azimuth's runtime DXBC translations and change when Azimuth's translator changes.
- Load `renodx-forzahorizon2.addon64` next to `azimuth.exe` with ReShade (addon build, as `dxgi.dll`).
- Hooks: FH2 tonemap/composite `0x362A82D8` (Hable filmic + zone/weather LUT) and Azimuth's present composites `0x3F8871B6` (display gamma ramp) / `0x395B7917` (identity ramp at in-game Brightness 50), which doubles as the HDR10 output pass into the existing R10G10B10A2 swapchain (no swapchain proxy).
- The tonemap target and display plane (backbuffer-sized R10G10B10A2 render targets) are upgraded to typed RGBA16F through clones because Azimuth places game textures over title-memory heaps; a typed format keeps Azimuth's UAV views valid.
- Azimuth's main graphics root signature is full (64 DWORDs): the tonemap reads its settings from Azimuth's pixel root CBV `b10`, borrowed around that one draw. Only tiny layouts (the output pass) get push-constant injection.
- HDR output and the HDR10 color space only activate once the output-pass replacement is confirmed running; otherwise the mod stays in SDR.
- Settings `ToneMapPeakNits`, `ToneMapGameNits`, `ToneMapUINits` follow standard RenoDX names; Peak Brightness defaults/resets to the display's reported peak.
- Tone mappers:
  - **Vanilla**: the game's Hable curve, SDR range.
  - **Neutwo** (default): vanilla output exactly up to 50%, then a per-channel extension that starts on the Hable tangent and rises toward the scene-linear mid-gray exposure, carrying the LUT/vignette/color-matrix grade. Peak compression uses `ToneMapWorkingColorSpace` (LMS per channel by default, or BT.709 max channel, BT.709 or AP1 per channel).
  - **PsychoV-17**: exposure and cone response matched to the vanilla curve at output 18%, with the game's grade carried over through `UpgradeToneMap`.
- Azimuth's 1025-entry output curve is applied in both SDR and HDR (linear continuation above 1.0).
- **Recommended** button (opt-in, user-tuned): Neutwo LMS with Contrast 53, Saturation 55, Shadows 52, Blowout 10. Brightness and EOTF are left untouched.
- **DLSS Frame Generation / DLAA (experimental, branch naireet-fh2-dlss):** NVIDIA Streamline 2.8 is injected below ReShade when NVIDIA's signed runtime is placed in `renodx-streamline\` next to `azimuth.exe` (sl.interposer, sl.common, sl.dlss_g, sl.dlss, sl.reflex, sl.pcl, nvngx_dlssg, nvngx_dlss). Settings: DLSS Frame Generation (2x/3x/4x), NVIDIA Reflex (Off/On/On + Boost, forced on with frame generation), DLAA (requires `antialiasing=off` in azimuth.ini). Inputs: native-resolution motion vectors from FH2's per-object velocity transforms, depth, HUD-less scene and HUD coverage (solid mask) for UI recomposition; DLAA uses a sub-pixel viewport jitter on scene passes. Development knobs build with `FH2_DEV_SETTINGS`.
