/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <utility>

#include <include/reshade.hpp>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include "../../utils/data.hpp"
#include "../../utils/log.hpp"
#include "../../utils/vtable.hpp"

// Streamline (DLSS-G) bootstrap for a game without native Streamline. Streamline's proxies sit below ReShade:
// ReShade wraps the Streamline swapchain, so ReShade effects and the RenoDX HDR10 output run before frame generation.
// ReShade's own device/queue wrappers keep their native objects (its interface upgrades would drop Streamline proxies,
// which only implement ID3D12Device..10 and ID3D12CommandQueue); instead the native CreateCommandQueue and
// CreateSwapChainForHwnd vtable slots are redirected through Streamline's proxies.
// Only active when NVIDIA's signed Streamline runtime is placed in <game>\renodx-streamline.
namespace forzahorizon2::streamline {

inline constexpr const wchar_t* PLUGIN_FOLDER_NAME = L"renodx-streamline";
// Custom-engine NGX identity (no NVIDIA application id).
inline constexpr const char* PROJECT_ID = "3f6b2c1e-8a47-4d59-9b3e-6c2d1f8e7a40";
inline constexpr const char* ENGINE_VERSION = "Azimuth 0.9.7";
inline constexpr std::size_t CREATE_COMMAND_QUEUE_VTABLE_INDEX = 8u;
inline constexpr std::size_t CREATE_SWAP_CHAIN_FOR_HWND_VTABLE_INDEX = 15u;
inline const sl::ViewportHandle VIEWPORT = {0u};

using CreateCommandQueueFunction = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device* device,
    const D3D12_COMMAND_QUEUE_DESC* desc,
    REFIID riid,
    void** command_queue);

using CreateSwapChainForHwndFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain);

struct Api {
  PFun_slInit* init = nullptr;
  PFun_slSetD3DDevice* set_d3d_device = nullptr;
  PFun_slUpgradeInterface* upgrade_interface = nullptr;
  PFun_slGetNativeInterface* get_native_interface = nullptr;
  PFun_slIsFeatureSupported* is_feature_supported = nullptr;
  PFun_slGetFeatureFunction* get_feature_function = nullptr;
  PFun_slGetNewFrameToken* get_new_frame_token = nullptr;
  PFun_slDLSSGSetOptions* dlssg_set_options = nullptr;
  PFun_slDLSSGGetState* dlssg_get_state = nullptr;
  PFun_slReflexSetOptions* reflex_set_options = nullptr;
  PFun_slReflexSleep* reflex_sleep = nullptr;
  PFun_slPCLSetMarker* pcl_set_marker = nullptr;
};

inline Api api = {};
inline bool initialized = false;
inline bool init_attempted = false;
inline bool feature_state_logged = false;
inline ID3D12Device* native_device = nullptr;
// Owned references; Streamline proxies stay alive for the process (Streamline is never unloaded).
inline ID3D12Device* proxy_device = nullptr;
inline IDXGIFactory2* proxy_factory = nullptr;
// Native queue -> Streamline queue proxy (owned reference), so swapchain creation gets the proxy even though
// ReShade's queue wrapper forwards the native queue.
inline renodx::utils::data::ParallelFlatHashMap<IUnknown*, IUnknown*, std::shared_mutex> proxy_queue_by_native_queue;
inline uint32_t frame_index = 0u;
inline sl::FrameToken* frame_token = nullptr;
inline std::atomic_bool swap_chain_created = false;
// Streamline's proxies call back into the native objects whose vtable slots are redirected here.
inline thread_local bool in_streamline_call = false;
inline CreateCommandQueueFunction original_create_command_queue = nullptr;
inline CreateSwapChainForHwndFunction original_create_swap_chain_for_hwnd = nullptr;
inline renodx::utils::vtable::Slot<CreateCommandQueueFunction> create_command_queue_slot = {};
inline renodx::utils::vtable::Slot<CreateSwapChainForHwndFunction> create_swap_chain_for_hwnd_slot = {};

inline void OnStreamlineLog(sl::LogType type, const char* message) {
  switch (type) {
    case sl::LogType::eError: renodx::utils::log::e("streamline: ", message); break;
    case sl::LogType::eWarn:  renodx::utils::log::w("streamline: ", message); break;
    default:                  renodx::utils::log::d("streamline: ", message); break;
  }
}

inline HRESULT STDMETHODCALLTYPE OnCreateCommandQueue(
    ID3D12Device* device,
    const D3D12_COMMAND_QUEUE_DESC* desc,
    REFIID riid,
    void** command_queue) {
  if (in_streamline_call || proxy_device == nullptr || device != native_device || command_queue == nullptr) {
    return original_create_command_queue(device, desc, riid, command_queue);
  }

  in_streamline_call = true;
  const HRESULT result = proxy_device->CreateCommandQueue(desc, riid, command_queue);
  in_streamline_call = false;
  if (FAILED(result)) {
    renodx::utils::log::e("streamline: CreateCommandQueue through proxy failed: ", renodx::utils::log::AsHex(static_cast<uint32_t>(result)));
    return original_create_command_queue(device, desc, riid, command_queue);
  }

  // ReShade keeps the native queue (its ID3D12CommandQueue1 upgrade bypasses the proxy); the proxy stays owned here.
  auto* queue_proxy = static_cast<IUnknown*>(*command_queue);
  void* queue_native = nullptr;
  if (api.get_native_interface(queue_proxy, &queue_native) != sl::Result::eOk || queue_native == nullptr || queue_native == queue_proxy) {
    renodx::utils::log::w("streamline: CreateCommandQueue returned a non-proxy queue (type ", static_cast<uint32_t>(desc->Type), ").");
    return result;
  }
  proxy_queue_by_native_queue.insert_or_assign(static_cast<IUnknown*>(queue_native), queue_proxy);
  *command_queue = queue_native;
  renodx::utils::log::i("streamline: command queue type ", static_cast<uint32_t>(desc->Type), " created through proxy.");
  return result;
}

inline HRESULT STDMETHODCALLTYPE OnCreateSwapChainForHwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain) {
  if (!initialized || in_streamline_call) {
    return original_create_swap_chain_for_hwnd(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);
  }

  if (proxy_factory == nullptr) {
    void* upgraded_factory = factory;
    if (const auto result = api.upgrade_interface(&upgraded_factory); result != sl::Result::eOk) {
      renodx::utils::log::e("streamline: factory upgrade failed (", static_cast<uint32_t>(result), "); using the native swapchain.");
      return original_create_swap_chain_for_hwnd(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);
    }
    proxy_factory = static_cast<IDXGIFactory2*>(upgraded_factory);
  }

  IUnknown* present_queue = device;
  proxy_queue_by_native_queue.if_contains(device, [&present_queue](const auto& entry) {
    present_queue = entry.second;
  });

  in_streamline_call = true;
  const HRESULT result = proxy_factory->CreateSwapChainForHwnd(present_queue, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);
  in_streamline_call = false;

  renodx::utils::log::i(
      "streamline: CreateSwapChainForHwnd through proxy: hr=", renodx::utils::log::AsHex(static_cast<uint32_t>(result)),
      ", queue proxy ", (present_queue != device ? "yes" : "no"),
      ", ", desc->Width, "x", desc->Height, ", format ", static_cast<uint32_t>(desc->Format),
      ", buffers ", desc->BufferCount, ", flags ", renodx::utils::log::AsHex(desc->Flags));
  if (FAILED(result)) {
    return original_create_swap_chain_for_hwnd(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);
  }
  swap_chain_created = true;
  return result;
}

// ReShade loads add-ons inside D3D12CreateDevice, so create_device runs before the native device or any swapchain exists.
inline bool OnCreateDevice(reshade::api::device_api device_api, uint32_t& /*api_version*/) {
  if (device_api != reshade::api::device_api::d3d12 || init_attempted) return false;
  init_attempted = true;

  std::array<wchar_t, MAX_PATH> executable_path = {};
  GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
  const auto plugin_folder = std::filesystem::path(executable_path.data()).parent_path() / PLUGIN_FOLDER_NAME;
  const auto interposer_path = plugin_folder / L"sl.interposer.dll";
  if (!std::filesystem::exists(interposer_path)) {
    renodx::utils::log::i("streamline: ", plugin_folder.string(), " not found; frame generation disabled.");
    return false;
  }
  if (!sl::security::verifyEmbeddedSignature(interposer_path.c_str())) {
    renodx::utils::log::e("streamline: sl.interposer.dll is not signed by NVIDIA; refusing to load it.");
    return false;
  }

  HMODULE interposer = LoadLibraryW(interposer_path.c_str());
  if (interposer == nullptr) {
    renodx::utils::log::e("streamline: LoadLibrary(sl.interposer.dll) failed: ", GetLastError());
    return false;
  }
  api.init = reinterpret_cast<PFun_slInit*>(GetProcAddress(interposer, "slInit"));
  api.set_d3d_device = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(interposer, "slSetD3DDevice"));
  api.upgrade_interface = reinterpret_cast<PFun_slUpgradeInterface*>(GetProcAddress(interposer, "slUpgradeInterface"));
  api.get_native_interface = reinterpret_cast<PFun_slGetNativeInterface*>(GetProcAddress(interposer, "slGetNativeInterface"));
  api.is_feature_supported = reinterpret_cast<PFun_slIsFeatureSupported*>(GetProcAddress(interposer, "slIsFeatureSupported"));
  api.get_feature_function = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(interposer, "slGetFeatureFunction"));
  api.get_new_frame_token = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(interposer, "slGetNewFrameToken"));
  if (api.init == nullptr || api.set_d3d_device == nullptr || api.upgrade_interface == nullptr || api.get_native_interface == nullptr
      || api.is_feature_supported == nullptr || api.get_feature_function == nullptr || api.get_new_frame_token == nullptr) {
    renodx::utils::log::e("streamline: sl.interposer.dll is missing core exports.");
    return false;
  }

  const std::wstring plugin_folder_string = plugin_folder.wstring();
  const wchar_t* plugin_paths[] = {plugin_folder_string.c_str()};
  const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  sl::Preferences preferences = {};
  preferences.logLevel = sl::LogLevel::eDefault;
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1u;
  preferences.pathToLogsAndData = plugin_folder_string.c_str();
  preferences.logMessageCallback = &OnStreamlineLog;
  // No OTA: Azimuth is firewalled and only the vetted local runtime may load.
  preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking
                      | sl::PreferenceFlags::eUseManualHooking
                      | sl::PreferenceFlags::eUseDXGIFactoryProxy;
  preferences.featuresToLoad = features;
  preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = ENGINE_VERSION;
  preferences.projectId = PROJECT_ID;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  if (const auto result = api.init(preferences, sl::kSDKVersion); result != sl::Result::eOk) {
    renodx::utils::log::e("streamline: slInit failed (", static_cast<uint32_t>(result), ").");
    return false;
  }

  // Every native DXGI factory shares this vtable, including the one ReShade forwards Azimuth's swapchain creation to.
  std::array<wchar_t, MAX_PATH> system_folder = {};
  GetSystemDirectoryW(system_folder.data(), static_cast<UINT>(system_folder.size()));
  const auto system_dxgi_path = std::filesystem::path(system_folder.data()) / L"dxgi.dll";
  const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory2)>(
      GetProcAddress(LoadLibraryW(system_dxgi_path.c_str()), "CreateDXGIFactory2"));
  IDXGIFactory2* probe_factory = nullptr;
  if (create_factory == nullptr || FAILED(create_factory(0u, IID_PPV_ARGS(&probe_factory)))) {
    renodx::utils::log::e("streamline: could not create a native DXGI factory to hook.");
    return false;
  }
  create_swap_chain_for_hwnd_slot = {
      .object = probe_factory,
      .index = CREATE_SWAP_CHAIN_FOR_HWND_VTABLE_INDEX,
      .original = &original_create_swap_chain_for_hwnd,
      .replacement = &OnCreateSwapChainForHwnd,
  };
  try {
    renodx::utils::vtable::Install(&create_swap_chain_for_hwnd_slot);
  } catch (const std::exception& error) {
    probe_factory->Release();
    renodx::utils::log::e("streamline: hooking CreateSwapChainForHwnd failed: ", error.what());
    return false;
  }
  probe_factory->Release();

  initialized = true;
  renodx::utils::log::i("streamline: initialized (manual hooking) from ", plugin_folder.string());
  return false;
}

// Runs before the game gets the device back, so every game queue is created after the CreateCommandQueue redirect.
inline void OnInitDevice(reshade::api::device* device) {
  if (!initialized || native_device != nullptr || device->get_api() != reshade::api::device_api::d3d12) return;
  native_device = reinterpret_cast<ID3D12Device*>(device->get_native());
  if (const auto result = api.set_d3d_device(native_device); result != sl::Result::eOk) {
    renodx::utils::log::e("streamline: slSetD3DDevice failed (", static_cast<uint32_t>(result), ").");
  }

  void* upgraded_device = native_device;
  if (const auto result = api.upgrade_interface(&upgraded_device); result != sl::Result::eOk || upgraded_device == native_device) {
    renodx::utils::log::e("streamline: device upgrade failed (", static_cast<uint32_t>(result), ").");
  } else {
    proxy_device = static_cast<ID3D12Device*>(upgraded_device);
    create_command_queue_slot = {
        .object = native_device,
        .index = CREATE_COMMAND_QUEUE_VTABLE_INDEX,
        .original = &original_create_command_queue,
        .replacement = &OnCreateCommandQueue,
    };
    try {
      renodx::utils::vtable::Install(&create_command_queue_slot);
    } catch (const std::exception& error) {
      renodx::utils::log::e("streamline: hooking CreateCommandQueue failed: ", error.what());
    }
  }

  api.get_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(api.dlssg_set_options));
  api.get_feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(api.dlssg_get_state));
  api.get_feature_function(sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&>(api.reflex_set_options));
  api.get_feature_function(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&>(api.reflex_sleep));
  api.get_feature_function(sl::kFeaturePCL, "slPCLSetMarker", reinterpret_cast<void*&>(api.pcl_set_marker));

  LUID adapter_luid = native_device->GetAdapterLuid();
  sl::AdapterInfo adapter_info = {};
  adapter_info.deviceLUID = reinterpret_cast<uint8_t*>(&adapter_luid);
  adapter_info.deviceLUIDSizeInBytes = sizeof(adapter_luid);
  for (const auto& [feature, name] : {std::pair{sl::kFeatureDLSS_G, "DLSS-G"}, std::pair{sl::kFeatureReflex, "Reflex"}, std::pair{sl::kFeaturePCL, "PCL"}}) {
    renodx::utils::log::i("streamline: ", name, " supported: ", static_cast<uint32_t>(api.is_feature_supported(feature, adapter_info)), " (0 = ok)");
  }
}

inline void SetMarker(sl::PCLMarker marker) {
  if (api.pcl_set_marker != nullptr && frame_token != nullptr) {
    api.pcl_set_marker(marker, *frame_token);
  }
}

// Azimuth has no Reflex integration; markers are synthesized around its present (DLSS-G requires them).
inline void BeginFrame() {
  ++frame_index;
  if (api.get_new_frame_token(frame_token, &frame_index) != sl::Result::eOk) {
    frame_token = nullptr;
    return;
  }
  if (api.reflex_sleep != nullptr) {
    api.reflex_sleep(*frame_token);
  }
  SetMarker(sl::PCLMarker::eSimulationStart);
  SetMarker(sl::PCLMarker::eSimulationEnd);
  SetMarker(sl::PCLMarker::eRenderSubmitStart);
}

inline void OnPresent(
    reshade::api::command_queue* /*queue*/,
    reshade::api::swapchain* /*swapchain*/,
    const reshade::api::rect* /*source_rect*/,
    const reshade::api::rect* /*dest_rect*/,
    uint32_t /*dirty_rect_count*/,
    const reshade::api::rect* /*dirty_rects*/) {
  if (!initialized || !swap_chain_created) return;
  if (frame_token == nullptr) {
    BeginFrame();
  }

  if (!feature_state_logged && api.dlssg_set_options != nullptr && api.dlssg_get_state != nullptr) {
    feature_state_logged = true;
    if (api.reflex_set_options != nullptr) {
      api.reflex_set_options(sl::ReflexOptions{});
    }
    // G0: Streamline owns the swapchain but frame generation stays off.
    const sl::DLSSGOptions options = {};
    renodx::utils::log::i("streamline: slDLSSGSetOptions(off): ", static_cast<uint32_t>(api.dlssg_set_options(VIEWPORT, options)));
    sl::DLSSGState state = {};
    const auto result = api.dlssg_get_state(VIEWPORT, state, &options);
    renodx::utils::log::i(
        "streamline: slDLSSGGetState: result ", static_cast<uint32_t>(result),
        ", status ", renodx::utils::log::AsHex(static_cast<uint32_t>(state.status)),
        ", minWidthOrHeight ", state.minWidthOrHeight,
        ", numFramesToGenerateMax ", state.numFramesToGenerateMax,
        ", vsyncSupport ", static_cast<uint32_t>(state.bIsVsyncSupportAvailable));
  }

  SetMarker(sl::PCLMarker::eRenderSubmitEnd);
  SetMarker(sl::PCLMarker::ePresentStart);
}

inline void OnFinishPresent(reshade::api::command_queue* /*queue*/, reshade::api::swapchain* /*swapchain*/) {
  if (!initialized || !swap_chain_created || frame_token == nullptr) return;
  SetMarker(sl::PCLMarker::ePresentEnd);
  BeginFrame();
}

// Streamline itself stays loaded: its proxies own the game's swapchain until process exit.
inline void Uninstall() {
  try {
    if (create_command_queue_slot.installed_address != nullptr) {
      renodx::utils::vtable::Uninstall(&create_command_queue_slot);
    }
    if (create_swap_chain_for_hwnd_slot.installed_address != nullptr) {
      renodx::utils::vtable::Uninstall(&create_swap_chain_for_hwnd_slot);
    }
  } catch (const std::exception& error) {
    renodx::utils::log::e("streamline: unhooking failed: ", error.what());
  }
}

}  // namespace forzahorizon2::streamline
