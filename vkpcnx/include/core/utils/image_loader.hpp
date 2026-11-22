#pragma once
#include <atomic>
#include <borealis.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vkpcnx::utils {

// A decoded, already downscaled RGBA8 image, ready to be uploaded as a texture.
struct DecodedImage {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;
};
using DecodedImagePtr = std::shared_ptr<const DecodedImage>;

// Handle of a pending load. cancel() is cheap and may be called from the UI
// thread at any time: a cancelled load that hasn't started is dropped, and the
// completion callback of one that has is never invoked.
class ImageRequest {
public:
  void cancel() { cancelled.store(true); }
  bool isCancelled() const { return cancelled.load(); }

private:
  std::atomic<bool> cancelled{false};
};
using ImageRequestPtr = std::shared_ptr<ImageRequest>;

// Downloads (or reads from the memory/disk cache), decodes and downscales the
// image at `url` on a small worker pool so it covers maxWidth x maxHeight
// (never upscaled). `onReady` runs on the UI thread, only if the request hasn't
// been cancelled; it is called synchronously when the image is already in the
// memory cache. The latest request is served first, so what is on screen wins
// over what was scrolled past.
ImageRequestPtr loadScaledImage(
  const std::string &url, int maxWidth, int maxHeight, std::function<void(DecodedImagePtr)> onReady
);

// UI thread only: uploads the pixels as a new texture and shows them. The
// image owns (and frees) its texture as usual.
void showDecodedImage(brls::Image *image, const DecodedImage &decoded);

} // namespace vkpcnx::utils
