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
    std::string pictureVerticalUrl; // picture_vertical, for the HOME shortcut icon
    int steamAppId = 0;             // from the Steam buy link, when the catalog has one
    int launcherId = 0; // game_launchers[].id / current_launcher, used for queue/run
    bool gamepadSupport = false;
    // Shown in the details dialog (all of it comes with the catalog reply)
    std::string description; // long_descr, else short_descr
    std::string genres;
    std::string publishers;
    int ageLimit = 0;
  };
  void setGame(Game game);
  // Drops the pending cover load (the card is being recycled / scrolled away)
  void cancelImageLoad();
  // + on the card: cover, description and the "Play" / "Add to home screen" buttons
  void openDetails();

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
