#include "view/controller_profile_dialog.hpp"

#include "core/settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace vkpcnx {

namespace {

// Same Switch overlay palette as the on-screen keyboard
struct Pal {
  bool dark = true;
  uint32_t page = 0x2b2b2b, plate = 0x404040, plateSoft = 0x353535, surface = 0x323232,
           rule = 0x4d4d4d, ink = 0xf5f5f5, inkSoft = 0xd6d6d6, muted = 0xb4b4b4,
           accent = 0x00c3e3, onAccent = 0x0b1b20;
};

Pal palette() {
  bool dark = brls::Application::getThemeVariant() != brls::ThemeVariant::LIGHT;
#ifdef __SWITCH__
  ColorSetId id{};
  if (R_SUCCEEDED(setsysGetColorSetId(&id)))
    dark = id != ColorSetId_Light;
#endif
  if (dark)
    return {};
  return Pal{false, 0xebebeb, 0xdcdcdc, 0xf5f5f5, 0xffffff, 0xcfcfcf, 0x2d2d2d, 0x464646,
             0x5a5a5a, 0x0062a8, 0xffffff};
}

NVGcolor hex(uint32_t rgb, int a = 255) {
  return nvgRGBA((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, a);
}

void fillRound(NVGcontext *vg, brls::Rect r, float rad, NVGcolor c) {
  nvgBeginPath(vg);
  nvgRoundedRect(vg, r.getMinX(), r.getMinY(), r.getWidth(), r.getHeight(), rad);
  nvgFillColor(vg, c);
  nvgFill(vg);
}

void strokeRound(NVGcontext *vg, brls::Rect r, float rad, NVGcolor c, float w) {
  nvgBeginPath(vg);
  nvgRoundedRect(vg, r.getMinX(), r.getMinY(), r.getWidth(), r.getHeight(), rad);
  nvgStrokeColor(vg, c);
  nvgStrokeWidth(vg, w);
  nvgStroke(vg);
}

void fillCircle(NVGcontext *vg, float cx, float cy, float rad, NVGcolor c) {
  nvgBeginPath(vg);
  nvgCircle(vg, cx, cy, rad);
  nvgFillColor(vg, c);
  nvgFill(vg);
}

void strokeCircle(NVGcontext *vg, float cx, float cy, float rad, NVGcolor c, float w) {
  nvgBeginPath(vg);
  nvgCircle(vg, cx, cy, rad);
  nvgStrokeColor(vg, c);
  nvgStrokeWidth(vg, w);
  nvgStroke(vg);
}

void label(
  NVGcontext *vg, int font, float size, NVGcolor c, float x, float y, const char *s,
  int align = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE
) {
  nvgFontFaceId(vg, font);
  nvgFontSize(vg, size);
  nvgTextAlign(vg, align);
  nvgFillColor(vg, c);
  nvgText(vg, x, y, s, nullptr);
}

constexpr float kRadiusKey = 8.f, kRadiusPanel = 14.f, kFrame = 3.f, kFrameGap = 2.f;
constexpr float kFocusMoveSecs = 0.14f, kGlowPeriod = 1.6f;

float easeOut(float t) {
  t = std::clamp(t, 0.f, 1.f);
  float u = 1.f - t;
  return 1.f - u * u * u;
}

float glow(float secs) { return 0.675f + 0.325f * std::sin(secs / kGlowPeriod * 6.2831853f); }

float nowSecs() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

void focusFrame(NVGcontext *vg, const Pal &p, brls::Rect r, float rad) {
  NVGcolor accent = hex(p.accent);
  float g = glow(nowSecs());
  for (int i = 0; i < 3; i++) {
    float o = kFrameGap + i;
    strokeRound(
      vg, brls::Rect(r.getMinX() - o, r.getMinY() - o, r.getWidth() + 2 * o, r.getHeight() + 2 * o),
      rad + o, accent, 1.25f
    );
  }
  for (int i = 0; i < 4; i++) {
    float o = kFrameGap + kFrame + i * 1.5f;
    float k = g * 0.35f * (1.f - i / 4.f);
    NVGcolor c = accent;
    c.a *= k;
    strokeRound(
      vg, brls::Rect(r.getMinX() - o, r.getMinY() - o, r.getWidth() + 2 * o, r.getHeight() + 2 * o),
      rad + o, c, 1.5f
    );
  }
}

// Control hit ids (sources use 0..S_COUNT-1)
enum Ctrl : int {
  C_PRESET0 = 100,
  C_RESET = 105,
  C_DONE = 106,
  C_DZ_MINUS = 107,
  C_DZ_PLUS = 108,
  C_INV_L = 109,
  C_INV_R = 110,
  C_SWAP = 111,
  C_TRIG = 112,
};

brls::ControllerButton kSourceButtons[controls::S_COUNT] = {
  brls::BUTTON_A,     brls::BUTTON_B,      brls::BUTTON_X,     brls::BUTTON_Y,
  brls::BUTTON_LB,    brls::BUTTON_RB,     brls::BUTTON_LT,    brls::BUTTON_RT,
  brls::BUTTON_BACK,  brls::BUTTON_START,  brls::BUTTON_LSB,   brls::BUTTON_RSB,
  brls::BUTTON_UP,    brls::BUTTON_DOWN,   brls::BUTTON_LEFT,  brls::BUTTON_RIGHT,
};

} // namespace

ControllerProfileContent::ControllerProfileContent(int gameId, const std::string &gameName)
    : gameId(gameId), gameName(gameName) {
  this->setAxis(brls::Axis::COLUMN);
  this->setGrow(1);
  this->setFocusable(true);
  // The only child is a GONE scrolling frame (DialogContent leftover); without
  // an explicit height yoga collapses this to 0 and only "Готово" is visible.
  this->setMinHeight(620);
  this->setHeight(620);

  if (gameId > 0) {
    if (!controls::loadGameOverride(gameId, profile))
      profile = controls::loadGlobal();
  } else {
    profile = controls::loadGlobal();
  }

  // DialogContent wants these; the editor is drawn on this view itself.
  scrolling = new brls::ScrollingFrame();
  scrolling->setVisibility(brls::Visibility::GONE);
  content = new brls::Box(brls::Axis::COLUMN);
  scrolling->setContentView(content);
  this->addView(scrolling);

  this->registerAction(
    "Выбрать", brls::BUTTON_A,
    [this](brls::View *) {
      if (mode_ == Mode::Capture)
        return true;
      if (focusId_ >= 0 && focusId_ < controls::S_COUNT)
        activateHit(focusId_);
      else
        activateHit(focusId_);
      return true;
    },
    false, false, brls::SOUND_CLICK
  );
  this->registerAction(
    "Назад", brls::BUTTON_B,
    [this](brls::View *) {
      if (mode_ == Mode::Capture) {
        cancelCapture();
        return true;
      }
      return false; // let the dialog close
    },
    false, false, brls::SOUND_BACK
  );
  auto move = [this](int dx, int dy) {
    return [this, dx, dy](brls::View *) {
      if (mode_ == Mode::Capture)
        return true;
      showFocus_ = true;
      focusId_ = nearestFocus(focusId_, dx, dy);
      if (focusId_ >= 0 && focusId_ < controls::S_COUNT)
        selected_ = focusId_;
      return true;
    };
  };
  this->registerAction("Влево", brls::BUTTON_LEFT, move(-1, 0), true, true);
  this->registerAction("Вправо", brls::BUTTON_RIGHT, move(1, 0), true, true);
  this->registerAction("Вверх", brls::BUTTON_UP, move(0, -1), true, true);
  this->registerAction("Вниз", brls::BUTTON_DOWN, move(0, 1), true, true);

  this->addGestureRecognizer(new brls::TapGestureRecognizer(
    [this](brls::TapGestureStatus status, brls::Sound *) {
      if (status.state != brls::GestureState::END)
        return;
      int h = hitTest(status.position);
      if (h >= 0)
        activateHit(h);
    }
  ));

  std::memset(&lastPad_, 0, sizeof lastPad_);
}

ControllerProfileContent::~ControllerProfileContent() {
  *alive = false;
  if (dirty)
    save();
}

const char *ControllerProfileContent::scopeText() const {
  if (gameId > 0)
    return controls::hasGameOverride(gameId) ? "Профиль этой игры" : "Общий профиль";
  return "Общий профиль";
}

void ControllerProfileContent::save() {
  dirty = false;
  if (gameId > 0) {
    bool ok = controls::saveGameOverride(gameId, profile);
    controls::Profile back;
    ok = ok && controls::loadGameOverride(gameId, back) && back == profile;
    brls::Logger::info("Controls: saved profile of game {}: {}", gameId, ok ? "ok" : "FAILED");
    if (!ok)
      brls::Application::notify("Не удалось сохранить");
    else
      brls::Application::notify("Сохранено");
  } else {
    controls::saveGlobal(profile);
    brls::Logger::info("Controls: saved the general profile");
    controls::Profile pos = controls::Profile::preset(true), lit = controls::Profile::preset(false);
    if (profile.map == pos.map)
      Settings::instance().set<std::string>("/controls/mapping_mode", "positional");
    else if (profile.map == lit.map)
      Settings::instance().set<std::string>("/controls/mapping_mode", "literal");
    brls::Application::notify("Сохранено");
  }
}

void ControllerProfileContent::saveSoon() {
  if (saveScheduled)
    return;
  saveScheduled = true;
  auto token = alive;
  brls::delay(250, [this, token] {
    if (!*token)
      return;
    saveScheduled = false;
    if (dirty)
      save();
  });
}

void ControllerProfileContent::changed() {
  dirty = true;
  save();
}

void ControllerProfileContent::applyNamedPreset(controls::Preset p) {
  controls::applyPreset(profile, p);
  changed();
  brls::Application::notify(controls::presetName(p));
}

void ControllerProfileContent::resetToGlobal() {
  controls::removeGameOverride(gameId);
  dirty = false;
  profile = controls::loadGlobal();
  brls::Application::notify("Общий профиль");
}

void ControllerProfileContent::resetToDefault() {
  profile = controls::Profile::preset(true);
  changed();
  brls::Application::notify("Сброшено");
}

void ControllerProfileContent::beginCapture() {
  mode_ = Mode::Capture;
  captureArmed_ = false;
  auto *input = brls::Application::getPlatform()->getInputManager();
  input->updateUnifiedControllerState(&lastPad_);
}

void ControllerProfileContent::cancelCapture() { mode_ = Mode::Browse; }

void ControllerProfileContent::assignTarget(int source, int target) {
  if (source < 0 || source >= controls::S_COUNT)
    return;
  profile.map[source] = static_cast<int8_t>(target);
  changed();
  mode_ = Mode::Browse;
  char buf[64];
  if (target < 0)
    std::snprintf(buf, sizeof buf, "%s → —", controls::kSourceShort[source]);
  else
    std::snprintf(
      buf, sizeof buf, "%s → %s", controls::kSourceShort[source], controls::kTargetShort[target]
    );
  brls::Application::notify(buf);
}

int ControllerProfileContent::sourceFromButton(brls::ControllerButton b) const {
  for (int i = 0; i < controls::S_COUNT; i++)
    if (kSourceButtons[i] == b)
      return i;
  return -1;
}

void ControllerProfileContent::activateHit(int hit) {
  if (hit >= 0 && hit < controls::S_COUNT) {
    selected_ = hit;
    focusId_ = hit;
    beginCapture();
    return;
  }
  focusId_ = hit;
  if (hit >= C_PRESET0 && hit < C_PRESET0 + (int)controls::Preset::Count) {
    applyNamedPreset(static_cast<controls::Preset>(hit - C_PRESET0));
    return;
  }
  switch (hit) {
  case C_RESET:
    if (gameId > 0)
      resetToGlobal();
    else
      resetToDefault();
    break;
  case C_DONE:
    if (auto *dialog = dynamic_cast<brls::Dialog *>(this->getParent()->getParent()))
      ; // closed by B / Готово
    break;
  case C_DZ_MINUS:
    profile.deadzone = std::max(0.f, profile.deadzone - 0.02f);
    dirty = true;
    saveSoon();
    break;
  case C_DZ_PLUS:
    profile.deadzone = std::min(0.5f, profile.deadzone + 0.02f);
    dirty = true;
    saveSoon();
    break;
  case C_INV_L:
    profile.invertLeftY = !profile.invertLeftY;
    changed();
    break;
  case C_INV_R:
    profile.invertRightY = !profile.invertRightY;
    changed();
    break;
  case C_SWAP:
    profile.swapSticks = !profile.swapSticks;
    changed();
    break;
  case C_TRIG:
    profile.triggersFull = !profile.triggersFull;
    changed();
    break;
  default:
    break;
  }
}

int ControllerProfileContent::hitTest(brls::Point p) const {
  auto inside = [&](brls::Rect r) {
    return p.x >= r.getMinX() && p.x <= r.getMaxX() && p.y >= r.getMinY() && p.y <= r.getMaxY();
  };
  for (int i = 0; i < (int)controls::Preset::Count; i++)
    if (inside(presetRects_[i]))
      return C_PRESET0 + i;
  if (inside(resetRect_))
    return C_RESET;
  if (inside(dzMinus_))
    return C_DZ_MINUS;
  if (inside(dzPlus_))
    return C_DZ_PLUS;
  if (inside(invL_))
    return C_INV_L;
  if (inside(invR_))
    return C_INV_R;
  if (inside(swap_))
    return C_SWAP;
  if (inside(trig_))
    return C_TRIG;
  for (int i = 0; i < controls::S_COUNT; i++)
    if (inside(sourceRects_[i]))
      return i;
  return -1;
}

void ControllerProfileContent::poll() {
  auto *input = brls::Application::getPlatform()->getInputManager();
  brls::ControllerState pad{};
  input->updateUnifiedControllerState(&pad);

  // Switch axes are already -1..1 (Y is down-positive). Same path as the stream.
  controls::applySticks(
    profile, pad.axes[brls::LEFT_X], pad.axes[brls::LEFT_Y], pad.axes[brls::RIGHT_X],
    pad.axes[brls::RIGHT_Y], lx_, ly_, rx_, ry_
  );

  if (mode_ == Mode::Capture) {
    if (captureArmed_ && pad.buttons[brls::BUTTON_LB] && pad.buttons[brls::BUTTON_RB]) {
      cancelCapture();
      lastPad_ = pad;
      return;
    }
    bool anyHeld = false;
    for (int b = 0; b < brls::_BUTTON_MAX; b++) {
      if (pad.buttons[b])
        anyHeld = true;
      bool down = pad.buttons[b] && !lastPad_.buttons[b];
      if (!captureArmed_)
        continue;
      if (!down)
        continue;
      if (b == brls::BUTTON_B && !lastPad_.buttons[brls::BUTTON_B]) {
        // B is also a rebindable source; a short B still cancels if we treat it
        // as a source first. Prefer rebind: B assigns B.
      }
      int src = sourceFromButton(static_cast<brls::ControllerButton>(b));
      if (src < 0)
        continue;
      // The physical button just pressed becomes the Xbox target of `selected_`.
      // Map: the source we are editing should send whatever this physical button
      // currently sends? User asked "press the button to rebind": press the
      // Switch button that should perform the selected control? Or press to
      // choose the Xbox target?
      // Clearer: select a Switch button on the diagram, then press the Xbox
      // role... we only have a Switch pad. So: select a Switch button, press
      // another Switch button to copy that one's *current target*? 
      // Best UX: select a Switch button, then press the Switch button you want
      // it to *act as* on the Xbox pad — i.e. assign selected_'s target to
      // the default (Nintendo/Xbox-neutral) target of the pressed source,
      // actually: assign selected_ → the Target corresponding to the pressed
      // physical button's "Xbox name" in literal layout (A→A, B→B, …).
      static const int8_t literal[controls::S_COUNT] = {
        controls::T_A,         controls::T_B,         controls::T_X,         controls::T_Y,
        controls::T_LB,        controls::T_RB,        controls::T_TRIGGER_L, controls::T_TRIGGER_R,
        controls::T_BACK,      controls::T_START,     controls::T_LSTICK,    controls::T_RSTICK,
        controls::T_DPAD_UP,   controls::T_DPAD_DOWN, controls::T_DPAD_LEFT, controls::T_DPAD_RIGHT,
      };
      // Hold − (Back) + the button to unbind (send nothing)
      if (pad.buttons[brls::BUTTON_BACK] && src != controls::S_MINUS) {
        assignTarget(selected_, controls::T_NONE);
        lastPad_ = pad;
        return;
      }
      assignTarget(selected_, literal[src]);
      lastPad_ = pad;
      return;
    }
    if (!anyHeld)
      captureArmed_ = true; // the A that opened capture has been released
  }

  lastPad_ = pad;
}

void ControllerProfileContent::rebuildLayout(float x, float y, float w, float h) {
  if (layoutValid_ && layoutW_ == w && layoutH_ == h)
    return;
  layoutValid_ = true;
  layoutW_ = w;
  layoutH_ = h;
  panel_ = brls::Rect(x + 8, y + 8, w - 16, h - 16);

  float px = panel_.getMinX(), py = panel_.getMinY(), pw = panel_.getWidth();
  // Header 28, presets 36, diagram ~200, sticks 130, hints 28
  float presetsY = py + 44;
  float presetW = (pw - 40 - 4 * 8) / 5;
  for (int i = 0; i < 5; i++)
    presetRects_[i] = brls::Rect(px + 20 + i * (presetW + 8), presetsY, presetW, 32);

  float diagY = presetsY + 44;
  float diagH = std::min(220.f, panel_.getHeight() * 0.42f);
  float diagW = std::min(pw - 40, diagH * 1.7f);
  diagram_ = brls::Rect(px + (pw - diagW) / 2, diagY, diagW, diagH);

  // Lite body: a landscape rounded slab. Face buttons on the right, d-pad / stick
  // on the left, bumpers above, − + in the middle.
  auto place = [&](int src, float nx, float ny, float nw, float nh) {
    sourceRects_[src] = brls::Rect(
      diagram_.getMinX() + nx * diagW, diagram_.getMinY() + ny * diagH, nw * diagW, nh * diagH
    );
  };
  // bumpers / triggers
  place(controls::S_L, 0.08f, 0.06f, 0.16f, 0.10f);
  place(controls::S_ZL, 0.08f, 0.18f, 0.16f, 0.10f);
  place(controls::S_R, 0.76f, 0.06f, 0.16f, 0.10f);
  place(controls::S_ZR, 0.76f, 0.18f, 0.16f, 0.10f);
  // minus / plus
  place(controls::S_MINUS, 0.32f, 0.28f, 0.10f, 0.10f);
  place(controls::S_PLUS, 0.58f, 0.28f, 0.10f, 0.10f);
  // left stick (Lite: below d-pad-ish; we put stick left-center, d-pad under)
  place(controls::S_LSTICK, 0.14f, 0.36f, 0.18f, 0.26f);
  place(controls::S_RSTICK, 0.48f, 0.56f, 0.16f, 0.24f);
  // d-pad
  place(controls::S_UP, 0.18f, 0.66f, 0.10f, 0.11f);
  place(controls::S_DOWN, 0.18f, 0.88f, 0.10f, 0.11f);
  place(controls::S_LEFT, 0.07f, 0.77f, 0.10f, 0.11f);
  place(controls::S_RIGHT, 0.29f, 0.77f, 0.10f, 0.11f);
  // face (right): Y top, B bottom, X left, A right
  place(controls::S_Y, 0.78f, 0.32f, 0.12f, 0.17f);
  place(controls::S_A, 0.88f, 0.50f, 0.12f, 0.17f);
  place(controls::S_B, 0.78f, 0.68f, 0.12f, 0.17f);
  place(controls::S_X, 0.68f, 0.50f, 0.12f, 0.17f);

  float sticksY = diagram_.getMaxY() + 10;
  float prevR = 44;
  previewL_ = brls::Rect(px + 24, sticksY, prevR * 2, prevR * 2);
  previewR_ = brls::Rect(px + 24 + prevR * 2 + 16, sticksY, prevR * 2, prevR * 2);

  float optX = previewR_.getMaxX() + 16;
  float optY = sticksY;
  float optW = (px + pw - 20 - optX);
  float optH = 28;
  swap_ = brls::Rect(optX, optY, optW * 0.48f, optH);
  trig_ = brls::Rect(optX + optW * 0.52f, optY, optW * 0.48f, optH);
  invL_ = brls::Rect(optX, optY + 34, optW * 0.48f, optH);
  invR_ = brls::Rect(optX + optW * 0.52f, optY + 34, optW * 0.48f, optH);
  dzMinus_ = brls::Rect(optX, optY + 68, 36, optH);
  dzPlus_ = brls::Rect(optX + optW - 36, optY + 68, 36, optH);

  resetRect_ = brls::Rect(px + 20, panel_.getMaxY() - 40, 200, 28);
}

void ControllerProfileContent::draw(
  NVGcontext *vg, float x, float y, float width, float height, brls::Style style,
  brls::FrameContext *ctx
) {
  (void)style;
  (void)ctx;
  poll();
  rebuildLayout(x, y, width, height);

  const auto pal = palette();
  const int font = brls::Application::getDefaultFont();
  const NVGcolor ink = hex(pal.ink);
  const NVGcolor muted = hex(pal.muted);
  const NVGcolor inkSoft = hex(pal.inkSoft);
  const NVGcolor keyFill = pal.dark ? hex(pal.plate) : hex(pal.surface);
  const NVGcolor accent = hex(pal.accent);
  const NVGcolor onAccent = hex(pal.onAccent);
  const NVGcolor page = hex(pal.page, 248);

  fillRound(
    vg,
    brls::Rect(panel_.getMinX(), panel_.getMinY() + 4, panel_.getWidth(), panel_.getHeight()),
    kRadiusPanel, nvgRGBA(0, 0, 0, 40)
  );
  fillRound(vg, panel_, kRadiusPanel, page);

  // Title + scope
  char title[96];
  if (gameId > 0)
    std::snprintf(title, sizeof title, "%s", gameName.empty() ? "Игра" : gameName.c_str());
  else
    std::snprintf(title, sizeof title, "Управление");
  label(
    vg, font, 22, ink, panel_.getMinX() + 20, panel_.getMinY() + 22, title,
    NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE
  );
  label(
    vg, font, 14, muted, panel_.getMaxX() - 20, panel_.getMinY() + 22, scopeText(),
    NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE
  );

  // Presets
  for (int i = 0; i < (int)controls::Preset::Count; i++) {
    auto r = presetRects_[i];
    fillRound(vg, r, 8, keyFill);
    label(vg, font, 13, ink, r.getMidX(), r.getMidY(), controls::presetName((controls::Preset)i));
  }

  // Lite body
  brls::Rect body(
    diagram_.getMinX() + diagram_.getWidth() * 0.04f, diagram_.getMinY() + diagram_.getHeight() * 0.22f,
    diagram_.getWidth() * 0.92f, diagram_.getHeight() * 0.74f
  );
  fillRound(vg, body, body.getHeight() * 0.22f, pal.dark ? hex(pal.plateSoft) : hex(pal.plate));

  auto mapLabel = [&](int src) -> const char * {
    int t = profile.map[src];
    return t < 0 ? "—" : controls::kTargetShort[t];
  };

  for (int i = 0; i < controls::S_COUNT; i++) {
    auto r = sourceRects_[i];
    bool sel = (i == selected_);
    bool isFace = i <= controls::S_Y;
    NVGcolor bg = keyFill;
    if (sel)
      bg = pal.dark ? hex(pal.rule) : hex(pal.plateSoft);
    if (isFace)
      fillCircle(vg, r.getMidX(), r.getMidY(), std::min(r.getWidth(), r.getHeight()) / 2, bg);
    else
      fillRound(vg, r, 7, bg);
    const char *srcN = controls::kSourceShort[i];
    const char *tgtN = mapLabel(i);
    if (std::strcmp(srcN, tgtN) == 0) {
      label(vg, font, 14, ink, r.getMidX(), r.getMidY(), tgtN);
    } else {
      label(vg, font, 10, muted, r.getMidX(), r.getMinY() + 8, srcN);
      label(vg, font, 14, ink, r.getMidX(), r.getMidY() + 4, tgtN);
    }
  }

  // Stick previews
  auto drawStick = [&](brls::Rect r, float sx, float sy, const char *name) {
    float cx = r.getMidX(), cy = r.getMidY(), rad = r.getWidth() / 2 - 2;
    strokeCircle(vg, cx, cy, rad, muted, 1.5f);
    // deadzone circle
    if (profile.deadzone > 0)
      strokeCircle(vg, cx, cy, rad * profile.deadzone, hex(pal.accent, 140), 1.25f);
    // Switch Y is down-positive, same as the screen
    float px = cx + sx * rad, py = cy + sy * rad;
    fillCircle(vg, px, py, 7, accent);
    label(vg, font, 11, muted, cx, r.getMaxY() + 10, name);
  };
  drawStick(previewL_, lx_, ly_, "Левый");
  drawStick(previewR_, rx_, ry_, "Правый");

  auto chip = [&](brls::Rect r, const char *text, bool on) {
    fillRound(vg, r, 8, on ? accent : keyFill);
    label(vg, font, 13, on ? onAccent : ink, r.getMidX(), r.getMidY(), text);
  };
  chip(swap_, "Стики наоборот", profile.swapSticks);
  chip(trig_, "ZL/ZR полностью", profile.triggersFull);
  chip(invL_, "Инв. Y лев.", profile.invertLeftY);
  chip(invR_, "Инв. Y прав.", profile.invertRightY);

  // Deadzone stepper
  fillRound(vg, dzMinus_, 8, keyFill);
  label(vg, font, 18, ink, dzMinus_.getMidX(), dzMinus_.getMidY(), "−");
  fillRound(vg, dzPlus_, 8, keyFill);
  label(vg, font, 18, ink, dzPlus_.getMidX(), dzPlus_.getMidY(), "+");
  char dz[48];
  std::snprintf(dz, sizeof dz, "Мёртвая зона %d%%", int(profile.deadzone * 200 + 0.5f));
  label(
    vg, font, 13, inkSoft, (dzMinus_.getMaxX() + dzPlus_.getMinX()) / 2, dzMinus_.getMidY(), dz
  );

  fillRound(vg, resetRect_, 8, keyFill);
  label(
    vg, font, 13, ink, resetRect_.getMidX(), resetRect_.getMidY(),
    gameId > 0 ? "Сбросить к общему" : "Сбросить"
  );

  // Capture banner
  if (mode_ == Mode::Capture) {
    char msg[80];
    std::snprintf(msg, sizeof msg, "Жмите кнопку для %s   L+R отмена   − и кнопка — выкл",
                  controls::kSourceShort[selected_]);
    label(vg, font, 14, accent, panel_.getMidX(), resetRect_.getMinY() - 16, msg);
  } else {
    label(vg, font, 13, muted, panel_.getMidX(), resetRect_.getMinY() - 16, "A назначить    B закрыть");
  }

  if (showFocus_) {
    auto target = rectFor(mode_ == Mode::Capture ? selected_ : focusId_);
    if (target.getWidth() <= 0)
      target = sourceRects_[std::clamp(selected_, 0, controls::S_COUNT - 1)];
    auto now = std::chrono::steady_clock::now();
    if (!glideOn_) {
      glideFrom_ = glideTo_ = target;
      glideSince_ = now;
      glideOn_ = true;
    } else if (target.getMinX() != glideTo_.getMinX() || target.getMinY() != glideTo_.getMinY()) {
      float t = easeOut(std::chrono::duration<float>(now - glideSince_).count() / kFocusMoveSecs);
      auto L = [t](float a, float b) { return a + (b - a) * t; };
      glideFrom_ = brls::Rect(
        L(glideFrom_.getMinX(), glideTo_.getMinX()), L(glideFrom_.getMinY(), glideTo_.getMinY()),
        L(glideFrom_.getWidth(), glideTo_.getWidth()), L(glideFrom_.getHeight(), glideTo_.getHeight())
      );
      glideTo_ = target;
      glideSince_ = now;
    }
    float t = easeOut(std::chrono::duration<float>(now - glideSince_).count() / kFocusMoveSecs);
    auto L = [t](float a, float b) { return a + (b - a) * t; };
    brls::Rect cur(
      L(glideFrom_.getMinX(), glideTo_.getMinX()), L(glideFrom_.getMinY(), glideTo_.getMinY()),
      L(glideFrom_.getWidth(), glideTo_.getWidth()), L(glideFrom_.getHeight(), glideTo_.getHeight())
    );
    int fid = mode_ == Mode::Capture ? selected_ : focusId_;
    float rad = (fid >= 0 && fid <= controls::S_Y) ? std::min(cur.getWidth(), cur.getHeight()) / 2 : 8;
    focusFrame(vg, pal, cur, rad);
  }
}

brls::Rect ControllerProfileContent::rectFor(int id) const {
  if (id >= 0 && id < controls::S_COUNT)
    return sourceRects_[id];
  if (id >= C_PRESET0 && id < C_PRESET0 + (int)controls::Preset::Count)
    return presetRects_[id - C_PRESET0];
  switch (id) {
  case C_RESET:
    return resetRect_;
  case C_DZ_MINUS:
    return dzMinus_;
  case C_DZ_PLUS:
    return dzPlus_;
  case C_INV_L:
    return invL_;
  case C_INV_R:
    return invR_;
  case C_SWAP:
    return swap_;
  case C_TRIG:
    return trig_;
  default:
    return {};
  }
}

int ControllerProfileContent::nearestFocus(int cur, int dx, int dy) const {
  int ids[] = {
    controls::S_A,     controls::S_B,    controls::S_X,    controls::S_Y,    controls::S_L,
    controls::S_R,     controls::S_ZL,   controls::S_ZR,   controls::S_MINUS,controls::S_PLUS,
    controls::S_LSTICK,controls::S_RSTICK,controls::S_UP,  controls::S_DOWN, controls::S_LEFT,
    controls::S_RIGHT, C_PRESET0,        C_PRESET0 + 1,    C_PRESET0 + 2,    C_PRESET0 + 3,
    C_PRESET0 + 4,     C_RESET,          C_DZ_MINUS,       C_DZ_PLUS,        C_INV_L,
    C_INV_R,           C_SWAP,           C_TRIG,
  };
  brls::Rect fr = rectFor(cur);
  float fx = fr.getMidX(), fy = fr.getMidY();
  int best = cur;
  float bestScore = 1e9f;
  for (int id : ids) {
    if (id == cur)
      continue;
    brls::Rect r = rectFor(id);
    if (r.getWidth() <= 0)
      continue;
    float vx = r.getMidX() - fx, vy = r.getMidY() - fy;
    float along = vx * (float)dx + vy * (float)dy;
    if (along <= 6.f)
      continue;
    float across = std::fabs(vx * (float)dy - vy * (float)dx);
    float score = along + across * 2.5f;
    if (score < bestScore) {
      bestScore = score;
      best = id;
    }
  }
  return best;
}

void openControllerProfileDialog(int gameId, const std::string &gameName) {
  brls::Logger::info("ControllerProfile: opening editor gameId={} name={}", gameId, gameName);
  auto *content = new ControllerProfileContent(gameId, gameName);
  auto *dialog = new brls::Dialog(content);
  dialog->addButton("Готово", [] {});
  dialog->setCancelable(true);
  dialog->getAppletFrame()->setWidth(920);
  dialog->getAppletFrame()->setHeightPercentage(92.0f);
  dialog->getAppletFrame()->setMinHeight(620);
  dialog->setLastFocusedView(content);
  dialog->open();
}

} // namespace vkpcnx
