/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
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
  PFun_slSetTagForFrame* set_tag_for_frame = nullptr;
  PFun_slSetConstants* set_constants = nullptr;
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
inline std::atomic<sl::FrameToken*> frame_token = nullptr;
// User toggle (settings); frame generation also requires inputs tagged during the frame being presented.
inline float frame_generation_setting = 1.f;
// Index into 2x/3x/4x (numFramesToGenerate 1..3).
inline float frame_generation_multiplier_setting = 0.f;
// sl::ReflexMode: Off / On / On + Boost.
inline float reflex_setting = 1.f;
// Reflex frame limiter: Off / 60 fps. Azimuth always paces its simulation to 60; this lets Reflex pace too.
inline float reflex_frame_limit_setting = 0.f;
inline constexpr uint32_t REFLEX_FRAME_LIMIT_60_US = 16666u;
inline constexpr uint32_t MAX_GENERATED_FRAMES = 3u;
inline std::atomic_bool inputs_tagged = false;
inline bool frame_generation_on = false;
inline uint32_t tagged_width = 0u;
inline uint32_t tagged_height = 0u;
inline std::atomic<uint32_t> tag_log_count = 0u;
inline std::atomic<uint32_t> present_log_count = 0u;
inline std::atomic<uint32_t> tag_error_log_count = 0u;
inline std::atomic<uint32_t> constants_error_log_count = 0u;
inline std::atomic_bool swap_chain_created = false;
inline uint32_t swap_chain_width = 0u;
inline uint32_t swap_chain_height = 0u;
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
  swap_chain_width = desc->Width;
  swap_chain_height = desc->Height;
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
  api.set_tag_for_frame = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(interposer, "slSetTagForFrame"));
  api.set_constants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(interposer, "slSetConstants"));
  if (api.init == nullptr || api.set_d3d_device == nullptr || api.upgrade_interface == nullptr || api.get_native_interface == nullptr
      || api.is_feature_supported == nullptr || api.get_feature_function == nullptr || api.get_new_frame_token == nullptr
      || api.set_tag_for_frame == nullptr || api.set_constants == nullptr) {
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
                      | sl::PreferenceFlags::eUseDXGIFactoryProxy
                      | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
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

inline bool IsActive() {
  return initialized && swap_chain_created;
}

inline void SetMarker(sl::PCLMarker marker) {
  auto* token = frame_token.load();
  if (api.pcl_set_marker != nullptr && token != nullptr) {
    api.pcl_set_marker(marker, *token);
  }
}

// Azimuth has no Reflex integration; markers are synthesized around its present (DLSS-G requires them).
inline void BeginFrame() {
  ++frame_index;
  sl::FrameToken* token = nullptr;
  if (api.get_new_frame_token(token, &frame_index) != sl::Result::eOk) {
    frame_token = nullptr;
    return;
  }
  frame_token = token;
  inputs_tagged = false;
  if (api.reflex_sleep != nullptr) {
    api.reflex_sleep(*token);
  }
  SetMarker(sl::PCLMarker::eSimulationStart);
  SetMarker(sl::PCLMarker::eSimulationEnd);
  SetMarker(sl::PCLMarker::eRenderSubmitStart);
}

// Called while recording the frame's tonemap draw. Both resources stay in shader-resource state until present.
inline void TagFrameInputs(
    reshade::api::command_list* cmd_list,
    reshade::api::resource motion_vectors,
    reshade::api::resource depth,
    uint32_t width,
    uint32_t height) {
  auto* token = frame_token.load();
  if (!IsActive() || token == nullptr) return;
  constexpr uint32_t SHADER_RESOURCE_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  sl::Resource motion_vector_resource = {sl::ResourceType::eTex2d, reinterpret_cast<void*>(motion_vectors.handle), SHADER_RESOURCE_STATE};
  sl::Resource depth_resource = {sl::ResourceType::eTex2d, reinterpret_cast<void*>(depth.handle), SHADER_RESOURCE_STATE};
  const sl::Extent extent = {.top = 0u, .left = 0u, .width = width, .height = height};
  // Backbuffer tag carries only the full-swapchain extent (Streamline already owns the backbuffer).
  const sl::Extent backbuffer_extent = {.top = 0u, .left = 0u, .width = swap_chain_width, .height = swap_chain_height};
  const sl::ResourceTag tags[] = {
      {&depth_resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent},
      {&motion_vector_resource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent},
      {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &backbuffer_extent},
  };
  const auto result = api.set_tag_for_frame(
      *token, VIEWPORT, tags, static_cast<uint32_t>(std::size(tags)), reinterpret_cast<sl::CommandBuffer*>(cmd_list->get_native()));
  if (result != sl::Result::eOk) {
    if (tag_error_log_count.fetch_add(1u) < 3u) {
      renodx::utils::log::e("streamline: slSetTagForFrame failed (", static_cast<uint32_t>(result), ").");
    }
    return;
  }
  tagged_width = width;
  tagged_height = height;
  inputs_tagged = true;
  if (tag_log_count.fetch_add(1u) < 3u) {
    renodx::utils::log::i("streamline: tagged inputs for frame ", static_cast<uint32_t>(*token), " on thread ", GetCurrentThreadId());
  }
}

inline sl::float4x4 IdentityMatrix() {
  sl::float4x4 matrix = {};
  matrix.setRow(0u, {1.f, 0.f, 0.f, 0.f});
  matrix.setRow(1u, {0.f, 1.f, 0.f, 0.f});
  matrix.setRow(2u, {0.f, 0.f, 1.f, 0.f});
  matrix.setRow(3u, {0.f, 0.f, 0.f, 1.f});
  return matrix;
}

// G1: camera matrices are not read back yet; motion vectors are dense (camera + objects), so only their
// conventions matter. FH2 uses standard depth (0 near, 1 far), per its depth linearization in 0x34ADA64E.
inline void SetCommonConstants(sl::FrameToken* token) {
  sl::Constants constants = {};
  constants.cameraViewToClip = IdentityMatrix();
  constants.clipToCameraView = IdentityMatrix();
  constants.clipToLensClip = IdentityMatrix();
  constants.clipToPrevClip = IdentityMatrix();
  constants.prevClipToClip = IdentityMatrix();
  constants.jitterOffset = {0.f, 0.f};
  constants.mvecScale = {1.f, 1.f};
  constants.cameraPinholeOffset = {0.f, 0.f};
  constants.cameraPos = {0.f, 0.f, 0.f};
  constants.cameraUp = {0.f, 1.f, 0.f};
  constants.cameraRight = {1.f, 0.f, 0.f};
  constants.cameraFwd = {0.f, 0.f, 1.f};
  constants.cameraNear = 0.1f;
  constants.cameraFar = 10000.f;
  constants.cameraFOV = 1.0472f;
  constants.cameraAspectRatio = (tagged_height != 0u ? static_cast<float>(tagged_width) / static_cast<float>(tagged_height) : 16.f / 9.f);
  constants.motionVectorsInvalidValue = 0.f;
  constants.depthInverted = sl::Boolean::eFalse;
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = sl::Boolean::eFalse;
  constants.orthographicProjection = sl::Boolean::eFalse;
  constants.motionVectorsDilated = sl::Boolean::eFalse;
  constants.motionVectorsJittered = sl::Boolean::eFalse;
  if (const auto result = api.set_constants(constants, *token, VIEWPORT);
      result != sl::Result::eOk && constants_error_log_count.fetch_add(1u) < 3u) {
    renodx::utils::log::e("streamline: slSetConstants failed (", static_cast<uint32_t>(result), ").");
  }
}

inline void LogFrameGenerationState(const sl::DLSSGOptions& options) {
  sl::DLSSGState state = {};
  const auto result = api.dlssg_get_state(VIEWPORT, state, &options);
  renodx::utils::log::i(
      "streamline: DLSS-G mode ", static_cast<uint32_t>(options.mode),
      ": state result ", static_cast<uint32_t>(result),
      ", status ", renodx::utils::log::AsHex(static_cast<uint32_t>(state.status)),
      ", numFramesToGenerateMax ", state.numFramesToGenerateMax,
      ", vsyncSupport ", static_cast<uint32_t>(state.bIsVsyncSupportAvailable),
      ", inputs ", tagged_width, "x", tagged_height);
}

struct OutputMode {
  bool frame_generation = false;
  uint32_t generated_frames = 1u;
  sl::ReflexMode reflex = sl::ReflexMode::eOff;
  uint32_t frame_limit_us = 0u;
};

inline bool operator==(const OutputMode& lhs, const OutputMode& rhs) {
  return lhs.frame_generation == rhs.frame_generation && lhs.generated_frames == rhs.generated_frames && lhs.reflex == rhs.reflex
         && lhs.frame_limit_us == rhs.frame_limit_us;
}

inline OutputMode applied_mode = {};
inline bool applied_mode_valid = false;

inline void ApplyOutputMode(const OutputMode& mode) {
  applied_mode = mode;
  applied_mode_valid = true;
  frame_generation_on = mode.frame_generation;
  if (api.reflex_set_options != nullptr) {
    sl::ReflexOptions reflex_options = {};
    reflex_options.mode = mode.reflex;
    reflex_options.frameLimitUs = mode.frame_limit_us;
    renodx::utils::log::i("streamline: slReflexSetOptions(mode ", static_cast<uint32_t>(mode.reflex), ", frameLimitUs ", mode.frame_limit_us, "): ",
                          static_cast<uint32_t>(api.reflex_set_options(reflex_options)));
  }
  sl::DLSSGOptions options = {};
  options.mode = (mode.frame_generation ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff);
  options.numFramesToGenerate = mode.generated_frames;
  const auto result = api.dlssg_set_options(VIEWPORT, options);
  renodx::utils::log::i("streamline: slDLSSGSetOptions(", (mode.frame_generation ? "on" : "off"), ", generated ",
                        mode.generated_frames, "): ", static_cast<uint32_t>(result));
  LogFrameGenerationState(options);
}

// DLSS-G must be off while the swapchain is resized or changes fullscreen state; OnPresent re-enables it.
inline void OnDestroySwapchain(reshade::api::swapchain* /*swapchain*/, bool /*resize*/) {
  if (!IsActive() || !frame_generation_on || api.dlssg_set_options == nullptr) return;
  OutputMode mode = applied_mode;
  mode.frame_generation = false;
  ApplyOutputMode(mode);
}

inline void OnPresent(
    reshade::api::command_queue* /*queue*/,
    reshade::api::swapchain* /*swapchain*/,
    const reshade::api::rect* /*source_rect*/,
    const reshade::api::rect* /*dest_rect*/,
    uint32_t /*dirty_rect_count*/,
    const reshade::api::rect* /*dirty_rects*/) {
  if (!IsActive()) return;
  if (frame_token.load() == nullptr) {
    BeginFrame();
  }
  auto* token = frame_token.load();

  if (token != nullptr && api.dlssg_set_options != nullptr && api.dlssg_get_state != nullptr) {
    if (!feature_state_logged) {
      feature_state_logged = true;
      LogFrameGenerationState({});
    }
    // Frames without tagged inputs (no 3D scene: loading, video) must not be interpolated.
    OutputMode mode = {
        .frame_generation = frame_generation_setting != 0.f && inputs_tagged,
        .generated_frames = std::clamp(static_cast<uint32_t>(frame_generation_multiplier_setting) + 1u, 1u, MAX_GENERATED_FRAMES),
        .reflex = static_cast<sl::ReflexMode>(std::clamp(static_cast<int>(reflex_setting), 0, 2)),
        .frame_limit_us = (reflex_frame_limit_setting != 0.f ? REFLEX_FRAME_LIMIT_60_US : 0u),
    };
    // DLSS-G requires Reflex; "Off" only applies while frame generation is off.
    if (mode.frame_generation && mode.reflex == sl::ReflexMode::eOff) {
      mode.reflex = sl::ReflexMode::eLowLatency;
    }
    if (mode.frame_generation) {
      SetCommonConstants(token);
    }
    if (!applied_mode_valid || !(mode == applied_mode)) {
      ApplyOutputMode(mode);
    }
  }
  if (token != nullptr && present_log_count.fetch_add(1u) < 3u) {
    renodx::utils::log::i("streamline: present frame ", static_cast<uint32_t>(*token), " on thread ", GetCurrentThreadId(), ", inputs tagged: ", inputs_tagged.load());
  }
  SetMarker(sl::PCLMarker::eRenderSubmitEnd);
  SetMarker(sl::PCLMarker::ePresentStart);
}

inline void OnFinishPresent(reshade::api::command_queue* /*queue*/, reshade::api::swapchain* /*swapchain*/) {
  if (!IsActive() || frame_token.load() == nullptr) return;
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
