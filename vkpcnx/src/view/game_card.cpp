#include "view/game_card.hpp"
#include "activity/launch_queue_activity.hpp"
#include "view/game_details.hpp"

namespace vkpcnx {
GameCard::GameCard() {
  this->inflateFromXMLRes("xml/views/game_card.xml");
  this->setFocusable(true);

  this->addGestureRecognizer(new brls::TapGestureRecognizer(this));
  this->registerClickAction([this](brls::View *view) {
    brls::Application::pushActivity(new LaunchQueueActivity(this->game));
    return true;
  });

  // + opens the details. The focused card's action wins over the screen's
  // "+ quits", which stays in effect wherever no card has the focus.
  this->registerAction("Подробнее", brls::BUTTON_START, [this](brls::View *) {
    this->openDetails();
    return true;
  });
}

void GameCard::openDetails() { openGameDetails(this->game); }

GameCard::~GameCard() { cancelImageLoad(); }

void GameCard::cancelImageLoad() {
  if (imageRequest)
    imageRequest->cancel();
  imageRequest.reset();
  if (!imageReady)
    imageUrl.clear();
}

void GameCard::setGame(Game game) {
  const bool sameCover = !game.pictureUrl.empty() && game.pictureUrl == imageUrl &&
                         (imageReady || imageRequest);
  this->game = game;
  this->label->setText(game.name);
  if (sameCover)
    return; // recycled for the same game (e.g. after a list reload): keep the texture

  cancelImageLoad();
  this->image->clear();
  imageUrl.clear();
  imageReady = false;
  if (game.pictureUrl.empty())
    return;

  imageUrl = game.pictureUrl;
  // The cover is drawn 280x160 dp ("fill"); a bit more covers a 1.5x docked scale.
  // The callback only runs on the UI thread and only while this request is live,
  // which ~GameCard() / cancelImageLoad() guarantee, so `this` is valid there.
  imageRequest = vkpcnx::utils::loadScaledImage(
    game.pictureUrl, 420, 240, [this](vkpcnx::utils::DecodedImagePtr decoded) {
      vkpcnx::utils::showDecodedImage(this->image, *decoded);
      this->imageReady = true;
    }
  );
}
} // namespace vkpcnx
