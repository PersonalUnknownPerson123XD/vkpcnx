#pragma once
// "Add to home screen": installs a tiny title (a forwarder) on the console so a
// game appears as its own icon on the HOME menu and starts vkpcnx straight into
// that game's queue. Switch only; elsewhere supported() is false.
#include <functional>
#include <string>

namespace vkpcnx::forwarder {

struct Request {
  int gameId = 0;
  int launcherId = 0;
  std::string name;     // title shown on HOME
  std::string coverUrl; // becomes the 256x256 HOME icon
  std::string args;     // extra argv appended after --launcher/--game (leading space), e.g. " --name=..."
};

struct Outcome {
  bool ok = false;
  std::string message; // user-facing (Russian) text, success or failure
};

enum class MmcKind { Unknown, SysMmc, EmuMmc };

bool supported();
// Which NAND the title would land in (Atmosphère's exosphere config item)
MmcKind detectMmc();
// Deterministic: the same game always maps to the same title id
unsigned long long titleIdForGame(int gameId);

// Blocking (downloads the cover, builds and installs the title): call from a worker thread.
// `progress` is called from that thread with short status texts.
Outcome install(const Request &request, const std::function<void(const std::string &)> &progress);

} // namespace vkpcnx::forwarder
