#include "view/onscreen_keyboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <iterator>

using namespace vkpcnx;

namespace {

enum class Kind : uint8_t {
  Char, // types a character: EN / RU labels, blank in the Fn layer unless fnDik
  Key,  // Tab, Backspace, Enter, Esc, Space, arrows: same in every layer
  Mod,  // sticky Shift / Ctrl / Alt / Win
  Caps,
  Lang, // EN <-> RU
  Fn,   // Fn layer
};

struct KeyDef {
  uint8_t row;
  float units;
  Kind kind;
  uint8_t dik; // DirectInput scan code (§9.1)
  const char *en;
  const char *enShift;
  const char *ru;
  const char *ruShift;
  uint8_t fnDik = 0; // Fn layer: replaces dik/label (0 = keep for Key, blank for Char)
  const char *fnLabel = nullptr;
  const char *icon = nullptr; // Material icon drawn instead of the label
};

constexpr uint8_t kRows = 5;
constexpr float kRowUnits = 15.0f;

// clang-format off
const KeyDef kKeys[] = {
  // Row 0: ` 1–0 - = Backspace; Fn: F1–F12, Del
  {0, 1, Kind::Char, 41, "`", "~", "ё", "Ё", 41, "`"},
  {0, 1, Kind::Char, 2, "1", "!", "1", "!", 59, "F1"},
  {0, 1, Kind::Char, 3, "2", "@", "2", "\"", 60, "F2"},
  {0, 1, Kind::Char, 4, "3", "#", "3", "№", 61, "F3"},
  {0, 1, Kind::Char, 5, "4", "$", "4", ";", 62, "F4"},
  {0, 1, Kind::Char, 6, "5", "%", "5", "%", 63, "F5"},
  {0, 1, Kind::Char, 7, "6", "^", "6", ":", 64, "F6"},
  {0, 1, Kind::Char, 8, "7", "&", "7", "?", 65, "F7"},
  {0, 1, Kind::Char, 9, "8", "*", "8", "*", 66, "F8"},
  {0, 1, Kind::Char, 10, "9", "(", "9", "(", 67, "F9"},
  {0, 1, Kind::Char, 11, "0", ")", "0", ")", 68, "F10"},
  {0, 1, Kind::Char, 12, "-", "_", "-", "_", 87, "F11"},
  {0, 1, Kind::Char, 13, "=", "+", "=", "+", 88, "F12"},
  {0, 2, Kind::Key, 14, "", "", "", "", 211, "Del", "\uE14A"},
  // Row 1: Tab q–p [ ] \; Fn: Ins Home PgUp PrtSc ScrLk Pause
  {1, 1.5f, Kind::Key, 15, "Tab", "Tab", "Tab", "Tab"},
  {1, 1, Kind::Char, 16, "q", "Q", "й", "Й", 210, "Ins"},
  {1, 1, Kind::Char, 17, "w", "W", "ц", "Ц", 199, "Home"},
  {1, 1, Kind::Char, 18, "e", "E", "у", "У", 201, "PgUp"},
  {1, 1, Kind::Char, 19, "r", "R", "к", "К", 86, "PrtSc"}, // 86 as the web client does
  {1, 1, Kind::Char, 20, "t", "T", "е", "Е", 70, "ScrLk"},
  {1, 1, Kind::Char, 21, "y", "Y", "н", "Н", 197, "Pause"},
  {1, 1, Kind::Char, 22, "u", "U", "г", "Г"},
  {1, 1, Kind::Char, 23, "i", "I", "ш", "Ш"},
  {1, 1, Kind::Char, 24, "o", "O", "щ", "Щ"},
  {1, 1, Kind::Char, 25, "p", "P", "з", "З"},
  {1, 1, Kind::Char, 26, "[", "{", "х", "Х"},
  {1, 1, Kind::Char, 27, "]", "}", "ъ", "Ъ"},
  {1, 1.5f, Kind::Char, 43, "\\", "|", "\\", "/"},
  // Row 2: Caps a–l ; ' Enter; Fn: Del End PgDn
  {2, 1.75f, Kind::Caps, 58, "Caps", "Caps", "Caps", "Caps"},
  {2, 1, Kind::Char, 30, "a", "A", "ф", "Ф", 211, "Del"},
  {2, 1, Kind::Char, 31, "s", "S", "ы", "Ы", 207, "End"},
  {2, 1, Kind::Char, 32, "d", "D", "в", "В", 209, "PgDn"},
  {2, 1, Kind::Char, 33, "f", "F", "а", "А"},
  {2, 1, Kind::Char, 34, "g", "G", "п", "П"},
  {2, 1, Kind::Char, 35, "h", "H", "р", "Р"},
  {2, 1, Kind::Char, 36, "j", "J", "о", "О"},
  {2, 1, Kind::Char, 37, "k", "K", "л", "Л"},
  {2, 1, Kind::Char, 38, "l", "L", "д", "Д"},
  {2, 1, Kind::Char, 39, ";", ":", "ж", "Ж"},
  {2, 1, Kind::Char, 40, "'", "\"", "э", "Э"},
  {2, 2.25f, Kind::Key, 28, "Enter", "Enter", "Enter", "Enter", 0, nullptr, "\uE31B"},
  // Row 3: Shift z–/ Shift Up
  {3, 2.25f, Kind::Mod, 42, "Shift", "Shift", "Shift", "Shift"},
  {3, 1, Kind::Char, 44, "z", "Z", "я", "Я"},
  {3, 1, Kind::Char, 45, "x", "X", "ч", "Ч"},
  {3, 1, Kind::Char, 46, "c", "C", "с", "С"},
  {3, 1, Kind::Char, 47, "v", "V", "м", "М"},
  {3, 1, Kind::Char, 48, "b", "B", "и", "И"},
  {3, 1, Kind::Char, 49, "n", "N", "т", "Т"},
  {3, 1, Kind::Char, 50, "m", "M", "ь", "Ь"},
  {3, 1, Kind::Char, 51, ",", "<", "б", "Б"},
  {3, 1, Kind::Char, 52, ".", ">", "ю", "Ю"},
  {3, 1, Kind::Char, 53, "/", "?", ".", ","},
  {3, 1.75f, Kind::Mod, 54, "Shift", "Shift", "Shift", "Shift"},
  {3, 1, Kind::Key, 200, "", "", "", "", 0, nullptr, "\uE316"},
  // Row 4: Esc Ctrl Win Alt EN/RU Space Fn Left Down Right
  {4, 1.25f, Kind::Key, 1, "Esc", "Esc", "Esc", "Esc"},
  {4, 1.25f, Kind::Mod, 29, "Ctrl", "Ctrl", "Ctrl", "Ctrl"},
  {4, 1, Kind::Mod, 219, "Win", "Win", "Win", "Win"},
  {4, 1.25f, Kind::Mod, 56, "Alt", "Alt", "Alt", "Alt"},
  {4, 1.25f, Kind::Lang, 0, "EN", "EN", "RU", "RU"},
  {4, 5, Kind::Key, 57, "", "", "", ""},
  {4, 1, Kind::Fn, 0, "Fn", "Fn", "Fn", "Fn"},
  {4, 1, Kind::Key, 203, "", "", "", "", 0, nullptr, "\uE314"},
  {4, 1, Kind::Key, 208, "", "", "", "", 0, nullptr, "\uE313"},
  {4, 1, Kind::Key, 205, "", "", "", "", 0, nullptr, "\uE315"},
};
// clang-format on

constexpr int kKeyCount = static_cast<int>(std::size(kKeys));

constexpr uint8_t kDikBackspace = 14, kDikEnter = 28, kDikSpace = 57, kDikLeftShift = 42;

int findKey(Kind kind, uint8_t dik) {
  for (int i = 0; i < kKeyCount; i++)
    if (kKeys[i].kind == kind && kKeys[i].dik == dik)
      return i;
  return 0;
}

// Units from the row's left edge to the key
float unitsBefore(int key) {
  float u = 0;
  for (int i = key - 1; i >= 0 && kKeys[i].row == kKeys[key].row; i--)
    u += kKeys[i].units;
  return u;
}

bool isLetter(const char *label) {
  auto c = static_cast<unsigned char>(label[0]);
  return (c >= 'a' && c <= 'z') || c == 0xD0 || c == 0xD1; // Latin or Cyrillic
}

// Geometry (window points)
constexpr float kPadX = 12, kPadBottom = 10, kStripH = 38, kGap = 6;
constexpr float kStripButtonW = 36, kStripButtonH = 28;
// A finger may wander this far outside its key before the press is cancelled
constexpr float kSlideTolerance = 10;
constexpr auto kLockWindow = std::chrono::milliseconds(400);

NVGcolor rgba(int r, int g, int b, int a) { return nvgRGBA(r, g, b, a); }

void fillRounded(NVGcontext *vg, const brls::Rect &r, float radius, NVGcolor color) {
  nvgBeginPath(vg);
  nvgRoundedRect(vg, r.getMinX(), r.getMinY(), r.getWidth(), r.getHeight(), radius);
  nvgFillColor(vg, color);
  nvgFill(vg);
}

void text(NVGcontext *vg, int font, float size, NVGcolor color, float cx, float cy, const char *s) {
  nvgFontFaceId(vg, font);
  nvgFontSize(vg, size);
  nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  nvgFillColor(vg, color);
  nvgText(vg, cx, cy, s, nullptr);
}

void strokeRounded(NVGcontext *vg, float x, float y, float w, float h, float radius, NVGcolor color, float width) {
  nvgBeginPath(vg);
  nvgRoundedRect(vg, x, y, w, h, radius);
  nvgStrokeColor(vg, color);
  nvgStrokeWidth(vg, width);
  nvgStroke(vg);
}

NVGcolor hexRgb(uint32_t rgb, int a = 255) {
  return nvgRGBA((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, a);
}

NVGcolor fade(NVGcolor c, float k) {
  c.a *= k;
  return c;
}

// Horizon's Switch overlay palette (horizon-theme/src/switch.rs)
struct SwitchPalette {
  bool dark = true;
  uint32_t page = 0x2b2b2b, plate = 0x404040, plateSoft = 0x353535, surface = 0x323232,
    rule = 0x4d4d4d, ink = 0xf5f5f5, inkSoft = 0xd6d6d6, muted = 0xb4b4b4, accent = 0x00c3e3,
    onAccent = 0x0b1b20;
};

SwitchPalette switchPalette() {
  // Follow the console's appearance; the rest of the app may stay dark.
  bool dark = brls::Application::getThemeVariant() != brls::ThemeVariant::LIGHT;
#ifdef __SWITCH__
  // The app forces a dark UI; the keyboard follows the console's own appearance.
  {
    ColorSetId id{};
    if (R_SUCCEEDED(setsysGetColorSetId(&id)))
      dark = id != ColorSetId_Light;
  }
#endif
  if (dark)
    return {};
  return SwitchPalette{
    false, 0xebebeb, 0xdcdcdc, 0xf5f5f5, 0xffffff, 0xcfcfcf, 0x2d2d2d, 0x464646, 0x5a5a5a,
    0x0062a8, 0xffffff
  };
}

constexpr float kRadiusKey = 8.f;
constexpr float kRadiusPanel = 14.f;
constexpr float kFrame = 3.f;
constexpr float kFrameGap = 2.f;
constexpr float kFocusMoveSecs = 0.14f;
constexpr float kGlowPeriodSecs = 1.6f;

float easeOut(float t) {
  t = std::clamp(t, 0.f, 1.f);
  float u = 1.f - t;
  return 1.f - u * u * u;
}

float glow(float secs) {
  return 0.675f + 0.325f * std::sin(secs / kGlowPeriodSecs * 6.2831853f);
}

float nowSecs() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

void focusFrame(NVGcontext *vg, const SwitchPalette &p, const brls::Rect &r) {
  const float x = r.getMinX(), y = r.getMinY(), w = r.getWidth(), h = r.getHeight();
  const NVGcolor accent = hexRgb(p.accent);
  const float g = glow(nowSecs());
  const float step = 1.f;
  const int rings = std::max(1, int(std::round(kFrame)));
  for (int i = 0; i < rings; i++) {
    float o = kFrameGap + i * step;
    strokeRounded(vg, x - o, y - o, w + 2 * o, h + 2 * o, kRadiusKey + o, accent, 1.25f);
  }
  for (int i = 0; i < 4; i++) {
    float o = kFrameGap + kFrame + i * 1.5f;
    float k = g * 0.35f * (1.f - i / 4.f);
    strokeRounded(vg, x - o, y - o, w + 2 * o, h + 2 * o, kRadiusKey + o, fade(accent, k), 1.5f);
  }
}

// Round (or pill) button glyph; returns its width.
float hintGlyph(NVGcontext *vg, int font, const SwitchPalette &p, float x, float y, float size, const char *label) {
  nvgFontFaceId(vg, font);
  nvgFontSize(vg, 12);
  float bounds[4];
  nvgTextBounds(vg, 0, 0, label, nullptr, bounds);
  float tw = bounds[2] - bounds[0];
  float w = std::max(size, tw + 12);
  fillRounded(vg, brls::Rect(x, y, w, size), size / 2, hexRgb(p.plate));
  strokeRounded(vg, x, y, w, size, size / 2, hexRgb(p.muted), 1.25f);
  text(vg, font, 12, hexRgb(p.ink), x + w / 2, y + size / 2, label);
  return w;
}

struct Hint {
  const char *glyph;
  const char *label;
};

void hintRow(NVGcontext *vg, int font, const SwitchPalette &p, float cx, float y, const Hint *hints, int n) {
  constexpr float size = 22, gap = 6, between = 16;
  nvgFontFaceId(vg, font);
  nvgFontSize(vg, 12);
  float widths[16][2]{};
  float total = between * std::max(0, n - 1);
  for (int i = 0; i < n; i++) {
    float bounds[4];
    nvgTextBounds(vg, 0, 0, hints[i].glyph, nullptr, bounds);
    widths[i][0] = std::max(size, (bounds[2] - bounds[0]) + 12);
    nvgFontSize(vg, 13);
    nvgTextBounds(vg, 0, 0, hints[i].label, nullptr, bounds);
    widths[i][1] = bounds[2] - bounds[0];
    nvgFontSize(vg, 12);
    total += widths[i][0] + gap + widths[i][1];
  }
  float x = std::round(cx - total / 2);
  for (int i = 0; i < n; i++) {
    hintGlyph(vg, font, p, x, y, size, hints[i].glyph);
    nvgFontFaceId(vg, font);
    nvgFontSize(vg, 13);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, hexRgb(p.inkSoft));
    nvgText(vg, x + widths[i][0] + gap, y + size / 2, hints[i].label, nullptr);
    x += widths[i][0] + gap + widths[i][1] + between;
  }
}

} // namespace

OnscreenKeyboard::OnscreenKeyboard() {
  for (int i = 0; i < kKeyCount; i++)
    if (kKeys[i].kind == Kind::Mod)
      mods_[i] = Mod::Off;
  focus_ = findKey(Kind::Char, 34); // G: middle of the letters
}

// ---- layout ------------------------------------------------------------------

brls::Rect OnscreenKeyboard::keyRect(int key) const {
  auto f = layoutFrame();
  float unit = (f.getWidth() - 2 * kPadX + kGap) / kRowUnits;
  float rowH = (f.getHeight() - kStripH - kPadBottom - (kRows - 1) * kGap) / kRows;
  const auto &k = kKeys[key];
  return brls::Rect(
    f.getMinX() + kPadX + unitsBefore(key) * unit,
    f.getMinY() + kStripH + k.row * (rowH + kGap),
    k.units * unit - kGap,
    rowH
  );
}

brls::Rect OnscreenKeyboard::stripButtonRect(StripButton b) const {
  auto f = layoutFrame();
  float right = f.getMaxX() - kPadX - (b == StripButton::Dock ? kStripButtonW + kGap : 0);
  return brls::Rect(
    right - kStripButtonW,
    f.getMinY() + (kStripH - kStripButtonH) / 2,
    kStripButtonW,
    kStripButtonH
  );
}

int OnscreenKeyboard::keyAt(brls::Point p) const {
  // Rects grown by half a gap so the gaps belong to the nearest key
  for (int i = 0; i < kKeyCount; i++) {
    auto r = keyRect(i);
    if (p.x >= r.getMinX() - kGap / 2 && p.x < r.getMaxX() + kGap / 2 &&
        p.y >= r.getMinY() - kGap / 2 && p.y < r.getMaxY() + kGap / 2)
      return i;
  }
  return -1;
}

OnscreenKeyboard::StripButton OnscreenKeyboard::stripButtonAt(brls::Point p) const {
  for (auto b : {StripButton::Dock, StripButton::Close})
    if (stripButtonRect(b).pointInside(p))
      return b;
  return StripButton::None;
}

// ---- key state ---------------------------------------------------------------

bool OnscreenKeyboard::shiftActive() const {
  for (const auto &[key, m] : mods_) {
    if (m != Mod::Off && (kKeys[key].dik == 42 || kKeys[key].dik == 54))
      return true;
  }
  return false;
}

uint8_t OnscreenKeyboard::dikFor(int key) const {
  const auto &k = kKeys[key];
  if (fnLayer_ && k.fnDik)
    return k.fnDik;
  if (fnLayer_ && k.kind == Kind::Char)
    return 0;
  return k.dik;
}

std::string OnscreenKeyboard::labelFor(int key) const {
  const auto &k = kKeys[key];
  if (fnLayer_ && k.fnLabel)
    return k.fnLabel;
  if (fnLayer_ && k.kind == Kind::Char)
    return "";
  if (k.kind == Kind::Lang)
    return russian_ ? k.ru : k.en;
  const char *plain = russian_ ? k.ru : k.en;
  const char *shifted = russian_ ? k.ruShift : k.enShift;
  bool upper = shiftActive();
  if (k.kind == Kind::Char && isLetter(plain))
    upper = upper != caps_;
  return upper ? shifted : plain;
}

void OnscreenKeyboard::tapModifier(int key) {
  auto now = std::chrono::steady_clock::now();
  Mod &m = mods_[key];
  uint8_t dik = kKeys[key].dik;
  switch (m) {
  case Mod::Off:
    m = Mod::Latched;
    latchedAt_[key] = now;
    if (onKey)
      onKey(dik, true, "");
    break;
  case Mod::Latched:
    if (now - latchedAt_[key] <= kLockWindow) {
      m = Mod::Locked;
      break;
    }
    [[fallthrough]];
  case Mod::Locked:
    m = Mod::Off;
    if (onKey)
      onKey(dik, false, "");
    break;
  }
}

void OnscreenKeyboard::releaseLatched() {
  for (auto &[key, m] : mods_) {
    if (m == Mod::Latched) {
      m = Mod::Off;
      if (onKey)
        onKey(kKeys[key].dik, false, "");
    }
  }
}

void OnscreenKeyboard::keyDown(int source, int key) {
  if (presses_.count(source))
    keyUp(source);
  const auto &k = kKeys[key];
  Press press{key, 0};
  switch (k.kind) {
  case Kind::Mod:
    tapModifier(key);
    break;
  case Kind::Lang:
    russian_ = !russian_;
    break;
  case Kind::Fn:
    fnLayer_ = !fnLayer_;
    break;
  case Kind::Caps:
    caps_ = !caps_;
    press.dik = k.dik;
    if (onKey)
      onKey(k.dik, true, "");
    if (onCapsChanged)
      onCapsChanged(caps_);
    break;
  case Kind::Char:
  case Kind::Key: {
    press.dik = dikFor(key);
    if (!press.dik)
      return; // blank in this layer
    std::string locale;
    if (k.kind == Kind::Char && !fnLayer_)
      locale = russian_ ? "ru-RU" : "en-US";
    if (onKey)
      onKey(press.dik, true, locale);
    break;
  }
  }
  presses_[source] = press;
  pressCount_[key]++;
}

void OnscreenKeyboard::keyUp(int source) {
  auto it = presses_.find(source);
  if (it == presses_.end())
    return;
  Press press = it->second;
  presses_.erase(it);
  if (--pressCount_[press.key] <= 0)
    pressCount_.erase(press.key);
  if (!press.dik)
    return;
  if (onKey)
    onKey(press.dik, false, "");
  if (kKeys[press.key].kind != Kind::Caps)
    releaseLatched(); // the latched modifiers applied to this key
}

void OnscreenKeyboard::releaseAll() {
  for (const auto &[source, press] : presses_)
    if (press.dik && onKey)
      onKey(press.dik, false, "");
  presses_.clear();
  pressCount_.clear();
  stripTouches_.clear();
  for (auto &[key, m] : mods_) {
    if (m != Mod::Off) {
      m = Mod::Off;
      if (onKey)
        onKey(kKeys[key].dik, false, "");
    }
  }
}

// ---- input -------------------------------------------------------------------

bool OnscreenKeyboard::touchDown(int finger, brls::Point p) {
  if (!getFrame().pointInside(p))
    return false;
  showFocus_ = false;
  if (p.y < getFrame().getMinY() + kStripH) {
    stripTouches_[finger] = stripButtonAt(p);
    return true;
  }
  int key = keyAt(p);
  if (key >= 0)
    keyDown(finger, key);
  return true;
}

void OnscreenKeyboard::touchMove(int finger, brls::Point p) {
  auto strip = stripTouches_.find(finger);
  if (strip != stripTouches_.end()) {
    if (stripButtonAt(p) != strip->second)
      strip->second = StripButton::None; // slid off: no action on release
    return;
  }
  auto it = presses_.find(finger);
  if (it == presses_.end())
    return;
  auto r = keyRect(it->second.key);
  if (p.x < r.getMinX() - kSlideTolerance || p.x > r.getMaxX() + kSlideTolerance ||
      p.y < r.getMinY() - kSlideTolerance || p.y > r.getMaxY() + kSlideTolerance)
    keyUp(finger);
}

void OnscreenKeyboard::touchUp(int finger) {
  auto strip = stripTouches_.find(finger);
  if (strip != stripTouches_.end()) {
    StripButton b = strip->second;
    stripTouches_.erase(strip);
    if (b == StripButton::Close && onClose)
      onClose();
    else if (b == StripButton::Dock && onDockToggle)
      onDockToggle();
    return;
  }
  keyUp(finger);
}

void OnscreenKeyboard::moveFocus(int dx, int dy) {
  if (!showFocus_) {
    showFocus_ = true; // first move just shows where the focus is
    return;
  }
  const auto &cur = kKeys[focus_];
  if (dx != 0) {
    int next = focus_ + dx;
    if (next >= 0 && next < kKeyCount && kKeys[next].row == cur.row)
      focus_ = next;
    return;
  }
  int row = std::clamp(static_cast<int>(cur.row) + dy, 0, kRows - 1);
  if (row == cur.row)
    return;
  // The key in the target row whose centre is closest to the current one
  float centre = unitsBefore(focus_) + cur.units / 2;
  float best = 1e9f;
  for (int i = 0; i < kKeyCount; i++) {
    if (kKeys[i].row != row)
      continue;
    float d = std::fabs(unitsBefore(i) + kKeys[i].units / 2 - centre);
    if (d < best) {
      best = d;
      focus_ = i;
    }
  }
}

void OnscreenKeyboard::pressFocused(bool down) {
  showFocus_ = true;
  if (down)
    keyDown(kSourceFocus, focus_);
  else
    keyUp(kSourceFocus);
}

void OnscreenKeyboard::pressAction(Action action, bool down) {
  int source = kSourceActionBase - static_cast<int>(action);
  if (!down) {
    keyUp(source);
    return;
  }
  int key = 0;
  switch (action) {
  case Action::Backspace:
    key = findKey(Kind::Key, kDikBackspace);
    break;
  case Action::Shift:
    key = findKey(Kind::Mod, kDikLeftShift);
    break;
  case Action::Space:
    key = findKey(Kind::Key, kDikSpace);
    break;
  case Action::Language:
    key = findKey(Kind::Lang, 0);
    break;
  case Action::FnLayer:
    key = findKey(Kind::Fn, 0);
    break;
  case Action::Enter:
    key = findKey(Kind::Key, kDikEnter);
    break;
  }
  keyDown(source, key);
}

// ---- drawing -----------------------------------------------------------------

void OnscreenKeyboard::draw(
  NVGcontext *vg,
  float x,
  float y,
  float width,
  float height,
  brls::Style style,
  brls::FrameContext *ctx
) {
  (void)style;
  (void)ctx;
  const auto pal = switchPalette();
  const int regular = brls::Application::getDefaultFont();
  const int material = brls::Application::getFont(brls::FONT_MATERIAL_ICONS);
  const NVGcolor keyFill = pal.dark ? hexRgb(pal.plate) : hexRgb(pal.surface);
  const NVGcolor ink = hexRgb(pal.ink);
  const NVGcolor accent = hexRgb(pal.accent);
  const NVGcolor onAccent = hexRgb(pal.onAccent);

  // Soft drop shadow, then a floating rounded plate (not a full-width bar)
  const float inset = 8;
  brls::Rect plate(x + inset, y + 4, width - 2 * inset, height - 4);
  fillRounded(vg, brls::Rect(plate.getMinX(), plate.getMinY() + 4, plate.getWidth(), plate.getHeight()), kRadiusPanel, nvgRGBA(0, 0, 0, 46));
  fillRounded(vg, plate, kRadiusPanel, hexRgb(pal.page, 248));

  // Strip: layer pill, round button hints, dock / close
  float stripMid = y + kStripH / 2;
  const char *layer = fnLayer_ ? "Fn" : russian_ ? "RU" : "EN";
  brls::Rect pill(x + kPadX + inset, stripMid - 11, 44, 22);
  bool layerOn = fnLayer_ || russian_;
  fillRounded(vg, pill, 11, layerOn ? accent : keyFill);
  if (!layerOn)
    strokeRounded(vg, pill.getMinX(), pill.getMinY(), pill.getWidth(), pill.getHeight(), 11, hexRgb(pal.rule), 1);
  text(vg, regular, 13, layerOn ? onAccent : ink, pill.getMidX(), pill.getMidY(), layer);

  auto dock = stripButtonRect(StripButton::Dock);
  auto close = stripButtonRect(StripButton::Close);
  Hint hints[] = {
    {"A", "Нажать"}, {"Y", "Стереть"}, {"X", "Shift"}, {"B", "Пробел"},
    {"L", "Язык"},   {"R", "Fn"},      {"+", "Enter"}, {"–", "Закрыть"},
  };
  const float hintsTop = y + (kStripH - 22) / 2;
  const float hintsLeft = pill.getMaxX() + 10;
  const float hintsRight = dock.getMinX() - kGap;
  nvgSave(vg);
  nvgIntersectScissor(vg, hintsLeft, y, std::max(0.f, hintsRight - hintsLeft), kStripH);
  hintRow(vg, regular, pal, (hintsLeft + hintsRight) / 2, hintsTop, hints, 8);
  nvgRestore(vg);

  auto stripBtn = [&](const brls::Rect &r, const char *icon) {
    fillRounded(vg, r, r.getHeight() / 2, keyFill);
    strokeRounded(vg, r.getMinX(), r.getMinY(), r.getWidth(), r.getHeight(), r.getHeight() / 2, hexRgb(pal.muted), 1.25f);
    text(vg, material, 18, ink, r.getMidX(), r.getMidY(), icon);
  };
  stripBtn(dock, "\uE8D5");  // swap_vert
  stripBtn(close, "\uE5CD"); // close

  brls::Rect focusTarget{};
  bool haveFocus = false;

  for (int i = 0; i < kKeyCount; i++) {
    const auto &k = kKeys[i];
    auto r = keyRect(i);
    std::string s = labelFor(i);
    bool blank = k.kind == Kind::Char && fnLayer_ && !k.fnDik;
    bool fnSwapped = fnLayer_ && k.fnLabel;

    bool active = false, locked = false;
    if (k.kind == Kind::Mod) {
      auto m = mods_.at(i);
      active = m != Mod::Off;
      locked = m == Mod::Locked;
    } else if (k.kind == Kind::Caps) {
      active = locked = caps_;
    } else if (k.kind == Kind::Lang) {
      active = russian_;
    } else if (k.kind == Kind::Fn) {
      active = fnLayer_;
    }

    bool focused = showFocus_ && i == focus_;
    bool pressed = pressCount_.count(i) > 0;
    NVGcolor bg = keyFill;
    NVGcolor fg = ink;
    if (k.kind == Kind::Key && k.dik == kDikEnter) {
      bg = accent;
      fg = onAccent;
    } else if (active) {
      bg = fade(accent, 0.28f);
      fg = ink;
    } else if (pressed) {
      bg = pal.dark ? hexRgb(pal.rule) : hexRgb(pal.plateSoft);
    } else if (focused) {
      bg = pal.dark ? hexRgb(pal.rule) : hexRgb(pal.plateSoft);
    }
    if (blank)
      bg.a *= 0.25f;
    fillRounded(vg, r, kRadiusKey, bg);

    if (locked) {
      brls::Rect bar(r.getMidX() - 8, r.getMaxY() - 7, 16, 3);
      fillRounded(vg, bar, 1.5f, fg);
    }

    if (k.icon && !fnSwapped)
      text(vg, material, 22, fg, r.getMidX(), r.getMidY(), k.icon);
    else if (!s.empty())
      text(
        vg, regular, k.kind == Kind::Char && !fnLayer_ ? 22 : 15, fg, r.getMidX(), r.getMidY(),
        s.c_str()
      );

    if (focused) {
      focusTarget = r;
      haveFocus = true;
    }
  }

  if (haveFocus) {
    auto now = std::chrono::steady_clock::now();
    if (!focusGlideOn_) {
      focusFrom_ = focusTo_ = focusTarget;
      focusSince_ = now;
      focusGlideOn_ = true;
    } else if (focusTarget.getMinX() != focusTo_.getMinX() ||
               focusTarget.getMinY() != focusTo_.getMinY() ||
               focusTarget.getWidth() != focusTo_.getWidth() ||
               focusTarget.getHeight() != focusTo_.getHeight()) {
      float t = easeOut(std::chrono::duration<float>(now - focusSince_).count() / kFocusMoveSecs);
      auto lerp = [t](float a, float b) { return a + (b - a) * t; };
      focusFrom_ = brls::Rect(
        lerp(focusFrom_.getMinX(), focusTo_.getMinX()),
        lerp(focusFrom_.getMinY(), focusTo_.getMinY()),
        lerp(focusFrom_.getWidth(), focusTo_.getWidth()),
        lerp(focusFrom_.getHeight(), focusTo_.getHeight())
      );
      focusTo_ = focusTarget;
      focusSince_ = now;
    }
    float t = easeOut(std::chrono::duration<float>(now - focusSince_).count() / kFocusMoveSecs);
    auto lerp = [t](float a, float b) { return a + (b - a) * t; };
    brls::Rect cur(
      lerp(focusFrom_.getMinX(), focusTo_.getMinX()),
      lerp(focusFrom_.getMinY(), focusTo_.getMinY()),
      lerp(focusFrom_.getWidth(), focusTo_.getWidth()),
      lerp(focusFrom_.getHeight(), focusTo_.getHeight())
    );
    focusFrame(vg, pal, cur);
  } else {
    focusGlideOn_ = false;
  }
}

