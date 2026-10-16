/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../detour.hpp"
#include "../directx.hpp"
#include "../log.hpp"

// Presents a D3D11 game through a private D3D12 swapchain, so D3D12-only presentation features (Streamline DLSS-G)
// can run on D3D11 games. The game, ReShade and RenoDX keep a D3D11 swapchain whose back buffer is an owned shared
// texture; every present copies it into a D3D12 flip swapchain on the game's window, created on the same adapter and
// ordered against the D3D11 work by one shared fence.
//
// Placement: dxgi.dll's own CreateSwapChainForHwnd is detoured, below ReShade's vtable hook. ReShade and the RenoDX
// swapchain upgrade therefore see (and modify) the description first, then wrap the proxy returned here exactly as
// they would wrap DXGI's swapchain. The private D3D12 objects are created natively and never reach ReShade.
namespace renodx::utils::dlss::d3d11_bridge {

using Microsoft::WRL::ComPtr;

struct Status {
  std::shared_mutex mutex;
  bool installed = false;
  bool active = false;
  uint32_t width = 0u;
  uint32_t height = 0u;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  uint64_t frames = 0u;
  // Averages over the last STATUS_WINDOW frames.
  double copy_gpu_ms = 0.;
  double bridge_cpu_ms = 0.;
  double present_cpu_ms = 0.;
  std::string last_error;
};

inline Status status;

// Only windows of this class are bridged (empty: any top-level window of the process).
inline std::wstring required_window_class;
// Debug-layer builds attach their info queue here; the private device never reaches ReShade's init_device.
inline void (*on_d3d12_device_created)(ID3D12Device* device) = nullptr;

namespace internal {

using CreateSwapChainForHwndFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain);

constexpr size_t FRAMES_IN_FLIGHT = 3u;
constexpr uint64_t STATUS_WINDOW = 120u;
constexpr uint64_t LOG_WINDOW = 1800u;
constexpr size_t CREATE_SWAP_CHAIN_FOR_HWND_SLOT = 15u;
constexpr DWORD FENCE_TIMEOUT_MS = 5000u;

inline CreateSwapChainForHwndFunction original_create_swap_chain_for_hwnd = nullptr;
inline std::mutex create_mutex;
inline ComPtr<ID3D12Device> d3d12_device;
inline ComPtr<ID3D12CommandQueue> d3d12_queue;
inline LUID d3d12_adapter_luid = {};
inline std::atomic<IDXGISwapChain4*> active_proxy = nullptr;

inline void SetError(const std::string& error) {
  renodx::utils::log::e("d3d11_bridge: ", error);
  const std::unique_lock lock(status.mutex);
  status.last_error = error;
}

}  // namespace internal

class DECLSPEC_UUID("5B0E7C2D-9F41-4A63-B8E2-1D7C4A9F3E60") ProxySwapChain final : public IDXGISwapChain4 {
 public:
  struct Objects {
    ComPtr<IUnknown> game_device;
    ComPtr<ID3D11Device5> d3d11_device;
    ComPtr<ID3D11DeviceContext4> d3d11_context;
    ComPtr<ID3D11Fence> d3d11_fence;
    ComPtr<ID3D12Fence> d3d12_fence;
    ComPtr<IDXGISwapChain4> swap_chain;
    UINT back_buffer_bind_flags = 0u;
  };

  explicit ProxySwapChain(Objects objects)
      : objects_(std::move(objects)), fence_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    auto* device = internal::d3d12_device.Get();
    QueryPerformanceFrequency(&qpc_frequency_);
    init_result_ = (fence_event_ == nullptr ? HRESULT_FROM_WIN32(GetLastError()) : S_OK);
    for (auto& allocator : allocators_) {
      if (FAILED(init_result_)) break;
      init_result_ = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    }
    if (SUCCEEDED(init_result_)) {
      init_result_ = device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&command_list_));
    }
    if (SUCCEEDED(init_result_)) {
      init_result_ = command_list_->Close();
    }
    if (SUCCEEDED(init_result_)) {
      const D3D12_QUERY_HEAP_DESC query_heap_desc = {
          .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
          .Count = static_cast<UINT>(internal::FRAMES_IN_FLIGHT * 2u),
          .NodeMask = 0u,
      };
      init_result_ = device->CreateQueryHeap(&query_heap_desc, IID_PPV_ARGS(&timestamp_heap_));
    }
    if (SUCCEEDED(init_result_)) {
      const D3D12_HEAP_PROPERTIES readback_heap = {.Type = D3D12_HEAP_TYPE_READBACK};
      const D3D12_RESOURCE_DESC readback_desc = {
          .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
          .Width = internal::FRAMES_IN_FLIGHT * 2u * sizeof(uint64_t),
          .Height = 1u,
          .DepthOrArraySize = 1u,
          .MipLevels = 1u,
          .SampleDesc = {.Count = 1u, .Quality = 0u},
          .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      };
      init_result_ = device->CreateCommittedResource(
          &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&timestamp_readback_));
    }
    if (SUCCEEDED(init_result_)) {
      init_result_ = internal::d3d12_queue->GetTimestampFrequency(&timestamp_frequency_);
    }
    if (SUCCEEDED(init_result_)) {
      init_result_ = CreateBuffers();
    }
  }

  ProxySwapChain(const ProxySwapChain&) = delete;
  ProxySwapChain& operator=(const ProxySwapChain&) = delete;
  ProxySwapChain(ProxySwapChain&&) = delete;
  ProxySwapChain& operator=(ProxySwapChain&&) = delete;

  ~ProxySwapChain() {
    if (objects_.d3d12_fence != nullptr && fence_event_ != nullptr) {
      WaitForGpu();
    }
    if (fence_event_ != nullptr) {
      CloseHandle(fence_event_);
    }
    IDXGISwapChain4* expected = this;
    if (internal::active_proxy.compare_exchange_strong(expected, nullptr)) {
      const std::unique_lock lock(status.mutex);
      status.active = false;
    }
  }

  [[nodiscard]] HRESULT InitResult() const { return init_result_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject)
        || riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2)
        || riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4) || riid == __uuidof(ProxySwapChain)) {
      AddRef();
      *object = static_cast<IDXGISwapChain4*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_count_)); }

  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (remaining == 0u) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override {
    return objects_.swap_chain->SetPrivateData(name, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override {
    return objects_.swap_chain->SetPrivateDataInterface(name, unknown);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override {
    return objects_.swap_chain->GetPrivateData(name, size, data);
  }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override {
    return objects_.swap_chain->GetParent(riid, parent);
  }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override {
    return objects_.game_device->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) override {
    return PresentThroughD3D12(sync_interval, flags, nullptr);
  }

  // D3D11 flip-model back buffers are only ever accessed as buffer 0; DXGI rotates the storage behind that one object.
  // Here the object never changes and the copy into the D3D12 swapchain does the rotation.
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT /*buffer*/, REFIID riid, void** surface) override {
    if (surface == nullptr) return E_POINTER;
    const std::shared_lock lock(mutex_);
    if (shared_back_buffer_ == nullptr) return DXGI_ERROR_INVALID_CALL;
    return shared_back_buffer_->QueryInterface(riid, surface);
  }

  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override {
    return objects_.swap_chain->SetFullscreenState(fullscreen, target);
  }
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override {
    return objects_.swap_chain->GetFullscreenState(fullscreen, target);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override { return objects_.swap_chain->GetDesc(desc); }

  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override {
    const std::unique_lock lock(mutex_);
    WaitForGpu();
    swap_chain_buffers_.clear();
    shared_back_buffer_d3d12_.Reset();
    if (shared_back_buffer_ != nullptr) {
      if (const ULONG remaining_references = shared_back_buffer_.Reset(); remaining_references != 0u) {
        renodx::utils::log::w("d3d11_bridge: ResizeBuffers with ", remaining_references, " outstanding back buffer reference(s).");
      }
    }
    HRESULT result = objects_.swap_chain->ResizeBuffers(buffer_count, width, height, format, flags);
    if (FAILED(result)) {
      renodx::utils::log::e("d3d11_bridge: D3D12 ResizeBuffers failed: ", renodx::utils::log::AsHex(static_cast<uint32_t>(result)));
    }
    if (const HRESULT buffers_result = CreateBuffers(); SUCCEEDED(result) && FAILED(buffers_result)) {
      result = buffers_result;
    }
    return result;
  }

  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* target) override { return objects_.swap_chain->ResizeTarget(target); }
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override { return objects_.swap_chain->GetContainingOutput(output); }
  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) override {
    return objects_.swap_chain->GetFrameStatistics(statistics);
  }
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override { return objects_.swap_chain->GetLastPresentCount(count); }

  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override { return objects_.swap_chain->GetDesc1(desc); }
  HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override {
    return objects_.swap_chain->GetFullscreenDesc(desc);
  }
  HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override { return objects_.swap_chain->GetHwnd(hwnd); }
  HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** unknown) override { return objects_.swap_chain->GetCoreWindow(riid, unknown); }
  HRESULT STDMETHODCALLTYPE Present1(UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) override {
    return PresentThroughD3D12(sync_interval, flags, parameters);
  }
  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return objects_.swap_chain->IsTemporaryMonoSupported(); }
  HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override { return objects_.swap_chain->GetRestrictToOutput(output); }
  HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override { return objects_.swap_chain->SetBackgroundColor(color); }
  HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override { return objects_.swap_chain->GetBackgroundColor(color); }
  HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return objects_.swap_chain->SetRotation(rotation); }
  HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override { return objects_.swap_chain->GetRotation(rotation); }

  HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override { return objects_.swap_chain->SetSourceSize(width, height); }
  HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override { return objects_.swap_chain->GetSourceSize(width, height); }
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return objects_.swap_chain->SetMaximumFrameLatency(latency); }
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override { return objects_.swap_chain->GetMaximumFrameLatency(latency); }
  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return objects_.swap_chain->GetFrameLatencyWaitableObject(); }
  HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override { return objects_.swap_chain->SetMatrixTransform(matrix); }
  HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override { return objects_.swap_chain->GetMatrixTransform(matrix); }

  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0u; }
  HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE color_space, UINT* support) override {
    return objects_.swap_chain->CheckColorSpaceSupport(color_space, support);
  }
  HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color_space) override {
    const HRESULT result = objects_.swap_chain->SetColorSpace1(color_space);
    renodx::utils::log::i("d3d11_bridge: SetColorSpace1(", static_cast<int>(color_space), ") = ", renodx::utils::log::AsHex(static_cast<uint32_t>(result)));
    if (SUCCEEDED(result)) {
      const std::unique_lock lock(status.mutex);
      status.color_space = color_space;
    }
    return result;
  }
  // Only D3D12 swapchains take creation node masks and present queues; a D3D11 caller has none to pass.
  HRESULT STDMETHODCALLTYPE ResizeBuffers1(
      UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
      const UINT* /*creation_node_mask*/, IUnknown* const* /*present_queue*/) override {
    return ResizeBuffers(buffer_count, width, height, format, flags);
  }
  HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) override {
    return objects_.swap_chain->SetHDRMetaData(type, size, metadata);
  }

 private:
  HRESULT CreateBuffers() {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    HRESULT result = objects_.swap_chain->GetDesc1(&desc);
    for (UINT index = 0u; SUCCEEDED(result) && index < desc.BufferCount; ++index) {
      result = objects_.swap_chain->GetBuffer(index, IID_PPV_ARGS(&swap_chain_buffers_.emplace_back()));
    }
    if (SUCCEEDED(result)) {
      const D3D11_TEXTURE2D_DESC texture_desc = {
          .Width = desc.Width,
          .Height = desc.Height,
          .MipLevels = 1u,
          .ArraySize = 1u,
          .Format = desc.Format,
          .SampleDesc = {.Count = 1u, .Quality = 0u},
          .Usage = D3D11_USAGE_DEFAULT,
          .BindFlags = objects_.back_buffer_bind_flags,
          .CPUAccessFlags = 0u,
          .MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
      };
      result = objects_.d3d11_device->CreateTexture2D(&texture_desc, nullptr, &shared_back_buffer_);
    }
    ComPtr<IDXGIResource1> shared_resource;
    if (SUCCEEDED(result)) {
      result = shared_back_buffer_.As(&shared_resource);
    }
    HANDLE shared_handle = nullptr;
    if (SUCCEEDED(result)) {
      result = shared_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared_handle);
    }
    if (SUCCEEDED(result)) {
      result = internal::d3d12_device->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&shared_back_buffer_d3d12_));
      CloseHandle(shared_handle);
    }
    if (FAILED(result)) {
      internal::SetError(renodx::utils::log::BuildString(
          "back buffer creation failed (", desc.Width, "x", desc.Height, " format ", static_cast<int>(desc.Format), "): ",
          renodx::utils::log::AsHex(static_cast<uint32_t>(result))));
      swap_chain_buffers_.clear();
      shared_back_buffer_d3d12_.Reset();
      shared_back_buffer_.Reset();
      return result;
    }
    renodx::utils::log::i(
        "d3d11_bridge: back buffer ", desc.Width, "x", desc.Height, " format ", static_cast<int>(desc.Format),
        ", D3D12 swapchain buffers ", desc.BufferCount, ", flags ", renodx::utils::log::AsHex(desc.Flags));
    const std::unique_lock lock(status.mutex);
    status.width = desc.Width;
    status.height = desc.Height;
    status.format = desc.Format;
    return S_OK;
  }

  void WaitForFence(uint64_t value) {
    if (objects_.d3d12_fence->GetCompletedValue() >= value) return;
    if (FAILED(objects_.d3d12_fence->SetEventOnCompletion(value, fence_event_))
        || WaitForSingleObject(fence_event_, internal::FENCE_TIMEOUT_MS) != WAIT_OBJECT_0) {
      renodx::utils::log::e("d3d11_bridge: timed out waiting for fence value ", value, " (completed ", objects_.d3d12_fence->GetCompletedValue(), ").");
    }
  }

  void WaitForGpu() {
    const uint64_t idle_value = ++fence_value_;
    internal::d3d12_queue->Signal(objects_.d3d12_fence.Get(), idle_value);
    WaitForFence(idle_value);
  }

  HRESULT PresentThroughD3D12(UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    if ((flags & DXGI_PRESENT_TEST) != 0u) return objects_.swap_chain->Present(sync_interval, flags);
    const std::unique_lock lock(mutex_);
    if (shared_back_buffer_d3d12_ == nullptr) return DXGI_ERROR_INVALID_CALL;
    LARGE_INTEGER bridge_start = {};
    QueryPerformanceCounter(&bridge_start);

    // The game's frame (and ReShade's effects on it) must finish before D3D12 reads the back buffer.
    const uint64_t frame_ready = ++fence_value_;
    objects_.d3d11_context->Signal(objects_.d3d11_fence.Get(), frame_ready);
    objects_.d3d11_context->Flush();
    internal::d3d12_queue->Wait(objects_.d3d12_fence.Get(), frame_ready);

    const size_t slot = frame_index_ % internal::FRAMES_IN_FLIGHT;
    WaitForFence(allocator_fence_values_[slot]);
    if (timestamp_pending_[slot]) {
      const D3D12_RANGE read_range = {.Begin = slot * 2u * sizeof(uint64_t), .End = (slot + 1u) * 2u * sizeof(uint64_t)};
      void* mapped = nullptr;
      if (SUCCEEDED(timestamp_readback_->Map(0u, &read_range, &mapped))) {
        const auto* ticks = static_cast<const uint64_t*>(mapped) + (slot * 2u);
        if (ticks[1] > ticks[0]) {
          copy_gpu_ms_sum_ += static_cast<double>(ticks[1] - ticks[0]) * 1000. / static_cast<double>(timestamp_frequency_);
          ++copy_gpu_samples_;
        }
        const D3D12_RANGE no_write = {.Begin = 0u, .End = 0u};
        timestamp_readback_->Unmap(0u, &no_write);
      }
      timestamp_pending_[slot] = false;
    }

    auto* allocator = allocators_[slot].Get();
    allocator->Reset();
    command_list_->Reset(allocator, nullptr);
    auto* target = swap_chain_buffers_[objects_.swap_chain->GetCurrentBackBufferIndex()].Get();
    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = target,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_PRESENT,
            .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST,
        },
    };
    command_list_->ResourceBarrier(1u, &barrier);
    const auto first_query = static_cast<UINT>(slot * 2u);
    command_list_->EndQuery(timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first_query);
    // The shared texture is in COMMON: the copy promotes it to COPY_SOURCE and it decays back after execution.
    command_list_->CopyResource(target, shared_back_buffer_d3d12_.Get());
    command_list_->EndQuery(timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first_query + 1u);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    command_list_->ResourceBarrier(1u, &barrier);
    command_list_->ResolveQueryData(
        timestamp_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first_query, 2u, timestamp_readback_.Get(), first_query * sizeof(uint64_t));
    command_list_->Close();
    std::array<ID3D12CommandList*, 1> command_lists = {command_list_.Get()};
    internal::d3d12_queue->ExecuteCommandLists(static_cast<UINT>(command_lists.size()), command_lists.data());
    const uint64_t copy_done = ++fence_value_;
    internal::d3d12_queue->Signal(objects_.d3d12_fence.Get(), copy_done);
    allocator_fence_values_[slot] = copy_done;
    timestamp_pending_[slot] = true;

    LARGE_INTEGER present_start = {};
    QueryPerformanceCounter(&present_start);
    HRESULT result = S_OK;
    if (parameters == nullptr) {
      result = objects_.swap_chain->Present(sync_interval, flags);
    } else {
      result = objects_.swap_chain->Present1(sync_interval, flags, parameters);
    }
    LARGE_INTEGER present_end = {};
    QueryPerformanceCounter(&present_end);
    // The game may only render into the back buffer again once D3D12 has copied it.
    objects_.d3d11_context->Wait(objects_.d3d11_fence.Get(), copy_done);

    const auto qpc_ms = [this](LONGLONG ticks) { return static_cast<double>(ticks) * 1000. / static_cast<double>(qpc_frequency_.QuadPart); };
    bridge_cpu_ms_sum_ += qpc_ms(present_start.QuadPart - bridge_start.QuadPart);
    present_cpu_ms_sum_ += qpc_ms(present_end.QuadPart - present_start.QuadPart);
    ++frame_index_;
    if (frame_index_ % internal::STATUS_WINDOW == 0u) {
      const double copy_gpu_ms = (copy_gpu_samples_ == 0u ? 0. : copy_gpu_ms_sum_ / static_cast<double>(copy_gpu_samples_));
      const double bridge_cpu_ms = bridge_cpu_ms_sum_ / static_cast<double>(internal::STATUS_WINDOW);
      const double present_cpu_ms = present_cpu_ms_sum_ / static_cast<double>(internal::STATUS_WINDOW);
      {
        const std::unique_lock status_lock(status.mutex);
        status.frames = frame_index_;
        status.copy_gpu_ms = copy_gpu_ms;
        status.bridge_cpu_ms = bridge_cpu_ms;
        status.present_cpu_ms = present_cpu_ms;
      }
      if (frame_index_ % internal::LOG_WINDOW == 0u || frame_index_ == internal::STATUS_WINDOW) {
        renodx::utils::log::i(
            "d3d11_bridge: frame ", frame_index_, ", copy GPU ", copy_gpu_ms, " ms, bridge CPU ", bridge_cpu_ms,
            " ms, D3D12 Present CPU ", present_cpu_ms, " ms, last Present ", renodx::utils::log::AsHex(static_cast<uint32_t>(result)));
      }
      copy_gpu_ms_sum_ = 0.;
      copy_gpu_samples_ = 0u;
      bridge_cpu_ms_sum_ = 0.;
      present_cpu_ms_sum_ = 0.;
    }
    return result;
  }

  Objects objects_;
  HANDLE fence_event_ = nullptr;
  HRESULT init_result_ = E_FAIL;
  LONG ref_count_ = 1;
  std::shared_mutex mutex_;
  uint64_t fence_value_ = 0u;
  ComPtr<ID3D11Texture2D> shared_back_buffer_;
  ComPtr<ID3D12Resource> shared_back_buffer_d3d12_;
  std::vector<ComPtr<ID3D12Resource>> swap_chain_buffers_;
  std::array<ComPtr<ID3D12CommandAllocator>, internal::FRAMES_IN_FLIGHT> allocators_;
  std::array<uint64_t, internal::FRAMES_IN_FLIGHT> allocator_fence_values_ = {};
  std::array<bool, internal::FRAMES_IN_FLIGHT> timestamp_pending_ = {};
  ComPtr<ID3D12GraphicsCommandList> command_list_;
  ComPtr<ID3D12QueryHeap> timestamp_heap_;
  ComPtr<ID3D12Resource> timestamp_readback_;
  uint64_t timestamp_frequency_ = 0u;
  LARGE_INTEGER qpc_frequency_ = {};
  uint64_t frame_index_ = 0u;
  double copy_gpu_ms_sum_ = 0.;
  uint32_t copy_gpu_samples_ = 0u;
  double bridge_cpu_ms_sum_ = 0.;
  double present_cpu_ms_sum_ = 0.;
};

inline HRESULT STDMETHODCALLTYPE OnCreateSwapChainForHwnd(
    IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain) {
  const auto create_unbridged = [&] {
    return internal::original_create_swap_chain_for_hwnd(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);
  };
  ComPtr<ID3D11Device> game_d3d11_device;
  if (device == nullptr || desc == nullptr || swap_chain == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&game_d3d11_device)))) {
    return create_unbridged();
  }

  std::array<wchar_t, 256> window_class = {};
  GetClassNameW(hwnd, window_class.data(), static_cast<int>(window_class.size()));
  DWORD window_process = 0u;
  GetWindowThreadProcessId(hwnd, &window_process);
  const bool flip_model = desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  const bool eligible_window = window_process == GetCurrentProcessId() && GetAncestor(hwnd, GA_ROOT) == hwnd
                               && (required_window_class.empty() || required_window_class == window_class.data());
  if (!flip_model || (desc->BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0u || !eligible_window || internal::active_proxy != nullptr) {
    renodx::utils::log::i(
        "d3d11_bridge: not bridging D3D11 swapchain (window class ", std::filesystem::path(window_class.data()).string(),
        ", swap effect ", static_cast<int>(desc->SwapEffect), ", usage ", renodx::utils::log::AsHex(desc->BufferUsage),
        ", already bridged ", internal::active_proxy != nullptr, ").");
    return create_unbridged();
  }

  const std::unique_lock lock(internal::create_mutex);
  const auto fall_back = [&](std::string_view step, HRESULT result) {
    internal::SetError(renodx::utils::log::BuildString(
        step, " failed (", renodx::utils::log::AsHex(static_cast<uint32_t>(result)), "); presenting through D3D11 without the bridge."));
    return create_unbridged();
  };

  // The game holds ReShade's device proxy; owned resources are created on the native device so no add-on (such as a
  // RenoDX resource upgrade) can change them.
  ID3D11Device* native_d3d11_device = game_d3d11_device.Get();
  renodx::utils::directx::NativeFromReShadeProxy(&native_d3d11_device);
  ComPtr<ID3D11Device5> d3d11_device;
  HRESULT result = native_d3d11_device->QueryInterface(IID_PPV_ARGS(&d3d11_device));
  if (FAILED(result)) return fall_back("ID3D11Device5", result);
  ComPtr<ID3D11DeviceContext> immediate_context;
  d3d11_device->GetImmediateContext(&immediate_context);
  ComPtr<ID3D11DeviceContext4> d3d11_context;
  result = immediate_context.As(&d3d11_context);
  if (FAILED(result)) return fall_back("ID3D11DeviceContext4", result);
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  result = d3d11_device.As(&dxgi_device);
  if (SUCCEEDED(result)) {
    result = dxgi_device->GetAdapter(&adapter);
  }
  DXGI_ADAPTER_DESC adapter_desc = {};
  if (SUCCEEDED(result)) {
    result = adapter->GetDesc(&adapter_desc);
  }
  if (FAILED(result)) return fall_back("adapter lookup", result);

  if (internal::d3d12_device == nullptr) {
    // Called inside ReShade's CreateSwapChainForHwnd, where its D3D12CreateDevice hook passes straight through, so the
    // device is native and invisible to ReShade and other add-ons.
    auto* create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(LoadLibraryW(L"d3d12.dll"), "D3D12CreateDevice"));
    result = (create_device == nullptr ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND) : create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&internal::d3d12_device)));
    if (FAILED(result)) return fall_back("D3D12CreateDevice", result);
    if (on_d3d12_device_created != nullptr) {
      on_d3d12_device_created(internal::d3d12_device.Get());
    }
    const D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0u,
    };
    result = internal::d3d12_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&internal::d3d12_queue));
    if (FAILED(result)) {
      internal::d3d12_device.Reset();
      return fall_back("D3D12 CreateCommandQueue", result);
    }
    internal::d3d12_adapter_luid = adapter_desc.AdapterLuid;
    renodx::utils::log::i("d3d11_bridge: private D3D12 device on ", std::filesystem::path(adapter_desc.Description).string(), ".");
  } else if (std::memcmp(&internal::d3d12_adapter_luid, &adapter_desc.AdapterLuid, sizeof(LUID)) != 0) {
    return fall_back("adapter match (game device moved to another adapter)", E_FAIL);
  }

  ComPtr<ID3D11Fence> d3d11_fence;
  result = d3d11_device->CreateFence(0u, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d11_fence));
  HANDLE fence_handle = nullptr;
  if (SUCCEEDED(result)) {
    result = d3d11_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
  }
  ComPtr<ID3D12Fence> d3d12_fence;
  if (SUCCEEDED(result)) {
    result = internal::d3d12_device->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&d3d12_fence));
    CloseHandle(fence_handle);
  }
  if (FAILED(result)) return fall_back("shared fence", result);

  DXGI_SWAP_CHAIN_DESC1 d3d12_desc = *desc;
  d3d12_desc.BufferCount = std::max(desc->BufferCount, 2u);
  ComPtr<IDXGISwapChain1> d3d12_swap_chain1;
  result = internal::original_create_swap_chain_for_hwnd(
      factory, internal::d3d12_queue.Get(), hwnd, &d3d12_desc, fullscreen_desc, restrict_to_output, &d3d12_swap_chain1);
  ComPtr<IDXGISwapChain4> d3d12_swap_chain;
  if (SUCCEEDED(result)) {
    result = d3d12_swap_chain1.As(&d3d12_swap_chain);
  }
  d3d12_swap_chain1.Reset();
  if (FAILED(result)) return fall_back("D3D12 CreateSwapChainForHwnd", result);

  UINT back_buffer_bind_flags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if ((desc->BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0u) {
    back_buffer_bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
  }
  auto* proxy = new ProxySwapChain({
      .game_device = device,
      .d3d11_device = d3d11_device,
      .d3d11_context = d3d11_context,
      .d3d11_fence = d3d11_fence,
      .d3d12_fence = d3d12_fence,
      .swap_chain = d3d12_swap_chain,
      .back_buffer_bind_flags = back_buffer_bind_flags,
  });
  d3d12_swap_chain.Reset();
  if (const HRESULT init_result = proxy->InitResult(); FAILED(init_result)) {
    proxy->Release();
    return fall_back("bridge initialization", init_result);
  }
  internal::active_proxy = proxy;
  {
    const std::unique_lock status_lock(status.mutex);
    status.active = true;
    status.last_error.clear();
  }
  renodx::utils::log::i(
      "d3d11_bridge: bridged swapchain on window class ", std::filesystem::path(window_class.data()).string(), ", ",
      desc->Width, "x", desc->Height, " format ", static_cast<int>(desc->Format), ", ", desc->BufferCount,
      " buffers, flags ", renodx::utils::log::AsHex(desc->Flags), ".");
  *swap_chain = proxy;
  return S_OK;
}

// Detours dxgi.dll's own CreateSwapChainForHwnd. ReShade patches the factory vtable slot in place, before add-ons
// load, so the original implementation is read from the slot's relocated value in dxgi.dll's file on disk; the other
// (unhooked) slots must match the loaded image, which proves file and image agree. Call after DXGI is loaded and not
// under the loader lock (from a D3D11 init_device event, before the game creates its swapchain).
inline bool Install() {
  const std::unique_lock lock(internal::create_mutex);
  if (internal::original_create_swap_chain_for_hwnd != nullptr) return true;

  std::array<wchar_t, MAX_PATH> system_directory = {};
  GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
  const std::filesystem::path dxgi_path = std::filesystem::path(system_directory.data()) / L"dxgi.dll";
  HMODULE dxgi_module = GetModuleHandleW(dxgi_path.c_str());
  auto* create_factory = (dxgi_module == nullptr ? nullptr : reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dxgi_module, "CreateDXGIFactory1")));
  ComPtr<IDXGIFactory2> factory;
  if (create_factory == nullptr || FAILED(create_factory(IID_PPV_ARGS(&factory)))) {
    internal::SetError("system dxgi.dll factory unavailable; bridge not installed.");
    return false;
  }
  auto* const* vtable = *reinterpret_cast<void* const* const*>(factory.Get());
  const auto image_base = reinterpret_cast<uintptr_t>(dxgi_module);
  const auto* image_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base)->e_lfanew);
  const uintptr_t image_size = image_nt->OptionalHeader.SizeOfImage;
  const uintptr_t vtable_rva = reinterpret_cast<uintptr_t>(vtable) - image_base;
  if (reinterpret_cast<uintptr_t>(vtable) < image_base || vtable_rva >= image_size) {
    internal::SetError("factory vtable is not in system dxgi.dll; bridge not installed.");
    return false;
  }

  std::ifstream file(dxgi_path, std::ios::binary);
  const std::vector<char> file_bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto* file_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file_bytes.data());
  if (file_bytes.size() < sizeof(IMAGE_DOS_HEADER) || file_dos->e_magic != IMAGE_DOS_SIGNATURE
      || static_cast<size_t>(file_dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file_bytes.size()) {
    internal::SetError("could not read system dxgi.dll; bridge not installed.");
    return false;
  }
  const auto* file_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file_bytes.data() + file_dos->e_lfanew);
  if (file_nt->Signature != IMAGE_NT_SIGNATURE || file_nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    internal::SetError("system dxgi.dll is not a PE32+ image; bridge not installed.");
    return false;
  }
  const IMAGE_SECTION_HEADER* first_section = IMAGE_FIRST_SECTION(file_nt);
  const std::vector<IMAGE_SECTION_HEADER> sections(first_section, first_section + file_nt->FileHeader.NumberOfSections);
  const auto relocated_slot = [&](size_t index) -> std::optional<uintptr_t> {
    const uintptr_t slot_rva = vtable_rva + (index * sizeof(void*));
    for (const auto& section : sections) {
      if (slot_rva < section.VirtualAddress || slot_rva + sizeof(uint64_t) > section.VirtualAddress + section.SizeOfRawData) continue;
      const size_t offset = slot_rva - section.VirtualAddress + section.PointerToRawData;
      if (offset + sizeof(uint64_t) > file_bytes.size()) return std::nullopt;
      uint64_t file_value = 0u;
      std::memcpy(&file_value, file_bytes.data() + offset, sizeof(file_value));
      return static_cast<uintptr_t>(file_value - file_nt->OptionalHeader.ImageBase + image_base);
    }
    return std::nullopt;
  };
  size_t matching_slots = 0u;
  for (size_t index = 0u; index < internal::CREATE_SWAP_CHAIN_FOR_HWND_SLOT; ++index) {
    if (relocated_slot(index) == reinterpret_cast<uintptr_t>(vtable[index])) {
      ++matching_slots;
    }
  }
  const std::optional<uintptr_t> original = relocated_slot(internal::CREATE_SWAP_CHAIN_FOR_HWND_SLOT);
  const bool original_is_code = original.has_value() && std::ranges::any_of(sections, [&](const IMAGE_SECTION_HEADER& section) {
                                  const uintptr_t rva = *original - image_base;
                                  return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0u && rva >= section.VirtualAddress && rva < section.VirtualAddress + section.Misc.VirtualSize;
                                });
  // CreateSwapChain (slot 10) is the only earlier slot ReShade replaces; allow one more from another overlay.
  if (matching_slots + 2u < internal::CREATE_SWAP_CHAIN_FOR_HWND_SLOT || !original_is_code) {
    internal::SetError(renodx::utils::log::BuildString(
        "system dxgi.dll on disk does not match the loaded image (", matching_slots, " of ",
        internal::CREATE_SWAP_CHAIN_FOR_HWND_SLOT, " slots match); bridge not installed."));
    return false;
  }

  internal::original_create_swap_chain_for_hwnd = reinterpret_cast<internal::CreateSwapChainForHwndFunction>(*original);
  const bool slot_replaced = reinterpret_cast<uintptr_t>(vtable[internal::CREATE_SWAP_CHAIN_FOR_HWND_SLOT]) != *original;
  try {
    const std::array functions = {
        renodx::utils::detour::Function(&internal::original_create_swap_chain_for_hwnd, &OnCreateSwapChainForHwnd),
    };
    renodx::utils::detour::Install(functions);
  } catch (const std::exception& error) {
    internal::original_create_swap_chain_for_hwnd = nullptr;
    internal::SetError(renodx::utils::log::BuildString("detouring CreateSwapChainForHwnd failed: ", error.what()));
    return false;
  }
  renodx::utils::log::i(
      "d3d11_bridge: installed ", (slot_replaced ? "below the existing factory vtable hook" : "before any factory vtable hook"), " (",
      matching_slots, " of ", internal::CREATE_SWAP_CHAIN_FOR_HWND_SLOT, " factory slots match dxgi.dll).");
  const std::unique_lock status_lock(status.mutex);
  status.installed = true;
  return true;
}

}  // namespace renodx::utils::dlss::d3d11_bridge
