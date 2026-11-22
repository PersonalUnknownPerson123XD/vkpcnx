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
bool g_earlyAdopted = false;
bool g_earlyLogged = false;
int g_lateAttempts = 0;
Result g_lastLateRc = 0xFFFFFFFF;
u64 g_lastTick = 0;
int g_tickItr = 0;
u64 g_lastInitAttempt = 0;
u64 g_lastFpsLog = 0;
ptrdiff_t g_offset = -1;
uintptr_t g_base = 0;

std::atomic<bool> g_beatStop{false};
std::atomic<bool> g_beatRunning{false};
std::thread g_beat;

// Status Monitor's CheckIfGameRunning(): finds MAGIC, writes pluginActive=false,
// sleeps 100 ms, expects true again -> GameRunning. The beat thread sees that
// false (only it re-arms the flag) and counts it: probes > 0 proves Status
// Monitor really scans THIS page and found our block.
std::atomic<uint32_t> g_probeCount{0};
std::atomic<u64> g_probeFirst{0};
std::atomic<u64> g_probeLast{0};
bool g_pmdmnt = false;
Result g_pmdmntInitRc = 0xFFFFFFFF;

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
  if (!g_beatRunning.load(std::memory_order_relaxed))
    b->pluginActive = true; // otherwise only the beat thread re-arms it
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

void logSessions(const char *when) {
  s64 cur = 0, lim = 0, peak = 0;
  Result rc = vkpcnx_session_usage(&cur, &lim, &peak);
  brls::Logger::info(
    "NX-FPS: sessions ({}) rc={:#x} current={} limit={} peak={}", when, rc, (long long)cur,
    (long long)lim, (long long)peak
  );
}

// Pick/clear the NX-FPS block in the mapped page and start writing.
// magicOff / allocOff come from the SaltySD handshake (-1 = none).
bool finishAttach(long magicOff, long allocOff) {
  g_base = reinterpret_cast<uintptr_t>(shmemGetAddr(&g_shmem));
  brls::Logger::info("NX-FPS: shm mapped at {:#x} size=0x1000", (unsigned long long)g_base);

  if (magicOff >= 0) {
    g_block = reinterpret_cast<NxFpsSharedBlock *>(g_base + magicOff);
    g_offset = magicOff;
    brls::Logger::info("NX-FPS: reusing existing MAGIC 0x465053 at offset {}", (long long)g_offset);
  } else if (allocOff >= 0) {
    g_block = reinterpret_cast<NxFpsSharedBlock *>(g_base + allocOff);
    g_offset = allocOff;
    std::memset(g_block, 0, sizeof(*g_block));
    brls::Logger::info("NX-FPS: using SaltySD-allocated slot at offset {}", (long long)g_offset);
  } else if (auto *slot = findFreeSlot(g_base)) {
    g_block = slot;
    g_offset = reinterpret_cast<uintptr_t>(slot) - g_base;
    brls::Logger::warning("NX-FPS: no allocation, using zeroed slot at offset {}", (long long)g_offset);
  } else {
    // Last resort: offset 0. Status Monitor scans the whole page for MAGIC.
    g_block = reinterpret_cast<NxFpsSharedBlock *>(g_base);
    g_offset = 0;
    std::memset(g_block, 0, sizeof(*g_block));
    brls::Logger::warning("NX-FPS: no free slot, writing MAGIC at offset 0");
  }

  primeBlock(g_block);
  const uint32_t magic = g_block->MAGIC;
  const uint8_t fps0 = g_block->FPS;
  brls::Logger::info(
    "NX-FPS: writing block magic=0x{:x} offset={} addr={:#x} pluginActive=1 FPS={}", magic,
    (long long)g_offset, (unsigned long long)(g_base + g_offset), fps0
  );
  return true;
}

void logEarly(const VkpcnxSaltyEarly *e, const char *tag) {
  brls::Logger::info(
    "NX-FPS: {} sessions with sm: open rc={:#x} current={} limit={} peak={}; "
    "sm: closed rc={:#x} current={} limit={} peak={}",
    tag, e->sessRc, (long long)e->sessCur, (long long)e->sessLimit, (long long)e->sessPeak,
    e->sessRcNoSm, (long long)e->sessCurNoSm, (long long)e->sessLimitNoSm,
    (long long)e->sessPeakNoSm
  );
  brls::Logger::info(
    "NX-FPS: {} SaltySD connect: attempts={} first rc={:#x} last rc={:#x}", tag, e->attempts,
    e->connectFirst, e->connectLast
  );
  brls::Logger::info(
    "NX-FPS: {} GetSharedMemoryHandle rc={:#x} handle={:#x}; shmemMap rc={:#x} mapped={}; "
    "MAGIC offset={}; CheckIfSharedMemoryAvailable rc={:#x} offset={}; Term rc={:#x}; "
    "smInitialize rc={:#x}",
    tag, e->handleRc, e->handle, e->mapRc, e->mapped, e->magicOffset, e->availRc, e->allocOffset,
    e->termRc, e->smReinitRc
  );
}

// Adopt the shared memory a handshake mapped. true: block ready.
bool adopt(const VkpcnxSaltyEarly *e) {
  g_earlyAdopted = true;
  if (!e->mapped)
    return false;
  g_shmem = e->shmem;
  g_mapped = true;
  return finishAttach(e->magicOffset, e->allocOffset);
}

// Retry from the main thread (frame loop). The handshake briefly closes the
// sm: session, so keep this rare.
bool tryLate() {
  g_lateAttempts++;
  VkpcnxSaltyEarly late;
  vkpcnx_nxfps_attach(&late, sizeof(NxFpsSharedBlock), 200);
  logEarly(&late, "late");
  return adopt(&late);
}

bool startBeat();

void beatLoop() {
  // Status Monitor sets pluginActive=false and waits 100ms. Keep the flag
  // alive even if the UI thread hitches while Tesla is open.
  while (!g_beatStop.load(std::memory_order_relaxed)) {
    if (g_block) {
      volatile bool *flag = reinterpret_cast<volatile bool *>(&g_block->pluginActive);
      if (!*flag) { // someone (Status Monitor) cleared it
        const u64 now = armGetSystemTick();
        u64 zero = 0;
        g_probeFirst.compare_exchange_strong(zero, now);
        g_probeLast.store(now);
        g_probeCount.fetch_add(1);
      }
      *flag = true;
    }
    svcSleepThread(2 * 1000 * 1000ull); // 2 ms, well under SM's 100 ms wait
  }
}

bool startBeat() {
  if (!g_beat.joinable()) {
    g_beatStop = false;
    g_beatRunning = true;
    g_beat = std::thread(beatLoop);
  }
  return true;
}

} // namespace

void attachEarly() {
  logSessions("before attach");
  VkpcnxSaltyEarly *e = vkpcnx_nxfps_early();
  vkpcnx_nxfps_attach(e, sizeof(NxFpsSharedBlock), 1500);
  logEarly(e, "early");
}

void init() {
  logProcessContext();
  logSessions("main loop start");
  VkpcnxSaltyEarly *e = vkpcnx_nxfps_early();
  if (e->state == 2 && adopt(e))
    startBeat();
  g_earlyAdopted = true;
  g_lastInitAttempt = armGetSystemTick();
  // Status Monitor drops GameRunning as soon as pmdmntGetApplicationProcessId
  // fails; look at what it will see for us.
  g_pmdmntInitRc = pmdmntInitialize();
  g_pmdmnt = R_SUCCEEDED(g_pmdmntInitRc);
  brls::Logger::info("NX-FPS: pmdmntInitialize rc={:#x}", g_pmdmntInitRc);
}

void tick() {
  if (!g_block) {
    // Up to 3 slow retries (every 5 s); each one briefly closes sm:
    if (g_lateAttempts >= 3)
      return;
    const u64 now = armGetSystemTick();
    if (now - g_lastInitAttempt < kTicksPerSec * 5)
      return;
    g_lastInitAttempt = now;
    if (tryLate())
      startBeat();
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
    // What Status Monitor sees of us
    const uint32_t probes = g_probeCount.load();
    const u64 first = g_probeFirst.load(), last = g_probeLast.load();
    u64 appPid = 0, ourPid = 0;
    Result prc = g_pmdmnt ? pmdmntGetApplicationProcessId(&appPid) : g_pmdmntInitRc;
    svcGetProcessId(&ourPid, CUR_PROCESS_HANDLE);
    brls::Logger::info(
      "NX-FPS: Status Monitor probes seen={} first={} ms ago last={} ms ago ({}); "
      "pmdmntGetApplicationProcessId rc={:#x} pid={} ours={} match={}",
      probes, first ? (long long)((now - first) * 1000 / kTicksPerSec) : -1LL,
      last ? (long long)((now - last) * 1000 / kTicksPerSec) : -1LL,
      probes ? "SM found our MAGIC" : "SM has not scanned this page",
      prc, (unsigned long long)appPid, (unsigned long long)ourPid, appPid == ourPid
    );
  }
}

void shutdown() {
  g_beatStop = true;
  g_beatRunning = false;
  if (g_beat.joinable())
    g_beat.join();
  if (g_block)
    g_block->pluginActive = false;
  g_block = nullptr;
  if (g_pmdmnt) {
    pmdmntExit();
    g_pmdmnt = false;
  }
  if (g_mapped) {
    shmemUnmap(&g_shmem);
    shmemClose(&g_shmem);
    g_mapped = false;
  }
}

} // namespace vkpcnx::nxfps

#else

namespace vkpcnx::nxfps {
void attachEarly() {}
void init() {}
void tick() {}
void shutdown() {}
} // namespace vkpcnx::nxfps

#endif
