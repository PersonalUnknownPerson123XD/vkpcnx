#pragma once
// Controller remapping for streaming: which button of the Switch pad is sent
// as which button of the virtual Xbox pad, plus the stick options.
//
// A profile is the global default (Settings "/controls/profile"), or a
// per-game override in <config dir>/controls/<game id>.json that wins while
// that game runs. The L+R+Y / L+R+Minus / L+R+Plus hotkeys are matched on the
// physical buttons before any profile is applied, so no profile can remap
// them away.
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace vkpcnx::controls {

// Where a Switch button can be sent. The values are indices into kTargetNames.
enum Target : int {
  T_NONE = -1, // not sent at all
  T_A = 0,
  T_B,
  T_X,
  T_Y,
  T_LB,
  T_RB,
  T_BACK,
  T_START,
  T_LSTICK,
  T_RSTICK,
  T_TRIGGER_L, // left trigger pushed fully
  T_TRIGGER_R, // right trigger pushed fully
  T_DPAD_UP,
  T_DPAD_DOWN,
  T_DPAD_LEFT,
  T_DPAD_RIGHT,
  T_COUNT
};

// Switch buttons that can be remapped, in the order of Profile::map
enum Source : int {
  S_A = 0,
  S_B,
  S_X,
  S_Y,
  S_L,
  S_R,
  S_ZL,
  S_ZR,
  S_MINUS,
  S_PLUS,
  S_LSTICK,
  S_RSTICK,
  S_UP,
  S_DOWN,
  S_LEFT,
  S_RIGHT,
  S_COUNT
};

extern const char *const kSourceNames[S_COUNT];
extern const char *const kTargetNames[T_COUNT]; // Xbox names, for the UI

struct Profile {
  std::array<int8_t, S_COUNT> map{}; // Target per Source
  bool swapSticks = false;           // left stick drives the right one and vice versa
  bool invertLeftY = false;          // applied to the stick that ends up as the left one
  bool invertRightY = false;
  float deadzone = 0.0f;             // 0..0.5, radial, rescaled so the edge stays at full tilt
  bool triggersFull = false;         // ZL/ZR (even an analog one) always pushes the trigger fully

  // Nintendo layout ("positional": the button at the same place on the Xbox
  // pad, A<->B and X<->Y swapped) or Xbox layout ("literal": same label)
  static Profile preset(bool positional);
  // Nintendo layout, no stick changes
  Profile() { fillLayout(true); }
  void fillLayout(bool positional); // only the button map

  nlohmann::json toJson() const;
  static bool fromJson(const nlohmann::json &j, Profile &out); // false if unusable

  bool operator==(const Profile &o) const;
};

// Both sticks after the profile's options (dead zone, swap, Y inversion); all values in [-1, 1]
void applySticks(
  const Profile &p, float lx, float ly, float rx, float ry, float &outLX, float &outLY, float &outRX,
  float &outRY
);

// ---- storage ---------------------------------------------------------------

// The global default (from "/controls/profile", else the old mapping_mode)
Profile loadGlobal();
void saveGlobal(const Profile &p);
// Switches the global default to a layout preset, keeping its stick options;
// the old "Controller layout" dialog uses it
void applyGlobalPreset(bool positional);

// Per-game override
bool hasGameOverride(int gameId);
bool loadGameOverride(int gameId, Profile &out);
bool saveGameOverride(int gameId, const Profile &p);
void removeGameOverride(int gameId);

// What a running game uses: its override if any, else the global default
Profile loadEffective(int gameId);

// Bumped by every save/remove, so a running stream can notice changes
uint32_t revision();

} // namespace vkpcnx::controls
