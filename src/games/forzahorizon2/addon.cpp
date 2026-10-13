/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <atomic>
#include <cstring>
#include <optional>
#include <shared_mutex>

#include <deps/imgui/imgui.h>
#include <embed/shaders.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../utils/bitwise.hpp"
#include "../../utils/data.hpp"
#include "../../utils/pipeline_layout.hpp"
#include "../../utils/resource_upgrade.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/shader.hpp"
#include "../../utils/state.hpp"
#include "../../utils/swapchain.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection = {};

// Tonemap target (txFinalTemp, HUD drawn on top) and the display plane copy. Azimuth places game textures over
// title-memory heaps, so upgrade through committed clones, never in place. The typed RGBA16F target matters:
// Azimuth creates integer UAV views on these targets, and unmapped view formats fall back to new_format.
std::vector<renodx::utils::resource::ResourceUpgradeInfo> upgrade_infos = {
    {
        .old_format = reshade::api::format::r10g10b10a2_typeless,
        .new_format = reshade::api::format::r16g16b16a16_float,
        .use_resource_view_cloning = true,
        .aspect_ratio = renodx::utils::resource::ResourceUpgradeInfo::BACK_BUFFER,
        .usage_include = reshade::api::resource_usage::render_target,
    },
    {
        .old_format = reshade::api::format::r10g10b10a2_unorm,
        .new_format = reshade::api::format::r16g16b16a16_float,
        .use_resource_view_cloning = true,
        .aspect_ratio = renodx::utils::resource::ResourceUpgradeInfo::BACK_BUFFER,
        .usage_include = reshade::api::resource_usage::render_target,
    },
};
bool upgrade_infos_applied = false;

// Azimuth's graphics root signature is full (64 DWORDs), so the tonemap replacement reads its settings from
// the pixel root CBV at FH2_SETTINGS_ROOT_CBV_REGISTER. It is bound right before that draw and rebound to the
// address Azimuth last set on the command list right after, because Azimuth may skip redundant root rebinds.
constexpr uint64_t SETTINGS_SLOT_SIZE = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t SETTINGS_SLOT_COUNT = 16u;  // Ring so in-flight frames never read a slot being rewritten.
constexpr uint32_t NO_SETTINGS_PARAM = UINT32_MAX;

reshade::api::resource settings_buffer = {0u};
uint8_t* settings_buffer_data = nullptr;
std::atomic<uint32_t> settings_buffer_next_slot = 0u;
std::atomic<bool> settings_binding_missing_logged = false;

struct SettingsRootCbvState {
  reshade::api::pipeline_layout layout = {0u};
  uint32_t layout_param = NO_SETTINGS_PARAM;
  bool has_azimuth_binding = false;
  reshade::api::buffer_range azimuth_binding = {};
  bool settings_bound = false;
};

renodx::utils::data::ParallelFlatHashMap<uint64_t, uint32_t, std::shared_mutex> settings_param_by_layout;
renodx::utils::data::ParallelFlatHashMap<reshade::api::command_list*, SettingsRootCbvState, std::shared_mutex> settings_root_cbvs;
// Invalidates OnPushDescriptors' per-thread fast path whenever any command list is reset or destroyed.
std::atomic<uint32_t> command_list_reset_generation = 0u;

void PushRootCbv(reshade::api::command_list* cmd_list, const SettingsRootCbvState& state, const reshade::api::buffer_range& range) {
  cmd_list->push_descriptors(
      reshade::api::shader_stage::all_graphics,
      state.layout,
      state.layout_param,
      reshade::api::descriptor_table_update{
          .count = 1u,
          .type = reshade::api::descriptor_type::constant_buffer,
          .descriptors = &range,
      });
}

void OnPushDescriptors(
    reshade::api::command_list* cmd_list,
    reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout,
    uint32_t layout_param,
    const reshade::api::descriptor_table_update& update) {
  if (!renodx::utils::bitwise::HasAnyFlag(stages, reshade::api::shader_stage::pixel)) return;

  // Azimuth binds ~a dozen pixel root CBVs per draw; skip the shared maps when this thread already recorded
  // the current command list and layout and the push is not the borrowed slot.
  struct RecordedLayout {
    reshade::api::command_list* cmd_list = nullptr;
    uint64_t layout = 0u;
    uint32_t settings_param = NO_SETTINGS_PARAM;
    uint32_t reset_generation = 0u;
  };
  thread_local RecordedLayout recorded = {};
  const uint32_t reset_generation = command_list_reset_generation.load(std::memory_order_relaxed);
  if (recorded.cmd_list == cmd_list
      && recorded.layout == layout.handle
      && recorded.reset_generation == reset_generation
      && layout_param != recorded.settings_param) {
    return;
  }

  uint32_t settings_param = NO_SETTINGS_PARAM;
  if (!settings_param_by_layout.if_contains(layout.handle, [&](const auto& pair) { settings_param = pair.second; })) {
    renodx::utils::pipeline_layout::GetPipelineLayoutData(layout, [&](const auto* layout_data) {
      for (uint32_t index = 0u; index < layout_data->params.size(); ++index) {
        const auto& param = layout_data->params[index];
        if (param.type == reshade::api::pipeline_layout_param_type::push_descriptors
            && param.push_descriptors.type == reshade::api::descriptor_type::constant_buffer
            && param.push_descriptors.dx_register_index == FH2_SETTINGS_ROOT_CBV_REGISTER
            && param.push_descriptors.dx_register_space == 0u
            && renodx::utils::bitwise::HasAnyFlag(param.push_descriptors.visibility, reshade::api::shader_stage::pixel)) {
          settings_param = index;
          break;
        }
      }
    });
    settings_param_by_layout.insert_or_assign(layout.handle, settings_param);
  }
  recorded = {
      .cmd_list = cmd_list,
      .layout = layout.handle,
      .settings_param = settings_param,
      .reset_generation = reset_generation,
  };
  if (settings_param == NO_SETTINGS_PARAM) return;

  const bool is_settings_param = layout_param == settings_param
                                 && update.type == reshade::api::descriptor_type::constant_buffer
                                 && update.count == 1u;
  settings_root_cbvs.lazy_emplace_l(
      cmd_list,
      [&](auto& pair) {
        auto& state = pair.second;
        if (state.layout.handle != layout.handle) {
          state = {.layout = layout, .layout_param = settings_param};
        }
        if (is_settings_param) {
          state.has_azimuth_binding = true;
          state.azimuth_binding = *static_cast<const reshade::api::buffer_range*>(update.descriptors);
        }
      },
      [&](const auto& construct) {
        SettingsRootCbvState state = {.layout = layout, .layout_param = settings_param};
        if (is_settings_param) {
          state.has_azimuth_binding = true;
          state.azimuth_binding = *static_cast<const reshade::api::buffer_range*>(update.descriptors);
        }
        construct(cmd_list, state);
      });
}

bool BindSettingsRootCbv(reshade::api::command_list* cmd_list) {
  std::optional<SettingsRootCbvState> state;
  settings_root_cbvs.if_contains(cmd_list, [&](const auto& pair) { state = pair.second; });
  if (!state.has_value() || settings_buffer.handle == 0u || settings_buffer_data == nullptr) {
    if (!settings_binding_missing_logged.exchange(true)) {
      reshade::log::message(
          reshade::log::level::warning,
          "forzahorizon2: settings root CBV unavailable for the tonemap draw; keeping vanilla for this draw.");
    }
    return false;
  }

  const uint64_t offset = (settings_buffer_next_slot.fetch_add(1u) % SETTINGS_SLOT_COUNT) * SETTINGS_SLOT_SIZE;
  std::memcpy(settings_buffer_data + offset, &shader_injection, sizeof(shader_injection));
  PushRootCbv(cmd_list, state.value(), {.buffer = settings_buffer, .offset = offset, .size = SETTINGS_SLOT_SIZE});
  settings_root_cbvs.modify_if(cmd_list, [](auto& pair) { pair.second.settings_bound = true; });
  return true;
}

void RestoreSettingsRootCbv(reshade::api::command_list* cmd_list) {
  std::optional<SettingsRootCbvState> state;
  settings_root_cbvs.modify_if(cmd_list, [&](auto& pair) {
    if (!pair.second.settings_bound) return;
    pair.second.settings_bound = false;
    state = pair.second;
  });
  if (!state.has_value() || !state->has_azimuth_binding) return;
  PushRootCbv(cmd_list, state.value(), state->azimuth_binding);
}

void OnResetCommandList(reshade::api::command_list* cmd_list) {
  settings_root_cbvs.erase(cmd_list);
  command_list_reset_generation.fetch_add(1u, std::memory_order_relaxed);
}

void OnDestroyCommandList(reshade::api::command_list* cmd_list) {
  settings_root_cbvs.erase(cmd_list);
  command_list_reset_generation.fetch_add(1u, std::memory_order_relaxed);
}

void OnDestroyPipelineLayout(reshade::api::device* /*device*/, reshade::api::pipeline_layout layout) {
  settings_param_by_layout.erase(layout.handle);
}

void OnInitDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d12) return;
  if (!upgrade_infos_applied) {
    upgrade_infos_applied = renodx::utils::resource::upgrade::SetUpgradeInfos(device, upgrade_infos);
  }
  if (settings_buffer.handle != 0u) return;

  constexpr uint64_t size = SETTINGS_SLOT_SIZE * SETTINGS_SLOT_COUNT;
  if (!device->create_resource(
          reshade::api::resource_desc(size, reshade::api::memory_heap::cpu_to_gpu, reshade::api::resource_usage::constant_buffer),
          nullptr,
          reshade::api::resource_usage::constant_buffer,
          &settings_buffer)) {
    reshade::log::message(reshade::log::level::error, "forzahorizon2: failed to create settings constant buffer.");
    return;
  }
  void* mapped_data = nullptr;
  if (!device->map_buffer_region(settings_buffer, 0u, size, reshade::api::map_access::write_only, &mapped_data)) {
    reshade::log::message(reshade::log::level::error, "forzahorizon2: failed to map settings constant buffer.");
    device->destroy_resource(settings_buffer);
    settings_buffer = {0u};
    return;
  }
  // Upload heaps may stay persistently mapped.
  settings_buffer_data = static_cast<uint8_t*>(mapped_data);
}

void OnDestroyDevice(reshade::api::device* device) {
  if (settings_buffer.handle == 0u) return;
  device->unmap_buffer_region(settings_buffer);
  device->destroy_resource(settings_buffer);
  settings_buffer = {0u};
  settings_buffer_data = nullptr;
}



// The replaced Azimuth composite is the HDR10 output pass. HDR output (and the HDR10 color space) is only used
// once that replacement is confirmed to run, so a missing replacement never shows SDR values as PQ.
std::atomic<bool> output_pass_replaced = false;
bool output_pass_active = false;

void OnOutputPassDrawn(reshade::api::command_list* cmd_list) {
  const auto pipeline = renodx::utils::state::GetBoundShaderPipeline(cmd_list, reshade::api::pipeline_stage::pixel_shader);
  renodx::utils::shader::WithReplacementPipeline(pipeline, [](auto replacement, auto /*stages*/) {
    if (replacement.handle != 0u) output_pass_replaced = true;
  });
}

// Hashes are Azimuth 0.9.7 runtime translations; re-hash after Azimuth updates.
renodx::mods::shader::CustomShaders custom_shaders = {
    // FH2 tonemap/composite (bloom, rays, flare, Hable, LUT, vignette)
    {0x362A82D8,
     {
         .crc32 = 0x362A82D8,
         .code = __0x362A82D8,
         .on_replace = &BindSettingsRootCbv,
         // Its layout is Azimuth's full root signature; settings come from the borrowed root CBV instead.
         .on_inject = [](reshade::api::command_list*) { return false; },
         .on_drawn = &RestoreSettingsRootCbv,
     }},
    // Azimuth display plane -> swapchain composite; replaced as the SDR/HDR10 output pass
    {0x3F8871B6,
     {
         .crc32 = 0x3F8871B6,
         .code = __0x3F8871B6,
         .on_drawn = &OnOutputPassDrawn,
     }},
};

renodx::utils::settings::Setting* output_mode_setting = nullptr;
renodx::utils::settings::Setting* tone_map_peak_nits_setting = nullptr;
constexpr float FALLBACK_PEAK_NITS = 1000.f;
float peak_nits_default = FALLBACK_PEAK_NITS;
bool peak_nits_default_initialized = false;
renodx::utils::settings::Setting* tone_map_game_nits_setting = nullptr;
renodx::utils::settings::Setting* tone_map_ui_nits_setting = nullptr;

std::optional<reshade::api::color_space> next_color_space = std::nullopt;
std::optional<reshade::api::color_space> current_color_space = std::nullopt;

bool IsHDREnabled() {
  return output_mode_setting != nullptr && output_mode_setting->GetValue() != 0.f;
}

bool IsCustomToneMapperEnabled() {
  return shader_injection.tone_map_type != RENODX_TONE_MAP_TYPE_VANILLA;
}

bool IsNeutwoEnabled() {
  return shader_injection.tone_map_type == RENODX_TONE_MAP_TYPE_NEUTWO;
}

bool IsPsychoVEnabled() {
  return shader_injection.tone_map_type == RENODX_TONE_MAP_TYPE_PSYCHOV17;
}

void SyncSwapChainOutputPreset() {
  if (output_mode_setting == nullptr
      || tone_map_peak_nits_setting == nullptr
      || tone_map_game_nits_setting == nullptr
      || tone_map_ui_nits_setting == nullptr) {
    return;
  }

  const auto queue_color_space = [](reshade::api::color_space color_space) {
    if (current_color_space.has_value() && current_color_space.value() == color_space) {
      next_color_space = std::nullopt;
      return;
    }
    next_color_space = color_space;
  };

  if (IsHDREnabled() && output_pass_active) {
    shader_injection.peak_white_nits = tone_map_peak_nits_setting->GetValue();
    shader_injection.diffuse_white_nits = tone_map_game_nits_setting->GetValue();
    shader_injection.graphics_white_nits = tone_map_ui_nits_setting->GetValue();
    shader_injection.swap_chain_output_preset = 1.f;
    queue_color_space(reshade::api::color_space::hdr10_st2084);
    return;
  }

  shader_injection.peak_white_nits = 1.f;
  shader_injection.diffuse_white_nits = 1.f;
  shader_injection.graphics_white_nits = 1.f;
  shader_injection.swap_chain_output_preset = 0.f;
  queue_color_space(reshade::api::color_space::srgb_nonlinear);
}

renodx::utils::settings::Settings settings = {
    output_mode_setting = new renodx::utils::settings::Setting{
        .key = "OutputMode",
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .can_reset = false,
        .label = "Output Mode",
        .section = "Output",
        .tooltip = "HDR uses HDR10 (PQ/BT.2020). SDR preserves the original sRGB output.",
        .labels = {"SDR", "HDR"},
        .on_change_value = [](float, float) { SyncSwapChainOutputPreset(); },
    },
    tone_map_peak_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &shader_injection.peak_white_nits,
        .default_value = FALLBACK_PEAK_NITS,
        .label = "Peak Brightness",
        .section = "Output",
        .tooltip = "Sets the display peak white in nits. Resets to the display's reported peak.",
        .min = 48.f,
        .max = 4000.f,
        .is_enabled = &IsHDREnabled,
    },
    tone_map_game_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Output",
        .tooltip = "Sets diffuse white in nits.",
        .min = 48.f,
        .max = 500.f,
        .is_enabled = &IsHDREnabled,
    },
    tone_map_ui_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapUINits",
        .binding = &shader_injection.graphics_white_nits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Output",
        .tooltip = "Sets HUD and menu white in nits.",
        .min = 48.f,
        .max = 500.f,
        .is_enabled = &IsHDREnabled,
    },
    new renodx::utils::settings::Setting{
        .key = "GammaCorrection",
        .binding = &shader_injection.gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "SDR EOTF Emulation",
        .section = "Output",
        .tooltip = "Emulates the 2.2 gamma display the game was graded for, in HDR output.",
        .labels = {"None", "2.2"},
        .is_enabled = &IsHDREnabled,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Vanilla keeps the game's Hable filmic curve (SDR range)."
                   "\nNeutwo keeps vanilla output exactly up to 50% and extends each channel along the curve's tangent above it."
                   "\nPsychoV-17 uses the vanilla output-18% exposure and slope with the game's grade carried over.",
        .labels = {"Vanilla", "Neutwo", "PsychoV-17"},
        .parse = [](float value) {
          if (value == 1.f) return RENODX_TONE_MAP_TYPE_NEUTWO;
          if (value == 2.f) return RENODX_TONE_MAP_TYPE_PSYCHOV17;
          return RENODX_TONE_MAP_TYPE_VANILLA;
        },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapWorkingColorSpace",
        .binding = &shader_injection.tone_map_working_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 3.f,
        .label = "Working Color Space",
        .section = "Tone Mapping",
        .tooltip = "Space for the Neutwo peak compression."
                   "\nLMS (default) compresses per cone response."
                   "\nBT.709 (Max Channel) compresses all channels together, preserving RGB ratios.",
        .labels = {"BT.709 (Max Channel)", "BT.709", "AP1", "LMS"},
        .is_enabled = &IsNeutwoEnabled,
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_enabled = &IsCustomToneMapperEnabled,
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsCustomToneMapperEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsCustomToneMapperEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsCustomToneMapperEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsCustomToneMapperEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeConeContrast",
        .binding = &shader_injection.tone_map_cone_contrast,
        .default_value = 50.f,
        .label = "Cone Contrast",
        .section = "Color Grading",
        .tooltip = "Scales PsychoV cone-response contrast after vanilla-slope matching.",
        .max = 100.f,
        .is_enabled = &IsPsychoVEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "PsychoVGamutCompression",
        .binding = &shader_injection.psychov_gamut_compression,
        .default_value = 1.f,
        .label = "Gamut Compression",
        .section = "Color Grading",
        .tooltip = "Controls PsychoV gamut compression."
                   "\n0 bypasses gamut mapping; 1 applies the full model.",
        .max = 1.f,
        .format = "%.2f",
        .is_enabled = &IsPsychoVEnabled,
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeBlowout",
        .binding = &shader_injection.tone_map_blowout,
        .default_value = 0.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .is_enabled = &IsCustomToneMapperEnabled,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeScene",
        .binding = &shader_injection.color_grade_strength,
        .default_value = 100.f,
        .label = "Scene Grading",
        .section = "Color Grading",
        .tooltip = "Strength of the game's colour grading (zone/weather/time-of-day LUT, vignette, color matrix).",
        .max = 100.f,
        .is_enabled = &IsCustomToneMapperEnabled,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Reset All",
        .section = "Options",
        .on_change = []() { renodx::utils::settings::ResetSettings(); },
    },
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"ToneMapType", 0.f},
      {"ToneMapPeakNits", peak_nits_default},
      {"ToneMapGameNits", 203.f},
      {"ToneMapUINits", 203.f},
      {"GammaCorrection", 0.f},
      {"ToneMapWorkingColorSpace", 3.f},
      {"ColorGradeExposure", 1.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"ColorGradeConeContrast", 50.f},
      {"PsychoVGamutCompression", 1.f},
      {"ColorGradeBlowout", 0.f},
      {"ColorGradeScene", 100.f},
  });
  SyncSwapChainOutputPreset();
}

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  if (peak_nits_default_initialized) return;
  peak_nits_default_initialized = true;
  peak_nits_default = roundf(renodx::utils::swapchain::GetPeakNits(swapchain).value_or(FALLBACK_PEAK_NITS));

  const bool was_using_default = tone_map_peak_nits_setting->GetValue() == tone_map_peak_nits_setting->default_value;
  tone_map_peak_nits_setting->default_value = peak_nits_default;
  if (was_using_default) tone_map_peak_nits_setting->Set(peak_nits_default)->Write();
}

void OnPresent(
    reshade::api::command_queue* /*queue*/,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect* /*source_rect*/,
    const reshade::api::rect* /*dest_rect*/,
    uint32_t /*dirty_rect_count*/,
    const reshade::api::rect* /*dirty_rects*/) {
  output_pass_active = output_pass_replaced.exchange(false);
  SyncSwapChainOutputPreset();
  if (swapchain == nullptr || !next_color_space.has_value()) return;

  const auto pending_color_space = next_color_space.value();
  if (!renodx::utils::swapchain::ChangeColorSpace(swapchain, pending_color_space)) return;
  current_color_space = pending_color_space;
  next_color_space = std::nullopt;
}
bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Forza Horizon 2 (Azimuth)";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        // Azimuth's layouts carry per-stage root constants; push-constant injection skips layouts without
        // DWORD budget (Azimuth's main graphics root signature), so only small layouts like the output pass get it.
        renodx::mods::shader::allow_multiple_push_constants = true;
        renodx::mods::shader::allow_undersized_shader_injection = false;
        // Only inject into tiny layouts like the composite's (texture table + sampler); leave Azimuth's
        // game-pass root signatures untouched.
        renodx::mods::shader::on_create_pipeline_layout = [](reshade::api::device*, std::span<reshade::api::pipeline_layout_param> params) {
          return params.size() <= 3u;
        };
        renodx::utils::resource::upgrade::use_resource_cloning_dx12_only = true;
        initialized = true;
      }

      // Module events first so their per-device data exists when this addon's init_device runs.
      renodx::utils::resource::upgrade::Use(fdw_reason);
      renodx::utils::swapchain::Use(fdw_reason);
      renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

      renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
      SyncSwapChainOutputPreset();
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyPipelineLayout);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::unregister_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::unregister_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::unregister_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyPipelineLayout);
      renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
      renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);
      renodx::utils::swapchain::Use(fdw_reason);
      renodx::utils::resource::upgrade::Use(fdw_reason);
      next_color_space = std::nullopt;
      current_color_space = std::nullopt;
      reshade::unregister_addon(h_module);
      break;
  }

  return TRUE;
}