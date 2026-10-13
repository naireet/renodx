#ifndef SRC_GAMES_FORZAHORIZON2_SHARED_H_
#define SRC_GAMES_FORZAHORIZON2_SHARED_H_

#define RENODX_TONE_MAP_TYPE_VANILLA      0.f
#define RENODX_TONE_MAP_TYPE_NEUTWO       3.f
#define RENODX_TONE_MAP_TYPE_PSYCHOV17    4.f

#define RENODX_WORKING_COLOR_SPACE_BT709_MAX_CHANNEL 0.f
#define RENODX_WORKING_COLOR_SPACE_BT709_PER_CHANNEL 1.f
#define RENODX_WORKING_COLOR_SPACE_AP1_PER_CHANNEL   2.f
#define RENODX_WORKING_COLOR_SPACE_LMS_PER_CHANNEL   3.f

// Must be 32bit aligned
struct ShaderInjectData {
  float tone_map_type;
  float peak_white_nits;
  float diffuse_white_nits;
  float graphics_white_nits;

  float tone_map_exposure;
  float tone_map_highlights;
  float tone_map_shadows;
  float tone_map_contrast;

  float tone_map_saturation;
  float tone_map_blowout;
  float tone_map_working_color_space;
  float gamma_correction;

  float color_grade_strength;
  float swap_chain_output_preset;
  float tone_map_cone_contrast;
  float psychov_gamut_compression;
};

#ifdef __cplusplus
static_assert(sizeof(ShaderInjectData) == 4u * 16u);
// Azimuth's graphics root signature already uses all 64 root DWORDs, so game-pass replacements read the
// settings from this existing pixel root CBV (bound only around the replaced draw), not an injected one.
static constexpr uint32_t FH2_SETTINGS_ROOT_CBV_REGISTER = 10u;
#endif

#ifndef __cplusplus
#ifdef FH2_SETTINGS_IN_GAME_ROOT_CBV
cbuffer injected_buffer : register(b10) {  // FH2_SETTINGS_ROOT_CBV_REGISTER
#else
// The output pass (Azimuth's small composite root signature) gets standard push-constant injection.
cbuffer injected_buffer : register(b13, space50) {
#endif
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_TONE_MAP_TYPE                 shader_injection.tone_map_type
#define RENODX_PEAK_WHITE_NITS               shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS            shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS           shader_injection.graphics_white_nits
#define RENODX_TONE_MAP_EXPOSURE             shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS           shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS              shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST             shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION           shader_injection.tone_map_saturation
#define RENODX_TONE_MAP_BLOWOUT              shader_injection.tone_map_blowout
#define RENODX_TONE_MAP_WORKING_COLOR_SPACE  shader_injection.tone_map_working_color_space
#define RENODX_TONE_MAP_CONE_CONTRAST        shader_injection.tone_map_cone_contrast
#define RENODX_PSYCHOV_GAMUT_COMPRESSION     shader_injection.psychov_gamut_compression
#define RENODX_COLOR_GRADE_STRENGTH          shader_injection.color_grade_strength
// SDR output re-encodes sRGB like vanilla, so 2.2 emulation only applies in HDR.
#define RENODX_GAMMA_CORRECTION              (shader_injection.swap_chain_output_preset == 0.f ? 0.f : shader_injection.gamma_correction)
#define RENODX_SWAP_CHAIN_OUTPUT_PRESET      shader_injection.swap_chain_output_preset
// The HUD alpha-blends onto the tonemapped target, so the intermediate stays gamma-encoded like vanilla
// (sRGB in SDR, 2.2 in HDR via the default RENODX_GAMMA_CORRECTION + 1).

#include "../../shaders/renodx.hlsl"
#endif

#endif  // SRC_GAMES_FORZAHORIZON2_SHARED_H_
