#pragma once
#include "core/utils/image_loader.hpp"
#include "view/game_card.hpp"
#include <borealis.hpp>

namespace vkpcnx {

// Body of the game details dialog: cover, title, genres/publisher/age and description
class GameDetailsContent : public brls::Box {
public:
  explicit GameDetailsContent(const GameCard::Game &game);
  ~GameDetailsContent() override;

private:
  brls::Image *cover = nullptr;
  vkpcnx::utils::ImageRequestPtr coverRequest;
};

// Opens the details dialog with its "Play" / "Add to home screen" / "Close" buttons
void openGameDetails(const GameCard::Game &game);

} // namespace vkpcnx
