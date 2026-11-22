#pragma once
#include "core/account_info.hpp"
#include "core/settings.hpp"
#include "core/diag/build_info.hpp"
#include "core/utils/thread.hpp"
#include "view/controller_profile_dialog.hpp"
#include "view/debug_report.hpp"
#include "view/feedback_qr_dialog.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include "view/stream_overlay/gamepad_mapping_dialog.hpp"
#include "view/stream_overlay/gamepad_pointer_dialog.hpp"
#include "view/stream_overlay/touchscreen_mode_dialog.hpp"
#include <borealis.hpp>

namespace vkpcnx {
class AccountDetails : public brls::Box {
public:
  AccountDetails() {
    this->inflateFromXMLRes("xml/views/account.xml");

    auto &accountInfo = AccountInfo::instance();

    accountInfoSub = accountInfo.updated().subscribe([this](const nlohmann::json &account) {
      renderSafely(account);
    });

    // Show what we have right away, and ask for fresh minutes/tariff either way
    if (accountInfo.get() && !accountInfo.get()->empty())
      renderSafely(*accountInfo.get());
    accountInfo.fetchAsync();

    initStreamSettings();

    this->gamepadMappingModeCell->setVisibility(brls::Visibility::GONE);
    this->gamepadMappingModeCell->registerClickAction([](auto &&) {
      openControllerProfileDialog(0, "");
      return true;
    });
    this->controllerProfileCell->registerClickAction([](auto &&) {
      openControllerProfileDialog(0, "");
      return true;
    });
    this->gamepadPointerCell->registerClickAction([this](auto &&) {
      this->openDialog(new GamepadPointerDialog());
      return true;
    });
    this->touchscreenModeCell->registerClickAction([this](auto &&) {
      this->openDialog(new TouchscreenModeDialog());
      return true;
    });
    this->controlsPlaygroundBtn->registerClickAction([this](auto &&) {
      this->openControlsPlayground();
      return true;
    });
    this->feedbackBtn->registerClickAction([](auto &&) {
      brls::Dialog *dialog = new brls::Dialog(new FeedbackQrDialog());
      dialog->addButton("Создать отчёт об ошибке", [] { openDebugReportDialog(); });
      dialog->addButton("Закрыть", [] {});
      dialog->setCancelable(true);
      dialog->getAppletFrame()->setWidth(800);
      dialog->open();
      return true;
    });
    this->logoutBtn->registerClickAction([this](auto &&) {
      this->openLogoutDialog();
      return true;
    });

    this->versionLabel->setText("vkpcnx " + diag::buildInfo().git);

    vkpcnx::utils::runDetached([this] { fetchServerLoad(); });
    this->controlsUpdLoopSub =
      brls::Application::getRunLoopEvent()->subscribe([this] { this->renderControlsInfo(); });
  };
  ~AccountDetails() {
    AccountInfo::instance().updated().unsubscribe(accountInfoSub);
    brls::Application::getRunLoopEvent()->unsubscribe(this->controlsUpdLoopSub);
  }

  // Video bitrate cap / frame rate / resolution of the stream, read by
  // StreamActivity::buildConfig() when a game starts
  void initStreamSettings() {
    static const int bitrates[] = {0, 3, 5, 8, 12};
    static const int fpsValues[] = {0, 30, 60};
    auto &settings = Settings::instance();
#ifdef __SWITCH__
    constexpr int DEFAULT_BITRATE = 5;
#else
    constexpr int DEFAULT_BITRATE = 0;
#endif
    int bitrate = settings.get<int>("/stream/bitrate_max_mbit", DEFAULT_BITRATE);
    int bitrateIdx = 0;
    for (int i = 0; i < 5; i++)
      if (bitrates[i] == bitrate)
        bitrateIdx = i;
    this->streamBitrateCell->init(
      "Битрейт видео",
      {"Авто (до 12 Мбит/с)", "3 Мбит/с", "5 Мбит/с", "8 Мбит/с", "12 Мбит/с"},
      bitrateIdx,
      [](int i) { Settings::instance().set<int>("/stream/bitrate_max_mbit", bitrates[i]); }
    );

    int fps = settings.get<int>("/stream/fps", 0);
    int fpsIdx = 0;
    for (int i = 0; i < 3; i++)
      if (fpsValues[i] == fps)
        fpsIdx = i;
    this->streamFpsCell->init(
      "Частота кадров", {"Авто (60)", "30", "60"}, fpsIdx,
      [](int i) { Settings::instance().set<int>("/stream/fps", fpsValues[i]); }
    );

    int height = settings.get<int>("/stream/height", 0);
    this->streamResolutionCell->init(
      "Разрешение", {"Авто (по экрану)", "720p"}, height == 720 ? 1 : 0,
      [](int i) {
        Settings::instance().set<int>("/stream/width", i == 1 ? 1280 : 0);
        Settings::instance().set<int>("/stream/height", i == 1 ? 720 : 0);
      }
    );
  }

private:
  // The views read many fields of the reply; a malformed one must not take the app down
  void renderSafely(const nlohmann::json &account) {
    try {
      renderAccountDetails(account);
    } catch (const nlohmann::json::exception &e) {
      brls::Logger::warning("AccountDetails: unexpected account json: {}", e.what());
    }
  }

  BRLS_BIND(brls::Box, container, "account_details/root");

  BRLS_BIND(brls::Image, avatar, "account_details/avatar");
  BRLS_BIND(brls::Label, username, "account_details/username");

  BRLS_BIND(brls::Label, planName, "account_details/plan/name");
  BRLS_BIND(brls::Label, planDescription, "account_details/plan/description");

  BRLS_BIND(brls::Label, serverLoadLabel, "account_details/server_load/label");
  BRLS_BIND(brls::Rectangle, serverLoadBar1, "account_details/server_load/bar_1");
  BRLS_BIND(brls::Rectangle, serverLoadBar2, "account_details/server_load/bar_2");
  BRLS_BIND(brls::Rectangle, serverLoadBar3, "account_details/server_load/bar_3");

  BRLS_BIND(brls::DetailCell, gamepadMappingModeCell, "account_details/gamepad_mapping_mode");
  BRLS_BIND(brls::SelectorCell, streamBitrateCell, "account_details/stream_bitrate");
  BRLS_BIND(brls::SelectorCell, streamFpsCell, "account_details/stream_fps");
  BRLS_BIND(brls::SelectorCell, streamResolutionCell, "account_details/stream_resolution");
  BRLS_BIND(brls::DetailCell, controllerProfileCell, "account_details/controller_profile");
  BRLS_BIND(brls::DetailCell, gamepadPointerCell, "account_details/gamepad_pointer");
  BRLS_BIND(brls::DetailCell, touchscreenModeCell, "account_details/touchscreen_mode");
  BRLS_BIND(brls::Button, controlsPlaygroundBtn, "account_details/controls_playground");
  BRLS_BIND(brls::Button, feedbackBtn, "account_details/feedback");

  BRLS_BIND(brls::Button, logoutBtn, "account_details/logout");
  BRLS_BIND(brls::Label, versionLabel, "account_details/version");

  std::string getUnlimitedTimespan() const;
  void openDialog(DialogContent *content);
  void openLogoutDialog();
  void openControlsPlayground();
  void renderAccountDetails(const nlohmann::json &account);
  void fetchServerLoad();

  void renderControlsInfo();
  brls::Event<const nlohmann::json &>::Subscription accountInfoSub;
  brls::VoidEvent::Subscription controlsUpdLoopSub;
  std::string lastMappingModeName;
  std::string lastGamepadPointerText;
  std::string lastTouchscreenModeText;
};
}; // namespace vkpcnx