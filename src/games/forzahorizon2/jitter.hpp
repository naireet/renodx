/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

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
inline constexpr uint32_t PHASE_COUNT = 16u;

inline uint32_t swap_chain_width = 0u;
inline uint32_t swap_chain_height = 0u;
inline uint32_t frame_index = 0u;

struct CommandListState {
  bool main_depth_bound = false;
  bool has_viewport = false;
  reshade::api::viewport viewport = {};
};

inline renodx::utils::data::ParallelFlatHashMap<reshade::api::command_list*, CommandListState, std::shared_mutex> command_list_states;
inline renodx::utils::data::ParallelFlatHashMap<uint64_t, bool, std::shared_mutex> main_depth_by_view;

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
  return jitter_setting != 0.f && swap_chain_width != 0u;
}

inline void ApplyViewport(reshade::api::command_list* cmd_list, const CommandListState& state) {
  if (!state.has_viewport) return;
  reshade::api::viewport viewport = state.viewport;
  if (state.main_depth_bound) {
    const auto offset = CurrentOffset();
    viewport.x += offset[0];
    viewport.y += offset[1];
  }
  cmd_list->bind_viewports(0u, 1u, &viewport);
}

inline bool IsMainDepth(reshade::api::device* device, reshade::api::resource_view dsv) {
  if (dsv.handle == 0u) return false;
  bool is_main = false;
  if (main_depth_by_view.if_contains(dsv.handle, [&](const auto& entry) { is_main = entry.second; })) return is_main;
  const auto desc = device->get_resource_desc(device->get_resource_from_view(dsv));
  is_main = desc.type == reshade::api::resource_type::texture_2d
            && desc.texture.width == swap_chain_width
            && desc.texture.height == swap_chain_height
            && desc.texture.samples == 1u;
  main_depth_by_view.insert_or_assign(dsv.handle, is_main);
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
    ApplyViewport(cmd_list, changed.value());
  }
}

inline void OnBindViewports(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count, const reshade::api::viewport* viewports) {
  if (!IsEnabled() || first != 0u || count == 0u) return;
  CommandListState state = {};
  command_list_states.lazy_emplace_l(
      cmd_list,
      [&](auto& entry) {
        entry.second.has_viewport = true;
        entry.second.viewport = viewports[0];
        state = entry.second;
      },
      [&](const auto& construct) {
        state = {.has_viewport = true, .viewport = viewports[0]};
        construct(cmd_list, state);
      });
  if (state.main_depth_bound) {
    ApplyViewport(cmd_list, state);
  }
}

inline void OnResetCommandList(reshade::api::command_list* cmd_list) {
  command_list_states.erase(cmd_list);
}

inline void OnDestroyResourceView(reshade::api::device* /*device*/, reshade::api::resource_view view) {
  main_depth_by_view.erase(view.handle);
}

inline void OnInitSwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_current_back_buffer());
  swap_chain_width = desc.texture.width;
  swap_chain_height = desc.texture.height;
  main_depth_by_view.clear();
}

inline void OnPresent() {
  ++frame_index;
}

}  // namespace forzahorizon2::jitter
