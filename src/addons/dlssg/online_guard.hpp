/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>
#include <iphlpapi.h>

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../utils/log.hpp"

#pragma comment(lib, "iphlpapi.lib")

// Single-player gate for games whose online mode runs from the same executable (e.g. Red Dead Online).
// Online is detected from the launch command line and from the game binding its peer-to-peer UDP port. Once
// detected, frame generation is latched off for the rest of the process. This cannot unload ReShade or Streamline:
// the only safe way to play online is without the mod installed.
namespace dlssg::online_guard {

inline constexpr auto POLL_INTERVAL = std::chrono::seconds(2);

inline std::atomic_bool online_detected = false;
inline std::atomic<uint16_t> detected_port = 0u;
// Poll state; presents can arrive from several threads, and a busy poller makes the others skip.
inline std::shared_mutex poll_mutex;
inline std::chrono::steady_clock::time_point next_poll = {};
inline std::vector<uint16_t> logged_ports;
inline uint32_t port_log_count = 0u;

inline std::wstring ToLower(std::wstring_view text) {
  std::wstring lowered(text);
  std::ranges::transform(lowered, lowered.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return lowered;
}

inline bool CommandLineRequestsOnline(std::span<const std::wstring_view> tokens) {
  const std::wstring command_line = ToLower(GetCommandLineW());
  return std::ranges::any_of(tokens, [&command_line](std::wstring_view token) {
    return command_line.find(ToLower(token)) != std::wstring::npos;
  });
}

// Cheap enough for the present thread at this interval (one IPv4 UDP table snapshot).
inline void Poll(std::span<const uint16_t> online_udp_ports) {
  if (online_detected || online_udp_ports.empty()) return;
  const std::unique_lock lock(poll_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  const auto now = std::chrono::steady_clock::now();
  if (now < next_poll) return;
  next_poll = now + POLL_INTERVAL;

  DWORD size = 0u;
  if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0u) != ERROR_INSUFFICIENT_BUFFER) return;
  std::vector<uint8_t> buffer(size);
  if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0u) != NO_ERROR) return;
  const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
  const DWORD process_id = GetCurrentProcessId();
  std::vector<uint16_t> ports;
  for (DWORD index = 0u; index < table->dwNumEntries; ++index) {
    const auto& row = table->table[index];
    if (row.dwOwningPid != process_id) continue;
    // dwLocalPort holds the port in network byte order in its low 16 bits.
    ports.push_back(_byteswap_ushort(static_cast<uint16_t>(row.dwLocalPort)));
  }
  std::ranges::sort(ports);
  // Gate evidence: the local UDP ports the game binds in single player (Online is captured without the mod).
  if ((ports != logged_ports || port_log_count == 0u) && port_log_count < 64u) {
    ++port_log_count;
    std::string port_list;
    for (const uint16_t port : ports) {
      port_list += std::to_string(port) + " ";
    }
    renodx::utils::log::i("dlssg: game local UDP ports: ", (port_list.empty() ? std::string("none") : port_list));
    logged_ports = ports;
  }
  for (const uint16_t port : ports) {
    if (std::ranges::find(online_udp_ports, port) == online_udp_ports.end()) continue;
    detected_port = port;
    online_detected = true;
    renodx::utils::log::e(
        "dlssg: online session detected (UDP port ", port, " bound by the game); frame generation is off for the rest of "
        "this process. Quit and remove the mod before playing online.");
    return;
  }
}

}  // namespace dlssg::online_guard
