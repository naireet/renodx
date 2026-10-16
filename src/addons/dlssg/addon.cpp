/*
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "../../utils/dlss/d3d11_bridge.hpp"
#include "../../utils/dlss/streamline_inject.hpp"
#include "../../utils/log.hpp"
#include "../../utils/settings.hpp"
#include "./ngx_inputs.hpp"
#include "./online_guard.hpp"
#include "./reflex.hpp"

// Generic DLSS Frame Generation for games that already run DLSS Super Resolution through NGX but ship no frame
// generation. Reuses the Streamline injector proven on Forza Horizon 2; per-game knowledge lives in PROFILES.
namespace {

namespace inject = renodx::utils::streamline::inject;
namespace d3d11_bridge = renodx::utils::dlss::d3d11_bridge;

// G0: Streamline bootstrap (DLSS-G off) plus observation of the game's DLSS inputs, NVAPI Reflex and online state.
// G1 tags the game's DLSS inputs and translates its Reflex into Streamline, then opens this gate.
// D3D11 games first run G0a: the D3D11 -> D3D12 present bridge alone, without Streamline.
constexpr bool FRAME_GENERATION_GATE_OPEN = false;

struct GameProfile {
  std::wstring_view executable;
  const char* name;
  inject::Config streamline;
  std::span<const std::wstring_view> online_command_line_tokens;
  std::span<const uint16_t> online_udp_ports;
  // D3D11 game: present through the private D3D12 swapchain (Streamline DLSS-G is D3D12/Vulkan only).
  bool d3d11_bridge = false;
  std::wstring_view d3d11_bridge_window_class;
};

// Red Dead Online runs from RDR2.exe. Rockstar's launcher switch straight into Online, and Rockstar's peer-to-peer
// UDP port; both still need confirming against G0 logs from story mode and Online.
constexpr std::array<std::wstring_view, 1> RDR2_ONLINE_COMMAND_LINE_TOKENS = {L"-StraightIntoFreemode"};
constexpr std::array<uint16_t, 1> RDR2_ONLINE_UDP_PORTS = {6672u};

constexpr std::array<GameProfile, 2> PROFILES = {{
    {
        .executable = L"rdr2.exe",
        .name = "Red Dead Redemption 2",
        .streamline = {
            .plugin_folder_name = L"renodx-streamline",
            .project_id = "8c4e1f3a-5b2d-4e7a-9f16-2d8b7c3e5a91",
            .engine_version = "RAGE (Red Dead Redemption 2)",
            .load_dlss = false,
            .hook_create_swap_chain = true,
            .synthesize_reflex = false,
        },
        .online_command_line_tokens = RDR2_ONLINE_COMMAND_LINE_TOKENS,
        .online_udp_ports = RDR2_ONLINE_UDP_PORTS,
    },
    // D3D11, DLSS SR through NGX, no native Reflex; optional co-op without anti-cheat, so no online guard.
    {
        .executable = L"nioh2.exe",
        .name = "Nioh 2",
        .streamline = {
            .plugin_folder_name = L"renodx-streamline",
            .project_id = "3f9b6d2e-7a41-4c85-b0e3-9d16a5c7f248",
            .engine_version = "Koei Tecmo (Nioh 2)",
            .load_dlss = false,
            .hook_create_swap_chain = false,
            .synthesize_reflex = true,
        },
        .d3d11_bridge = true,
        .d3d11_bridge_window_class = L"NIOH",
    },
}};

const GameProfile* active_profile = nullptr;
std::atomic_bool evaluated_last_frame = false;

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::CUSTOM,
        .section = "Status",
        .on_draw = [] {
          ImGui::Text("Game: %s", active_profile->name);
          if (active_profile->d3d11_bridge) {
            ImGui::Text("Gate: G0a (D3D11 -> D3D12 bridge only: no Streamline, frame generation off)");
            const std::shared_lock lock(d3d11_bridge::status.mutex);
            const auto& bridge = d3d11_bridge::status;
            if (bridge.active) {
              ImGui::Text("D3D11 bridge: active, %ux%u, format %d, colour space %d", bridge.width, bridge.height,
                          static_cast<int>(bridge.format), static_cast<int>(bridge.color_space));
              ImGui::Text("Copy %.3f ms GPU, bridge %.3f ms CPU, D3D12 Present %.3f ms CPU (%llu frames)",
                          bridge.copy_gpu_ms, bridge.bridge_cpu_ms, bridge.present_cpu_ms, static_cast<unsigned long long>(bridge.frames));
            } else if (bridge.installed) {
              ImGui::Text("D3D11 bridge: installed, game swapchain not bridged");
            } else {
              ImGui::Text("D3D11 bridge: not installed");
            }
            if (!bridge.last_error.empty()) {
              ImGui::TextWrapped("Bridge error: %s", bridge.last_error.c_str());
            }
          } else {
            ImGui::Text("Gate: %s", (FRAME_GENERATION_GATE_OPEN ? "G1+ (frame generation available)" : "G0 (Streamline bootstrap, frame generation off)"));
          }
          if (active_profile->d3d11_bridge) {
            ImGui::Text("Streamline: not used at G0a");
          } else if (!inject::initialized) {
            ImGui::TextWrapped("Streamline: not loaded (place NVIDIA's signed runtime in renodx-streamline next to the game executable).");
          } else {
            ImGui::Text("Streamline: %s, swapchain %ux%u", (inject::IsActive() ? "active" : "loaded, no proxied swapchain yet"),
                        inject::swap_chain_width, inject::swap_chain_height);
          }
          const auto summary = [] {
            const std::shared_lock lock(dlssg::ngx::summary_mutex);
            return dlssg::ngx::last_summary;
          }();
          ImGui::Text("DLSS SR: %s, %llu evaluates, render %ux%u -> %ux%u",
                      (evaluated_last_frame ? "evaluating" : "idle"),
                      static_cast<unsigned long long>(dlssg::ngx::evaluate_count.load()),
                      summary.render_width, summary.render_height, summary.output_width, summary.output_height);
          if (active_profile->d3d11_bridge) {
            ImGui::Text("Game Reflex: none (synthesized markers from G0b)");
          } else {
            constexpr std::array<const char*, 4> REFLEX_MODES = {"not set", "Off", "On", "On + Boost"};
            ImGui::Text("Game Reflex: %s, %llu sleeps, %llu markers",
                        REFLEX_MODES.at(std::min(dlssg::reflex::game_reflex_mode.load(), 3u)),
                        static_cast<unsigned long long>(dlssg::reflex::game_sleep_calls.load()),
                        static_cast<unsigned long long>(dlssg::reflex::game_marker_calls.load()));
          }
          if (active_profile->online_udp_ports.empty()) {
            ImGui::Text("Online guard: not needed for this game");
          } else if (dlssg::online_guard::online_detected) {
            ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "Online session detected: frame generation disabled. Quit and remove the mod before playing online.");
          } else {
            ImGui::Text("Online guard: single player");
          }
          return false;
        },
    },
    new renodx::utils::settings::Setting{
        .key = "FrameGeneration",
        .binding = &inject::frame_generation_setting,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 1.f,
        .label = "DLSS Frame Generation",
        .section = "Frame Generation",
        .tooltip = "Experimental. Single player only. Requires NVIDIA's Streamline runtime in renodx-streamline next to the game executable.",
        .labels = {"Off", "On"},
        .is_enabled = [] { return FRAME_GENERATION_GATE_OPEN && inject::IsActive() && !dlssg::online_guard::online_detected; },
        .is_visible = [] { return inject::initialized; },
    },
    new renodx::utils::settings::Setting{
        .key = "FrameGenerationMultiplier",
        .binding = &inject::frame_generation_multiplier_setting,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Frame Generation Multiplier",
        .section = "Frame Generation",
        .tooltip = "Output frames per rendered frame. 3x/4x need DLSS multi frame generation support (RTX 50 series).",
        .labels = {"2x", "3x", "4x"},
        .is_enabled = [] { return FRAME_GENERATION_GATE_OPEN && inject::IsActive() && inject::frame_generation_setting != 0.f; },
        .is_visible = [] { return inject::initialized; },
    },
};

// D3D11 profiles install the bridge here: DXGI is loaded, the loader lock is not held, and the game has not created its
// swapchain yet.
void OnInitDevice(reshade::api::device* device) {
  if (active_profile->d3d11_bridge) {
    if (device->get_api() == reshade::api::device_api::d3d11) {
      d3d11_bridge::Install();
    }
    return;
  }
  if (device->get_api() != reshade::api::device_api::d3d12) return;
  dlssg::reflex::Install();
}

// Registered before the injector's present handler, so a veto applies to the frame being presented.
void OnPresent(
    reshade::api::command_queue* /*queue*/,
    reshade::api::swapchain* /*swapchain*/,
    const reshade::api::rect* /*source_rect*/,
    const reshade::api::rect* /*dest_rect*/,
    uint32_t /*dirty_rect_count*/,
    const reshade::api::rect* /*dirty_rects*/) {
  evaluated_last_frame = dlssg::ngx::evaluated_since_present.exchange(false);
  dlssg::online_guard::Poll(active_profile->online_udp_ports);
  if (dlssg::online_guard::online_detected) {
    inject::frame_generation_allowed = false;
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX DLSS-G";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX DLSS Frame Generation for games with DLSS Super Resolution";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID /*lpv_reserved*/) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      {
        std::array<wchar_t, MAX_PATH> executable_path = {};
        GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
        const std::wstring executable = dlssg::online_guard::ToLower(std::filesystem::path(executable_path.data()).filename().wstring());
        for (const auto& profile : PROFILES) {
          if (profile.executable == executable) {
            active_profile = &profile;
          }
        }
      }
      if (active_profile == nullptr) {
        renodx::utils::log::i("dlssg: this executable has no profile; add-on inactive.");
        break;
      }
      if (dlssg::online_guard::CommandLineRequestsOnline(active_profile->online_command_line_tokens)) {
        renodx::utils::log::e("dlssg: ", active_profile->name, " was launched into its online mode; add-on inactive.");
        active_profile = nullptr;
        break;
      }
      renodx::utils::log::i(
          "dlssg: ", active_profile->name, " profile, gate ",
          (active_profile->d3d11_bridge ? "G0a (D3D11 bridge only)" : (FRAME_GENERATION_GATE_OPEN ? "open" : "G0 (frame generation off)")), ".");

      // Hooks into this module (Streamline vtable slots, NVAPI, NGX, loader) live for the process, like Streamline.
      {
        HMODULE pinned_module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(h_module), &pinned_module)
            == FALSE) {
          renodx::utils::log::e("dlssg: could not pin the add-on module; add-on inactive.");
          active_profile = nullptr;
          break;
        }
      }
      inject::config = active_profile->streamline;
      inject::frame_generation_allowed = FRAME_GENERATION_GATE_OPEN;
      renodx::utils::settings::use_presets = false;
      renodx::utils::settings::global_name = "RENODX-DLSSG";

      reshade::register_event<reshade::addon_event::present>(OnPresent);
      if (active_profile->d3d11_bridge) {
        d3d11_bridge::required_window_class = active_profile->d3d11_bridge_window_class;
      } else {
        inject::Use(fdw_reason);
      }
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      dlssg::ngx::Install();
      renodx::utils::settings::Use(fdw_reason, &settings);
      break;
    case DLL_PROCESS_DETACH:
      // An active profile pins this module, so detach only happens at process exit, where hooked code may still run
      // on other threads: its hooks and events are deliberately left in place.
      reshade::unregister_addon(h_module);
      break;
    default:
      break;
  }
  return TRUE;
}
