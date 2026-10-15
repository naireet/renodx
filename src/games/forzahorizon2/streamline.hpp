/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>

#include <include/reshade.hpp>

#include <sl.h>
#include <sl_dlss.h>

#include "../../utils/dlss/streamline_inject.hpp"
#include "../../utils/log.hpp"

// FH2 adapter over the shared Streamline injector: Azimuth has no Reflex (markers are synthesized), FH2's own
// motion vectors/depth are tagged, and DLAA runs DLSS SR through Streamline.
namespace forzahorizon2::streamline {

namespace inject = renodx::utils::streamline::inject;

inline constexpr inject::Config CONFIG = {
    .plugin_folder_name = L"renodx-streamline",
    .project_id = "3f6b2c1e-8a47-4d59-9b3e-6c2d1f8e7a40",
    .engine_version = "Azimuth 0.9.7",
    .load_dlss = true,
    .hook_create_swap_chain = false,
    .synthesize_reflex = true,
};

inline std::atomic<uint32_t> tag_log_count = 0u;
inline std::atomic<uint32_t> constants_error_log_count = 0u;

// Called while recording the frame's tonemap draw. Both resources stay in shader-resource state until present.
inline void TagFrameInputs(
    reshade::api::command_list* cmd_list,
    reshade::api::resource motion_vectors,
    reshade::api::resource depth,
    uint32_t width,
    uint32_t height,
    float jitter_x,
    float jitter_y) {
  auto* token = inject::frame_token.load();
  if (!inject::IsActive() || token == nullptr) return;
  sl::Resource motion_vector_resource = {sl::ResourceType::eTex2d, reinterpret_cast<void*>(motion_vectors.handle), inject::SHADER_RESOURCE_STATE};
  sl::Resource depth_resource = {sl::ResourceType::eTex2d, reinterpret_cast<void*>(depth.handle), inject::SHADER_RESOURCE_STATE};
  const sl::Extent extent = {.top = 0u, .left = 0u, .width = width, .height = height};
  // Backbuffer tag carries only the full-swapchain extent (Streamline already owns the backbuffer).
  const sl::Extent backbuffer_extent = {.top = 0u, .left = 0u, .width = inject::swap_chain_width, .height = inject::swap_chain_height};
  const sl::ResourceTag tags[] = {
      {&depth_resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent},
      {&motion_vector_resource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent},
      {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &backbuffer_extent},
  };
  if (!inject::SetTags(token, tags, static_cast<uint32_t>(std::size(tags)), cmd_list->get_native(), "inputs")) return;
  inject::tagged_width = width;
  inject::tagged_height = height;
  // FH2 motion vectors are unjittered, native resolution, 1:1 scale; depth is standard (0 near, 1 far), per its
  // depth linearization in 0x34ADA64E.
  sl::Constants constants = inject::DefaultConstants();
  constants.jitterOffset = {jitter_x, jitter_y};
  constants.cameraAspectRatio = (height != 0u ? static_cast<float>(width) / static_cast<float>(height) : 16.f / 9.f);
  // Common constants must be set before any evaluate (DLAA) and before present (DLSS-G).
  if (const auto result = inject::api.set_constants(constants, *token, inject::VIEWPORT);
      result != sl::Result::eOk && constants_error_log_count.fetch_add(1u) < 3u) {
    renodx::utils::log::e("streamline: slSetConstants failed (", static_cast<uint32_t>(result), ").");
  }  inject::inputs_tagged = true;
  if (tag_log_count.fetch_add(1u) < 3u) {
    renodx::utils::log::i("streamline: tagged inputs for frame ", static_cast<uint32_t>(*token), " on thread ", GetCurrentThreadId());
  }
}

inline std::atomic<uint32_t> dlaa_error_log_count = 0u;
inline uint32_t dlaa_options_width = 0u;
inline uint32_t dlaa_options_height = 0u;

// DLSS Super Resolution at 1.0x (DLAA) on the jittered scene colour. Needs this frame's inputs tagged first
// (depth, motion vectors, common constants with the jitter). The caller restores command list state afterwards.
inline bool EvaluateDlaa(
    reshade::api::command_list* cmd_list,
    reshade::api::resource input,
    uint32_t input_state,
    reshade::api::resource output,
    uint32_t output_state,
    uint32_t width,
    uint32_t height) {
  auto* token = inject::frame_token.load();
  if (!inject::IsActive() || token == nullptr || !inject::inputs_tagged || inject::api.dlss_set_options == nullptr || inject::api.evaluate_feature == nullptr) return false;
  const auto log_error = [](const char* call, sl::Result result) {
    if (dlaa_error_log_count.fetch_add(1u) < 3u) {
      renodx::utils::log::e("streamline: ", call, " failed (", static_cast<uint32_t>(result), ").");
    }
  };

  if (dlaa_options_width != width || dlaa_options_height != height) {
    sl::DLSSOptions options = {};
    options.mode = sl::DLSSMode::eDLAA;
    options.outputWidth = width;
    options.outputHeight = height;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    // The scene colour is pre-exposure linear HDR; FH2 applies its adaptive exposure in the tonemap.
    options.useAutoExposure = sl::Boolean::eTrue;
    if (const auto result = inject::api.dlss_set_options(inject::VIEWPORT, options); result != sl::Result::eOk) {
      log_error("slDLSSSetOptions", result);
      return false;
    }
    dlaa_options_width = width;
    dlaa_options_height = height;
    renodx::utils::log::i("streamline: DLAA enabled at ", width, "x", height, ".");
  }

  sl::Resource input_resource = {sl::ResourceType::eTex2d, reinterpret_cast<void*>(input.handle), input_state};
  sl::Resource output_resource = {sl::ResourceType::eTex2d, reinterpret_cast<void*>(output.handle), output_state};
  const sl::Extent extent = {.top = 0u, .left = 0u, .width = width, .height = height};
  const sl::ResourceTag tags[] = {
      {&input_resource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &extent},
      {&output_resource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &extent},
  };
  auto* command_buffer = reinterpret_cast<sl::CommandBuffer*>(cmd_list->get_native());
  if (const auto result = inject::api.set_tag_for_frame(*token, inject::VIEWPORT, tags, static_cast<uint32_t>(std::size(tags)), command_buffer);
      result != sl::Result::eOk) {
    log_error("slSetTagForFrame(DLAA)", result);
    return false;
  }
  const sl::BaseStructure* inputs[] = {&inject::VIEWPORT};
  if (const auto result = inject::api.evaluate_feature(sl::kFeatureDLSS, *token, inputs, static_cast<uint32_t>(std::size(inputs)), command_buffer);
      result != sl::Result::eOk) {
    log_error("slEvaluateFeature(DLSS)", result);
    return false;
  }
  return true;
}

}  // namespace forzahorizon2::streamline