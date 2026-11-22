#pragma once
#include "core/controller_profile.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include <borealis.hpp>
#include <memory>
#include <string>
#include <vector>

namespace vkpcnx {

// Editor of a controller profile (core/controller_profile.hpp): layout presets,
// any Switch button -> any Xbox button, stick swap / Y inversion / dead zone,
// trigger mode. Every change is saved at once, so a running stream picks it up
// live. gameId == 0 edits the global default; otherwise the game's own profile
// (created by the first change, dropped by "use the general profile").
class ControllerProfileContent : public DialogContent {
public:
  ControllerProfileContent(int gameId, const std::string &gameName);
  ~ControllerProfileContent() override;

  brls::ScrollingFrame *getScrollingFrame() override { return scrolling; }
  brls::Box *getContentBox() override { return content; }

private:
  int gameId;
  controls::Profile profile;
  bool updating = false; // callbacks fired while the cells are being refreshed
  bool dirty = false;
  bool saveScheduled = false;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  brls::ScrollingFrame *scrolling = nullptr;
  brls::Box *content = nullptr;
  brls::Label *scopeLabel = nullptr;
  std::vector<brls::SelectorCell *> mapCells;
  brls::BooleanCell *swapCell = nullptr;
  brls::BooleanCell *invertLeftCell = nullptr;
  brls::BooleanCell *invertRightCell = nullptr;
  brls::BooleanCell *triggersCell = nullptr;
  brls::SliderCell *deadzoneCell = nullptr;

  void save();           // now
  void saveSoon();       // coalesces slider drags
  void changed();        // after any edit: saves (or schedules)
  void refreshCells();   // profile -> cells
  void refreshScope();
  void applyPreset(bool positional);
  static std::string deadzoneTitle(float deadzone);
};

// Opens the editor in a dialog. gameId 0: the global profile.
void openControllerProfileDialog(int gameId, const std::string &gameName);

} // namespace vkpcnx
