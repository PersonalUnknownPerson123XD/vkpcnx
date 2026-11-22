#pragma once
#include "core/account_info.hpp"
#include "view/games_data_source.hpp"
#include "view/title_bar.hpp"
#include <borealis.hpp>
#include <memory>

class MainActivity : public vkpcnx::CustomTitlebarActivity {
public:
  MainActivity() : CustomTitlebarActivity("main/root") {}

  CONTENT_FROM_XML_RES("activity/main.xml");
  BRLS_BIND(brls::AppletFrame, appletFrame, "main/root");

  void onContentReady() override {
    overrideBackAction();
    // + quits from the root screens only (no global quit: on the stream
    // screen, its overlay and dialogs + belongs to the stream)
    registerExitAction();

    auto &accountInfo = AccountInfo::instance();
    accountInfo.fetch();

    if (accountInfo.get()) {
      renderGamesList();
    } else {
      // The subscription handle must outlive this function (the callback runs
      // on a later fetch), so it lives on the heap rather than on this stack
      using Sub = brls::Event<const nlohmann::json &>::Subscription;
      auto sub = std::make_shared<Sub>();
      *sub = accountInfo.updated().subscribe([this, sub](const nlohmann::json &account) {
        renderGamesList();
        AccountInfo::instance().updated().unsubscribe(*sub);
      });
    }
  }

private:
  BRLS_BIND(brls::Box, loader, "main/loader");
  BRLS_BIND(brls::RecyclerFrame, content, "main/content");
  int page = 0;
  bool loading = false;
  bool hasMore = true;

  void overrideBackAction();
  void renderGamesList();

  void fetchMoreGames(vkpcnx::GamesDataSource::MoreGamesCallback cb);
};
