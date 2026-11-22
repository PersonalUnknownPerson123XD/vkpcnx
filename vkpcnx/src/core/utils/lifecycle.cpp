#include "core/utils/lifecycle.hpp"

#include "activity/main_activity.hpp"
#include "core/diag/log.hpp"

#include <borealis.hpp>
#include <cstdlib>

#ifdef __SWITCH__
#include <switch.h>
extern "C" u32 __nx_applet_exit_mode;
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

[[noreturn]] void exitToHome() {
#ifdef __SWITCH__
  // Official Application exit for an NRO launched as a HOME-shortcut
  // Application: set __nx_applet_exit_mode=1 so libnx appletExit issues
  // ISelfController::Exit on the existing self-controller session, then
  // std::exit so userAppExit runs (socketExit + appletUnlockExit).
  // Opening a second appletOE proxy and calling Exit while LockExit was
  // still held, then svcExitProcess, is what produced the Atmosphere error
  // screen after "Log: clean exit". Do not return into the forwarder (it
  // unmaps this NRO's .text). appletRequestExitToSelf is LibraryApplet-only.
  brls::Logger::info("exitToHome: official SelfExit (__nx_applet_exit_mode=1, UnlockExit in userAppExit)");
  vkpcnx::diag::flushLogging();
  __nx_applet_exit_mode = 1;
  vkpcnx::diag::shutdownLogging();
  std::exit(EXIT_SUCCESS);
  __builtin_unreachable();
#else
  std::exit(EXIT_SUCCESS);
  __builtin_unreachable();
#endif
}
} // namespace vkpcnx::utils
