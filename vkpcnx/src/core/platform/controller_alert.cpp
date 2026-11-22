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

// Apply the HOME-button notification LED pattern to every UniquePad we can
// see. The Switch Lite body is HidsysUniquePadType_Embedded; GetUniquePadsFromNpad
// on Handheld can miss it, so we also walk hidsysGetUniquePadIds. Both
// SetNotificationLedPattern and SetNotificationLedPatternWithTimeout are tried
// (the latter needs [9.0.0+] pad firmware). Pads without a HOME LED fail
// those calls; results are logged with an "LED:" prefix so a Lite test can
// tell whether the visible LED is this API or the power/charge LED.
// hidsysInitialize/Exit are refcounted, so this is safe from any thread.
const char *padTypeName(HidsysUniquePadType type) {
  switch (type) {
  case HidsysUniquePadType_Embedded:
    return "Embedded";
  case HidsysUniquePadType_FullKeyController:
    return "FullKey";
  case HidsysUniquePadType_RightController:
    return "Right";
  case HidsysUniquePadType_LeftController:
    return "Left";
  case HidsysUniquePadType_DebugPadController:
    return "DebugPad";
  default:
    return "Unknown";
  }
}

// Only the first alert of the run logs the hidsys probing; the rest stay quiet.
bool g_ledVerbose = true;

bool lightPad(const HidsysNotificationLedPattern &pattern, HidsysUniquePadId pad, const char *via) {
  HidsysUniquePadType type{};
  Result typeRc = hidsysGetUniquePadType(pad, &type);
  // The Switch Lite's own pad shows up as a DebugPad (type 4) and rejects both
  // commands with 0x64aca; its HOME LED is a GPIO line (see LiteLed below).
  if (R_SUCCEEDED(typeRc) && type == HidsysUniquePadType_DebugPadController) {
    if (g_ledVerbose)
      brls::Logger::info(
        "LED: hidsys pad={:#x} via={} is a DebugPad (type 4), skipped", (unsigned long long)pad.id, via
      );
    return false;
  }
  // 3 s timeout: long enough for the 4-blink alert
  Result timeoutRc = hidsysSetNotificationLedPatternWithTimeout(&pattern, pad, 3000000000ULL);
  Result setRc = R_SUCCEEDED(timeoutRc) ? timeoutRc : hidsysSetNotificationLedPattern(&pattern, pad);
  if (g_ledVerbose)
    brls::Logger::info(
      "LED: hidsys via={} pad={:#x} type_rc={:#x} type={} ({}) timeout_rc={:#x} set_rc={:#x}", via,
      (unsigned long long)pad.id, typeRc, static_cast<unsigned>(type),
      R_SUCCEEDED(typeRc) ? padTypeName(type) : "?", timeoutRc, setRc
    );
  return R_SUCCEEDED(timeoutRc) || R_SUCCEEDED(setRc);
}

// HOME-button notification LED of Joy-Cons / Pro controllers (hid:sys).
void applyPattern(const HidsysNotificationLedPattern &pattern) {
  Result rc = hidsysInitialize();
  if (R_FAILED(rc)) {
    if (g_ledVerbose)
      brls::Logger::warning("LED: hidsysInitialize failed rc={:#x}", rc);
    g_ledVerbose = false;
    return;
  }
  int lit = 0;
  for (HidNpadIdType npad : kNpads) {
    HidsysUniquePadId pads[8];
    s32 count = 0;
    Result nrc = hidsysGetUniquePadsFromNpad(npad, pads, 8, &count);
    if (g_ledVerbose && (count > 0 || npad == HidNpadIdType_Handheld))
      brls::Logger::info(
        "LED: GetUniquePadsFromNpad id={} rc={:#x} count={}", static_cast<int>(npad), nrc, count
      );
    if (R_FAILED(nrc) || count <= 0)
      continue;
    for (s32 i = 0; i < count; i++)
      if (lightPad(pattern, pads[i], "npad"))
        lit++;
  }
  if (g_ledVerbose)
    brls::Logger::info("LED: hidsys pattern accepted on {} pad(s)", lit);
  g_ledVerbose = false;
  hidsysExit();
}

// Switch Lite: the LED by the HOME button is a plain on/off GPIO line, not a
// hid:sys controller LED (hid:sys sees the Lite's pad as a DebugPad and
// answers 0x64aca). Found in marcinsz89/lite-led: gpio device code
// 0x35000065 (Hoag motherboard notification LED), access mode 3, output;
// High = lit. Original direction and value are restored afterwards.
constexpr u32 kLiteGpioDeviceCode = 0x35000065U;
constexpr u32 kLiteGpioAccessMode = 3U;

bool isLiteModel() {
  static int cached = -1;
  if (cached < 0) {
    SetSysProductModel model = SetSysProductModel_Invalid;
    Result rc = setsysGetProductModel(&model);
    cached = R_SUCCEEDED(rc) && model == SetSysProductModel_Hoag ? 1 : 0;
    brls::Logger::info(
      "LED: setsysGetProductModel rc={:#x} model={} -> {}", rc, static_cast<int>(model),
      cached ? "Switch Lite (GPIO LED)" : "not a Lite (hid:sys LED)"
    );
  }
  return cached == 1;
}

bool g_liteLedLit = false; // we drove it high at some point (guarded by alertMutex)

class LiteLed {
public:
  ~LiteLed() { end(); }

  bool begin() {
    Result rc = gpioInitialize();
    if (R_FAILED(rc)) {
      brls::Logger::warning("LED: gpioInitialize rc={:#x}", rc);
      return false;
    }
    gpio_ = true;
    rc = gpioOpenSession2(&pad_, kLiteGpioDeviceCode, kLiteGpioAccessMode);
    if (R_FAILED(rc)) {
      brls::Logger::warning(
        "LED: gpioOpenSession2({:#x}, {}) rc={:#x}", kLiteGpioDeviceCode, kLiteGpioAccessMode, rc
      );
      return false;
    }
    open_ = true;
    Result drc = gpioPadGetDirection(&pad_, &origDir_);
    Result vrc = gpioPadGetValue(&pad_, &origVal_);
    haveOrig_ = R_SUCCEEDED(drc) && R_SUCCEEDED(vrc);
    Result src = gpioPadSetDirection(&pad_, GpioDirection_Output);
    brls::Logger::info(
      "LED: gpio open ok; direction rc={:#x} dir={}; value rc={:#x} val={}; set output rc={:#x}", drc,
      static_cast<int>(origDir_), vrc, static_cast<int>(origVal_), src
    );
    return R_SUCCEEDED(src);
  }

  void set(bool on) {
    if (!open_)
      return;
    Result rc = gpioPadSetValue(&pad_, on ? GpioValue_High : GpioValue_Low);
    if (on)
      g_liteLedLit = true;
    if (R_FAILED(rc) || !logged_) {
      brls::Logger::info("LED: gpio set {} rc={:#x}", on ? "high" : "low", rc);
      logged_ = true;
    }
  }

  void end() {
    if (open_) {
      Result vrc = gpioPadSetValue(&pad_, haveOrig_ ? origVal_ : GpioValue_Low);
      Result drc = haveOrig_ ? gpioPadSetDirection(&pad_, origDir_) : 0;
      gpioPadClose(&pad_);
      open_ = false;
      brls::Logger::info("LED: gpio restore value rc={:#x} direction rc={:#x}", vrc, drc);
    }
    if (gpio_) {
      gpioExit();
      gpio_ = false;
    }
  }

private:
  GpioPadSession pad_{};
  GpioDirection origDir_ = GpioDirection_Output;
  GpioValue origVal_ = GpioValue_Low;
  bool haveOrig_ = false;
  bool open_ = false;
  bool gpio_ = false;
  bool logged_ = false;
};

// Vibration handles of every connected pad. Switch Lite rumble goes through
// HidNpadIdType_Handheld + NpadHandheld (confirmed working on the user's Lite).
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
  const bool lite = isLiteModel();
  LiteLed liteLed;
  const bool liteOk = lite && liteLed.begin();
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
      if (liteOk)
        liteLed.set(true);
      else if (!lite)
        applyPattern(pattern);
    }
    std::this_thread::sleep_until(blink + milliseconds(kBuzzMs));
    {
      std::lock_guard<std::mutex> lock(alertMutex);
      if (generation != gen)
        return;
      sendAll(devices, kStill);
      if (liteOk)
        liteLed.set(false);
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
  if (isLiteModel()) {
    if (g_liteLedLit) {
      LiteLed led;
      if (led.begin())
        led.set(false);
      g_liteLedLit = false;
    }
    return;
  }
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
