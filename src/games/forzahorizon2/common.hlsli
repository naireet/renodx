#include "./shared.h"

// FH2 (ForzaTech, 2014) uses a parameterized Hable/Uncharted2 filmic curve:
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

float ApplyHable(float x, HableParams params) {
  float numerator = (params.a * x + params.c_times_b) * x + params.d_times_e;
  float denominator = (params.a * x + params.b) * x + params.d_times_f;
  return (numerator / denominator - params.e_over_f) * params.white_scale;
}

float SignCbrt(float x) {
  return sign(x) * pow(abs(x), 1.f / 3.f);
}

// Largest real root of the inflection cubic (second derivative == 0), as proven in gtav-enhanced.
float FindHableInflection(HableParams params) {
  float a = params.a;
  float b = params.b;
  float c_times_b = params.c_times_b;
  float d_times_e = params.d_times_e;
  float d_times_f = params.d_times_f;

  float a3 = a * a * (b - c_times_b);
  float a2 = 3.f * a * a * (d_times_f - d_times_e);
  float a1 = 3.f * a * (c_times_b * d_times_f - b * d_times_e);
  float a0 = a * d_times_e * d_times_f - a * d_times_f * d_times_f - b * b * d_times_e + b * c_times_b * d_times_f;

  float inv_a3 = 1.f / a3;
  float cb = a2 * inv_a3;
  float cc = a1 * inv_a3;
  float cd = a0 * inv_a3;

  float shift = cb / 3.f;
  float p = cc - (cb * cb) / 3.f;
  float q = (2.f * cb * cb * cb) / 27.f - (cb * cc) / 3.f + cd;
  float half_q = 0.5f * q;
  float third_p = p / 3.f;
  float discriminant = half_q * half_q + third_p * third_p * third_p;

  float inflection;
  if (discriminant > 0.f) {
    float s = sqrt(discriminant);
    inflection = SignCbrt(-half_q + s) + SignCbrt(-half_q - s) - shift;
  } else {
    static const float PI = 3.14159265358979323846f;
    float t = 2.f * sqrt(-third_p);
    float phi = acos(clamp(-half_q / sqrt(-(third_p * third_p * third_p)), -1.f, 1.f)) / 3.f;
    float3 roots = float3(
        t * cos(phi) - shift,
        t * cos(phi - 2.f * PI / 3.f) - shift,
        t * cos(phi - 4.f * PI / 3.f) - shift);
    inflection = max(roots.x, max(roots.y, roots.z));
  }
  return inflection;
}

// untonemapped: exposed linear scene before the filmic curve.
// graded_sdr: vanilla filmic + LUT + vignette + color matrix, linear, before output encoding.
float3 ApplyFH2ToneMap(float3 untonemapped, float3 graded_sdr, HableParams params, bool hable_enabled) {
  if (hable_enabled) {
    // Anchor RenoDRT on the vanilla curve's knee so midtones keep the vanilla brightness.
    float mid_gray_in = FindHableInflection(params);
    float mid_gray_out = ApplyHable(mid_gray_in, params);
    if (mid_gray_in > 0.f && mid_gray_out > 0.f) {
      untonemapped *= mid_gray_out / mid_gray_in;
    }
  }
  return renodx::draw::ToneMapPass(untonemapped, graded_sdr);
}
