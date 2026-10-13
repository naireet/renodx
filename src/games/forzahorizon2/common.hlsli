#include "./shared.h"

// FH2 (ForzaTech, 2014) uses a parameterized Hable/Uncharted2 filmic curve, per channel:
// ((x * (a * x + c_times_b) + d_times_e) / (x * (a * x + b) + d_times_f) - e_over_f) * white_scale
struct HableParams {
  float a;
  float b;
  float c_times_b;
  float d_times_e;
  float d_times_f;
  float e_over_f;
  float white_scale;
};

float HableDerivative(float x, HableParams params) {
  float denominator = (params.a * x * x) + (params.b * x) + params.d_times_f;
  float numerator = (params.a * (params.b - params.c_times_b) * x * x)
                    + (2.f * params.a * (params.d_times_f - params.d_times_e) * x)
                    + (params.c_times_b * params.d_times_f - params.b * params.d_times_e);
  return params.white_scale * numerator / (denominator * denominator);
}

// Smallest positive x with Hable(x) == y (quadratic solve, as in gtav-enhanced).
float HableInverse(float y, HableParams params) {
  float target = y / params.white_scale + params.e_over_f;
  float qa = params.a * (1.f - target);
  float qb = params.c_times_b - (target * params.b);
  float qc = params.d_times_e - (target * params.d_times_f);
  if (abs(qa) < 1e-6f) return max(0.f, -qc / qb);

  float sqrt_discriminant = sqrt(max(0.f, (qb * qb) - (4.f * qa * qc)));
  float root_a = (-qb + sqrt_discriminant) / (2.f * qa);
  float root_b = (-qb - sqrt_discriminant) / (2.f * qa);
  if (root_a > 0.f && root_b > 0.f) return min(root_a, root_b);
  return max(max(root_a, root_b), 0.f);
}

// Neutwo keeps vanilla output up to this level exactly; above it each channel continues along the tangent.
static const float HABLE_EXTENSION_PIVOT = 0.5f;
static const float MID_GRAY = 0.18f;

float3 ApplyDisplayMapping(float3 color, float peak) {
  if (RENODX_TONE_MAP_WORKING_COLOR_SPACE == RENODX_WORKING_COLOR_SPACE_BT709_PER_CHANNEL) {
    return renodx::tonemap::neutwo::PerChannel(color, peak.xxx);
  }
  if (RENODX_TONE_MAP_WORKING_COLOR_SPACE == RENODX_WORKING_COLOR_SPACE_AP1_PER_CHANNEL) {
    float3 ap1 = renodx::color::ap1::from::BT709(color);
    ap1 = renodx::tonemap::neutwo::PerChannel(ap1, renodx::color::ap1::from::BT709(peak.xxx));
    return renodx::color::bt709::from::AP1(ap1);
  }
  if (RENODX_TONE_MAP_WORKING_COLOR_SPACE == RENODX_WORKING_COLOR_SPACE_LMS_PER_CHANNEL) {
    float3 lms = renodx::color::lms::from::BT709(color);
    lms = renodx::tonemap::neutwo::PerChannel(lms, renodx::color::lms::from::BT709(peak.xxx));
    return renodx::color::bt709::from::LMS(lms);
  }
  return renodx::tonemap::neutwo::MaxChannel(color, peak);
}

// untonemapped: exposed linear scene before the filmic curve.
// vanilla_tonemapped: vanilla Hable result (before the LUT).
// graded_sdr: vanilla Hable + LUT + vignette + color matrix, linear, before output encoding.
float3 ApplyFH2ToneMap(float3 untonemapped, float3 vanilla_tonemapped, float3 graded_sdr, HableParams params, bool hable_enabled) {
  const bool is_hdr = RENODX_SWAP_CHAIN_OUTPUT_PRESET == renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_HDR10;
  float peak = 1.f;
  if (is_hdr) {
    peak = max(1.f, RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS);
    if (RENODX_GAMMA_CORRECTION == renodx::draw::GAMMA_CORRECTION_GAMMA_2_2) {
      peak = renodx::color::correct::GammaSafe(peak, true, 2.2f);
    }
  }

  if (RENODX_TONE_MAP_TYPE == RENODX_TONE_MAP_TYPE_PSYCHOV17) {
    // Match vanilla output mid gray and its local slope (cone response) there.
    float exposure = 1.f;
    float cone_response = 1.f;
    if (hable_enabled) {
      float mid_gray_in = HableInverse(MID_GRAY, params);
      exposure = MID_GRAY / max(mid_gray_in, 1e-6f);
      cone_response = mid_gray_in * HableDerivative(mid_gray_in, params) / MID_GRAY;
    }
    float3 psychov = renodx::tonemap::psychov::psychotm_test17(
        untonemapped * exposure,
        peak,
        RENODX_TONE_MAP_EXPOSURE,
        RENODX_TONE_MAP_HIGHLIGHTS,
        RENODX_TONE_MAP_SHADOWS,
        RENODX_TONE_MAP_CONTRAST,
        RENODX_TONE_MAP_SATURATION,
        RENODX_TONE_MAP_BLOWOUT,
        100.f,
        1.f,
        1.f,
        1,
        cone_response * max(0.f, RENODX_TONE_MAP_CONE_CONTRAST),
        MID_GRAY.xxx,
        MID_GRAY.xxx,
        saturate(RENODX_PSYCHOV_GAMUT_COMPRESSION),
        is_hdr ? 1 : 0);
    // Carry the game's LUT/vignette/matrix grade as a luminance delta over the vanilla tonemap.
    return renodx::tonemap::UpgradeToneMap(psychov, vanilla_tonemapped, graded_sdr, RENODX_COLOR_GRADE_STRENGTH);
  }

  float3 extended = vanilla_tonemapped;
  float3 grade_ratio = 1.f;
  if (is_hdr && hable_enabled) {
    float hable_white = (1.f - params.e_over_f) * params.white_scale;
    float pivot_out = min(HABLE_EXTENSION_PIVOT, 0.8f * hable_white);
    float pivot_in = HableInverse(pivot_out, params);
    float pivot_slope = HableDerivative(pivot_in, params);
    float3 tangent = pivot_out + pivot_slope * (untonemapped - pivot_in);
    bool3 above_pivot = untonemapped > pivot_in;
    extended = above_pivot ? tangent : vanilla_tonemapped;
    // Carry the LUT/vignette/matrix grade into the extension; below the pivot the result is exactly vanilla.
    grade_ratio = above_pivot ? extended / max(vanilla_tonemapped, 1e-6f) : 1.f;
  }

  float3 color = lerp(extended, graded_sdr * grade_ratio, RENODX_COLOR_GRADE_STRENGTH);
  color = renodx::color::grade::UserColorGrading(
      color,
      RENODX_TONE_MAP_EXPOSURE,
      RENODX_TONE_MAP_HIGHLIGHTS,
      RENODX_TONE_MAP_SHADOWS,
      RENODX_TONE_MAP_CONTRAST,
      RENODX_TONE_MAP_SATURATION,
      RENODX_TONE_MAP_BLOWOUT,
      0.f,
      color);
  if (!is_hdr) return color;
  return ApplyDisplayMapping(color, peak);
}
