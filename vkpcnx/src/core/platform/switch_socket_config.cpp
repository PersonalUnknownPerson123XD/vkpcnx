// Socket buffer sizes for streaming. borealis' userAppInit() initialises the
// socket service with libnx's defaults, which give each UDP socket a 42 KB
// receive buffer (0xA500). A single video keyframe arrives as a burst of
// several hundred KB, so packets were dropped whenever the WebRTC thread
// didn't drain the socket fast enough, and the picture broke up until the
// next keyframe. Every socketInitialize() call is redirected here with
// -Wl,--wrap=socketInitialize (see CMakeLists.txt) to raise the limits.
#ifdef __SWITCH__

#include "core/platform/switch_socket_config.hpp"

#include <algorithm>
#include <borealis/core/logger.hpp>
#include <switch.h>

#include "core/saltynx/saltynx_api.h"

extern "C" {

Result __real_socketInitialize(const SocketInitConfig *config);

// Written from userAppInit (before main / the logger). Zero in BSS if wrap
// never ran.
Result vkpcnx_socket_init_rc = 1; // "not called" unless wrap stores a real rc
u32 vkpcnx_socket_bsd_sessions = 0;
u32 vkpcnx_socket_sb_efficiency = 0;
u32 vkpcnx_socket_udp_rx = 0;

Result __wrap_socketInitialize(const SocketInitConfig *config) {
  // socketInitializeDefault() passes NULL: libnx then uses its own defaults
  SocketInitConfig cfg = config ? *config : *socketGetDefaultInitConfig();
  // Transfer memory grows with sb_efficiency * (tcp max + udp buffers):
  // 8 * (0x40000 * 2 + 0x2400 + 0x80000) ≈ 8.3 MB of the app heap
  cfg.udp_rx_buf_size = std::max<u32>(cfg.udp_rx_buf_size, 0x80000);
  cfg.sb_efficiency = std::max<u32>(cfg.sb_efficiency, 8);
  // Keep the harica14 session count. harica15 raised this to 24 and every
  // HTTP request then failed in ~2–5 ms with curl "Couldn't connect to server"
  // (bsdInitialize / sessionmgrCreate can fail when sm or bsd:u won't clone
  // that many sessions). Application userAppInit already asks for 12.
  // The SaltySD handshake needs one free session; later in startup the
  // process has used most of its session budget (bsd alone takes ~14), so
  // grab Status Monitor's shared memory now, on a helper thread.
  {
    AppletType at = appletGetAppletType();
    if (at == AppletType_Application || at == AppletType_SystemApplication)
      vkpcnx_nxfps_early_start(174);
  }
  Result rc = __real_socketInitialize(&cfg);
  vkpcnx_socket_init_rc = rc;
  vkpcnx_socket_bsd_sessions = cfg.num_bsd_sessions;
  vkpcnx_socket_sb_efficiency = cfg.sb_efficiency;
  vkpcnx_socket_udp_rx = cfg.udp_rx_buf_size;
  return rc;
}

} // extern "C"

namespace vkpcnx::platform {

void logNetworkStartup() {
  brls::Logger::info(
    "NET: socketInitialize rc={:#x} bsd_sessions={} sb_efficiency={} udp_rx={:#x}",
    vkpcnx_socket_init_rc, vkpcnx_socket_bsd_sessions, vkpcnx_socket_sb_efficiency,
    vkpcnx_socket_udp_rx
  );
  s64 cur = 0, lim = 0, peak = 0;
  Result src = vkpcnx_session_usage(&cur, &lim, &peak);
  brls::Logger::info(
    "NET: sessions rc={:#x} current={} limit={} peak={}", src, (long long)cur, (long long)lim,
    (long long)peak
  );
  NifmInternetConnectionType type{};
  u32 strength = 0;
  NifmInternetConnectionStatus status{};
  Result nrc = nifmGetInternetConnectionStatus(&type, &strength, &status);
  const char *typeName = "?";
  if (type == NifmInternetConnectionType_WiFi)
    typeName = "wifi";
  else if (type == NifmInternetConnectionType_Ethernet)
    typeName = "ethernet";
  const char *statusName = "other";
  if (status == NifmInternetConnectionStatus_Connected)
    statusName = "connected";
  else if (status <= NifmInternetConnectionStatus_ConnectingUnknown4)
    statusName = "connecting";
  brls::Logger::info(
    "NET: nifmGetInternetConnectionStatus rc={:#x} type={} ({}) strength={} status={} ({})",
    nrc, static_cast<int>(type), typeName, strength, static_cast<int>(status), statusName
  );
}

} // namespace vkpcnx::platform

#endif
