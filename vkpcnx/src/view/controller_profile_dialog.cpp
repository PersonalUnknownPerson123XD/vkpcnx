#include "view/controller_profile_dialog.hpp"

#include "core/settings.hpp"

#include <algorithm>
#include <cmath>

namespace vkpcnx {

namespace {
brls::Label *textLabel(const std::string &text, float size, bool dim) {
  auto *l = new brls::Label();
  l->setText(text);
  l->setFontSize(size);
  l->setSingleLine(false);
  if (dim)
    l->setAlpha(0.5f);
  return l;
}
} // namespace

std::string ControllerProfileContent::deadzoneTitle(float deadzone) {
  return "Мёртвая зона стиков: " + std::to_string(static_cast<int>(deadzone * 200.0f + 0.5f)) + "%";
}

ControllerProfileContent::ControllerProfileContent(int gameId, const std::string &gameName)
    : gameId(gameId) {
  this->setAxis(brls::Axis::COLUMN);
  this->setGrow(1);

  if (gameId > 0) {
    if (!controls::loadGameOverride(gameId, profile))
      profile = controls::loadGlobal(); // the first change creates the game's own profile
  } else {
    profile = controls::loadGlobal();
  }

  scrolling = new brls::ScrollingFrame();
  scrolling->setWidth(brls::View::AUTO);
  scrolling->setHeight(brls::View::AUTO);
  scrolling->setGrow(1);
  this->addView(scrolling);

  content = new brls::Box(brls::Axis::COLUMN);
  content->setPadding(30, 30, 30, 30);
  content->setWidth(brls::View::AUTO);
  content->setHeight(brls::View::AUTO);
  scrolling->setContentView(content);

  content->addView(textLabel(gameId > 0 ? "Управление: " + gameName : "Управление (общий профиль)", 28, false));
  scopeLabel = textLabel("", 15, true);
  scopeLabel->setMarginTop(8);
  scopeLabel->setMarginBottom(16);
  content->addView(scopeLabel);
  auto *hotkeys = textLabel(
    "Комбинации L+R+Y (панель), L+R+− (клавиатура) и L+R+(+) (контроллер как мышь) работают всегда "
    "и не переназначаются.",
    15, true
  );
  hotkeys->setMarginBottom(20);
  content->addView(hotkeys);

  // -- presets
  auto *nintendo = new brls::Button();
  nintendo->setText("Раскладка Nintendo (A↔B, X↔Y как на кнопках Xbox)");
  nintendo->setStyle(&brls::BUTTONSTYLE_DEFAULT);
  nintendo->setMarginBottom(10);
  nintendo->registerClickAction([this](brls::View *) {
    applyPreset(true);
    return true;
  });
  content->addView(nintendo);
  auto *xbox = new brls::Button();
  xbox->setText("Раскладка Xbox (буква в букву)");
  xbox->setStyle(&brls::BUTTONSTYLE_DEFAULT);
  xbox->setMarginBottom(24);
  xbox->registerClickAction([this](brls::View *) {
    applyPreset(false);
    return true;
  });
  content->addView(xbox);

  // -- sticks and triggers
  content->addView(textLabel("Стики и курки", 15, true));
  swapCell = new brls::BooleanCell();
  swapCell->init("Поменять стики местами", profile.swapSticks, [this](bool on) {
    if (updating)
      return;
    profile.swapSticks = on;
    changed();
  });
  content->addView(swapCell);
  invertLeftCell = new brls::BooleanCell();
  invertLeftCell->init("Инвертировать ось Y левого стика", profile.invertLeftY, [this](bool on) {
    if (updating)
      return;
    profile.invertLeftY = on;
    changed();
  });
  content->addView(invertLeftCell);
  invertRightCell = new brls::BooleanCell();
  invertRightCell->init("Инвертировать ось Y правого стика", profile.invertRightY, [this](bool on) {
    if (updating)
      return;
    profile.invertRightY = on;
    changed();
  });
  content->addView(invertRightCell);
  triggersCell = new brls::BooleanCell();
  triggersCell->init("ZL/ZR всегда на полное нажатие", profile.triggersFull, [this](bool on) {
    if (updating)
      return;
    profile.triggersFull = on;
    changed();
  });
  content->addView(triggersCell);
  deadzoneCell = new brls::SliderCell();
  deadzoneCell->init(deadzoneTitle(profile.deadzone), profile.deadzone / 0.5f, [this](float progress) {
    if (updating)
      return;
    float v = std::min(0.5f, std::max(0.0f, progress * 0.5f));
    v = std::round(v * 100.0f) / 100.0f;
    if (v == profile.deadzone)
      return;
    profile.deadzone = v;
    deadzoneCell->setText(deadzoneTitle(v));
    dirty = true;
    saveSoon();
  });
  content->addView(deadzoneCell);

  // -- every button
  auto *buttonsTitle = textLabel("Кнопки: что отправляется в игру", 15, true);
  buttonsTitle->setMarginTop(24);
  content->addView(buttonsTitle);
  std::vector<std::string> options{"Не передавать"};
  for (int t = 0; t < controls::T_COUNT; t++)
    options.push_back(controls::kTargetNames[t]);
  for (int src = 0; src < controls::S_COUNT; src++) {
    auto *cell = new brls::SelectorCell();
    cell->init(
      std::string(controls::kSourceNames[src]) + " →", options, profile.map[src] + 1,
      [this, src](int selected) {
        if (updating)
          return;
        profile.map[src] = static_cast<int8_t>(selected - 1);
        changed();
      }
    );
    content->addView(cell);
    mapCells.push_back(cell);
  }

  if (gameId > 0) {
    auto *reset = new brls::Button();
    reset->setText("Убрать профиль игры (использовать общий)");
    reset->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    reset->setMarginTop(24);
    reset->registerClickAction([this](brls::View *) {
      controls::removeGameOverride(this->gameId);
      dirty = false;
      profile = controls::loadGlobal();
      refreshCells();
      return true;
    });
    content->addView(reset);
  }
  refreshScope();
}

ControllerProfileContent::~ControllerProfileContent() {
  *alive = false;
  if (dirty)
    save();
}

void ControllerProfileContent::refreshScope() {
  if (gameId > 0)
    scopeLabel->setText(
      controls::hasGameOverride(gameId)
        ? "У этой игры свой профиль: изменения действуют только в ней."
        : "Сейчас игра использует общий профиль. Любое изменение создаст для неё свой профиль."
    );
  else
    scopeLabel->setText("Общий профиль действует во всех играх, у которых нет своего.");
}

void ControllerProfileContent::refreshCells() {
  updating = true;
  for (int src = 0; src < controls::S_COUNT; src++)
    mapCells[src]->setSelection(profile.map[src] + 1, true);
  swapCell->setOn(profile.swapSticks, false);
  invertLeftCell->setOn(profile.invertLeftY, false);
  invertRightCell->setOn(profile.invertRightY, false);
  triggersCell->setOn(profile.triggersFull, false);
  deadzoneCell->slider->setProgress(profile.deadzone / 0.5f);
  deadzoneCell->setText(deadzoneTitle(profile.deadzone));
  updating = false;
  refreshScope();
}

void ControllerProfileContent::save() {
  dirty = false;
  if (gameId > 0) {
    if (!controls::saveGameOverride(gameId, profile))
      brls::Application::notify("Не удалось сохранить профиль игры");
  } else {
    controls::saveGlobal(profile);
    // the old two-way setting stays truthful for the layouts it knows
    controls::Profile pos = controls::Profile::preset(true), lit = controls::Profile::preset(false);
    if (profile.map == pos.map)
      Settings::instance().set<std::string>("/controls/mapping_mode", "positional");
    else if (profile.map == lit.map)
      Settings::instance().set<std::string>("/controls/mapping_mode", "literal");
  }
  refreshScope();
}

void ControllerProfileContent::saveSoon() {
  if (saveScheduled)
    return;
  saveScheduled = true;
  auto token = alive;
  brls::delay(300, [this, token] {
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

void ControllerProfileContent::applyPreset(bool positional) {
  profile.fillLayout(positional);
  refreshCells();
  changed();
}

void openControllerProfileDialog(int gameId, const std::string &gameName) {
  auto *content = new ControllerProfileContent(gameId, gameName);
  auto *dialog = new brls::Dialog(content);
  dialog->addButton("Готово", [] {});
  dialog->setCancelable(true);
  dialog->getAppletFrame()->setWidth(800);
  dialog->getAppletFrame()->setHeightPercentage(90.0f);
  dialog->setLastFocusedView(content->getScrollingFrame());
  dialog->open();
}

} // namespace vkpcnx
