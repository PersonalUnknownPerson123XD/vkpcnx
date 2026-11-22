#include "core/forwarder/forwarder.hpp"

#include <cstdio>

#ifdef __SWITCH__
#include "core/forwarder/nca_builder.hpp"
#include "core/http.hpp"
#include "core/settings.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION // screenshot.cpp, the other user, is desktop-only
#include "extern/stb_image_write.h"

#include <borealis.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <regex>
#include <switch.h>
#include <vector>

namespace vkpcnx::utils {
std::string getExecPath(); // lifecycle.cpp
}

// stb_image is compiled into borealis' nanovg (C); no extern "C" guard in its header
extern "C" {
unsigned char *stbi_load_from_memory(
  const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file, int desired_channels
);
void stbi_image_free(void *retval_from_stbi_load);
}
#endif

namespace vkpcnx::forwarder {

#ifndef __SWITCH__

bool supported() { return false; }
MmcKind detectMmc() { return MmcKind::Unknown; }
unsigned long long titleIdForGame(int gameId) { return 0x0500000000000000ull | (unsigned(gameId) << 12); }
Outcome install(const Request &, const std::function<void(const std::string &)> &) {
  return {false, "Ярлыки на главном экране работают только на Nintendo Switch"};
}

#else

namespace {

using Bytes = std::vector<u8>;
namespace fs = std::filesystem;

constexpr NcmStorageId kStorage = NcmStorageId_BuiltInUser;
constexpr size_t kMaxIconBytes = 0x20000; // the HOME menu rejects bigger icons
constexpr const char *kDefaultNroPath = "sdmc:/switch/vkpcnx.nro";

std::string hex(Result rc) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%X", static_cast<unsigned>(rc));
  return buf;
}

bool readFile(const std::string &path, Bytes &out) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(size));
  size_t n = size ? std::fread(out.data(), 1, out.size(), f) : 0;
  std::fclose(f);
  return n == out.size();
}

// ---- icon: cover -> centered square -> 256x256 JPEG ----------------------------

void jpegSink(void *ctx, void *data, int size) {
  auto *out = static_cast<Bytes *>(ctx);
  auto *p = static_cast<const u8 *>(data);
  out->insert(out->end(), p, p + size);
}

// Area-averaged resize of the centered square of an RGB image to 256x256
Bytes squareIcon(const unsigned char *rgb, int w, int h) {
  const int side = std::min(w, h);
  // Tall posters (aspect > 1.4: Steam capsules) keep their top, where the logo is
  const bool pinTop = h > w * 14 / 10;
  const int x0 = (w - side) / 2, y0 = pinTop ? 0 : (h - side) / 2;
  constexpr int N = 256;
  Bytes out(N * N * 3);
  for (int y = 0; y < N; y++) {
    const int sy0 = y0 + y * side / N;
    const int sy1 = std::max(sy0 + 1, y0 + (y + 1) * side / N);
    for (int x = 0; x < N; x++) {
      const int sx0 = x0 + x * side / N;
      const int sx1 = std::max(sx0 + 1, x0 + (x + 1) * side / N);
      unsigned sum[3] = {0, 0, 0};
      unsigned count = 0;
      for (int sy = sy0; sy < sy1; sy++)
        for (int sx = sx0; sx < sx1; sx++) {
          const unsigned char *p = rgb + (static_cast<size_t>(sy) * w + sx) * 3;
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          count++;
        }
      u8 *dst = out.data() + (static_cast<size_t>(y) * N + x) * 3;
      for (int c = 0; c < 3; c++)
        dst[c] = static_cast<u8>(sum[c] / count);
    }
  }
  return out;
}

bool encodeIcon(const std::string &imageBytes, Bytes &jpeg) {
  int w = 0, h = 0, ch = 0;
  unsigned char *rgb = stbi_load_from_memory(
    reinterpret_cast<const unsigned char *>(imageBytes.data()), static_cast<int>(imageBytes.size()), &w,
    &h, &ch, 3
  );
  if (!rgb || w < 16 || h < 16) {
    if (rgb)
      stbi_image_free(rgb);
    return false;
  }
  Bytes square = squareIcon(rgb, w, h);
  stbi_image_free(rgb);

  for (int quality : {90, 75, 55}) {
    jpeg.clear();
    if (!stbi_write_jpg_to_func(jpegSink, &jpeg, 256, 256, 3, square.data(), quality))
      return false;
    if (jpeg.size() <= kMaxIconBytes)
      return true;
  }
  return false;
}

// Steam app id of the game: from the catalog's buy link, else a store search by exact name
int findSteamAppId(const Request &req) {
  if (req.steamAppId > 0)
    return req.steamAppId;
  if (req.name.empty())
    return 0;
  auto response = Http::get(
    "https://store.steampowered.com/api/storesearch/?l=english&cc=us&term=" + Http::urlEncode(req.name)
  );
  if (!response.ok())
    return 0;
  auto lower = [](std::string t) {
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
    return t;
  };
  try {
    auto j = nlohmann::json::parse(response.body);
    for (const auto &item : j.value("items", nlohmann::json::array()))
      if (item.value("type", "") == "app" && lower(item.value("name", "")) == lower(req.name))
        return item.value("id", 0);
  } catch (const std::exception &) {
  }
  return 0;
}

// "build 26798866": the public branch's build id from steamcmd.net's mirror of
// the Steam app info. Empty when anything is off; never blocks the install.
std::string steamBuildVersion(int appId) {
  if (appId <= 0)
    return "";
  try {
    auto response = Http::get("https://api.steamcmd.net/v1/info/" + std::to_string(appId));
    if (!response.ok())
      return "";
    auto j = nlohmann::json::parse(response.body);
    if (j.value("status", "") != "success")
      return "";
    const auto &pub = j.at("data").at(std::to_string(appId)).at("depots").at("branches").at("public");
    const std::string build = pub.at("buildid").get<std::string>();
    if (build.empty() || build.size() > 9) // "build " + id must fit the 16-byte field
      return "";
    brls::Logger::info("forwarder: steam build {} for app {}", build, appId);
    return "build " + build;
  } catch (const std::exception &) {
    return "";
  }
}

bool tryIcon(const std::string &url, Bytes &jpeg) {
  if (url.empty())
    return false;
  auto response = Http::get(url);
  if (response.ok() && encodeIcon(response.body, jpeg)) {
    brls::Logger::info("forwarder: icon from {}", url);
    return true;
  }
  brls::Logger::warning("forwarder: icon source failed: {}", url);
  return false;
}

// Icon sources, best first: the Steam library capsule (top-pinned crop), the
// catalog's vertical poster, the banner (centre-cropped), the app's own icon
void makeIcon(const Request &req, int appId, Bytes &jpeg) {
  if (appId > 0) {
    const std::string base = "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/" +
                             std::to_string(appId) + "/library_600x900";
    if (tryIcon(base + "_2x.jpg", jpeg) || tryIcon(base + ".jpg", jpeg))
      return;
  }
  if (tryIcon(req.verticalUrl, jpeg) || tryIcon(req.coverUrl, jpeg))
    return;
  jpeg.clear();
}

// ---- ns application manager: the two commands libnx doesn't wrap ------------------

struct NsAppManager {
  Service srv{};
  bool owned = false;

  Result open() {
    Result rc = nsInitialize();
    if (R_FAILED(rc))
      return rc;
    if (hosversionAtLeast(3, 0, 0)) {
      rc = nsGetApplicationManagerInterface(&srv);
      if (R_FAILED(rc)) {
        nsExit();
        return rc;
      }
      owned = true;
    } else {
      srv = *nsGetServiceSession_ApplicationManagerInterface();
    }
    return 0;
  }
  void close() {
    if (owned)
      serviceClose(&srv);
    nsExit();
  }

  // IApplicationManagerInterface::PushApplicationRecord (16), event type Installed (3)
  Result pushApplicationRecord(u64 tid, const ContentStorageRecord *records, u32 count) {
    const struct {
      u8 lastModifiedEvent;
      u8 padding[7];
      u64 tid;
    } in = {3, {0}, tid};
    return serviceDispatchIn(
      &srv, 16, in, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
      .buffers = {{records, sizeof(*records) * count}}
    );
  }
  // IApplicationManagerInterface::InvalidateApplicationControlCache (404)
  Result invalidateControlCache(u64 tid) { return serviceDispatchIn(&srv, 404, tid); }
};

std::string failure(const char *step, Result rc) {
  return std::string("Ярлык не добавлен: ") + step + " " + hex(rc);
}

// Quotes a path for hbloader's argv parsing when needed
std::string quoteArg(const std::string &s) {
  return s.find(' ') == std::string::npos ? s : "\"" + s + "\"";
}

} // namespace

bool supported() { return true; }

MmcKind detectMmc() {
  if (R_FAILED(splInitialize()))
    return MmcKind::Unknown;
  u64 type = 0;
  // Atmosphère's ExosphereEmummcType: 0 = running from the real NAND, 1/2 = emuMMC partition/file
  Result rc = splGetConfig(static_cast<SplConfigItem>(65007), &type);
  splExit();
  if (R_FAILED(rc))
    return MmcKind::Unknown;
  return type == 0 ? MmcKind::SysMmc : MmcKind::EmuMmc;
}

unsigned long long titleIdForGame(int gameId) {
  const std::string seed = "vkpcnx:game:" + std::to_string(gameId);
  u64 hash[SHA256_HASH_SIZE / sizeof(u64)];
  sha256CalculateHash(hash, seed.data(), seed.size());
  // 0x05... is the range used for homebrew titles; the low 12 bits stay clear (program index space)
  return 0x0500000000000000ull | (hash[0] & 0x00FFFFFFFFFFF000ull);
}

Outcome install(const Request &req, const std::function<void(const std::string &)> &progress) {
  if (req.launcherId == 0 || req.gameId == 0)
    return {false, "У игры нет запускаемого лаунчера"};

  const u64 tid = titleIdForGame(req.gameId);

  // --- inputs
  Bytes icon;
  const int steamAppId = findSteamAppId(req);
  makeIcon(req, steamAppId, icon);
  if (icon.empty() &&
      (!readFile(BRLS_ASSET("icon/icon.jpg"), icon) || icon.empty() || icon.size() > kMaxIconBytes))
    return {false, "Ярлык не добавлен: нет значка"};

  Bytes loaderMain, loaderNpdm;
  if (!readFile(BRLS_ASSET("forwarder/main"), loaderMain) || !readFile(BRLS_ASSET("forwarder/main.npdm"), loaderNpdm) ||
      loaderMain.empty() || loaderNpdm.empty())
    return {false, "Ярлык не добавлен: нет загрузчика"};

  // --- where the loader finds its target (read by the forwarder at start-up)
  std::string nro = utils::getExecPath();
  if (nro.rfind("sdmc:/", 0) != 0 && nro.rfind("/", 0) != 0)
    nro = kDefaultNroPath;
  if (nro.rfind("/", 0) == 0)
    nro = "sdmc:" + nro;
  const std::string argv =
    quoteArg(nro) + " --launcher=" + std::to_string(req.launcherId) + " --game=" + std::to_string(req.gameId) + req.args;
  {
    std::error_code ec;
    fs::create_directories(Settings::configDir() + "/forwarders", ec);
    char name[40];
    std::snprintf(name, sizeof name, "/forwarders/%016llX.cfg", static_cast<unsigned long long>(tid));
    const std::string cfgPath = Settings::configDir() + name;
    FILE *f = std::fopen(cfgPath.c_str(), "wb");
    if (!f)
      return {false, "Ярлык не добавлен: нет записи на SD"};
    std::string body = nro + "\n" + argv + "\n";
    bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok)
      return {false, "Ярлык не добавлен: нет записи на SD"};
  }

  // --- build the title
  PackageInput input;
  input.titleId = tid;
  input.exefsMain = std::move(loaderMain);
  input.exefsNpdm = std::move(loaderNpdm);
  input.nacp = buildNacp(req.name.empty() ? "VK Play" : req.name, tid, steamBuildVersion(steamAppId));
  input.iconJpeg = std::move(icon);
  input.nroPath = nro;
  input.argv = argv;
  input.storageId = kStorage;

  Package pkg;
  {
    Result rc = splCryptoInitialize();
    if (R_FAILED(rc))
      return {false, failure("spl", rc)};
    u8 headerKey[0x20];
    rc = deriveHeaderKey(headerKey);
    splCryptoExit();
    if (R_FAILED(rc))
      return {false, failure("ключ NCA", rc)};

    std::string error;
    if (!buildPackage(input, headerKey, pkg, error))
      return {false, "Ярлык не добавлен: " + error};
  }

  // --- install
  Result rc = ncmInitialize();
  if (R_FAILED(rc))
    return {false, failure("ncm", rc)};
  NsAppManager ns;
  rc = ns.open();
  if (R_FAILED(rc)) {
    ncmExit();
    return {false, failure("ns", rc)};
  }

  auto cleanup = [&] {
    ns.close();
    ncmExit();
  };

  // A previous shortcut for this game (same title id): remove it first
  nsDeleteApplicationCompletely(tid);

  {
    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, kStorage);
    if (R_FAILED(rc)) {
      cleanup();
      return {false, failure("хранилище", rc)};
    }
    for (const auto &c : pkg.contents) {
      NcmPlaceHolderId placeholder;
      rc = ncmContentStorageGeneratePlaceHolderId(&cs, &placeholder);
      if (R_SUCCEEDED(rc)) {
        ncmContentStorageDeletePlaceHolder(&cs, &placeholder);
        rc = ncmContentStorageCreatePlaceHolder(&cs, &c.id, &placeholder, static_cast<s64>(c.data.size()));
      }
      if (R_SUCCEEDED(rc))
        rc = ncmContentStorageWritePlaceHolder(&cs, &placeholder, 0, c.data.data(), c.data.size());
      if (R_SUCCEEDED(rc)) {
        ncmContentStorageDelete(&cs, &c.id); // an older copy of the same content
        rc = ncmContentStorageRegister(&cs, &c.id, &placeholder);
      }
      if (R_FAILED(rc)) {
        ncmContentStorageClose(&cs);
        cleanup();
        return {false, failure("запись NCA", rc)};
      }
    }
    ncmContentStorageClose(&cs);
  }

  {
    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, kStorage);
    if (R_FAILED(rc)) {
      cleanup();
      return {false, failure("база ncm", rc)};
    }
    rc = ncmContentMetaDatabaseSet(&db, &pkg.metaKey, &pkg.metaData, sizeof(pkg.metaData));
    if (R_SUCCEEDED(rc))
      rc = ncmContentMetaDatabaseCommit(&db);
    ncmContentMetaDatabaseClose(&db);
    if (R_FAILED(rc)) {
      cleanup();
      return {false, failure("запись в базу ncm", rc)};
    }
  }

  rc = ns.pushApplicationRecord(tid, &pkg.record, 1);
  if (R_SUCCEEDED(rc))
    ns.invalidateControlCache(tid); // make HOME re-read the title and its icon
  cleanup();
  if (R_FAILED(rc))
    return {false, failure("запись в ns", rc)};

  brls::Logger::info("forwarder: installed {:016X} for game {} (launcher {})", tid, req.gameId, req.launcherId);
  return {true, "Ярлык добавлен"};
}

#endif // __SWITCH__

} // namespace vkpcnx::forwarder
