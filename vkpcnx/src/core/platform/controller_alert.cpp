#include "core/platform/controller_alert.hpp"

#ifdef __SWITCH__

#include <atomic>
#include <borealis/core/logger.hpp>
#include <chrono>
#include <cstring>
#include <mutex>
#include <switch.h>
#include <thread>
#include <vector>

#include "core/utils/thread.hpp"

namespace vkpcnx::platform {

namespace {

// Each blink is sent separately: a one-shot LED pattern the controller runs in
// firmware (fade in, hold, fade out) plus a buzz started at the same moment.
// Resending per blink keeps the two in step; a multi-cycle firmware pattern
// drifts away from a software-timed rumble within a few cycles.
constexpr int kStepMs = 100;   // baseMiniCycleDuration 0x8 = 8 × 12.5 ms
constexpr int kFadeSteps = 1;  // transitionSteps
constexpr int kHoldSteps = 2;  // finalStepDuration
constexpr int kBlinks = 4;
constexpr int kBlinkPeriodMs = 700;
// Buzz while the LED fades in and holds; stop as it starts fading out
constexpr int kBuzzMs = (kFadeSteps + kHoldSteps) * kStepMs;

// A light buzz at the motors' default resonant frequencies
// {amp_low, freq_low, amp_high, freq_high}
constexpr HidVibrationValue kBuzz = {0.2f, 160.0f, 0.2f, 320.0f};
constexpr HidVibrationValue kStill = {0.0f, 160.0f, 0.0f, 320.0f};

constexpr HidNpadIdType kNpads[] = {
  HidNpadIdType_Handheld,
  HidNpadIdType_No1,
  HidNpadIdType_No2,
  HidNpadIdType_No3,
  HidNpadIdType_No4,
  HidNpadIdType_No5,
  HidNpadIdType_No6,
  HidNpadIdType_No7,
  HidNpadIdType_No8,
};

// Bumped by every alert/clear; an alert thread whose generation is no longer
// current stops. alertMutex makes "still current? then send" atomic, so a
// clear can't be followed by a stale blink or buzz that outlives it.
std::atomic<unsigned> generation{0};
std::mutex alertMutex;

// Applies the pattern to every controller that has a HOME LED. hidsysSetNotificationLedPattern
// fails for pads without one (left Joy-Con, third-party pads); that's expected
// and ignored. hidsysInitialize/Exit are refcounted, so this is safe to call
// from any thread without keeping the service open between notifications.
void applyPattern(const HidsysNotificationLedPattern &pattern) {
  Result rc = hidsysInitialize();
  if (R_FAILED(rc)) {
    brls::Logger::warning("ControllerAlert: hidsysInitialize failed: 0x{:x}", rc);
    return;
  }
  int lit = 0;
  for (HidNpadIdType npad : kNpads) {
    HidsysUniquePadId pads[2]; // a dual Joy-Con npad has two
    s32 count = 0;
    if (R_FAILED(hidsysGetUniquePadsFromNpad(npad, pads, 2, &count)))
      continue;
    for (s32 i = 0; i < count; i++) {
      if (R_SUCCEEDED(hidsysSetNotificationLedPattern(&pattern, pads[i])))
        lit++;
    }
  }
  brls::Logger::debug("ControllerAlert: LED pattern set on {} controller(s)", lit);
  hidsysExit();
}

// Vibration handles of every connected pad. Switch Lite has no Joy-Con and
// no HOME LED; it rumble-only, via HidNpadIdType_Handheld + NpadHandheld.
bool addVibration(std::vector<HidVibrationDeviceHandle> &out, HidNpadIdType npad,
                  HidNpadStyleTag style, int count) {
  HidVibrationDeviceHandle handles[2];
  Result rc = hidInitializeVibrationDevices(handles, count, npad, style);
  if (R_FAILED(rc)) {
    if (count == 2) // Lite sometimes exposes a single handle
      return addVibration(out, npad, style, 1);
    return false;
  }
  out.insert(out.end(), handles, handles + count);
  return true;
}

std::vector<HidVibrationDeviceHandle> vibrationDevices() {
  std::vector<HidVibrationDeviceHandle> out;
  hidPermitVibration(true);
  // Always try the handheld slot first (Switch Lite / Joy-Cons attached).
  // Style set can read as 0 if this runs off the input thread, so don't
  // require hidGetNpadStyleSet to be non-zero.
  addVibration(out, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2);
  for (HidNpadIdType npad : kNpads) {
    if (npad == HidNpadIdType_Handheld && !out.empty())
      continue; // already added
    u32 styles = hidGetNpadStyleSet(npad);
    HidNpadStyleTag style;
    int count = 2;
    if (styles & HidNpadStyleTag_NpadFullKey)
      style = HidNpadStyleTag_NpadFullKey;
    else if (styles & HidNpadStyleTag_NpadHandheld)
      style = HidNpadStyleTag_NpadHandheld;
    else if (styles & HidNpadStyleTag_NpadJoyDual)
      style = HidNpadStyleTag_NpadJoyDual;
    else if (styles & HidNpadStyleTag_NpadJoyLeft)
      style = HidNpadStyleTag_NpadJoyLeft, count = 1;
    else if (styles & HidNpadStyleTag_NpadJoyRight)
      style = HidNpadStyleTag_NpadJoyRight, count = 1;
    else
      continue;
    addVibration(out, npad, style, count);
  }
  return out;
}

void sendAll(const std::vector<HidVibrationDeviceHandle> &devices, const HidVibrationValue &v) {
  for (auto handle : devices)
    hidSendVibrationValue(handle, &v);
}

// One fade in → hold → fade out, ending on intensity 0 so the LED stays dark
// afterwards
HidsysNotificationLedPattern blinkPattern() {
  HidsysNotificationLedPattern p;
  std::memset(&p, 0, sizeof p);
  p.baseMiniCycleDuration = 0x8;
  p.totalMiniCycles = 0x1; // 2 mini cycles
  p.totalFullCycles = 0x1;
  p.startIntensity = 0x0;
  for (int i = 0; i < 2; i++) {
    p.miniCycles[i].ledIntensity = i == 0 ? 0xF : 0x0;
    p.miniCycles[i].transitionSteps = kFadeSteps;
    p.miniCycles[i].finalStepDuration = i == 0 ? kHoldSteps : 0;
  }
  return p;
}

void runAlert(unsigned gen) {
  auto devices = vibrationDevices();
  brls::Logger::info("ControllerAlert: rumble on {} device(s)", devices.size());
  if (devices.empty())
    brls::Logger::warning(
      "ControllerAlert: no vibration devices (handheld style 0x{:x})",
      hidGetNpadStyleSet(HidNpadIdType_Handheld)
    );
  auto pattern = blinkPattern();
  auto start = std::chrono::steady_clock::now();
  using std::chrono::milliseconds;
  for (int i = 0; i < kBlinks; i++) {
    auto blink = start + milliseconds(i * kBlinkPeriodMs);
    std::this_thread::sleep_until(blink);
    {
      // Superseded: whoever bumped the generation owns the controllers now
      std::lock_guard<std::mutex> lock(alertMutex);
      if (generation != gen)
        return;
      sendAll(devices, kBuzz);
      applyPattern(pattern);
    }
    std::this_thread::sleep_until(blink + milliseconds(kBuzzMs));
    {
      std::lock_guard<std::mutex> lock(alertMutex);
      if (generation != gen)
        return;
      sendAll(devices, kStill);
    }
  }
}

} // namespace

void alertControllers() {
  unsigned gen;
  {
    std::lock_guard<std::mutex> lock(alertMutex);
    gen = ++generation;
  }
  utils::runDetached([gen] { runAlert(gen); });
}

void clearControllerAlert() {
  std::lock_guard<std::mutex> lock(alertMutex);
  ++generation;
  sendAll(vibrationDevices(), kStill);
  HidsysNotificationLedPattern p;
  std::memset(&p, 0, sizeof p); // all zero = off
  applyPattern(p);
}

} // namespace vkpcnx::platform

#else

namespace vkpcnx::platform {

void alertControllers() {}
void clearControllerAlert() {}

} // namespace vkpcnx::platform

#endif
