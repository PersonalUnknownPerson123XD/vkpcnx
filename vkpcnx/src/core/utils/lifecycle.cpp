#include "core/utils/lifecycle.hpp"

#include "activity/main_activity.hpp"

#include <borealis.hpp>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <unistd.h>
#endif

namespace vkpcnx::utils {

namespace {
std::string &execPath() {
  static std::string path;
  return path;
}
} // namespace

void setExecPath(const std::string &path) { execPath() = path; }
std::string getExecPath() { return execPath(); }

namespace {
bool g_shortcutSession = false;
}
void setShortcutSession(bool on) { g_shortcutSession = on; }
bool shortcutSession() { return g_shortcutSession; }

void finishScreen(std::function<void()> then) {
  if (g_shortcutSession) {
    // Tear the screens down the normal way (the stream activity owns the
    // session, decoder and audio and frees them when popped) and only then
    // end the main loop, one iteration later and on the UI thread: quitting
    // with the stream still alive crashed on the way out.
    brls::sync([] {
      auto quit = [] { brls::sync([] { brls::Application::quit(); }); };
      if (brls::Application::getActivitiesStack().size() > 1) {
        brls::Logger::info("shortcut session over: closing the stream, then quitting");
        brls::Application::popActivity(brls::TransitionAnimation::NONE, quit);
      } else {
        brls::Logger::info("shortcut session over: quitting");
        quit();
      }
    });
    return;
  }
  brls::Application::popActivity(
    brls::TransitionAnimation::FADE, then ? std::move(then) : std::function<void()>([] {})
  );
}

void fallBackToCatalog() {
  g_shortcutSession = false;
  brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::FADE);
}

void relaunch() {
#ifdef __SWITCH__
  envSetNextLoad(execPath().c_str(), execPath().c_str());
  brls::Application::quit();
#else
  char *args[] = {execPath().data(), nullptr};
  brls::Application::quit();
  execv(execPath().c_str(), args);
#endif
}
} // namespace vkpcnx::utils
