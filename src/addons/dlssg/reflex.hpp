/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>

#include "../../utils/detour.hpp"
#include "../../utils/log.hpp"

// The game's own NVAPI Reflex (NvAPI_D3D_SetSleepMode / Sleep / SetLatencyMarker). DLSS-G cannot use a native
// Reflex integration (Streamline's DLSS-G guide, section 8), so G1 translates these calls into sl.reflex/sl.pcl and
// stops forwarding them to NVAPI: exactly one Reflex, driven by the game's own markers and frame ids.
// G0 only observes: every call is forwarded unchanged and logged with its caller module.
namespace dlssg::reflex {

// Interface ids from NVIDIA's nvapi_interface.h.
inline constexpr uint32_t SET_SLEEP_MODE_ID = 0xAC1CA9E0u;
inline constexpr uint32_t SLEEP_ID = 0x852CD1D2u;
inline constexpr uint32_t SET_LATENCY_MARKER_ID = 0xD9984C05u;
inline constexpr uint32_t MARKER_TYPE_COUNT = 16u;

// Mirrors NV_SET_SLEEP_MODE_PARAMS_V1 (NvBool is NvU8).
struct SleepModeParams {
  uint32_t version;
  uint8_t low_latency_mode;
  uint8_t low_latency_boost;
  uint32_t minimum_interval_us;
  uint8_t use_markers_to_optimize;
  uint8_t reserved[31];
};

// Mirrors NV_LATENCY_MARKER_PARAMS_V1; marker types match sl::PCLMarker (simulation start = 0 ... present end = 5).
struct LatencyMarkerParams {
  uint32_t version;
  uint64_t frame_id;
  int32_t marker_type;
  uint64_t reserved0;
  uint8_t reserved[56];
};

static_assert(sizeof(SleepModeParams) == 44u && offsetof(SleepModeParams, minimum_interval_us) == 8u);
static_assert(sizeof(LatencyMarkerParams) == 88u && offsetof(LatencyMarkerParams, frame_id) == 8u && offsetof(LatencyMarkerParams, marker_type) == 16u);

using QueryInterfaceFunction = void*(__cdecl*)(uint32_t id);
using SetSleepModeFunction = int(__cdecl*)(IUnknown* device, SleepModeParams* params);
using SleepFunction = int(__cdecl*)(IUnknown* device);
using SetLatencyMarkerFunction = int(__cdecl*)(IUnknown* device, LatencyMarkerParams* params);

inline SetSleepModeFunction original_set_sleep_mode = nullptr;
inline SleepFunction original_sleep = nullptr;
inline SetLatencyMarkerFunction original_set_latency_marker = nullptr;
inline bool installed = false;
inline HMODULE game_module = nullptr;
inline std::atomic<uint64_t> game_sleep_calls = 0u;
inline std::atomic<uint64_t> game_marker_calls = 0u;
inline std::atomic<uint64_t> other_calls = 0u;
inline std::atomic<uint32_t> other_call_log_count = 0u;
inline std::atomic<uint32_t> marker_log_count = 0u;
inline std::array<std::atomic<uint64_t>, MARKER_TYPE_COUNT> marker_counts = {};
// Last mode the game requested: 0 = never set, 1 = off, 2 = on, 3 = on + boost.
inline std::atomic<uint32_t> game_reflex_mode = 0u;
inline std::atomic<uint32_t> game_minimum_interval_us = 0u;

// Calls from the game executable are the game's Reflex; anything else (Streamline's sl.reflex, overlays) is not.
inline bool IsGameCaller(void* return_address, const char* function) {
  HMODULE caller = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      static_cast<LPCWSTR>(return_address), &caller);
  if (caller == game_module) return true;
  other_calls.fetch_add(1u);
  if (other_call_log_count.fetch_add(1u) < 8u) {
    std::array<wchar_t, MAX_PATH> path = {};
    GetModuleFileNameW(caller, path.data(), static_cast<DWORD>(path.size()));
    renodx::utils::log::i("dlssg: ", function, " called from ", std::filesystem::path(path.data()).filename().string(), " (not the game).");
  }
  return false;
}

inline int __cdecl OnSetSleepMode(IUnknown* device, SleepModeParams* params) {
  if (IsGameCaller(_ReturnAddress(), "NvAPI_D3D_SetSleepMode") && params != nullptr) {
    const uint32_t mode = (params->low_latency_mode == 0u ? 1u : (params->low_latency_boost == 0u ? 2u : 3u));
    const uint32_t previous = game_reflex_mode.exchange(mode);
    const uint32_t previous_interval = game_minimum_interval_us.exchange(params->minimum_interval_us);
    if (previous != mode || previous_interval != params->minimum_interval_us) {
      renodx::utils::log::i(
          "dlssg: game Reflex sleep mode: low latency ", static_cast<uint32_t>(params->low_latency_mode),
          ", boost ", static_cast<uint32_t>(params->low_latency_boost),
          ", minimum interval ", params->minimum_interval_us, " us, use markers ", static_cast<uint32_t>(params->use_markers_to_optimize));
    }
  }
  return original_set_sleep_mode(device, params);
}

inline int __cdecl OnSleep(IUnknown* device) {
  if (IsGameCaller(_ReturnAddress(), "NvAPI_D3D_Sleep")) {
    game_sleep_calls.fetch_add(1u);
  }
  return original_sleep(device);
}

inline int __cdecl OnSetLatencyMarker(IUnknown* device, LatencyMarkerParams* params) {
  if (IsGameCaller(_ReturnAddress(), "NvAPI_D3D_SetLatencyMarker") && params != nullptr) {
    game_marker_calls.fetch_add(1u);
    if (params->marker_type >= 0 && static_cast<uint32_t>(params->marker_type) < MARKER_TYPE_COUNT) {
      marker_counts[params->marker_type].fetch_add(1u);
    }
    if (marker_log_count.fetch_add(1u) < 24u) {
      renodx::utils::log::i("dlssg: game Reflex marker ", params->marker_type, " frame ", params->frame_id, " on thread ", GetCurrentThreadId());
    }
  }
  return original_set_latency_marker(device, params);
}

// NVAPI hands out these entry points through nvapi_QueryInterface; detouring the functions themselves also catches
// pointers the game resolved before this add-on loaded. Process lifetime: the add-on is pinned.
inline void Install() {
  if (installed) return;
  installed = true;
  game_module = GetModuleHandleW(nullptr);
  HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
  const auto query_interface = (nvapi != nullptr ? reinterpret_cast<QueryInterfaceFunction>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr);
  if (query_interface == nullptr) {
    renodx::utils::log::w("dlssg: nvapi64.dll unavailable; game Reflex not observed.");
    return;
  }
  original_set_sleep_mode = reinterpret_cast<SetSleepModeFunction>(query_interface(SET_SLEEP_MODE_ID));
  original_sleep = reinterpret_cast<SleepFunction>(query_interface(SLEEP_ID));
  original_set_latency_marker = reinterpret_cast<SetLatencyMarkerFunction>(query_interface(SET_LATENCY_MARKER_ID));
  if (original_set_sleep_mode == nullptr || original_sleep == nullptr || original_set_latency_marker == nullptr) {
    renodx::utils::log::w("dlssg: NVAPI Reflex entry points unavailable (driver too old?); game Reflex not observed.");
    original_set_sleep_mode = nullptr;
    original_sleep = nullptr;
    original_set_latency_marker = nullptr;
    return;
  }
  const std::array functions = {
      renodx::utils::detour::Function(&original_set_sleep_mode, &OnSetSleepMode),
      renodx::utils::detour::Function(&original_sleep, &OnSleep),
      renodx::utils::detour::Function(&original_set_latency_marker, &OnSetLatencyMarker),
  };
  try {
    renodx::utils::detour::Install(functions);
    renodx::utils::log::i("dlssg: observing the game's NVAPI Reflex calls.");
  } catch (const std::exception& error) {
    renodx::utils::log::e("dlssg: hooking NVAPI Reflex failed: ", error.what());
  }
}


}  // namespace dlssg::reflex
