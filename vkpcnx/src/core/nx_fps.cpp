#include "core/nx_fps.hpp"

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>

#ifdef __SWITCH__

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

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
constexpr u64 kTicksPerSec = 19200000ull;
constexpr uint32_t kTick60 = static_cast<uint32_t>(kTicksPerSec / 60);

SharedMemory g_shmem{};
NxFpsSharedBlock *g_block = nullptr;
bool g_mapped = false;
bool g_gaveUpSalty = false;
u64 g_lastTick = 0;
int g_tickItr = 0;
u64 g_lastInitAttempt = 0;
u64 g_lastFpsLog = 0;
ptrdiff_t g_offset = -1;
uintptr_t g_base = 0;

std::atomic<bool> g_beatStop{false};
std::thread g_beat;

NxFpsSharedBlock *findBlock(uintptr_t base) {
  for (ptrdiff_t off = 0; off + 4 <= 0x1000; off += 4) {
    auto *p = reinterpret_cast<NxFpsSharedBlock *>(base + off);
    if (p->MAGIC == kMagic)
      return p;
  }
  return nullptr;
}

NxFpsSharedBlock *findFreeSlot(uintptr_t base) {
  // Prefer a 174-byte zero run so we don't clobber another plugin's slot.
  const size_t need = sizeof(NxFpsSharedBlock);
  for (ptrdiff_t off = 0; off + static_cast<ptrdiff_t>(need) <= 0x1000; off += 4) {
    auto *p = reinterpret_cast<const uint8_t *>(base + off);
    bool empty = true;
    for (size_t i = 0; i < need; i++) {
      if (p[i]) {
        empty = false;
        break;
      }
    }
    if (empty)
      return reinterpret_cast<NxFpsSharedBlock *>(base + off);
  }
  return nullptr;
}

bool portUp(const char *name) {
  Handle h = 0;
  for (int i = 0; i < 8; i++) {
    if (R_SUCCEEDED(svcConnectToNamedPort(&h, name))) {
      svcCloseHandle(h);
      return true;
    }
    svcSleepThread(1000 * 1000);
  }
  return false;
}

bool mapShared(Handle handle) {
  shmemLoadRemote(&g_shmem, handle, 0x1000, Perm_Rw);
  Result rc = shmemMap(&g_shmem);
  if (R_FAILED(rc)) {
    brls::Logger::warning("NX-FPS: shmemMap failed: 0x{:x} handle={:#x}", rc, handle);
    return false;
  }
  g_mapped = true;
  return true;
}

void writeSample(NxFpsSharedBlock *b, uint32_t delta, float fps) {
  // Status Monitor averages all 10 FPSticks (zeros would make FPSavg huge → "n/d").
  b->FPSticks[g_tickItr] = delta ? delta : kTick60;
  g_tickItr = (g_tickItr + 1) % 10;
  uint64_t sum = 0;
  for (int i = 0; i < 10; i++)
    sum += b->FPSticks[i] ? b->FPSticks[i] : kTick60;
  const float avg = (10.f * static_cast<float>(kTicksPerSec)) / static_cast<float>(sum);
  b->FPSavg = avg;
  const uint8_t rounded = static_cast<uint8_t>(std::lround(std::clamp(avg, 1.f, 255.f)));
  b->FPS = fps > 0.f ? static_cast<uint8_t>(std::lround(std::clamp(fps, 1.f, 255.f))) : rounded;
  b->API = kApiEgl;
  b->pluginActive = true;
  b->frameNumber++;
}

void primeBlock(NxFpsSharedBlock *b) {
  for (int i = 0; i < 10; i++)
    b->FPSticks[i] = kTick60;
  b->API = kApiEgl;
  b->FPS = 60;
  b->FPSavg = 60.f;
  b->pluginActive = true;
  b->MAGIC = kMagic;
}

void logProcessContext() {
  AppletType at = appletGetAppletType();
  const bool isApp = at == AppletType_Application || at == AppletType_SystemApplication;
  u64 programId = 0, isKernelApp = 0;
  svcGetInfo(&programId, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&isKernelApp, InfoType_IsApplication, CUR_PROCESS_HANDLE, 0);
  // SaltyNX hijack.c: TID > 0x01FFFFFFFFFFFFFF or (TID & 0x1F00) != 0 →
  // "homebrew application, aborting bootstrap". Process name "hbloader" is
  // also skipped as title-replacement. Our HOME-shortcut TID is 05… so
  // SaltyNX will not inject NX-FPS into this process; we write the block
  // ourselves into the sysmodule's 0x1000 shared memory.
  const bool saltyWouldInject =
    programId > 0x010000000000FFFFULL && programId <= 0x01FFFFFFFFFFFFFFULL &&
    (programId & 0x1F00) == 0;
  brls::Logger::info(
    "NX-FPS: applet type={} ({}) programId={:016X} kernelIsApplication={} "
    "SaltyNX-would-inject={}",
    static_cast<int>(at), isApp ? "Application" : "applet", programId, (int)isKernelApp,
    saltyWouldInject
  );
  if (!isApp) {
    brls::Logger::info(
      "NX-FPS: not an Application — Status Monitor's pmdmntGetApplicationProcessId "
      "will miss us. Launch from a HOME shortcut, not hbmenu/album."
    );
  }
  if (!saltyWouldInject) {
    brls::Logger::info(
      "NX-FPS: SaltyNX will not inject Core/NX-FPS into this homebrew title "
      "(or a process named hbloader). Writing the shared-memory block ourselves."
    );
  }
}

bool tryInitOnce() {
  const bool inject = portUp("InjectServ");
  const bool salty = portUp("SaltySD");
  brls::Logger::info("NX-FPS: ports InjectServ={} SaltySD={}", inject, salty);
  if (!inject && !salty) {
    brls::Logger::info(
      "NX-FPS: SaltyNX is not running (no InjectServ/SaltySD). "
      "Install masagrator/SaltyNX and Status Monitor Overlay, then reboot."
    );
    g_gaveUpSalty = true;
    return false;
  }

  Result rc = vkpcnx_SaltySD_Connect();
  if (R_FAILED(rc)) {
    brls::Logger::warning("NX-FPS: SaltySD_Connect failed: 0x{:x} (port busy? will retry)", rc);
    return false;
  }
  brls::Logger::info("NX-FPS: SaltySD connected");

  Handle remote = 0;
  rc = vkpcnx_SaltySD_GetSharedMemoryHandle(&remote);
  brls::Logger::info("NX-FPS: GetSharedMemoryHandle rc=0x{:x} handle={:#x}", rc, remote);
  if (R_FAILED(rc) || !remote) {
    brls::Logger::warning("NX-FPS: no shared-memory handle");
    vkpcnx_SaltySD_Term();
    return false;
  }

  if (!mapShared(remote)) {
    vkpcnx_SaltySD_Term();
    return false;
  }

  g_base = reinterpret_cast<uintptr_t>(shmemGetAddr(&g_shmem));
  brls::Logger::info("NX-FPS: shm mapped at {:#x} size=0x1000", (unsigned long long)g_base);

  g_block = findBlock(g_base);
  if (g_block) {
    g_offset = reinterpret_cast<uintptr_t>(g_block) - g_base;
    brls::Logger::info("NX-FPS: found existing MAGIC 0x465053 at offset {}", (long long)g_offset);
  } else {
    ptrdiff_t offset = -1;
    rc = vkpcnx_SaltySD_CheckIfSharedMemoryAvailable(&offset, sizeof(NxFpsSharedBlock));
    brls::Logger::info(
      "NX-FPS: CheckIfSharedMemoryAvailable rc=0x{:x} offset={}", rc, (long long)offset
    );
    if (R_SUCCEEDED(rc) && offset >= 0 &&
        static_cast<size_t>(offset) + sizeof(NxFpsSharedBlock) <= 0x1000) {
      g_block = reinterpret_cast<NxFpsSharedBlock *>(g_base + offset);
      g_offset = offset;
      std::memset(g_block, 0, sizeof(*g_block));
    } else if (auto *slot = findFreeSlot(g_base)) {
      g_block = slot;
      g_offset = reinterpret_cast<uintptr_t>(slot) - g_base;
      brls::Logger::warning(
        "NX-FPS: alloc failed, using zeroed fallback slot at offset {}", (long long)g_offset
      );
    } else {
      // Last resort: offset 0. Status Monitor scans the whole page for MAGIC.
      g_block = reinterpret_cast<NxFpsSharedBlock *>(g_base);
      g_offset = 0;
      std::memset(g_block, 0, sizeof(*g_block));
      brls::Logger::warning("NX-FPS: no free slot, writing MAGIC at offset 0");
    }
  }

  vkpcnx_SaltySD_Term();

  if (!g_block) {
    shmemUnmap(&g_shmem);
    shmemClose(&g_shmem);
    g_mapped = false;
    return false;
  }

  primeBlock(g_block);
  {
    const uint32_t magic = g_block->MAGIC;
    const uint8_t fps = g_block->FPS;
    brls::Logger::info(
      "NX-FPS: writing block magic=0x{:x} offset={} addr={:#x} pluginActive=1 FPS={}",
      magic, (long long)g_offset, (unsigned long long)(g_base + g_offset), fps
    );
  }
  return true;
}

void beatLoop() {
  // Status Monitor sets pluginActive=false and waits 100ms. Keep the flag
  // alive even if the UI thread hitches while Tesla is open.
  while (!g_beatStop.load(std::memory_order_relaxed)) {
    if (g_block)
      g_block->pluginActive = true;
    svcSleepThread(10 * 1000 * 1000ull); // 10 ms
  }
}

} // namespace

void init() {
  logProcessContext();
  if (tryInitOnce() && !g_beat.joinable()) {
    g_beatStop = false;
    g_beat = std::thread(beatLoop);
  }
  g_lastInitAttempt = armGetSystemTick();
}

void tick() {
  if (!g_block) {
    if (g_gaveUpSalty)
      return;
    const u64 now = armGetSystemTick();
    if (now - g_lastInitAttempt < kTicksPerSec * 3)
      return;
    g_lastInitAttempt = now;
    brls::Logger::info("NX-FPS: retrying attach to SaltyNX shared memory");
    if (tryInitOnce() && !g_beat.joinable()) {
      g_beatStop = false;
      g_beat = std::thread(beatLoop);
    }
    return;
  }

  const u64 now = armGetSystemTick();
  uint32_t delta = g_lastTick ? static_cast<uint32_t>(now - g_lastTick) : kTick60;
  if (delta < kTicksPerSec / 240)
    delta = kTicksPerSec / 240; // cap at 240 fps
  if (delta > kTicksPerSec)
    delta = kTick60;
  g_lastTick = now;
  const float fps = static_cast<float>(kTicksPerSec) / static_cast<float>(delta);
  writeSample(g_block, delta, fps);

  if (now - g_lastFpsLog >= kTicksPerSec * 5) {
    g_lastFpsLog = now;
    const uint8_t fps = g_block->FPS;
    const float avg = g_block->FPSavg;
    const bool active = g_block->pluginActive;
    const uint64_t frame = g_block->frameNumber;
    brls::Logger::info(
      "NX-FPS: wrote FPS={} FPSavg={:.1f} pluginActive={} frame={} offset={}",
      fps, avg, active, (unsigned long long)frame, (long long)g_offset
    );
  }
}

void shutdown() {
  g_beatStop = true;
  if (g_beat.joinable())
    g_beat.join();
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
