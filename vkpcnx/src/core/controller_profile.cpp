#include "core/controller_profile.hpp"

#include "core/settings.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace vkpcnx::controls {

const char *const kSourceNames[S_COUNT] = {
  "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "−", "+", "Нажатие левого стика", "Нажатие правого стика",
  "Крестовина вверх", "Крестовина вниз", "Крестовина влево", "Крестовина вправо",
};

const char *const kTargetNames[T_COUNT] = {
  "A (Xbox)", "B (Xbox)", "X (Xbox)", "Y (Xbox)", "LB", "RB", "Back / View", "Start / Menu",
  "Нажатие левого стика", "Нажатие правого стика", "LT (курок, полностью)", "RT (курок, полностью)",
  "Крестовина вверх", "Крестовина вниз", "Крестовина влево", "Крестовина вправо",
};

const char *const kSourceShort[S_COUNT] = {
  "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "−", "+", "L3", "R3", "↑", "↓", "←", "→",
};

const char *const kTargetShort[T_COUNT] = {
  "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "L3", "R3", "LT", "RT", "↑", "↓", "←", "→",
};

const char *presetName(Preset p) {
  switch (p) {
  case Preset::Nintendo:
    return "Стандарт";
  case Preset::Xbox:
    return "Xbox";
  case Preset::SwapSticks:
    return "Стики ↔";
  case Preset::Southpaw:
    return "Южанин";
  case Preset::LeftHanded:
    return "Левша";
  default:
    return "";
  }
}

void applyPreset(Profile &profile, Preset p) {
  switch (p) {
  case Preset::Nintendo:
    profile.fillLayout(true);
    profile.swapSticks = false;
    break;
  case Preset::Xbox:
    profile.fillLayout(false);
    profile.swapSticks = false;
    break;
  case Preset::SwapSticks:
    // Keep the current button map; only swap the sticks
    profile.swapSticks = true;
    break;
  case Preset::Southpaw:
    // Nintendo layout, movement on the right stick (common FPS)
    profile.fillLayout(true);
    profile.swapSticks = true;
    break;
  case Preset::LeftHanded:
    // Face buttons and bumpers mirrored, sticks swapped
    profile.fillLayout(true);
    profile.swapSticks = true;
    std::swap(profile.map[S_A], profile.map[S_Y]);
    std::swap(profile.map[S_B], profile.map[S_X]);
    std::swap(profile.map[S_L], profile.map[S_R]);
    std::swap(profile.map[S_ZL], profile.map[S_ZR]);
    break;
  default:
    break;
  }
}

namespace {
std::atomic<uint32_t> g_revision{1};

constexpr const char *kGlobalKey = "/controls/profile";

std::string gamePath(int gameId) {
  return Settings::configDir() + "/controls/" + std::to_string(gameId) + ".json";
}
} // namespace

void Profile::fillLayout(bool positional) {
  auto &m = map;
  m[S_A] = positional ? T_B : T_A;
  m[S_B] = positional ? T_A : T_B;
  m[S_X] = positional ? T_Y : T_X;
  m[S_Y] = positional ? T_X : T_Y;
  m[S_L] = T_LB;
  m[S_R] = T_RB;
  m[S_ZL] = T_TRIGGER_L;
  m[S_ZR] = T_TRIGGER_R;
  m[S_MINUS] = T_BACK;
  m[S_PLUS] = T_START;
  m[S_LSTICK] = T_LSTICK;
  m[S_RSTICK] = T_RSTICK;
  m[S_UP] = T_DPAD_UP;
  m[S_DOWN] = T_DPAD_DOWN;
  m[S_LEFT] = T_DPAD_LEFT;
  m[S_RIGHT] = T_DPAD_RIGHT;
}

Profile Profile::preset(bool positional) {
  Profile p;
  p.fillLayout(positional);
  return p;
}

bool Profile::operator==(const Profile &o) const {
  return map == o.map && swapSticks == o.swapSticks && invertLeftY == o.invertLeftY &&
         invertRightY == o.invertRightY && deadzone == o.deadzone && triggersFull == o.triggersFull;
}

nlohmann::json Profile::toJson() const {
  nlohmann::json j;
  j["map"] = nlohmann::json::array();
  for (int8_t t : map)
    j["map"].push_back(static_cast<int>(t));
  j["swap_sticks"] = swapSticks;
  j["invert_left_y"] = invertLeftY;
  j["invert_right_y"] = invertRightY;
  j["deadzone"] = deadzone;
  j["triggers_full"] = triggersFull;
  return j;
}

bool Profile::fromJson(const nlohmann::json &j, Profile &out) {
  if (!j.is_object())
    return false;
  Profile p;
  try {
    if (j.contains("map") && j["map"].is_array()) {
      if (j["map"].size() != S_COUNT)
        return false;
      for (int i = 0; i < S_COUNT; i++) {
        int t = j["map"][i].get<int>();
        if (t < T_NONE || t >= T_COUNT)
          return false;
        p.map[i] = static_cast<int8_t>(t);
      }
    }
    p.swapSticks = j.value("swap_sticks", false);
    p.invertLeftY = j.value("invert_left_y", false);
    p.invertRightY = j.value("invert_right_y", false);
    p.deadzone = std::clamp(j.value("deadzone", 0.0f), 0.0f, 0.5f);
    p.triggersFull = j.value("triggers_full", false);
  } catch (const nlohmann::json::exception &) {
    return false;
  }
  out = p;
  return true;
}

namespace {
// Radial dead zone: inside it nothing, outside it rescaled so full tilt stays full
void deadzone(float dz, float x, float y, float &ox, float &oy) {
  const float mag = std::sqrt(x * x + y * y);
  if (dz <= 0.0f) {
    ox = x;
    oy = y;
    return;
  }
  if (mag <= dz) {
    ox = oy = 0.0f;
    return;
  }
  const float scaled = std::min(1.0f, (mag - dz) / (1.0f - dz));
  ox = x / mag * scaled;
  oy = y / mag * scaled;
}
} // namespace

void applySticks(
  const Profile &p, float lx, float ly, float rx, float ry, float &outLX, float &outLY, float &outRX,
  float &outRY
) {
  deadzone(p.deadzone, lx, ly, lx, ly);
  deadzone(p.deadzone, rx, ry, rx, ry);
  if (p.swapSticks) {
    std::swap(lx, rx);
    std::swap(ly, ry);
  }
  if (p.invertLeftY)
    ly = -ly;
  if (p.invertRightY)
    ry = -ry;
  outLX = lx;
  outLY = ly;
  outRX = rx;
  outRY = ry;
}

Profile loadGlobal() {
  Profile p;
  const auto &settings = Settings::instance();
  if (settings.has(kGlobalKey)) {
    nlohmann::json j = settings.get<nlohmann::json>(kGlobalKey, nlohmann::json());
    if (Profile::fromJson(j, p))
      return p;
  }
  // No profile saved yet: the old two-way setting decides the layout
  bool positional = settings.get<std::string>("/controls/mapping_mode", "positional") == "positional";
  return Profile::preset(positional);
}

void saveGlobal(const Profile &p) {
  Settings::instance().set<nlohmann::json>(kGlobalKey, p.toJson());
  g_revision++;
}

void applyGlobalPreset(bool positional) {
  Profile current = loadGlobal();
  Profile fresh = Profile::preset(positional);
  fresh.swapSticks = current.swapSticks;
  fresh.invertLeftY = current.invertLeftY;
  fresh.invertRightY = current.invertRightY;
  fresh.deadzone = current.deadzone;
  fresh.triggersFull = current.triggersFull;
  saveGlobal(fresh);
}

bool hasGameOverride(int gameId) {
  Profile p;
  return loadGameOverride(gameId, p);
}

bool loadGameOverride(int gameId, Profile &out) {
  if (gameId <= 0)
    return false;
  std::ifstream in(gamePath(gameId));
  if (!in)
    return false;
  nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
  return !j.is_discarded() && Profile::fromJson(j, out);
}

bool saveGameOverride(int gameId, const Profile &p) {
  if (gameId <= 0)
    return false;
  std::error_code ec;
  fs::create_directories(Settings::configDir() + "/controls", ec);
  const std::string path = gamePath(gameId);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
      return false;
    out << p.toJson().dump(2);
    if (!out)
      return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    ec.clear();
    fs::rename(tmp, path, ec);
  }
  g_revision++;
  return !ec;
}

void removeGameOverride(int gameId) {
  std::error_code ec;
  fs::remove(gamePath(gameId), ec);
  g_revision++;
}

Profile loadEffective(int gameId) {
  Profile p;
  if (loadGameOverride(gameId, p))
    return p;
  return loadGlobal();
}

std::string describe(int gameId, const Profile &p) {
  std::string out = "game " + std::to_string(gameId) + ", " +
                    (hasGameOverride(gameId) ? "own profile " + gamePath(gameId) : std::string("general profile")) +
                    ": ";
  for (int i = 0; i < S_COUNT; i++) {
    out += kSourceNames[i];
    out += "->";
    out += p.map[i] < 0 ? "-" : kTargetNames[p.map[i]];
    out += i + 1 < S_COUNT ? ", " : "";
  }
  out += "; swap=" + std::to_string(p.swapSticks) + " invL=" + std::to_string(p.invertLeftY) +
         " invR=" + std::to_string(p.invertRightY) + " deadzone=" + std::to_string(p.deadzone) +
         " trig=" + std::to_string(p.triggersFull);
  return out;
}

uint32_t revision() { return g_revision.load(); }

} // namespace vkpcnx::controls
