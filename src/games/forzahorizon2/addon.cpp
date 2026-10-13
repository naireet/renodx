/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

#include <deps/imgui/imgui.h>
#include <embed/shaders.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../utils/bitwise.hpp"
#include "../../utils/data.hpp"
#include "../../utils/pipeline_layout.hpp"
#include "../../utils/render.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/resource_upgrade.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/shader.hpp"
#include "../../utils/state.hpp"
#include "../../utils/swapchain.hpp"
#include "./shared.h"
#include "./jitter.hpp"
#include "./streamline.hpp"

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

// DLSS inputs (frame generation, later DLAA):
// - At the tonemap draw: full-resolution unclamped motion vectors and a depth copy, drawn on Azimuth's root
//   signature so that draw's depth, stencil, cb1 and cb13 bindings are reused as-is; plus the HUD-less tonemap
//   output, by re-issuing the tonemap draw into an owned target (the HUD draws onto Azimuth's target afterwards).
// - At the output pass: the HUD-less scene encoded like the backbuffer, and the UI alpha from the display plane.
// Azimuth's own resources are only read where its bindings already make them readable; all writes and state
// transitions are on owned targets, independent of Azimuth's enhanced barriers.
struct FrameGenerationInputs {
  std::mutex mutex;
  reshade::api::resource motion_vectors = {0u};
  reshade::api::resource depth = {0u};
  reshade::api::resource hudless_scene = {0u};
  reshade::api::resource hudless = {0u};
  reshade::api::resource ui_alpha = {0u};
  reshade::api::resource_view hudless_scene_rtv = {0u};
  // Display plane (A copied, HUD included), captured as the render target of Azimuth's A -> P copy draw.
  reshade::api::resource display_plane = {0u};
  // Resolved scene colour (R11G11B10) that bloom and the tonemap read; captured from the resolve draw 0x0B23432B.
  reshade::api::resource scene_color = {0u};
  reshade::api::resource dlaa_output = {0u};
  renodx::utils::render::RenderPass dlaa_copy_pass;
  reshade::api::resource dlaa_copy_pass_scene_color = {0u};
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint64_t layout = 0u;
  renodx::utils::render::RenderPass motion_vector_pass;
  renodx::utils::render::RenderPass ui_pass;
  reshade::api::resource ui_pass_display_plane = {0u};
  bool hudless_scene_ready = false;
  bool failure_logged = false;
};
FrameGenerationInputs frame_generation_inputs;

// Recorded before mods::shader replays a matched draw, so its on_drawn callbacks (same thread) can re-issue it.
struct LastDraw {
  reshade::api::command_list* cmd_list = nullptr;
  uint32_t vertex_count = 0u;
  uint32_t instance_count = 0u;
  uint32_t first_vertex = 0u;
  uint32_t first_instance = 0u;
};
thread_local LastDraw last_draw = {};

bool OnDraw(reshade::api::command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
  last_draw = {
      .cmd_list = cmd_list,
      .vertex_count = vertex_count,
      .instance_count = instance_count,
      .first_vertex = first_vertex,
      .first_instance = first_instance,
  };
  return false;
}

// Any other draw type invalidates the recorded arguments, so a re-issue never replays the wrong geometry.
bool OnDrawIndexed(reshade::api::command_list* /*cmd_list*/, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  last_draw = {};
  return false;
}

bool OnDrawOrDispatchIndirect(reshade::api::command_list* /*cmd_list*/, reshade::api::indirect_command, reshade::api::resource, uint64_t, uint32_t, uint32_t) {
  last_draw = {};
  return false;
}

// Replaced targets and their PSOs may still be referenced by in-flight frames (and Streamline at present), so
// they are destroyed only after enough presents for the GPU to have finished with them.
struct RetiredFrameGenerationInputs {
  std::vector<reshade::api::resource> resources;
  std::vector<reshade::api::resource_view> views;
  std::vector<renodx::utils::render::RenderPass> passes;
  uint64_t retired_at_present = 0u;
};
constexpr uint64_t RETIRED_INPUTS_PRESENT_DELAY = 8u;
std::vector<RetiredFrameGenerationInputs> retired_frame_generation_inputs;
std::atomic<uint64_t> present_count = 0u;

void DestroyRetiredFrameGenerationInputs(reshade::api::device* device, bool all) {
  const uint64_t presents = present_count.load();
  std::erase_if(retired_frame_generation_inputs, [&](RetiredFrameGenerationInputs& retired) {
    if (!all && retired.retired_at_present + RETIRED_INPUTS_PRESENT_DELAY > presents) return false;
    for (auto& pass : retired.passes) {
      pass.DestroyAll(device);
    }
    for (const auto& view : retired.views) {
      device->destroy_resource_view(view);
    }
    for (const auto& resource : retired.resources) {
      device->destroy_resource(resource);
    }
    return true;
  });
}

void RetireUiPass() {
  auto& inputs = frame_generation_inputs;
  retired_frame_generation_inputs.push_back({.passes = {}, .retired_at_present = present_count.load()});
  retired_frame_generation_inputs.back().passes.push_back(std::move(inputs.ui_pass));
  inputs.ui_pass = {};
  inputs.ui_pass_display_plane = {0u};
}

void RetireDlaaCopyPass() {
  auto& inputs = frame_generation_inputs;
  retired_frame_generation_inputs.push_back({.passes = {}, .retired_at_present = present_count.load()});
  retired_frame_generation_inputs.back().passes.push_back(std::move(inputs.dlaa_copy_pass));
  inputs.dlaa_copy_pass = {};
  inputs.dlaa_copy_pass_scene_color = {0u};
}

void RetireFrameGenerationInputs() {
  auto& inputs = frame_generation_inputs;
  RetireDlaaCopyPass();
  RetireUiPass();
  auto& retired = retired_frame_generation_inputs.back();
  retired.passes.push_back(std::move(inputs.motion_vector_pass));
  if (inputs.hudless_scene_rtv.handle != 0u) {
    retired.views.push_back(inputs.hudless_scene_rtv);
  }
  inputs.hudless_scene_rtv = {0u};
  for (auto* resource : {&inputs.motion_vectors, &inputs.depth, &inputs.hudless_scene, &inputs.hudless, &inputs.ui_alpha, &inputs.dlaa_output}) {
    if (resource->handle != 0u) {
      retired.resources.push_back(*resource);
    }
    *resource = {0u};
  }
  inputs.motion_vector_pass = {};
  inputs.hudless_scene_ready = false;
  inputs.width = 0u;
  inputs.height = 0u;
  inputs.layout = 0u;
}

void LogFrameGenerationFailure(const char* message) {
  if (frame_generation_inputs.failure_logged) return;
  frame_generation_inputs.failure_logged = true;
  reshade::log::message(reshade::log::level::error, message);
}

// Clone-upgraded targets are accessed through their RGBA16F clone; the game's view handles name the original.
reshade::api::resource ResolveUpgradedResource(reshade::api::resource resource) {
  const auto clone = renodx::utils::resource::upgrade::GetResourceClone(resource, {.allow_create = false});
  return (clone.handle != 0u ? clone : resource);
}

// Owned targets live in shader-resource state between passes; the DLAA output stays in unordered-access state.
constexpr uint32_t D3D12_SHADER_RESOURCE_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Streamline (manual hooking, no command-list state tracking) sets its own descriptor heaps and compute root
// signature on the native list during evaluate, behind ReShade's caches. Binding zero tables makes ReShade
// re-issue SetDescriptorHeaps with Azimuth's last heaps and the root signature; the snapshot then replays the
// root arguments, tables and pipelines.
void RestoreAfterStreamlineEvaluate(reshade::api::command_list* cmd_list, const renodx::utils::state::CommandListSnapshot& snapshot) {
  using renodx::utils::state::PipelineBindPoint;
  const auto compute = renodx::utils::state::CopyBoundDescriptorTables(cmd_list, PipelineBindPoint::COMPUTE);
  const auto graphics = renodx::utils::state::CopyBoundDescriptorTables(cmd_list, PipelineBindPoint::GRAPHICS);
  if (compute.has_value() && compute->layout.handle != 0u) {
    cmd_list->bind_descriptor_tables(reshade::api::shader_stage::all_compute, compute->layout, 0u, 0u, nullptr);
  }
  if (graphics.has_value() && graphics->layout.handle != 0u) {
    cmd_list->bind_descriptor_tables(reshade::api::shader_stage::all_graphics, graphics->layout, 0u, 0u, nullptr);
  }
  snapshot.Apply();
}
// Before the tonemap draw (its bindings are set): DLSS inputs for frame generation and DLAA, then DLAA itself.
void RecordDlssInputs(reshade::api::command_list* cmd_list) {
  if (!forzahorizon2::streamline::IsActive()) return;

  reshade::api::pipeline_layout layout = {0u};
  settings_root_cbvs.if_contains(cmd_list, [&](const auto& pair) { layout = pair.second.layout; });
  const auto target_view = renodx::utils::state::GetRenderTarget(cmd_list, 0u);
  if (layout.handle == 0u || target_view.handle == 0u) return;

  auto* device = cmd_list->get_device();
  const auto target_desc = device->get_resource_desc(ResolveUpgradedResource(device->get_resource_from_view(target_view)));
  const uint32_t width = target_desc.texture.width;
  const uint32_t height = target_desc.texture.height;

  const std::unique_lock lock(frame_generation_inputs.mutex);
  auto& inputs = frame_generation_inputs;
  DestroyRetiredFrameGenerationInputs(device, false);
  if (inputs.width != width || inputs.height != height || inputs.layout != layout.handle) {
    RetireFrameGenerationInputs();
    const auto create_target = [&](reshade::api::format format, reshade::api::resource_usage usage, reshade::api::resource_usage state, reshade::api::resource* resource) {
      return device->create_resource(
          reshade::api::resource_desc(width, height, 1u, 1u, format, 1u, reshade::api::memory_heap::gpu_only, usage),
          nullptr, state, resource);
    };
    constexpr auto RENDER_TARGET_USAGE = reshade::api::resource_usage::render_target | reshade::api::resource_usage::shader_resource;
    constexpr auto STORAGE_USAGE = reshade::api::resource_usage::unordered_access | reshade::api::resource_usage::shader_resource;
    constexpr auto READABLE = reshade::api::resource_usage::shader_resource;
    const bool created = create_target(reshade::api::format::r16g16_float, RENDER_TARGET_USAGE, READABLE, &inputs.motion_vectors)
                         && create_target(reshade::api::format::r32_float, RENDER_TARGET_USAGE, READABLE, &inputs.depth)
                         && create_target(reshade::api::format::r16g16b16a16_float, RENDER_TARGET_USAGE, READABLE, &inputs.hudless_scene)
                         && create_target(reshade::api::format::r10g10b10a2_unorm, RENDER_TARGET_USAGE, READABLE, &inputs.hudless)
                         && create_target(reshade::api::format::r16_float, RENDER_TARGET_USAGE, READABLE, &inputs.ui_alpha)
                         && create_target(reshade::api::format::r16g16b16a16_float, STORAGE_USAGE, reshade::api::resource_usage::unordered_access, &inputs.dlaa_output)
                         && device->create_resource_view(
                             inputs.hudless_scene, reshade::api::resource_usage::render_target,
                             reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float), &inputs.hudless_scene_rtv);
    if (!created) {
      LogFrameGenerationFailure("forzahorizon2: failed to create DLSS input targets.");
      RetireFrameGenerationInputs();
      return;
    }
    inputs.width = width;
    inputs.height = height;
    inputs.layout = layout.handle;
    auto& pass = inputs.motion_vector_pass;
    pass.layout = layout;
    pass.use_render_pass = false;
    pass.revert_state_after_render = true;
    pass.pipeline_subobjects.vertex_shader = __fullscreen_triangle;
    pass.pipeline_subobjects.pixel_shader = __motion_vectors;
    pass.render_target_slots.resources = {inputs.motion_vectors, inputs.depth};
    pass.render_target_slots.view_descs = {
        reshade::api::resource_view_desc(reshade::api::format::r16g16_float),
        reshade::api::resource_view_desc(reshade::api::format::r32_float),
    };
    reshade::log::message(reshade::log::level::info, "forzahorizon2: created DLSS input targets.");
  }

  const reshade::api::resource targets[2] = {inputs.motion_vectors, inputs.depth};
  const reshade::api::resource_usage readable[2] = {reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::shader_resource};
  const reshade::api::resource_usage writable[2] = {reshade::api::resource_usage::render_target, reshade::api::resource_usage::render_target};
  cmd_list->barrier(2u, targets, readable, writable);
  const bool rendered = inputs.motion_vector_pass.Render(cmd_list);
  cmd_list->barrier(2u, targets, writable, readable);
  if (!rendered) {
    LogFrameGenerationFailure("forzahorizon2: DLSS motion vector pass failed.");
    return;
  }
  const bool dlaa = forzahorizon2::jitter::dlaa_setting != 0.f;
  const auto jitter = (forzahorizon2::jitter::IsEnabled() ? forzahorizon2::jitter::CurrentOffset() : std::array<float, 2>{0.f, 0.f});
  const auto jitter_sign = static_cast<uint32_t>(forzahorizon2::jitter::jitter_sign_setting);
  if (static thread_local uint32_t logged_jitter_sign = UINT32_MAX; dlaa && logged_jitter_sign != jitter_sign) {
    logged_jitter_sign = jitter_sign;
    reshade::log::message(
        reshade::log::level::info,
        renodx::utils::log::BuildString(
            "forzahorizon2: DLAA jitter sign ", jitter_sign, " (x ", ((jitter_sign & 1u) != 0u ? "flipped" : "default"),
            ", y ", ((jitter_sign & 2u) != 0u ? "flipped" : "default"), "), current offset ", jitter[0], ", ", jitter[1], " px")
            .c_str());
  }
  forzahorizon2::streamline::TagFrameInputs(
      cmd_list, inputs.motion_vectors, inputs.depth, width, height,
      jitter[0] * ((jitter_sign & 1u) != 0u ? -1.f : 1.f),
      jitter[1] * ((jitter_sign & 2u) != 0u ? -1.f : 1.f));

  if (!dlaa || inputs.scene_color.handle == 0u) return;
  const auto scene_desc = device->get_resource_desc(inputs.scene_color);
  if (scene_desc.texture.width != width || scene_desc.texture.height != height) {
    LogFrameGenerationFailure("forzahorizon2: scene colour size differs from the tonemap target; DLAA disabled.");
    return;
  }
  if (inputs.dlaa_copy_pass_scene_color.handle != inputs.scene_color.handle) {
    RetireDlaaCopyPass();
    inputs.dlaa_copy_pass_scene_color = inputs.scene_color;
    auto& copy_pass = inputs.dlaa_copy_pass;
    copy_pass.use_render_pass = false;
    copy_pass.revert_state_after_render = true;
    copy_pass.pipeline_subobjects.vertex_shader = __fullscreen_triangle;
    copy_pass.pipeline_subobjects.pixel_shader = __copy_texture;
    copy_pass.shader_resource_slots.resources = {inputs.dlaa_output};
    copy_pass.shader_resource_slots.view_descs = {reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float)};
    copy_pass.render_target_slots.resources = {inputs.scene_color};
    copy_pass.render_target_slots.view_descs = {
        reshade::api::resource_view_desc(reshade::api::format_to_default_typed(scene_desc.texture.format, 0))};
  }

  // The scene colour is read by this tonemap draw (and was read by bloom), so it is in shader-resource state here.
  const auto snapshot = renodx::utils::state::GetSnapshot(cmd_list);
  if (!snapshot.has_value()) return;
  const bool evaluated = forzahorizon2::streamline::EvaluateDlaa(
      cmd_list, inputs.scene_color, D3D12_SHADER_RESOURCE_STATE, inputs.dlaa_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, width, height);
  RestoreAfterStreamlineEvaluate(cmd_list, snapshot.value());
  if (!evaluated) return;
  forzahorizon2::jitter::dlaa_evaluated_this_frame = true;

  const reshade::api::resource copy_resources[2] = {inputs.dlaa_output, inputs.scene_color};
  const reshade::api::resource_usage copy_before[2] = {reshade::api::resource_usage::unordered_access, reshade::api::resource_usage::shader_resource};
  const reshade::api::resource_usage copy_during[2] = {reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::render_target};
  cmd_list->barrier(2u, copy_resources, copy_before, copy_during);
  if (!inputs.dlaa_copy_pass.Render(cmd_list)) {
    LogFrameGenerationFailure("forzahorizon2: DLAA copy pass failed.");
  }
  cmd_list->barrier(2u, copy_resources, copy_during, copy_before);
}

// After the tonemap draw: the HUD-less scene for DLSS-G, by re-issuing that draw (replaced tonemap PSO and every
// binding still set) into the owned RGBA16F target, then restoring Azimuth's render targets.
void RecordHudlessScene(reshade::api::command_list* cmd_list) {
  if (!forzahorizon2::streamline::IsActive()) return;
  const auto target_view = renodx::utils::state::GetRenderTarget(cmd_list, 0u);
  if (target_view.handle == 0u) return;
  auto* device = cmd_list->get_device();
  const auto target_desc = device->get_resource_desc(ResolveUpgradedResource(device->get_resource_from_view(target_view)));

  const std::unique_lock lock(frame_generation_inputs.mutex);
  auto& inputs = frame_generation_inputs;
  inputs.hudless_scene_ready = false;
  if (inputs.hudless_scene_rtv.handle == 0u || last_draw.cmd_list != cmd_list
      || target_desc.texture.format != reshade::api::format::r16g16b16a16_float
      || target_desc.texture.width != inputs.width || target_desc.texture.height != inputs.height) return;
  auto snapshot = renodx::utils::state::GetSnapshot(cmd_list);
  if (!snapshot.has_value()) return;
  cmd_list->barrier(inputs.hudless_scene, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::render_target);
  cmd_list->bind_render_targets_and_depth_stencil(1u, &inputs.hudless_scene_rtv);
  cmd_list->draw(last_draw.vertex_count, last_draw.instance_count, last_draw.first_vertex, last_draw.first_instance);
  cmd_list->barrier(inputs.hudless_scene, reshade::api::resource_usage::render_target, reshade::api::resource_usage::shader_resource);
  snapshot->ApplyGraphics(false);
  inputs.hudless_scene_ready = true;
}

void CaptureSceneColor(reshade::api::command_list* cmd_list) {
  if (!forzahorizon2::streamline::IsActive()) return;
  const auto view = renodx::utils::state::GetRenderTarget(cmd_list, 0u);
  if (view.handle == 0u) return;
  const auto scene_color = ResolveUpgradedResource(cmd_list->get_device()->get_resource_from_view(view));
  const std::unique_lock lock(frame_generation_inputs.mutex);
  frame_generation_inputs.scene_color = scene_color;
}

void CaptureDisplayPlane(reshade::api::command_list* cmd_list) {
  if (!forzahorizon2::streamline::IsActive()) return;
  const auto view = renodx::utils::state::GetRenderTarget(cmd_list, 0u);
  if (view.handle == 0u) return;
  const auto display_plane = ResolveUpgradedResource(cmd_list->get_device()->get_resource_from_view(view));
  const std::unique_lock lock(frame_generation_inputs.mutex);
  frame_generation_inputs.display_plane = display_plane;
}

void RecordFrameGenerationUiInputs(reshade::api::command_list* cmd_list) {
  if (!forzahorizon2::streamline::IsActive()) return;
  auto* device = cmd_list->get_device();

  const std::unique_lock lock(frame_generation_inputs.mutex);
  auto& inputs = frame_generation_inputs;
  if (!inputs.hudless_scene_ready || inputs.display_plane.handle == 0u) return;
  inputs.hudless_scene_ready = false;
  const auto display_plane = inputs.display_plane;
  const auto display_plane_desc = device->get_resource_desc(display_plane);
  if (display_plane_desc.texture.width != inputs.width || display_plane_desc.texture.height != inputs.height) {
    LogFrameGenerationFailure("forzahorizon2: display plane size differs from the tonemap target; DLSS-G UI separation disabled.");
    return;
  }

  if (inputs.ui_pass_display_plane.handle != display_plane.handle) {
    RetireUiPass();
    inputs.ui_pass_display_plane = display_plane;
    auto& pass = inputs.ui_pass;
    pass.use_render_pass = false;
    pass.revert_state_after_render = true;
    pass.pipeline_subobjects.vertex_shader = __fullscreen_triangle;
    pass.pipeline_subobjects.pixel_shader = __hud_split;
    pass.shader_resource_slots.resources = {inputs.hudless_scene, display_plane};
    pass.shader_resource_slots.view_descs = {
        reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float),
        reshade::api::resource_view_desc(reshade::api::format_to_default_typed(display_plane_desc.texture.format, 0)),
    };
    pass.render_target_slots.resources = {inputs.hudless, inputs.ui_alpha};
    pass.render_target_slots.view_descs = {
        reshade::api::resource_view_desc(reshade::api::format::r10g10b10a2_unorm),
        reshade::api::resource_view_desc(reshade::api::format::r16_float),
    };
    pass.push_constants[renodx::utils::render::ConstantBuffersSlots{.slot = 13u, .space = 50u}] = std::span<const float>(
        reinterpret_cast<const float*>(&shader_injection), sizeof(shader_injection) / sizeof(float));
  }

  const reshade::api::resource targets[2] = {inputs.hudless, inputs.ui_alpha};
  const reshade::api::resource_usage readable[2] = {reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::shader_resource};
  const reshade::api::resource_usage writable[2] = {reshade::api::resource_usage::render_target, reshade::api::resource_usage::render_target};
  cmd_list->barrier(2u, targets, readable, writable);
  const bool rendered = inputs.ui_pass.Render(cmd_list);
  cmd_list->barrier(2u, targets, writable, readable);
  if (!rendered) {
    LogFrameGenerationFailure("forzahorizon2: DLSS-G UI separation pass failed.");
    return;
  }
  forzahorizon2::streamline::TagUiInputs(cmd_list, inputs.hudless, inputs.ui_alpha, inputs.width, inputs.height);
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
  {
    const std::unique_lock lock(frame_generation_inputs.mutex);
    RetireFrameGenerationInputs();
    DestroyRetiredFrameGenerationInputs(device, true);
  }
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
  RecordFrameGenerationUiInputs(cmd_list);
}

// Hashes are Azimuth 0.9.7 runtime translations; re-hash after Azimuth updates.
renodx::mods::shader::CustomShaders custom_shaders = {
    // FH2 scene colour resolve (also used without MSAA); its render target is the scene colour post passes read.
    {0x0B23432B,
     {
         .crc32 = 0x0B23432B,
         .on_inject = [](reshade::api::command_list*) { return false; },
         .on_drawn = &CaptureSceneColor,
     }},
    // FH2 copy of the tonemap target (HUD included) to the display plane; full and limited range variants.
    {0xD2A7D2FC,
     {
         .crc32 = 0xD2A7D2FC,
         .on_inject = [](reshade::api::command_list*) { return false; },
         .on_drawn = &CaptureDisplayPlane,
     }},
    {0xD9FE4C7D,
     {
         .crc32 = 0xD9FE4C7D,
         .on_inject = [](reshade::api::command_list*) { return false; },
         .on_drawn = &CaptureDisplayPlane,
     }},
    // FH2 tonemap/composite (bloom, rays, flare, Hable, LUT, vignette)
    {0x362A82D8,
     {
         .crc32 = 0x362A82D8,
         .code = __0x362A82D8,
         .on_replace = [](reshade::api::command_list* cmd_list) {
           RecordDlssInputs(cmd_list);
           return BindSettingsRootCbv(cmd_list);
         },
         // Its layout is Azimuth's full root signature; settings come from the borrowed root CBV instead.
         .on_inject = [](reshade::api::command_list*) { return false; },
         .on_drawn = [](reshade::api::command_list* cmd_list) {
           RecordHudlessScene(cmd_list);
           RestoreSettingsRootCbv(cmd_list);
         },
     }},
    // Azimuth display plane -> swapchain composite; replaced as the SDR/HDR10 output pass
    {0x3F8871B6,
     {
         .crc32 = 0x3F8871B6,
         .code = __0x3F8871B6,
         .on_drawn = &OnOutputPassDrawn,
     }},
    // Azimuth present composite when the display gamma ramp is identity (Brightness 50)
    {0x395B7917,
     {
         .crc32 = 0x395B7917,
         .code = __0x395B7917,
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
        .key = "FrameGeneration",
        .binding = &forzahorizon2::streamline::frame_generation_setting,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 1.f,
        .label = "DLSS Frame Generation",
        .section = "Frame Generation",
        .tooltip = "Experimental. Requires NVIDIA's Streamline runtime in the renodx-streamline folder next to azimuth.exe.",
        .labels = {"Off", "On"},
        .is_enabled = []() { return forzahorizon2::streamline::IsActive(); },
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "FrameGenerationMultiplier",
        .binding = &forzahorizon2::streamline::frame_generation_multiplier_setting,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Frame Generation Multiplier",
        .section = "Frame Generation",
        .tooltip = "Output frames per rendered frame. 3x/4x need DLSS multi frame generation support (RTX 50 series).",
        .labels = {"2x", "3x", "4x"},
        .is_enabled = []() { return forzahorizon2::streamline::IsActive() && forzahorizon2::streamline::frame_generation_setting != 0.f; },
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "Reflex",
        .binding = &forzahorizon2::streamline::reflex_setting,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "NVIDIA Reflex",
        .section = "Frame Generation",
        .tooltip = "Reflex low latency. DLSS Frame Generation requires Reflex, so Off only applies while frame generation is off.",
        .labels = {"Off", "On", "On + Boost"},
        .is_enabled = []() { return forzahorizon2::streamline::IsActive(); },
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "FrameGenerationUiMask",
        .binding = &shader_injection.frame_generation_ui_mask,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Frame Generation HUD Mask",
        .section = "Frame Generation",
        .tooltip = "How the HUD is kept out of generated frames."
                   "\nSolid: any HUD pixel (including translucent panels and edges) comes from the real frame."
                   "\nExact: translucent HUD pixels blend over the interpolated scene.",
        .labels = {"Exact", "Solid"},
        .is_enabled = []() { return forzahorizon2::streamline::IsActive() && forzahorizon2::streamline::frame_generation_setting != 0.f; },
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "Dlaa",
        .binding = &forzahorizon2::jitter::dlaa_setting,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .label = "DLAA",
        .section = "DLAA",
        .tooltip = "Experimental. NVIDIA DLAA (DLSS at native resolution) on the scene before post-processing."
                   "\nRequires antialiasing=off in azimuth.ini and sl.dlss.dll / nvngx_dlss.dll in renodx-streamline.",
        .labels = {"Off", "On"},
        .is_enabled = []() { return forzahorizon2::streamline::IsActive(); },
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "DlaaJitterSign",
        .binding = &forzahorizon2::jitter::jitter_sign_setting,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "DLAA Jitter Sign (debug)",
        .section = "DLAA",
        .tooltip = "Sign of the sub-pixel jitter reported to DLSS. Pick the option with the sharpest, most stable image.",
        .labels = {"Default", "Flip X", "Flip Y", "Flip Both"},
        .is_enabled = []() { return forzahorizon2::jitter::dlaa_setting != 0.f; },
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "DlaaJitterTest",
        .binding = &forzahorizon2::jitter::jitter_setting,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .can_reset = false,
        .label = "DLAA Jitter (test)",
        .section = "DLAA",
        .tooltip = "Development test for DLAA: sub-pixel jitters scene rendering every frame (shimmers without DLAA).",
        .labels = {"Off", "On"},
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "ReflexFrameLimit",
        .binding = &forzahorizon2::streamline::reflex_frame_limit_setting,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Reflex Frame Limit",
        .section = "Frame Generation",
        .tooltip = "Lets Reflex pace rendered frames at 60 fps (the game simulation is always 60).\nCompare frame-time evenness with Off, where only Azimuth paces.",
        .labels = {"Off", "60 fps"},
        .is_enabled = []() { return forzahorizon2::streamline::IsActive(); },
        .is_visible = []() { return forzahorizon2::streamline::initialized; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Recommended",
        .section = "Options",
        .group = "button-line-1",
        .tooltip = "Tuned FH2 look: slightly more contrast, saturation and blowout than the accuracy-first defaults."
                   "\nLeaves brightness and EOTF settings untouched.",
        .on_change = []() {
          renodx::utils::settings::UpdateSettings({
              {"ToneMapType", 1.f},
              {"ToneMapWorkingColorSpace", 3.f},
              {"ColorGradeExposure", 1.f},
              {"ColorGradeHighlights", 50.f},
              {"ColorGradeShadows", 52.f},
              {"ColorGradeContrast", 53.f},
              {"ColorGradeSaturation", 55.f},
              {"ColorGradeBlowout", 10.f},
              {"ColorGradeScene", 100.f},
          });
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Reset All",
        .section = "Options",
        .group = "button-line-1",
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
  present_count.fetch_add(1u);
  forzahorizon2::jitter::OnPresent();
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
        // DLSS input passes restore Azimuth's command list state from tracked snapshots; only pay for that
        // tracking when the Streamline runtime is installed.
        std::array<wchar_t, MAX_PATH> executable_path = {};
        GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
        renodx::utils::state::use_snapshot = std::filesystem::exists(
            std::filesystem::path(executable_path.data()).parent_path() / forzahorizon2::streamline::PLUGIN_FOLDER_NAME / L"sl.interposer.dll");
        initialized = true;
      }

      // Before mods::shader, so draws are recorded before it replays them for on_drawn callbacks.
      reshade::register_event<reshade::addon_event::draw>(OnDraw);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(forzahorizon2::jitter::OnBindRenderTargetsAndDepthStencil);
      reshade::register_event<reshade::addon_event::bind_viewports>(forzahorizon2::jitter::OnBindViewports);
      reshade::register_event<reshade::addon_event::reset_command_list>(forzahorizon2::jitter::OnResetCommandList);
      reshade::register_event<reshade::addon_event::destroy_resource>(forzahorizon2::jitter::OnDestroyResource);
      reshade::register_event<reshade::addon_event::init_swapchain>(forzahorizon2::jitter::OnInitSwapchain);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(OnDrawOrDispatchIndirect);
      // Module events first so their per-device data exists when this addon's init_device runs.
      renodx::utils::resource::upgrade::Use(fdw_reason);
      renodx::utils::swapchain::Use(fdw_reason);
      renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

      renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
      SyncSwapChainOutputPreset();
      reshade::register_event<reshade::addon_event::create_device>(forzahorizon2::streamline::OnCreateDevice);
      reshade::register_event<reshade::addon_event::init_device>(forzahorizon2::streamline::OnInitDevice);
      reshade::register_event<reshade::addon_event::present>(forzahorizon2::streamline::OnPresent);
      reshade::register_event<reshade::addon_event::finish_present>(forzahorizon2::streamline::OnFinishPresent);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(forzahorizon2::streamline::OnDestroySwapchain);
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
      reshade::unregister_event<reshade::addon_event::create_device>(forzahorizon2::streamline::OnCreateDevice);
      reshade::unregister_event<reshade::addon_event::init_device>(forzahorizon2::streamline::OnInitDevice);
      reshade::unregister_event<reshade::addon_event::present>(forzahorizon2::streamline::OnPresent);
      reshade::unregister_event<reshade::addon_event::finish_present>(forzahorizon2::streamline::OnFinishPresent);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(forzahorizon2::streamline::OnDestroySwapchain);
      forzahorizon2::streamline::Uninstall();
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::draw>(OnDraw);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(forzahorizon2::jitter::OnBindRenderTargetsAndDepthStencil);
      reshade::unregister_event<reshade::addon_event::bind_viewports>(forzahorizon2::jitter::OnBindViewports);
      reshade::unregister_event<reshade::addon_event::reset_command_list>(forzahorizon2::jitter::OnResetCommandList);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(forzahorizon2::jitter::OnDestroyResource);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(forzahorizon2::jitter::OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(OnDrawOrDispatchIndirect);
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