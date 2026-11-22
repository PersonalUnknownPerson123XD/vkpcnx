#include "view/game_details.hpp"

#include "activity/launch_queue_activity.hpp"
#include "core/forwarder/forwarder.hpp"
#include "core/http.hpp"
#include "core/utils/thread.hpp"
#include "view/controller_profile_dialog.hpp"

#include <atomic>

namespace vkpcnx {

namespace {

// Cuts at a UTF-8 character boundary and marks the cut
std::string clip(const std::string &text, size_t maxBytes) {
  if (text.size() <= maxBytes)
    return text;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
    cut--;
  return text.substr(0, cut) + "…";
}

brls::Label *makeLabel(const std::string &text, float fontSize, bool dim = false) {
  auto *label = new brls::Label();
  label->setText(text);
  label->setFontSize(fontSize);
  label->setSingleLine(false);
  if (dim)
    label->setTextColor(nvgRGB(160, 160, 165));
  return label;
}

void showMessage(const std::string &text) {
  auto *dialog = new brls::Dialog(text);
  dialog->addButton("OK", [] {});
  dialog->setCancelable(true);
  dialog->open();
}

std::atomic<bool> installing{false};

void startInstall(const GameCard::Game &game) {
  if (installing.exchange(true)) {
    brls::Application::notify("Ярлык уже устанавливается");
    return;
  }
  brls::Application::notify("Устанавливаем ярлык…");

  forwarder::Request request;
  request.gameId = game.id;
  request.launcherId = game.launcherId;
  request.name = game.name;
  request.coverUrl = game.pictureUrl;

  // The shortcut starts vkpcnx again with the game's ids; the name and cover
  // ride along (url-encoded) so the queue screen can show them right away
  request.args = " --name=" + Http::urlEncode(game.name);
  if (!game.pictureUrl.empty())
    request.args += " --cover=" + Http::urlEncode(game.pictureUrl);

  vkpcnx::utils::runDetached([request] {
    forwarder::Outcome outcome = forwarder::install(request, nullptr);
    brls::sync([outcome] {
      installing = false;
      if (outcome.ok)
        showMessage(
          outcome.message +
          "\n\nЗакройте vkpcnx: значок игры появится на главном экране Switch. Если он при запуске "
          "сразу вылетает с ошибкой, в системе нет сигпатчей (Atmosphère + sigpatches)."
        );
      else
        showMessage(outcome.message);
    });
  });
}

void confirmInstall(const GameCard::Game &game) {
  std::string text;
  switch (forwarder::detectMmc()) {
  case forwarder::MmcKind::EmuMmc:
    text = "Ярлык ставится на главный экран как отдельное приложение (маленький неподписанный пакет). "
           "Вы работаете в emuMMC — это рекомендуемый вариант.\n\nДля запуска нужны сигпатчи "
           "(Atmosphère + sigpatches).";
    break;
  case forwarder::MmcKind::SysMmc:
    text = "ВНИМАНИЕ: вы работаете в sysMMC (настоящая системная память). Ярлык — это неподписанный "
           "пакет, установленный в системную NAND; это рискованно (проблемы с системой, возможен бан "
           "при выходе в сеть). Ярлыки лучше ставить только в emuMMC.\n\nУстановить всё равно?";
    break;
  default:
    text = "Ярлык ставится на главный экран как отдельное неподписанное приложение. Определить, emuMMC "
           "это или sysMMC, не удалось — ярлыки лучше ставить только в emuMMC.\n\nДля запуска нужны "
           "сигпатчи (Atmosphère + sigpatches). Установить?";
    break;
  }
  auto *dialog = new brls::Dialog(text);
  dialog->addButton("Отмена", [] {});
  dialog->addButton("Установить", [game] { startInstall(game); });
  dialog->setCancelable(true);
  dialog->open();
}

} // namespace

GameDetailsContent::GameDetailsContent(const GameCard::Game &game) {
  this->setAxis(brls::Axis::COLUMN);
  this->setPadding(24, 28, 12, 28);

  auto *top = new brls::Box(brls::Axis::ROW);

  cover = new brls::Image();
  cover->setWidth(280);
  cover->setHeight(158);
  cover->setScalingType(brls::ImageScalingType::FILL);
  cover->setCornerRadius(8);
  top->addView(cover);

  auto *info = new brls::Box(brls::Axis::COLUMN);
  info->setGrow(1);
  info->setMarginLeft(22);
  info->addView(makeLabel(game.name, 26));

  std::string meta = game.genres;
  auto addMeta = [&meta](const std::string &part) {
    if (part.empty())
      return;
    meta += (meta.empty() ? "" : "  •  ") + part;
  };
  addMeta(game.publishers);
  if (game.ageLimit > 0)
    addMeta(std::to_string(game.ageLimit) + "+");
  if (game.gamepadSupport)
    addMeta("геймпад");
  if (!meta.empty()) {
    auto *metaLabel = makeLabel(meta, 16, true);
    metaLabel->setMarginTop(10);
    info->addView(metaLabel);
  }
  top->addView(info);
  this->addView(top);

  std::string description = game.description.empty() ? "Описание недоступно." : game.description;
  auto *descr = makeLabel(clip(description, 900), 18);
  descr->setMarginTop(18);
  this->addView(descr);

  if (!game.pictureUrl.empty())
    coverRequest = vkpcnx::utils::loadScaledImage(
      game.pictureUrl, 420, 240, [this](vkpcnx::utils::DecodedImagePtr decoded) {
        vkpcnx::utils::showDecodedImage(this->cover, *decoded);
      }
    );
}

GameDetailsContent::~GameDetailsContent() {
  if (coverRequest)
    coverRequest->cancel();
}

void openGameDetails(const GameCard::Game &game) {
  auto *dialog = new brls::Dialog(new GameDetailsContent(game));
  dialog->addButton("Играть", [game] {
    brls::Application::pushActivity(new LaunchQueueActivity(game));
  });
  dialog->addButton("Управление", [game] { openControllerProfileDialog(game.id, game.name); });
  if (forwarder::supported() && game.launcherId != 0)
    dialog->addButton("Добавить на главный экран", [game] { confirmInstall(game); });
  dialog->setCancelable(true);
  dialog->getAppletFrame()->setWidth(860);
  dialog->open();
}

} // namespace vkpcnx
