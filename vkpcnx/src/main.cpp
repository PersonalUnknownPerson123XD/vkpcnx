#include <borealis.hpp>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "activity/launch_queue_activity.hpp"
#include "activity/login_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/stream_activity.hpp"
#include "core/diag/build_info.hpp"
#include "core/diag/crash_handler.hpp"
#include "core/diag/log.hpp"
#include "core/diag/report.hpp"
#include "core/http.hpp"
#include "core/nx_fps.hpp"
#include "core/platform/switch_heap.hpp"
#include "core/platform/switch_socket_config.hpp"
#include "core/settings.hpp"
#include "core/utils/lifecycle.hpp"
#include "core/utils/thread.hpp"
#include "view/animated_webp.hpp"
#include "view/debug_report.hpp"
#include "view/fill_scrolling_frame.hpp"
#include "view/onscreen_keyboard.hpp"
#include "view/qrcode.hpp"
#include "view/stream_view.hpp"
#include "view/striped_list.hpp"

#include <glad/glad.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {
// Percent-decoding for --name= / --cover= (the forwarder writes them url-encoded
// so they survive the loader's space-separated argv)
std::string urlDecode(std::string_view in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size() && std::isxdigit((unsigned char)in[i + 1]) &&
        std::isxdigit((unsigned char)in[i + 2])) {
      out += static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

int parseInt(std::string_view v) {
  try {
    return std::stoi(std::string(v));
  } catch (...) {
    return 0;
  }
}
} // namespace

int main(int argc, char *argv[]) {
  // First: everything below may log, and crashes need the log file open
  vkpcnx::diag::initLogging(argc, argv);
  vkpcnx::diag::installCrashHandlers();

  vkpcnx::utils::setExecPath(argv[0]);
  brls::Logger::info("exec path: {}", argv[0] ? argv[0] : "(null)");
#ifdef __SWITCH__
  vkpcnx::platform::logNetworkStartup();
#endif
#ifdef VKPCNX_HAS_DEBUG_KEYS
  new vkpcnx::utils::DebugKeyListener();
#endif

  // --test-crash=segv|abort|throw crashes on purpose once the UI is up, to
  // check the crash section, the report offer and symbolization end to end
  std::string_view testCrash;
  // --launcher=<id> [--game=<id> --name=<text> --cover=<url>]: set by the HOME menu
  // shortcuts ("Add to home screen"); go straight into that game's queue
  vkpcnx::GameCard::Game launchGame;
  for (int i = 1; i < argc; i++) {
    std::string_view arg(argv[i]);
    if (arg == "--create-report") {
      // For when the app can't get as far as its UI
      auto r = vkpcnx::diag::createReport("command line");
      std::printf("%s\n", r.ok ? r.path.c_str() : r.error.c_str());
      vkpcnx::diag::shutdownLogging();
      return r.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (arg == "--sw-decode")
      setenv("VKPCNX_SW_DECODE", "1", 1); // read by VideoDecoder::open()
    // The Switch has no environment; these let nxlink set the mock-server ones
    else if (arg == "--insecure-tls")
      setenv("VKPCNX_INSECURE_TLS", "1", 1);
    else if (arg.rfind("--play-url=", 0) == 0)
      setenv("VKPCNX_PLAY_URL", std::string(arg.substr(11)).c_str(), 1);
    else if (arg.rfind("--test-crash=", 0) == 0)
      testCrash = arg.substr(13);
    else if (arg.rfind("--launcher=", 0) == 0)
      launchGame.launcherId = parseInt(arg.substr(11));
    else if (arg.rfind("--game=", 0) == 0)
      launchGame.id = parseInt(arg.substr(7));
    else if (arg.rfind("--name=", 0) == 0)
      launchGame.name = urlDecode(arg.substr(7));
    else if (arg.rfind("--cover=", 0) == 0)
      launchGame.pictureUrl = urlDecode(arg.substr(8));
  }

#ifdef __SWITCH__
  if (const auto &fix = heapFixup(); fix.count > 0) {
    brls::Logger::warning(
      "heap: inherited heap {:#x}..{:#x} has {} unusable block(s), using {}..{}", fix.heapStart,
      fix.heapEnd, fix.count, static_cast<void *>(fake_heap_start),
      static_cast<void *>(fake_heap_end)
    );
    for (int i = 0; i < fix.count && i < HeapFixup::kMaxBad; i++)
      brls::Logger::warning(
        "heap:   {:#x}..{:#x} perm={:#x} attr={:#x}", fix.bad[i].start, fix.bad[i].end,
        fix.bad[i].perm, fix.bad[i].attr
      );
  }
#endif

#ifdef __SWITCH__
  // borealis renders all text with the zh-Hans shared font on Switch, which draws
  // Cyrillic full-width. Its custom font replaces it, so use Nintendo's standard
  // font (proportional Latin + Cyrillic); CJK and icon fallbacks stay attached.
  brls::FontLoader::USER_FONT_PATH = BRLS_ASSET("font/switch_font.ttf");
#endif

  if (!brls::Application::init()) {
    brls::Logger::error("Application::init failed");
    return EXIT_FAILURE;
  }

  // HOME / OnExitRequest only sets quitRequested; Application::exit() then
  // runs inside mainLoop (logs "Exiting..." and deletes the root frame).
  // harica15 stopped right after that delete with no "main loop exited" or
  // "Log: clean exit", so flush here to see whether we die in clear(),
  // Threading::stop(), or while destroying the platform.
  brls::Application::getExitEvent()->subscribe([] {
    brls::Logger::info("exit: Application::exit starting (tearing down UI)");
    vkpcnx::diag::flushLogging();
#ifdef __SWITCH__
    // Teardown stopped dead after "delete AppletFrame main/root" in harica15
    // logs: the OS then force-kills us (error screen). If the UI/threads
    // still haven't finished in 6 s, release the exit lock and leave ourselves.
    static std::atomic<bool> exitFinished{false};
    brls::Application::getExitDoneEvent()->subscribe([] { exitFinished = true; });
    std::thread([] {
      for (int i = 0; i < 60 && !exitFinished; i++)
        svcSleepThread(100ULL * 1000 * 1000);
      if (exitFinished)
        return;
      brls::Logger::error("exit: watchdog — UI teardown stuck for 6 s, forcing exit");
      vkpcnx::diag::flushLogging();
      appletUnlockExit();
      svcExitProcess();
    }).detach();
#endif
  });
  brls::Application::getExitDoneEvent()->subscribe([] {
    brls::Logger::info("exit: UI and worker threads stopped, destroying platform");
    vkpcnx::diag::flushLogging();
  });

  brls::Application::registerXMLView("vkpcnx:AnimatedWebp", vkpcnx::AnimatedWebp::create);
  brls::Application::registerXMLView(
    "vkpcnx:FillScrollingFrame", vkpcnx::FillScrollingFrame::create
  );
  brls::Application::registerXMLView("vkpcnx:OnscreenKeyboard", [] {
    return new vkpcnx::OnscreenKeyboard();
  });
  brls::Application::registerXMLView("vkpcnx:QrCode", vkpcnx::QrCode::create);
  brls::Application::registerXMLView("vkpcnx:StreamView", [] { return new vkpcnx::StreamView(); });
  brls::Application::registerXMLView("vkpcnx:StripedList", vkpcnx::StripedList::create);
  brls::Application::registerXMLView("vkpcnx:StripedListRow", vkpcnx::StripedListRow::create);

  Settings::instance().load();
  Http::init();

  brls::Application::createWindow("VK Play Cloud");

  brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);

  auto glString = [](GLenum name) {
    auto *s = reinterpret_cast<const char *>(glGetString(name));
    return std::string(s ? s : "?");
  };
  vkpcnx::diag::setSystemInfo(
    "gpu", glString(GL_RENDERER) + " (" + glString(GL_VENDOR) + ", " + glString(GL_VERSION) + ")"
  );

  // Development aid: jump straight into a stream with a known play_url
  // (e.g. from tests/mock_server.py), bypassing login and the REST queue.
  if (const char *devPlayUrl = std::getenv("VKPCNX_PLAY_URL")) {
    auto playUrl = vkpcnx::stream::PlayUrl::parse(devPlayUrl);
    if (!playUrl) {
      brls::Logger::error("VKPCNX_PLAY_URL is not a valid playkey:// URL");
      return EXIT_FAILURE;
    }
    brls::Logger::info("VKPCNX_PLAY_URL: {}", vkpcnx::stream::PlayUrl::redact(devPlayUrl));
    vkpcnx::GameCard::Game game;
    game.name = "VKPCNX_PLAY_URL";
    brls::Application::pushActivity(new StreamActivity(game, *playUrl));
  } else if (Settings::instance().has("account")) {
    if (launchGame.launcherId > 0) {
      // Started from a HOME menu shortcut: straight into the game's queue and
      // stream, no catalog in between; the app ends with the session
      brls::Logger::info(
        "launch from shortcut: game {} launcher {}", launchGame.id, launchGame.launcherId
      );
      if (launchGame.name.empty())
        launchGame.name = "Игра";
      vkpcnx::utils::setShortcutSession(true);
      try {
        brls::Application::pushActivity(
          new LaunchQueueActivity(launchGame), brls::TransitionAnimation::NONE
        );
      } catch (const std::exception &e) {
        brls::Logger::error("launch from shortcut failed: {}", e.what());
        vkpcnx::utils::setShortcutSession(false);
        brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
        brls::Application::notify("Не удалось запустить игру");
      }
    } else {
      brls::Application::pushActivity(new MainActivity());
    }
  } else {
    brls::Application::pushActivity(new LoginActivity());
    if (launchGame.launcherId > 0)
      brls::delay(800, [] { brls::Application::notify("Войдите в аккаунт и запустите игру снова"); });
  }

  vkpcnx::offerReportAfterUncleanExit();

  if (!testCrash.empty())
    brls::delay(2000, [testCrash] {
      brls::Logger::warning("--test-crash={}: crashing on purpose", testCrash);
      if (testCrash == "throw")
        throw std::runtime_error("--test-crash");
      if (testCrash == "abort")
        std::abort();
      // A plain null write is UB that GCC deletes; a volatile store can't be
      volatile int *volatile target = nullptr;
      *target = 1;
    });

  brls::Logger::info("entering main loop");
  vkpcnx::nxfps::init();
  while (brls::Application::mainLoop()) {
    vkpcnx::diag::heartbeat();
    vkpcnx::nxfps::tick();
  }
  brls::Logger::info("main loop exited");
  vkpcnx::diag::flushLogging();

  brls::Logger::info("exit: joining detached workers");
  vkpcnx::diag::flushLogging();
  vkpcnx::utils::shutdownDetached();

  brls::Logger::info("exit: shutting down HTTP");
  vkpcnx::diag::flushLogging();
  Http::shutdown();

  brls::Logger::info("exit: shutting down NX-FPS");
  vkpcnx::diag::flushLogging();
  vkpcnx::nxfps::shutdown();

#ifdef __SWITCH__
  {
    AppletType at = appletGetAppletType();
    const bool isApp = at == AppletType_Application || at == AppletType_SystemApplication;
    if (isApp || vkpcnx::utils::shortcutSession()) {
      brls::Logger::info(
        "exit: Application/shortcut — official SelfExit (type {}) so we don't return to the forwarder",
        static_cast<int>(at)
      );
      vkpcnx::diag::flushLogging();
      vkpcnx::utils::exitToHome();
    }
  }
#endif

  vkpcnx::diag::shutdownLogging();
  return EXIT_SUCCESS;
}
