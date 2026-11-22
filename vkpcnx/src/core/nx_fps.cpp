#include "core/nx_fps.hpp"

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>

#ifdef __SWITCH__

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/saltynx/saltynx_api.h"

#define NX_PACKED __attribute__((packed))

namespace vkpcnx::nxfps {
namespace {

struct resolutionCalls {
  uint16_t width;
  uint16_t height;
  uint16_t calls;
};

// Layout must match Status Monitor Overlay's NxFpsSharedBlock (174 bytes).
struct NxFpsSharedBlock {
  uint32_t MAGIC;
  uint8_t FPS;
  float FPSavg;
  bool pluginActive;
  uint8_t FPSlocked;
  uint8_t FPSmode;
  uint8_t ZeroSync;
  uint8_t patchApplied;
  uint8_t API;
  uint32_t FPSticks[10];
  uint8_t Buffers;
  uint8_t SetBuffers;
  uint8_t ActiveBuffers;
  uint8_t SetActiveBuffers;
  union {
    struct {
      bool handheld : 1;
      bool docked : 1;
      bool reserved : 6;
    } ds;
    uint8_t general;
  } displaySync;
  resolutionCalls renderCalls[8];
  resolutionCalls viewportCalls[8];
  bool forceOriginalRefreshRate;
  bool dontForce60InDocked;
  bool forceSuspend;
  uint8_t currentRefreshRate;
  float readSpeedPerSecond;
  uint8_t FPSlockedDocked;
  uint64_t frameNumber;
  int8_t expectedSetBuffers;
} NX_PACKED;

static_assert(sizeof(NxFpsSharedBlock) == 174, "NxFpsSharedBlock must match Status Monitor");

constexpr uint32_t kMagic = 0x465053; // 'SPF'
constexpr uint8_t kApiEgl = 2;

SharedMemory g_shmem{};
NxFpsSharedBlock *g_block = nullptr;
bool g_mapped = false;
u64 g_lastTick = 0;
int g_tickItr = 0;

NxFpsSharedBlock *findBlock(uintptr_t base) {
  for (ptrdiff_t off = 0; off + 4 <= 0x1000; off += 4) {
    auto *p = reinterpret_cast<NxFpsSharedBlock *>(base + off);
    if (p->MAGIC == kMagic)
      return p;
  }
  return nullptr;
}

bool mapShared(Handle handle) {
  shmemLoadRemote(&g_shmem, handle, 0x1000, Perm_Rw);
  if (R_FAILED(shmemMap(&g_shmem))) {
    brls::Logger::warning("NX-FPS: shmemMap failed");
    return false;
  }
  g_mapped = true;
  return true;
}

} // namespace

void init() {
  AppletType at = appletGetAppletType();
  const bool isApp = at == AppletType_Application || at == AppletType_SystemApplication;
  if (!isApp) {
    brls::Logger::info(
      "NX-FPS: launched as applet (type {}); Status Monitor only reads an Application PID "
      "— use a HOME shortcut for the firmware FPS overlay",
      static_cast<int>(at)
    );
  }

  Result rc = vkpcnx_SaltySD_Connect();
  if (R_FAILED(rc)) {
    brls::Logger::info("NX-FPS: SaltyNX not found (SaltySD port), Status Monitor FPS unavailable");
    return;
  }

  Handle remote = 0;
  rc = vkpcnx_SaltySD_GetSharedMemoryHandle(&remote);
  if (R_FAILED(rc) || !remote) {
    brls::Logger::warning("NX-FPS: GetSharedMemoryHandle failed: 0x{:x}", rc);
    vkpcnx_SaltySD_Term();
    return;
  }

  if (!mapShared(remote)) {
    vkpcnx_SaltySD_Term();
    return;
  }

  uintptr_t base = reinterpret_cast<uintptr_t>(shmemGetAddr(&g_shmem));
  g_block = findBlock(base);
  if (!g_block) {
    // No NX-FPS plugin hooked us (static EGL). Allocate our own slot.
    ptrdiff_t offset = 0;
    rc = vkpcnx_SaltySD_CheckIfSharedMemoryAvailable(&offset, sizeof(NxFpsSharedBlock));
    if (R_SUCCEEDED(rc) && offset >= 0 &&
        static_cast<size_t>(offset) + sizeof(NxFpsSharedBlock) <= 0x1000) {
      g_block = reinterpret_cast<NxFpsSharedBlock *>(base + offset);
      std::memset(g_block, 0, sizeof(*g_block));
      g_block->MAGIC = kMagic;
      brls::Logger::info("NX-FPS: allocated Status Monitor block at offset {}", (long long)offset);
    } else {
      brls::Logger::warning("NX-FPS: CheckIfSharedMemoryAvailable failed: 0x{:x}", rc);
    }
  } else {
    brls::Logger::info("NX-FPS: found existing Status Monitor block");
  }

  vkpcnx_SaltySD_Term();

  if (!g_block) {
    shmemUnmap(&g_shmem);
    shmemClose(&g_shmem);
    g_mapped = false;
    return;
  }

  g_block->API = kApiEgl;
  g_block->pluginActive = true;
  brls::Logger::info("NX-FPS: writing FPS for Status Monitor (EGL, Application={})", isApp);
}

void tick() {
  if (!g_block)
    return;
  g_block->pluginActive = true;
  g_block->API = kApiEgl;

  const float fps = static_cast<float>(brls::Application::getFPS());
  g_block->FPS = static_cast<uint8_t>(std::lround(std::clamp(fps, 0.f, 255.f)));

  const u64 now = armGetSystemTick();
  if (g_lastTick) {
    g_block->FPSticks[g_tickItr] = static_cast<uint32_t>(now - g_lastTick);
    g_tickItr = (g_tickItr + 1) % 10;
    uint64_t sum = 0;
    int n = 0;
    for (int i = 0; i < 10; i++) {
      uint32_t t = g_block->FPSticks[i];
      if (t) {
        sum += t;
        n++;
      }
    }
    if (n && sum)
      g_block->FPSavg = (static_cast<float>(n) * 19200000.f) / static_cast<float>(sum);
    else
      g_block->FPSavg = fps;
  } else {
    g_block->FPSavg = fps;
  }
  g_lastTick = now;
  g_block->frameNumber++;
}

void shutdown() {
  if (g_block)
    g_block->pluginActive = false;
  g_block = nullptr;
  if (g_mapped) {
    shmemUnmap(&g_shmem);
    shmemClose(&g_shmem);
    g_mapped = false;
  }
}

} // namespace vkpcnx::nxfps

#else

namespace vkpcnx::nxfps {
void init() {}
void tick() {}
void shutdown() {}
} // namespace vkpcnx::nxfps

#endif
