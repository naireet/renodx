/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <shared_mutex>

#include <include/reshade.hpp>

#include "../../utils/data.hpp"

// Sub-pixel jitter for DLSS (DLAA). Azimuth renders without projection jitter, so the viewport is offset instead:
// every viewport bound while the main (swapchain-sized) depth buffer is bound shifts by the frame's Halton
// offset. Equivalent to a projection jitter for rasterization; post passes, shadow maps and UI do not bind
// that depth buffer and stay unjittered. FH2's motion vectors (and ours) are computed from unjittered matrices.
namespace forzahorizon2::jitter {

// Off / On (test: jitter without DLAA visibly shimmers).
inline float jitter_setting = 0.f;
// DLAA (DLSS SR at 1.0x) needs the jitter, so it enables it too.
inline float dlaa_setting = 0.f;
// Sign of the jitter reported to DLSS: bit 0 flips X, bit 1 flips Y (tuning aid until the convention is confirmed).
inline float jitter_sign_setting = 0.f;
inline constexpr uint32_t PHASE_COUNT = 16u;

inline uint32_t swap_chain_width = 0u;
inline uint32_t swap_chain_height = 0u;
inline uint32_t frame_index = 0u;

inline constexpr uint32_t MAX_VIEWPORTS = 16u;

struct CommandListState {
  bool main_depth_bound = false;
  uint32_t viewport_count = 0u;
  std::array<reshade::api::viewport, MAX_VIEWPORTS> viewports = {};
};

inline renodx::utils::data::ParallelFlatHashMap<reshade::api::command_list*, CommandListState, std::shared_mutex> command_list_states;
// Keyed by resource: D3D12 descriptor slots are overwritten without destroy events, resources are not.
inline renodx::utils::data::ParallelFlatHashMap<uint64_t, bool, std::shared_mutex> main_depth_by_resource;

inline float Halton(uint32_t index, uint32_t base) {
  float fraction = 1.f;
  float result = 0.f;
  while (index > 0u) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

// Viewport offset in pixels for the current frame, in [-0.5, 0.5).
inline std::array<float, 2> CurrentOffset() {
  const uint32_t phase = (frame_index % PHASE_COUNT) + 1u;
  return {Halton(phase, 2u) - 0.5f, Halton(phase, 3u) - 0.5f};
}

inline bool IsEnabled() {
  return (jitter_setting != 0.f || dlaa_setting != 0.f) && swap_chain_width != 0u;
}

// D3D12 replaces the whole viewport array, so every viewport the game bound is rebound.
inline void ApplyViewports(reshade::api::command_list* cmd_list, const CommandListState& state) {
  if (state.viewport_count == 0u) return;
  std::array<reshade::api::viewport, MAX_VIEWPORTS> viewports = state.viewports;
  if (state.main_depth_bound) {
    const auto offset = CurrentOffset();
    for (uint32_t index = 0u; index < state.viewport_count; ++index) {
      viewports[index].x += offset[0];
      viewports[index].y += offset[1];
    }
  }
  cmd_list->bind_viewports(0u, state.viewport_count, viewports.data());
}

inline bool IsMainDepth(reshade::api::device* device, reshade::api::resource_view dsv) {
  if (dsv.handle == 0u) return false;
  const auto resource = device->get_resource_from_view(dsv);
  if (resource.handle == 0u) return false;
  bool is_main = false;
  if (main_depth_by_resource.if_contains(resource.handle, [&](const auto& entry) { is_main = entry.second; })) return is_main;
  const auto desc = device->get_resource_desc(resource);
  is_main = desc.type == reshade::api::resource_type::texture_2d
            && desc.texture.width == swap_chain_width
            && desc.texture.height == swap_chain_height
            && desc.texture.samples == 1u;
  main_depth_by_resource.insert_or_assign(resource.handle, is_main);
  return is_main;
}

inline void OnBindRenderTargetsAndDepthStencil(
    reshade::api::command_list* cmd_list, uint32_t /*count*/, const reshade::api::resource_view* /*rtvs*/, reshade::api::resource_view dsv) {
  if (!IsEnabled()) return;
  const bool main_depth_bound = IsMainDepth(cmd_list->get_device(), dsv);
  std::optional<CommandListState> changed;
  command_list_states.lazy_emplace_l(
      cmd_list,
      [&](auto& entry) {
        if (entry.second.main_depth_bound == main_depth_bound) return;
        entry.second.main_depth_bound = main_depth_bound;
        changed = entry.second;
      },
      [&](const auto& construct) { construct(cmd_list, CommandListState{.main_depth_bound = main_depth_bound}); });
  if (changed.has_value()) {
    ApplyViewports(cmd_list, changed.value());
  }
}

inline void OnBindViewports(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count, const reshade::api::viewport* viewports) {
  if (!IsEnabled() || first != 0u || count == 0u || count > MAX_VIEWPORTS) return;
  CommandListState state = {};
  const auto store = [&](CommandListState* target) {
    target->viewport_count = count;
    std::copy_n(viewports, count, target->viewports.begin());
  };
  command_list_states.lazy_emplace_l(
      cmd_list,
      [&](auto& entry) {
        store(&entry.second);
        state = entry.second;
      },
      [&](const auto& construct) {
        store(&state);
        construct(cmd_list, state);
      });
  if (state.main_depth_bound) {
    ApplyViewports(cmd_list, state);
  }
}

inline void OnResetCommandList(reshade::api::command_list* cmd_list) {
  command_list_states.erase(cmd_list);
}

inline void OnDestroyResource(reshade::api::device* /*device*/, reshade::api::resource resource) {
  main_depth_by_resource.erase(resource.handle);
}

inline void OnInitSwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_current_back_buffer());
  swap_chain_width = desc.texture.width;
  swap_chain_height = desc.texture.height;
  main_depth_by_resource.clear();
}

inline void OnPresent() {
  ++frame_index;
}

}  // namespace forzahorizon2::jitter
