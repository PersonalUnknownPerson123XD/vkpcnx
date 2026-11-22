#include "core/utils/image_loader.hpp"

#include "core/http.hpp"
#include "core/settings.hpp"
#include "core/utils/thread.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <unordered_map>

// stb_image is compiled into borealis' nanovg (C); its header has no extern "C"
// guard, so declare what's needed by hand.
extern "C" {
unsigned char *stbi_load_from_memory(
  const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file, int desired_channels
);
void stbi_image_free(void *retval_from_stbi_load);
}

namespace fs = std::filesystem;

namespace vkpcnx::utils {

namespace {

constexpr int kWorkers = 3;
constexpr size_t kMemoryCacheBytes = 48u * 1024 * 1024;
constexpr size_t kDiskCacheMaxFiles = 600;
constexpr size_t kDiskCacheKeepFiles = 450;
constexpr size_t kMaxDownloadBytes = 8u * 1024 * 1024;

// ---- in-memory LRU of decoded images ---------------------------------------

class MemoryCache {
public:
  DecodedImagePtr get(const std::string &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end())
      return nullptr;
    order_.splice(order_.begin(), order_, it->second.pos);
    return it->second.image;
  }

  void put(const std::string &key, DecodedImagePtr image) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_.count(key))
      return;
    bytes_ += image->rgba.size();
    order_.push_front(key);
    map_[key] = {image, order_.begin()};
    while (bytes_ > kMemoryCacheBytes && order_.size() > 1) {
      auto last = std::prev(order_.end());
      auto it = map_.find(*last);
      bytes_ -= it->second.image->rgba.size();
      map_.erase(it);
      order_.erase(last);
    }
  }

private:
  struct Entry {
    DecodedImagePtr image;
    std::list<std::string>::iterator pos;
  };
  std::mutex mutex_;
  std::list<std::string> order_;
  std::unordered_map<std::string, Entry> map_;
  size_t bytes_ = 0;
};

MemoryCache &memoryCache() {
  static MemoryCache *cache = new MemoryCache(); // never destroyed: workers may outlive main()
  return *cache;
}

// ---- disk cache of the original downloads ----------------------------------

std::string fnv1a(const std::string &s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

const std::string &cacheDir() {
  static const std::string dir = [] {
    std::string d = Settings::configDir() + "/cache";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
  }();
  return dir;
}

void pruneDiskCache() {
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  for (fs::directory_iterator it(cacheDir(), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    auto t = it->last_write_time(e2);
    if (!e2 && it->path().extension() == ".img")
      files.emplace_back(t, it->path());
  }
  if (files.size() <= kDiskCacheMaxFiles)
    return;
  std::sort(files.begin(), files.end());
  for (size_t i = 0; i + kDiskCacheKeepFiles < files.size(); i++) {
    std::error_code e3;
    fs::remove(files[i].second, e3);
  }
}

std::string diskPath(const std::string &url) { return cacheDir() + "/" + fnv1a(url) + ".img"; }

bool readDisk(const std::string &url, std::string &out) {
  std::ifstream in(diskPath(url), std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty();
}

void writeDisk(const std::string &url, const std::string &data) {
  std::string path = diskPath(url);
  std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out)
      return;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec)
    fs::remove(tmp, ec);
}

void removeDisk(const std::string &url) {
  std::error_code ec;
  fs::remove(diskPath(url), ec);
}

// ---- decode + downscale ------------------------------------------------------

// Area-average downscale so the result covers maxW x maxH; never upscales.
DecodedImagePtr decodeScaled(const std::string &bytes, int maxW, int maxH) {
  if (bytes.empty() || bytes.size() > static_cast<size_t>(0x7fffffff))
    return nullptr;
  int w = 0, h = 0, comp = 0;
  unsigned char *pixels = stbi_load_from_memory(
    reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()), &w, &h,
    &comp, 4
  );
  if (!pixels)
    return nullptr;
  struct Free {
    unsigned char *p;
    ~Free() { stbi_image_free(p); }
  } guard{pixels};
  if (w <= 0 || h <= 0)
    return nullptr;

  double scale = std::max(maxW / static_cast<double>(w), maxH / static_cast<double>(h));
  auto out = std::make_shared<DecodedImage>();
  if (scale >= 1.0) {
    out->width = w;
    out->height = h;
    out->rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    return out;
  }

  int nw = std::max(1, static_cast<int>(w * scale + 0.5));
  int nh = std::max(1, static_cast<int>(h * scale + 0.5));
  out->width = nw;
  out->height = nh;
  out->rgba.resize(static_cast<size_t>(nw) * nh * 4);
  for (int dy = 0; dy < nh; dy++) {
    int sy0 = static_cast<int>(static_cast<int64_t>(dy) * h / nh);
    int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<int64_t>(dy + 1) * h / nh));
    sy1 = std::min(sy1, h);
    for (int dx = 0; dx < nw; dx++) {
      int sx0 = static_cast<int>(static_cast<int64_t>(dx) * w / nw);
      int sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<int64_t>(dx + 1) * w / nw));
      sx1 = std::min(sx1, w);
      unsigned sum[4] = {0, 0, 0, 0};
      for (int sy = sy0; sy < sy1; sy++) {
        const unsigned char *row = pixels + (static_cast<size_t>(sy) * w + sx0) * 4;
        for (int sx = sx0; sx < sx1; sx++, row += 4) {
          sum[0] += row[0];
          sum[1] += row[1];
          sum[2] += row[2];
          sum[3] += row[3];
        }
      }
      unsigned n = static_cast<unsigned>((sy1 - sy0) * (sx1 - sx0));
      unsigned char *dst = &out->rgba[(static_cast<size_t>(dy) * nw + dx) * 4];
      for (int c = 0; c < 4; c++)
        dst[c] = static_cast<unsigned char>((sum[c] + n / 2) / n);
    }
  }
  return out;
}

// ---- worker pool ---------------------------------------------------------------

struct Job {
  std::string url;
  int maxW, maxH;
  ImageRequestPtr request;
  std::function<void(DecodedImagePtr)> onReady;
};

std::string cacheKey(const std::string &url, int w, int h) {
  return url + "@" + std::to_string(w) + "x" + std::to_string(h);
}

class Pool {
public:
  void submit(Job job) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
      started_ = true;
      for (int i = 0; i < kWorkers; i++)
        runDetached([this] { workerLoop(); });
    }
    queue_.push_back(std::move(job));
    cv_.notify_one();
  }

private:
  bool next(Job &out) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      // Newest first; anything cancelled while it waited is dropped here
      while (!queue_.empty()) {
        Job job = std::move(queue_.back());
        queue_.pop_back();
        if (!job.request->isCancelled()) {
          out = std::move(job);
          return true;
        }
      }
      cv_.wait(lock);
    }
  }

  void workerLoop() {
    static std::once_flag pruned;
    std::call_once(pruned, [] {
      try {
        pruneDiskCache();
      } catch (...) {
      }
    });
    Job job;
    while (next(job)) {
      try {
        process(job);
      } catch (const std::exception &e) {
        brls::Logger::warning("ImageLoader: {}: {}", job.url, e.what());
      } catch (...) {
      }
      job = Job{};
    }
  }

  void process(Job &job) {
    const std::string key = cacheKey(job.url, job.maxW, job.maxH);
    DecodedImagePtr image = memoryCache().get(key);

    if (!image) {
      std::string bytes;
      bool fromDisk = readDisk(job.url, bytes);
      if (fromDisk) {
        image = decodeScaled(bytes, job.maxW, job.maxH);
        if (!image) {
          removeDisk(job.url); // corrupt entry
          fromDisk = false;
        }
      }
      if (!image) {
        if (job.request->isCancelled())
          return;
        Http::Response res = Http::get(job.url);
        if (!res.ok() || res.body.empty() || res.body.size() > kMaxDownloadBytes) {
          brls::Logger::debug("ImageLoader: {} -> {} {}", job.url, res.status, res.error);
          return;
        }
        // Keep the download even when nobody wants it any more (scrolled away)
        image = decodeScaled(res.body, job.maxW, job.maxH);
        if (!image) {
          brls::Logger::warning("ImageLoader: cannot decode {}", job.url);
          return;
        }
        writeDisk(job.url, res.body);
      }
      memoryCache().put(key, image);
    }

    if (job.request->isCancelled())
      return;
    brls::sync([request = job.request, onReady = std::move(job.onReady), image] {
      if (!request->isCancelled() && onReady)
        onReady(image);
    });
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool started_ = false;
};

Pool &pool() {
  static Pool *p = new Pool(); // never destroyed: workers may outlive main()
  return *p;
}

} // namespace

ImageRequestPtr loadScaledImage(
  const std::string &url, int maxWidth, int maxHeight, std::function<void(DecodedImagePtr)> onReady
) {
  auto request = std::make_shared<ImageRequest>();
  if (url.empty())
    return request;

  if (DecodedImagePtr hit = memoryCache().get(cacheKey(url, maxWidth, maxHeight))) {
    if (onReady)
      onReady(hit);
    return request;
  }

  pool().submit(Job{url, maxWidth, maxHeight, request, std::move(onReady)});
  return request;
}

void showDecodedImage(brls::Image *image, const DecodedImage &decoded) {
  if (!image || decoded.width <= 0 || decoded.height <= 0)
    return;
  NVGcontext *vg = brls::Application::getNVGContext();
  int tex = nvgCreateImageRGBA(vg, decoded.width, decoded.height, 0, decoded.rgba.data());
  image->innerSetImage(tex);
}

} // namespace vkpcnx::utils
