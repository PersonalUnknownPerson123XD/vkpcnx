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

[[noreturn]] void exitToHome() {
#ifdef __SWITCH__
  brls::Logger::info("exitToHome: ISelfController::Exit + svcExitProcess");
  Result rc = 1;
  Handle proc = envGetOwnProcessHandle();
  // appletOE is Application / SystemApplication (HOME shortcut, title override)
  const char *names[] = {"appletOE", "appletAE"};
  for (const char *name : names) {
    Service applet{};
    if (R_FAILED(smGetService(&applet, name)))
      continue;
    Service proxy{}, self{};
    const u64 reserved = 0;
    rc = serviceDispatchIn(
      &applet, 0, reserved, .in_send_pid = true, .in_num_handles = 1, .in_handles = {proc},
      .out_num_objects = 1, .out_objects = &proxy
    );
    if (R_SUCCEEDED(rc)) {
      rc = serviceDispatch(&proxy, 1, .out_num_objects = 1, .out_objects = &self);
      if (R_SUCCEEDED(rc)) {
        rc = serviceDispatch(&self, 0); // ISelfController::Exit
        serviceClose(&self);
      }
      serviceClose(&proxy);
    }
    serviceClose(&applet);
    if (R_SUCCEEDED(rc))
      break;
  }
  if (R_SUCCEEDED(rc))
    for (int i = 0; i < 100; i++)
      svcSleepThread(100000000ULL);
  svcExitProcess();
  __builtin_unreachable();
#else
  std::exit(EXIT_SUCCESS);
  __builtin_unreachable();
#endif
}
} // namespace vkpcnx::utils
