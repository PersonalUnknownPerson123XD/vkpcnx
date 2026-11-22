#pragma once
#include "core/utils/image_loader.hpp"
#include <borealis.hpp>
#include <memory>

namespace vkpcnx {
class GameCard : public brls::Box {
public:
  GameCard();
  ~GameCard() override;

  struct Game {
    int id = 0;
    std::string name;
    std::string pictureUrl;
    int launcherId = 0; // game_launchers[].id / current_launcher, used for queue/run
    bool gamepadSupport = false;
  };
  void setGame(Game game);
  // Drops the pending cover load (the card is being recycled / scrolled away)
  void cancelImageLoad();

private:
  BRLS_BIND(brls::Image, image, "game_card/image");
  BRLS_BIND(brls::Label, label, "game_card/label");

  Game game;

  // Pending cover load; cancelled when the card gets other contents or goes away.
  vkpcnx::utils::ImageRequestPtr imageRequest;
  std::string imageUrl;    // cover currently shown or being loaded
  bool imageReady = false; // imageUrl's pixels are on the image
};
}; // namespace vkpcnx
