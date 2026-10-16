/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <d3d11.h>
#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include "../../utils/data.hpp"
#include "../../utils/detour.hpp"
#include "../../utils/dlss/nvngx.hpp"
#include "../../utils/log.hpp"

// The game's own DLSS Super Resolution evaluate is the generic source of DLSS-G inputs (depth, motion vectors,
// jitter, render extents, MV scale, reset). NGX maps its DLSS snippet (nvngx_dlss.dll) with LoadLibraryExW and calls
// its exports right after, so the snippet is hooked as LoadLibraryExW returns: outside the loader lock, and before
// the feature's CreateFeature. G0 only observes and logs.
namespace dlssg::ngx {

inline constexpr std::wstring_view SNIPPET_FILE_STEM = L"nvngx_dlss";
inline constexpr DWORD DATA_ONLY_LOAD_FLAGS = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

using LoadLibraryExWFunction = HMODULE(WINAPI*)(LPCWSTR name, HANDLE file, DWORD flags);
using FreeLibraryFunction = BOOL(WINAPI*)(HMODULE module);

struct FeatureInfo {
  int create_flags = 0;
  unsigned int render_width = 0u;
  unsigned int render_height = 0u;
  unsigned int output_width = 0u;
  unsigned int output_height = 0u;
  int quality = 0;
};

// What G1 derives the DLSS-G constants and extents from, as seen on the latest evaluate.
struct EvaluateSummary {
  unsigned int render_width = 0u;
  unsigned int render_height = 0u;
  unsigned int output_width = 0u;
  unsigned int output_height = 0u;
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  float mv_scale_x = 0.f;
  float mv_scale_y = 0.f;
  int create_flags = 0;
  int reset = 0;
  bool create_seen = false;
};

inline LoadLibraryExWFunction original_load_library_ex_w = nullptr;
inline FreeLibraryFunction original_free_library = nullptr;
inline std::shared_mutex snippet_mutex;
inline std::atomic<HMODULE> hooked_snippet = nullptr;
inline std::vector<renodx::utils::detour::Export> snippet_exports;
// Only the DLSS SR snippet is hooked, so every handle evaluated through it is a super sampling feature.
inline renodx::utils::data::ParallelFlatHashMap<const NVSDK_NGX_Handle*, FeatureInfo, std::shared_mutex> features;
// Every evaluate entering the hook, before any filtering: tells "hook never entered" apart from "create missed".
inline std::atomic<uint64_t> evaluate_count = 0u;
inline std::atomic<uint64_t> unknown_handle_evaluate_count = 0u;
inline std::atomic<uint32_t> evaluate_log_count = 0u;
inline std::atomic_bool evaluated_since_present = false;
inline std::shared_mutex summary_mutex;
inline EvaluateSummary last_summary = {};

inline void OnCreateFeature(NVSDK_NGX_Feature feature_id, const NVSDK_NGX_Parameter* parameters, const NVSDK_NGX_Handle* handle) {
  if (parameters == nullptr) return;
  FeatureInfo info = {};
  parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &info.create_flags);
  parameters->Get(NVSDK_NGX_Parameter_Width, &info.render_width);
  parameters->Get(NVSDK_NGX_Parameter_Height, &info.render_height);
  parameters->Get(NVSDK_NGX_Parameter_OutWidth, &info.output_width);
  parameters->Get(NVSDK_NGX_Parameter_OutHeight, &info.output_height);
  parameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &info.quality);
  features.insert_or_assign(handle, info);
  renodx::utils::log::i(
      "dlssg: NGX CreateFeature(feature ", static_cast<int>(feature_id), "): render ", info.render_width, "x", info.render_height,
      ", output ", info.output_width, "x", info.output_height, ", quality ", info.quality,
      ", flags ", renodx::utils::log::AsHex(static_cast<uint32_t>(info.create_flags)),
      " (HDR ", (info.create_flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0,
      ", MVLowRes ", (info.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0,
      ", MVJittered ", (info.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0,
      ", DepthInverted ", (info.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0,
      ", AutoExposure ", (info.create_flags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0, ")");
}

template <typename CommandList, typename Resource>
inline void OnEvaluateFeature(CommandList* /*command_list*/, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) {
  const uint64_t evaluate_index = evaluate_count.fetch_add(1u) + 1u;
  evaluated_since_present = true;
  if (parameters == nullptr) return;

  FeatureInfo info = {};
  const bool create_seen = features.if_contains(handle, [&info](const auto& entry) {
    const auto& [feature_handle, feature_info] = entry;
    info = feature_info;
  });
  if (!create_seen) {
    unknown_handle_evaluate_count.fetch_add(1u);
  }
  EvaluateSummary summary = {
      .output_width = info.output_width,
      .output_height = info.output_height,
      .create_flags = info.create_flags,
      .create_seen = create_seen,
  };
  parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &summary.render_width);
  parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &summary.render_height);
  if (summary.render_width == 0u || summary.render_height == 0u) {
    summary.render_width = info.render_width;
    summary.render_height = info.render_height;
  }
  parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &summary.jitter_x);
  parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &summary.jitter_y);
  parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &summary.mv_scale_x);
  parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &summary.mv_scale_y);
  parameters->Get(NVSDK_NGX_Parameter_Reset, &summary.reset);

  bool extents_changed = false;
  {
    const std::unique_lock lock(summary_mutex);
    extents_changed = summary.render_width != last_summary.render_width || summary.render_height != last_summary.render_height
                      || summary.output_width != last_summary.output_width || summary.output_height != last_summary.output_height;
    last_summary = summary;
  }
  if ((evaluate_index > 3u && !extents_changed && summary.reset == 0) || evaluate_log_count.fetch_add(1u) >= 40u) return;

  const auto describe_resource = [parameters](const char* name) -> std::string {
    Resource* resource = nullptr;
    if (NVSDK_NGX_FAILED(parameters->Get(name, &resource)) || resource == nullptr) {
      void* untyped = nullptr;
      if (NVSDK_NGX_FAILED(parameters->Get(name, &untyped)) || untyped == nullptr) return "none";
      resource = static_cast<Resource*>(untyped);
    }
    if constexpr (std::is_same_v<Resource, ID3D12Resource>) {
      const D3D12_RESOURCE_DESC desc = resource->GetDesc();
      return renodx::utils::log::BuildString(desc.Width, "x", desc.Height, " fmt ", static_cast<uint32_t>(desc.Format));
    } else {
      ID3D11Texture2D* texture = nullptr;
      if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return "not a 2D texture";
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      texture->Release();
      return renodx::utils::log::BuildString(
          desc.Width, "x", desc.Height, " fmt ", static_cast<uint32_t>(desc.Format), " bind ",
          renodx::utils::log::AsHex(desc.BindFlags), " misc ", renodx::utils::log::AsHex(desc.MiscFlags));
    }
  };
  renodx::utils::log::i(
      "dlssg: NGX DLSS SR evaluate #", evaluate_index, " on thread ", GetCurrentThreadId(),
      (create_seen ? "" : " (create not seen)"),
      ": render ", summary.render_width, "x", summary.render_height, ", output ", summary.output_width, "x", summary.output_height,
      ", jitter ", summary.jitter_x, ",", summary.jitter_y, ", MV scale ", summary.mv_scale_x, ",", summary.mv_scale_y,
      ", reset ", summary.reset,
      "; color ", describe_resource(NVSDK_NGX_Parameter_Color),
      ", depth ", describe_resource(NVSDK_NGX_Parameter_Depth),
      ", MVs ", describe_resource(NVSDK_NGX_Parameter_MotionVectors),
      ", output ", describe_resource(NVSDK_NGX_Parameter_Output),
      ", exposure ", describe_resource(NVSDK_NGX_Parameter_ExposureTexture));
}

inline void HookSnippet(HMODULE module) {
  const std::unique_lock lock(snippet_mutex);
  if (hooked_snippet == module) return;
  std::array<wchar_t, MAX_PATH> path = {};
  GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  const std::string file_name = std::filesystem::path(path.data()).filename().string();
  if (hooked_snippet != nullptr) {
    // The previously hooked copy was unloaded behind FreeLibrary's back; its patched code is gone with it.
    for (auto& export_function : snippet_exports) {
      *export_function.original = nullptr;
    }
    features.clear();
    hooked_snippet = nullptr;
  }
  if (snippet_exports.empty()) {
    for (const auto& [name, original, replacement] : renodx::utils::dlss::nvngx::DLSS_HOOKS) {
      snippet_exports.emplace_back(name, original, replacement);
    }
  }
  try {
    const auto result = renodx::utils::detour::Install(module, snippet_exports);
    namespace nvngx = renodx::utils::dlss::nvngx;
    renodx::utils::log::i(
        "dlssg: hooked ", file_name, " (", std::filesystem::path(path.data()).parent_path().string(), "): ",
        result.installed, " installed, ", result.missing, " missing, ", result.failed, " failed; D3D12 CreateFeature ",
        nvngx::real_NVSDK_NGX_D3D12_CreateFeature != nullptr, ", EvaluateFeature ", nvngx::real_NVSDK_NGX_D3D12_EvaluateFeature != nullptr,
        ", EvaluateFeature_C ", nvngx::real_NVSDK_NGX_D3D12_EvaluateFeature_C != nullptr,
        "; D3D11 CreateFeature ", nvngx::real_NVSDK_NGX_D3D11_CreateFeature != nullptr,
        ", EvaluateFeature ", nvngx::real_NVSDK_NGX_D3D11_EvaluateFeature != nullptr);
    if (result.Active() != 0u) {
      hooked_snippet = module;
    }
  } catch (const std::exception& error) {
    renodx::utils::log::e("dlssg: hooking ", file_name, " failed: ", error.what());
  }
}

// Matches the game-folder DLL and NVIDIA's OTA/driver-override copies of the snippet, but not nvngx_dlssg/dlssd.
inline bool IsSnippet(std::wstring_view path) {
  const auto file_start = path.find_last_of(L"\\/");
  const std::wstring_view file_name = (file_start == std::wstring_view::npos ? path : path.substr(file_start + 1u));
  const auto extension_start = file_name.find_last_of(L'.');
  const std::wstring_view stem = (extension_start == std::wstring_view::npos ? file_name : file_name.substr(0u, extension_start));
  return stem.size() == SNIPPET_FILE_STEM.size() && _wcsnicmp(stem.data(), SNIPPET_FILE_STEM.data(), stem.size()) == 0;
}

inline HMODULE WINAPI OnLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  HMODULE module = original_load_library_ex_w(name, file, flags);
  if (module != nullptr && name != nullptr && (flags & DATA_ONLY_LOAD_FLAGS) == 0u && IsSnippet(name)) {
    HookSnippet(module);
  }
  return module;
}

inline BOOL WINAPI OnFreeLibrary(HMODULE module) {
  const BOOL result = original_free_library(module);
  if (module == nullptr || module != hooked_snippet.load()) return result;
  const std::unique_lock lock(snippet_mutex);
  HMODULE still_loaded = nullptr;
  if (module == hooked_snippet
      && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(module), &still_loaded)
             == FALSE) {
    for (auto& export_function : snippet_exports) {
      *export_function.original = nullptr;
    }
    features.clear();
    hooked_snippet = nullptr;
    renodx::utils::log::i("dlssg: DLSS snippet unloaded.");
  }
  return result;
}

// Process lifetime: the add-on is pinned, so these hooks are never removed.
inline void Install() {
  renodx::utils::dlss::nvngx::on_d3d12_create_feature = &OnCreateFeature;
  renodx::utils::dlss::nvngx::on_d3d12_evaluate_feature = &OnEvaluateFeature<ID3D12GraphicsCommandList, ID3D12Resource>;
  renodx::utils::dlss::nvngx::on_d3d11_create_feature = &OnCreateFeature;
  renodx::utils::dlss::nvngx::on_d3d11_evaluate_feature = &OnEvaluateFeature<ID3D11DeviceContext, ID3D11Resource>;
  HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  original_load_library_ex_w = reinterpret_cast<LoadLibraryExWFunction>(GetProcAddress(kernelbase, "LoadLibraryExW"));
  original_free_library = reinterpret_cast<FreeLibraryFunction>(GetProcAddress(kernelbase, "FreeLibrary"));
  const std::array functions = {
      renodx::utils::detour::Function(&original_load_library_ex_w, &OnLoadLibraryExW),
      renodx::utils::detour::Function(&original_free_library, &OnFreeLibrary),
  };
  try {
    renodx::utils::detour::Install(functions);
  } catch (const std::exception& error) {
    renodx::utils::log::e("dlssg: hooking LoadLibraryExW/FreeLibrary failed (", error.what(), "); DLSS inputs unavailable.");
  }
  // Normally mapped later, when the game creates its DLSS feature; an already-mapped snippet misses its creates.
  if (HMODULE module = GetModuleHandleW(L"nvngx_dlss.dll"); module != nullptr) {
    HookSnippet(module);
  }
}

}  // namespace dlssg::ngx