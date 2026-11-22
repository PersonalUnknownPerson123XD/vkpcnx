#pragma once
#include "core/controller_profile.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include <borealis.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace vkpcnx {

// Switch-style editor of a controller profile: Lite diagram, press-to-rebind,
// named presets, live stick preview. Every change is saved at once so a
// running stream picks it up. gameId == 0 edits the global default.
class ControllerProfileContent : public DialogContent {
public:
  ControllerProfileContent(int gameId, const std::string &gameName);
  ~ControllerProfileContent() override;

  brls::ScrollingFrame *getScrollingFrame() override { return scrolling; }
  brls::Box *getContentBox() override { return content; }

  void draw(
    NVGcontext *vg, float x, float y, float width, float height, brls::Style style,
    brls::FrameContext *ctx
  ) override;
  brls::View *getDefaultFocus() override { return this; }
  brls::View *getNextFocus(brls::FocusDirection, brls::View *) override { return this; }

private:
  int gameId = 0;
  std::string gameName;
  controls::Profile profile;
  bool dirty = false;
  bool saveScheduled = false;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  brls::ScrollingFrame *scrolling = nullptr; // unused; DialogContent wants one
  brls::Box *content = nullptr;

  enum class Mode { Browse, Capture };
  Mode mode_ = Mode::Browse;
  int selected_ = controls::S_A; // last source (for capture / diagram highlight)
  int focusId_ = controls::S_A;  // source 0..S_COUNT-1 or a control id >= 100
  int hit_ = -1;
  bool showFocus_ = true;

  // Focus frame glide
  bool glideOn_ = false;
  brls::Rect glideFrom_{}, glideTo_{};
  std::chrono::steady_clock::time_point glideSince_{};

  // Capture: ignore the button that opened it until it is released
  bool captureArmed_ = false;
  brls::ControllerState lastPad_{};

  // Live sticks (after the profile), -1..1
  float lx_ = 0, ly_ = 0, rx_ = 0, ry_ = 0;

  // Layout rectangles, rebuilt every draw
  brls::Rect panel_{}, diagram_{}, previewL_{}, previewR_{};
  brls::Rect presetRects_[5]{};
  brls::Rect resetRect_{}, doneRect_{};
  brls::Rect dzMinus_{}, dzPlus_{}, invL_{}, invR_{}, swap_{}, trig_{};
  brls::Rect sourceRects_[controls::S_COUNT]{}; // hit targets on the diagram
  bool layoutValid_ = false;
  float layoutW_ = 0, layoutH_ = 0;

  void rebuildLayout(float x, float y, float w, float h);
  void save();
  void saveSoon();
  void changed();
  void applyNamedPreset(controls::Preset p);
  void resetToGlobal(); // per-game: drop override
  void resetToDefault(); // global: Nintendo preset
  void beginCapture();
  void cancelCapture();
  void assignTarget(int source, int target); // target is Target or T_NONE
  int sourceFromButton(brls::ControllerButton b) const;
  void poll();
  int hitTest(brls::Point p) const; // source 0..S_COUNT-1, or a control id >= 100
  void activateHit(int hit);
  int nearestFocus(int cur, int dx, int dy) const;
  brls::Rect rectFor(int id) const;
  const char *scopeText() const;
};

void openControllerProfileDialog(int gameId, const std::string &gameName);

} // namespace vkpcnx
