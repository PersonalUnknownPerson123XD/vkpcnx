#pragma once
#include "core/account_info.hpp"
#include "view/games_data_source.hpp"
#include "view/title_bar.hpp"
#include <borealis.hpp>
#include <memory>
#include <string>

class MainActivity : public vkpcnx::CustomTitlebarActivity {
public:
  MainActivity() : CustomTitlebarActivity("main/root") {}

  ~MainActivity() override {
    *alive = false;
    if (haveAccountSub)
      AccountInfo::instance().updated().unsubscribe(accountSub);
  }

  CONTENT_FROM_XML_RES("activity/main.xml");
  BRLS_BIND(brls::AppletFrame, appletFrame, "main/root");

  void onContentReady() override {
    overrideBackAction();
    // + quits from the root screens only (no global quit: on the stream
    // screen, its overlay and dialogs + belongs to the stream)
    registerExitAction();
    registerSearchAction();

    auto &accountInfo = AccountInfo::instance();
    if (accountInfo.get()) {
      renderGamesList();
    } else {
      // The first /users/me reply (fetchAsync below) starts the catalog. The
      // handle stays subscribed until ~MainActivity: unsubscribing from inside
      // the callback would invalidate the event's iteration.
      accountSub = accountInfo.updated().subscribe([this](const nlohmann::json &) {
        if (!dataSource)
          renderGamesList();
      });
      haveAccountSub = true;
    }

    // Fresh minutes/tariff on every visit, and every minute while on the menus
    accountInfo.fetchAsync();
    scheduleAccountRefresh();
  }

  // Back on top again, e.g. after a stream session ended
  void onResume() override;

private:
  BRLS_BIND(brls::Box, loader, "main/loader");
  BRLS_BIND(brls::Label, status, "main/status");
  BRLS_BIND(brls::RecyclerFrame, content, "main/content");
  int page = 0;
  bool loading = false;
  bool hasMore = true;

  // Catalog search (the API's `search` parameter), asked for with Y
  std::string searchQuery;
  brls::ActionIdentifier clearSearchAction = ACTION_NONE;
  // Bumped on every new search so replies to an older one are dropped
  int generation = 0;
  vkpcnx::GamesDataSource *dataSource = nullptr; // owned by `content`

  brls::Event<const nlohmann::json &>::Subscription accountSub;
  bool haveAccountSub = false;
  // Cleared in the destructor; guards callbacks that may outlive the activity
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  void overrideBackAction();
  void renderGamesList();

  void fetchMoreGames(vkpcnx::GamesDataSource::MoreGamesCallback cb);

  void registerSearchAction();
  void openSearch();
  void setSearch(std::string query);
  void restartGames();

  void scheduleAccountRefresh();
};
