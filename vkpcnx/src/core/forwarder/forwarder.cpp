#include "core/forwarder/forwarder.hpp"

#include <cstdio>

#ifdef __SWITCH__
#include "core/forwarder/nca_builder.hpp"
#include "core/http.hpp"
#include "core/settings.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION // screenshot.cpp, the other user, is desktop-only
#include "extern/stb_image_write.h"

#include <borealis.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
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
  const int x0 = (w - side) / 2, y0 = (h - side) / 2;
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

// The cover if it can be fetched, else the app's own icon
bool makeIcon(const std::string &coverUrl, Bytes &jpeg, bool &usedCover) {
  usedCover = false;
  if (!coverUrl.empty()) {
    auto response = Http::get(coverUrl);
    if (response.ok() && encodeIcon(response.body, jpeg)) {
      usedCover = true;
      return true;
    }
  }
  return readFile(BRLS_ASSET("icon/icon.jpg"), jpeg) && !jpeg.empty() && jpeg.size() <= kMaxIconBytes;
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
  return std::string("Не удалось установить ярлык (") + step + ", код " + hex(rc) + ").";
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
  auto say = [&](const char *text) {
    if (progress)
      progress(text);
  };
  if (req.launcherId == 0 || req.gameId == 0)
    return {false, "У игры нет запускаемого лаунчера"};

  const u64 tid = titleIdForGame(req.gameId);

  // --- inputs
  say("Загрузка обложки…");
  Bytes icon;
  bool usedCover = false;
  if (!makeIcon(req.coverUrl, icon, usedCover))
    return {false, "Не удалось подготовить значок"};

  Bytes loaderMain, loaderNpdm;
  if (!readFile(BRLS_ASSET("forwarder/main"), loaderMain) || !readFile(BRLS_ASSET("forwarder/main.npdm"), loaderNpdm) ||
      loaderMain.empty() || loaderNpdm.empty())
    return {false, "В этой сборке нет загрузчика ярлыков (forwarder/main). Соберите приложение заново."};

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
      return {false, "Не удалось записать " + cfgPath};
    std::string body = nro + "\n" + argv + "\n";
    bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok)
      return {false, "Не удалось записать " + cfgPath};
  }

  // --- build the title
  say("Сборка пакета…");
  PackageInput input;
  input.titleId = tid;
  input.exefsMain = std::move(loaderMain);
  input.exefsNpdm = std::move(loaderNpdm);
  input.nacp = buildNacp(req.name.empty() ? "VK Play" : req.name, tid);
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
      return {false, "Не удалось собрать пакет: " + error};
  }

  // --- install
  say("Установка…");
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
  return {true, std::string("Ярлык «") + req.name + "» добавлен на главный экран" +
                  (usedCover ? "." : " (значок приложения: обложку скачать не удалось).")};
}

#endif // __SWITCH__

} // namespace vkpcnx::forwarder
