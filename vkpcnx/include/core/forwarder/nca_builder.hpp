#pragma once
// Builds the three NCAs of a minimal forwarder title (program, control, meta)
// from an nx-hbloader based exefs, a NACP and an icon. Pure data transformation:
// no services are touched here (see forwarder.cpp for the installation).
//
// The NCA container layout (PFS0 / IVFC hash trees, FS headers, unsigned and
// unencrypted "none" sections, CNMT) follows the switchbrew NCA/PFS0/RomFS
// format documentation. The construction order and the ncm/ns install sequence
// were taken over from Sphaira's forwarder installer (source/owo.cpp, GPL-3.0,
// https://github.com/ITotalJustice/sphaira) and re-written here; see
// forwarder/NOTICE.md for the licensing consequences. Titles built this way need
// a patched system (sigpatches), like every forwarder.
#ifdef __SWITCH__
#include <switch.h>
#include <string>
#include <vector>

namespace vkpcnx::forwarder {

struct PackageInput {
  u64 titleId = 0;
  std::vector<u8> exefsMain;   // NSO of the loader
  std::vector<u8> exefsNpdm;   // its main.npdm (the program id is patched here)
  std::vector<u8> nacp;        // 0x4000 bytes
  std::vector<u8> iconJpeg;    // 256x256 JPEG
  std::string nroPath;         // kept in the program's romfs for reference
  std::string argv;
  NcmStorageId storageId = NcmStorageId_BuiltInUser;
};

struct Content {
  std::vector<u8> data;
  NcmContentId id{};
  u8 type = 0; // NcmContentType
};

struct ContentStorageRecord {
  NcmContentMetaKey key;
  u8 storageId;
  u8 padding[7];
};
static_assert(sizeof(ContentStorageRecord) == 0x18);

struct ContentMetaData {
  NcmContentMetaHeader header;
  NcmApplicationMetaExtendedHeader extended;
  NcmContentInfo infos[3]; // meta, program, control
};
static_assert(sizeof(ContentMetaData) == 0x60);

struct Package {
  std::vector<Content> contents; // program, control, meta (write order)
  NcmContentMetaKey metaKey{};
  ContentMetaData metaData{};
  ContentStorageRecord record{};
};

// Derives the NCA header key through the spl crypto service. Needs splCryptoInitialize().
Result deriveHeaderKey(u8 outKey[0x20]);

// Fills `out`. On failure returns false and sets `error`.
bool buildPackage(const PackageInput &in, const u8 headerKey[0x20], Package &out, std::string &error);

// The NACP for the forwarder: `name` in every language slot, no saves, no accounts
std::vector<u8> buildNacp(const std::string &name, u64 titleId, const std::string &displayVersion = "1.0.0");

} // namespace vkpcnx::forwarder
#endif
