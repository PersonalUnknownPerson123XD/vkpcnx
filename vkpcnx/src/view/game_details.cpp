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

std::atomic<bool> installing{false};

void startInstall(const GameCard::Game &game) {
  if (installing.exchange(true)) {
    return;
  }
  brls::Application::notify("Ярлык устанавливается…");

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
      brls::Application::notify(outcome.message);
    });
  });
}

// sysMMC is the only case worth a question: the shortcut is an unsigned title
void confirmInstall(const GameCard::Game &game) {
  if (forwarder::detectMmc() != forwarder::MmcKind::SysMmc) {
    startInstall(game);
    return;
  }
  auto *dialog = new brls::Dialog("sysMMC: ярлык — неподписанное приложение. Ставить?");
  dialog->addButton("Нет", [] {});
  dialog->addButton("Да", [game] { startInstall(game); });
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

  if (!game.description.empty()) {
    auto *descr = makeLabel(clip(game.description, 500), 17);
    descr->setMarginTop(16);
    this->addView(descr);
  }

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
