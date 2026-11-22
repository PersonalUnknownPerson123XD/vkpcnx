#include "activity/main_activity.hpp"
#include "borealis/core/view.hpp"
#include "core/account_info.hpp"
#include "core/utils/thread.hpp"
#include "core/vkpcapi.hpp"
#include "view/game_card_row.hpp"
#include "view/games_data_source.hpp"
#include "activity/stream_activity.hpp"
#include <algorithm>
#include <string>

using namespace brls::literals;

// After login this activity sits on top of LoginActivity (borealis never
// pops the root), so the frame's default B would pop back to the login
// screen. Behave like a root activity instead: offer to exit the app.
void MainActivity::overrideBackAction() {
  appletFrame->registerAction(
    "hints/exit"_i18n,
    brls::BUTTON_B,
    [](brls::View *) {
      auto *dialog = new brls::Dialog("hints/exit_hint"_i18n);
      dialog->addButton("hints/cancel"_i18n, [] {});
      dialog->addButton("hints/ok"_i18n, [] { brls::Application::quit(); });
      dialog->setCancelable(true);
      dialog->open();
      return true;
    },
    true,
    false,
    brls::SOUND_BACK
  );
}

void MainActivity::renderGamesList() {
  vkpcnx::GamesDataSource::OnNeedMore onNeedMore =
    [this](vkpcnx::GamesDataSource::MoreGamesCallback cb) { this->fetchMoreGames(cb); };

  this->content->registerCell("GameCardRow", []() { return vkpcnx::GameCardRow::create(); });
  this->dataSource = new vkpcnx::GamesDataSource(onNeedMore);
  this->content->setDataSource(this->dataSource);
}

void MainActivity::fetchMoreGames(vkpcnx::GamesDataSource::MoreGamesCallback cb) {
  if (this->loading || !this->hasMore)
    return;

  this->loading = true;
  this->page++;

  auto params = VKPCAPI::QueryParams{
    {"page", std::to_string(this->page)}, {"pageSize", "24"}, {"o", "-priority"}
  };
  if (!this->searchQuery.empty())
    params["search"] = this->searchQuery;

  // Read on the UI thread: the account is replaced there by every refresh
  const auto &account = AccountInfo::instance().get();
  // TODO: indicate on main screen if user has no plan
  try {
    if (account && account->is_object() && account->contains("plan") &&
        account->at("plan").is_object()) {
      params["plan_family_slug"] = account->at("plan").at("plan_family").get<std::string>();
    }
  } catch (const nlohmann::json::exception &e) {
    brls::Logger::warning("MainActivity: no plan family in account: {}", e.what());
  }

  const int gen = this->generation;
  const int requestedPage = this->page;
  const std::string query = this->searchQuery;
  auto aliveToken = this->alive;

  vkpcnx::utils::runDetached([=, cb = std::move(cb)] {
    vkpcnx::GamesDataSource::GamesList gamesList;
    try {
      auto games = VKPCAPI::indexGames(params);
      // TODO: error handling
      if (games.is_object() && games.contains("results") && games.at("results").is_array())
        for (auto &game : games.at("results"))
          gamesList.push_back(game);
    } catch (const std::exception &e) {
      brls::Logger::warning("MainActivity: games request failed: {}", e.what());
    }

    brls::sync([=, gamesList = std::move(gamesList)]() mutable {
      // The activity is gone, or a newer search replaced the one this belongs to
      if (!*aliveToken || gen != this->generation)
        return;

      bool empty = gamesList.empty();
      cb(std::move(gamesList));

      this->loading = false;
      if (empty)
        this->hasMore = false;

      if (requestedPage == 1) {
        this->loader->setVisibility(brls::Visibility::GONE);
        this->content->setVisibility(brls::Visibility::VISIBLE);
        if (empty) {
          this->status->setText(
            query.empty() ? "Игры не найдены" : "Ничего не найдено по запросу «" + query + "»"
          );
          this->status->setVisibility(brls::Visibility::VISIBLE);
        } else {
          this->status->setVisibility(brls::Visibility::GONE);
          // The list was rebuilt (new search): give the focus back to it
          if (!brls::Application::getCurrentFocus())
            brls::Application::giveFocus(this->content);
        }
      }
    });
  });
}

// ---- search ----------------------------------------------------------------

void MainActivity::registerSearchAction() {
  appletFrame->registerAction("Поиск", brls::BUTTON_Y, [this](brls::View *) {
    openSearch();
    return true;
  });
}

void MainActivity::openSearch() {
  auto aliveToken = this->alive;
  // The on-screen keyboard (swkbd on the console) takes Russian as well as Latin
  bool opened = brls::Application::getImeManager()->openForText(
    [this, aliveToken](std::string text) {
      if (!*aliveToken)
        return;
      setSearch(std::move(text));
    },
    "Поиск игры",
    "Название игры; пустое поле показывает весь каталог",
    64,
    this->searchQuery
  );
  if (!opened)
    brls::Application::notify("Не удалось открыть клавиатуру");
}

void MainActivity::setSearch(std::string query) {
  // Trim blanks around the query
  const char *ws = " \t\r\n";
  size_t b = query.find_first_not_of(ws);
  query = b == std::string::npos ? "" : query.substr(b, query.find_last_not_of(ws) - b + 1);

  if (query == this->searchQuery)
    return;
  this->searchQuery = query;

  if (query.empty()) {
    appletFrame->updateActionHint(brls::BUTTON_Y, "Поиск");
    if (clearSearchAction != ACTION_NONE) {
      appletFrame->unregisterAction(clearSearchAction);
      clearSearchAction = ACTION_NONE;
    }
  } else {
    std::string shown = query.size() > 24 ? query.substr(0, 24) + "…" : query;
    appletFrame->updateActionHint(brls::BUTTON_Y, "Поиск: " + shown);
    if (clearSearchAction == ACTION_NONE)
      clearSearchAction = appletFrame->registerAction("Сбросить поиск", brls::BUTTON_X, [this](brls::View *) {
        setSearch("");
        return true;
      });
  }

  restartGames();
}

void MainActivity::restartGames() {
  // Replies to the previous search are dropped by their generation
  this->generation++;
  this->page = 0;
  this->hasMore = true;
  this->loading = false;

  if (!this->dataSource)
    return; // the catalog isn't up yet; renderGamesList() will use the query

  this->status->setText(this->searchQuery.empty() ? "Загружаем каталог…" : "Ищем…");
  this->status->setVisibility(brls::Visibility::VISIBLE);
  this->dataSource->reload();
}

// ---- account refresh -------------------------------------------------------

void MainActivity::onResume() {
  CustomTitlebarActivity::onResume();
  AccountInfo::instance().fetchAsync();
  // The server may settle the minutes balance a moment after the session ended
  auto aliveToken = this->alive;
  brls::delay(5000, [aliveToken] {
    if (*aliveToken)
      AccountInfo::instance().fetchAsync();
  });
}

void MainActivity::scheduleAccountRefresh() {
  auto aliveToken = this->alive;
  brls::delay(60 * 1000, [this, aliveToken] {
    if (!*aliveToken)
      return;
    // Only while on the menus: the stream has its own traffic to look after
    auto stack = brls::Application::getActivitiesStack();
    bool streaming = !stack.empty() && dynamic_cast<StreamActivity *>(stack.back()) != nullptr;
    if (!streaming)
      AccountInfo::instance().fetchAsync();
    scheduleAccountRefresh();
  });
}
