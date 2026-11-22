#include "core/forwarder/nca_builder.hpp"

#ifdef __SWITCH__
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vkpcnx::forwarder {

namespace {

using Bytes = std::vector<u8>;

constexpr u32 kIvfcBlock = 0x4000;
constexpr u32 kPfs0ExefsBlock = 0x10000;
constexpr u32 kPfs0MetaBlock = 0x1000;
constexpr u32 kPfs0Padding = 0x200;
constexpr u32 kRomfsEmpty = 0xFFFFFFFF;
constexpr u32 kRomfsDataOffset = 0x200;
constexpr u32 kNca3Magic = 0x3341434E;

// Key sources for the NCA header key (the same on every console)
constexpr u8 kHeaderKekSrc[0x10] = {0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D,
                                    0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A};
constexpr u8 kHeaderKeySrc[0x20] = {0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2,
                                    0x5D, 0x19, 0x7B, 0xF5, 0xD0, 0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28,
                                    0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2};

// ---- growable output buffer ------------------------------------------------

struct Buf {
  Bytes bytes;
  u64 pos = 0;
  void write(const void *data, u64 size) {
    if (pos + size > bytes.size())
      bytes.resize(pos + size);
    if (size)
      std::memcpy(bytes.data() + pos, data, size);
    pos += size;
  }
  void write(const Bytes &b) { write(b.data(), b.size()); }
  void seek(u64 p) { pos = p; }
  u64 tell() const { return pos; }
};

// Pads to the next multiple of `block` (always adds something when already aligned,
// matching what the reference installer writes)
u64 writePadding(Buf &buf, u64 off, u64 block) {
  const u64 size = block - (off % block);
  Bytes zero(size);
  buf.write(zero);
  return size;
}

u32 alignU32(u32 v, u32 a) { return (v + (a - 1)) & ~(a - 1); }
u64 alignU64(u64 v, u64 a) { return (v + (a - 1)) & ~(a - 1); }

struct FileEntry {
  std::string name; // "/name" in romfs, "name" in pfs0
  Bytes data;
};
using FileEntries = std::vector<FileEntry>;

void addFile(FileEntries &entries, const std::string &name, const void *data, u64 size) {
  FileEntry e;
  e.name = name;
  e.data.resize(size);
  if (size)
    std::memcpy(e.data.data(), data, size);
  entries.push_back(std::move(e));
}
void addFile(FileEntries &entries, const std::string &name, const Bytes &data) {
  addFile(entries, name, data.data(), data.size());
}

// ---- NCA structures ----------------------------------------------------------

#pragma pack(push, 1)
struct SectionTableEntry {
  u32 mediaStart; // in 0x200 units
  u32 mediaEnd;
  u8 unk8[4];
  u8 unkC[4];
};
struct LayerRegion {
  u64 offset;
  u64 size;
};
struct HierarchicalSha256Data {
  u8 masterHash[0x20];
  u32 blockSize;
  u32 layerCount;
  LayerRegion hashLayer;
  LayerRegion pfs0Layer;
  LayerRegion unused[3];
  u8 pad[0x80];
};
struct IvfcLevel {
  u64 logicalOffset;
  u64 hashDataSize;
  u32 blockSizeLog2;
  u32 reserved;
};
struct IvfcInfoLevelHash {
  u32 maxLayers;
  IvfcLevel levels[6];
  u8 signatureSalt[0x20];
};
struct IntegrityMetaInfo {
  u32 magic; // "IVFC"
  u32 version;
  u32 masterHashSize;
  IvfcInfoLevelHash infoLevelHash;
  u8 masterHash[0x20];
  u8 pad[0x18];
};
struct FsHeader {
  u16 version;
  u8 fsType;
  u8 hashType;
  u8 encryptionType;
  u8 metadataHashType;
  u8 pad6[2];
  union {
    HierarchicalSha256Data sha256;
    IntegrityMetaInfo integrity;
  } hash;
  u8 patchInfo[0x40];
  u64 sectionCtr;
  u8 sparseInfo[0x30];
  u8 compressionInfo[0x28];
  u8 metaDataHashDataInfo[0x30];
  u8 reserved[0x30];
};
struct NcaHeader {
  u8 rsaFixedKey[0x100];
  u8 rsaNpdm[0x100];
  u32 magic;
  u8 distributionType;
  u8 contentType;
  u8 oldKeyGen;
  u8 kaekIndex;
  u64 size;
  u64 programId;
  u32 contextId;
  u32 sdkVersion;
  u8 keyGen;
  u8 sigKeyGen;
  u8 pad222[0xE];
  u8 rightsId[0x10];
  SectionTableEntry fsTable[4];
  u8 fsHeaderHash[4][0x20];
  u8 keyArea[4][0x10];
  u8 pad340[0xC0];
  FsHeader fsHeader[4];
};
struct Pfs0Header {
  u32 magic;
  u32 totalFiles;
  u32 stringTableSize;
  u32 padding;
};
struct Pfs0FileEntry {
  u64 dataOffset;
  u64 dataSize;
  u32 nameOffset;
  u32 padding;
};
struct CnmtHeader {
  u64 titleId;
  u32 titleVersion;
  u8 metaType;
  u8 pad0D;
  NcmContentMetaHeader metaHeader;
  u8 installType;
  u8 pad17;
  u32 requiredSysVersion;
  u8 pad1C[4];
};
struct RomfsHeader {
  u64 headerSize;
  u64 dirHashTableOff;
  u64 dirHashTableSize;
  u64 dirTableOff;
  u64 dirTableSize;
  u64 fileHashTableOff;
  u64 fileHashTableSize;
  u64 fileTableOff;
  u64 fileTableSize;
  u64 fileDataOff;
};
struct RomfsDir {
  u32 parent;
  u32 sibling;
  u32 childDir;
  u32 childFile;
  u32 nextHash;
  u32 nameLen;
};
struct RomfsFile {
  u32 parent;
  u32 sibling;
  u64 dataOff;
  u64 dataSize;
  u32 nextHash;
  u32 nameLen;
};
#pragma pack(pop)

static_assert(sizeof(HierarchicalSha256Data) == 0xF8);
static_assert(sizeof(IntegrityMetaInfo) == 0xF8);
static_assert(sizeof(FsHeader) == 0x200);
static_assert(sizeof(NcaHeader) == 0xC00);
static_assert(sizeof(CnmtHeader) == 0x20);
static_assert(sizeof(RomfsHeader) == 0x50);
static_assert(sizeof(RomfsDir) == 0x18);
static_assert(sizeof(RomfsFile) == 0x20);

enum : u8 { ContentTypeProgram = 0, ContentTypeMeta = 1, ContentTypeControl = 2 };
enum : u8 { FsTypeRomFs = 0, FsTypePfs0 = 1 };
enum : u8 { HashTypeHierarchicalSha256 = 2, HashTypeHierarchicalIntegrity = 3 };
enum : u8 { EncryptionNone = 1 };

// ---- RomFS (flat: every file in the root directory) ---------------------------

u32 romfsPathHash(u32 parent, const u8 *path, u32 len) {
  u32 hash = parent ^ 123456789;
  for (u32 i = 0; i < len; i++) {
    hash = (hash >> 5) | (hash << 27);
    hash ^= path[i];
  }
  return hash;
}

u32 romfsHashTableCount(u32 entries) {
  if (entries < 3)
    return 3;
  if (entries < 19)
    return entries | 1;
  u32 count = entries;
  while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 ||
         count % 13 == 0 || count % 17 == 0)
    count++;
  return count;
}

// `entries` names start with '/'
Bytes buildRomfs(const FileEntries &entries) {
  const u32 numFiles = static_cast<u32>(entries.size());
  const u32 dirHashCount = romfsHashTableCount(1);
  const u32 fileHashCount = romfsHashTableCount(numFiles);

  std::vector<u32> entryOffset(numFiles);
  std::vector<u64> dataOffset(numFiles);
  u32 fileTableSize = 0;
  u64 partition = 0;
  for (u32 i = 0; i < numFiles; i++) {
    const u32 nameLen = static_cast<u32>(entries[i].name.size()) - 1; // without '/'
    entryOffset[i] = fileTableSize;
    fileTableSize += sizeof(RomfsFile) + alignU32(nameLen, 4);
    partition = alignU64(partition, 0x10);
    dataOffset[i] = partition;
    partition += entries[i].data.size();
  }

  std::vector<u32> dirHash(dirHashCount, kRomfsEmpty);
  std::vector<u32> fileHash(fileHashCount, kRomfsEmpty);
  Bytes fileTable(fileTableSize);
  RomfsDir root{};
  root.parent = 0;
  root.sibling = kRomfsEmpty;
  root.childDir = kRomfsEmpty;
  root.childFile = numFiles ? entryOffset[0] : kRomfsEmpty;
  root.nameLen = 0;
  dirHash[romfsPathHash(0, nullptr, 0) % dirHashCount] = 0;
  root.nextHash = kRomfsEmpty;

  for (u32 i = 0; i < numFiles; i++) {
    const auto &e = entries[i];
    const u32 nameLen = static_cast<u32>(e.name.size()) - 1;
    RomfsFile f{};
    f.parent = 0;
    f.sibling = i + 1 < numFiles ? entryOffset[i + 1] : kRomfsEmpty;
    f.dataOff = dataOffset[i];
    f.dataSize = e.data.size();
    const u32 h = romfsPathHash(0, reinterpret_cast<const u8 *>(e.name.c_str()) + 1, nameLen);
    f.nextHash = fileHash[h % fileHashCount];
    fileHash[h % fileHashCount] = entryOffset[i];
    f.nameLen = nameLen;
    std::memcpy(fileTable.data() + entryOffset[i], &f, sizeof(f));
    std::memcpy(fileTable.data() + entryOffset[i] + sizeof(f), e.name.c_str() + 1, nameLen);
  }

  RomfsHeader header{};
  header.headerSize = sizeof(header);
  header.fileDataOff = kRomfsDataOffset;
  header.dirHashTableOff = alignU64(partition + kRomfsDataOffset, 4);
  header.dirHashTableSize = 4ull * dirHashCount;
  header.dirTableOff = header.dirHashTableOff + header.dirHashTableSize;
  header.dirTableSize = sizeof(RomfsDir);
  header.fileHashTableOff = header.dirTableOff + header.dirTableSize;
  header.fileHashTableSize = 4ull * fileHashCount;
  header.fileTableOff = header.fileHashTableOff + header.fileHashTableSize;
  header.fileTableSize = fileTableSize;

  Buf buf;
  buf.write(&header, sizeof(header));
  for (u32 i = 0; i < numFiles; i++) {
    buf.seek(dataOffset[i] + kRomfsDataOffset);
    buf.write(entries[i].data);
  }
  buf.seek(header.dirHashTableOff);
  buf.write(dirHash.data(), header.dirHashTableSize);
  buf.write(&root, sizeof(root));
  buf.write(fileHash.data(), header.fileHashTableSize);
  buf.write(fileTable);
  return buf.bytes;
}

// ---- PFS0 -------------------------------------------------------------------

Bytes buildPfs0(const FileEntries &entries) {
  Pfs0Header header{};
  std::vector<Pfs0FileEntry> table(entries.size());
  std::vector<char> strings;
  u64 stringOffset = 0, dataOffset = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    table[i].dataOffset = dataOffset;
    table[i].dataSize = entries[i].data.size();
    table[i].nameOffset = static_cast<u32>(stringOffset);
    table[i].padding = 0;
    strings.resize(stringOffset + entries[i].name.size() + 1);
    std::memcpy(strings.data() + stringOffset, entries[i].name.c_str(), entries[i].name.size() + 1);
    dataOffset += entries[i].data.size();
    stringOffset += entries[i].name.size() + 1;
  }
  strings.resize((strings.size() + 0x1F) & ~size_t(0x1F));

  header.magic = 0x30534650; // "PFS0"
  header.totalFiles = static_cast<u32>(entries.size());
  header.stringTableSize = static_cast<u32>(strings.size());

  Buf buf;
  buf.write(&header, sizeof(header));
  buf.write(table.data(), sizeof(Pfs0FileEntry) * table.size());
  buf.write(strings.data(), strings.size());
  for (const auto &e : entries)
    buf.write(e.data);
  return buf.bytes;
}

Bytes sha256Blocks(const Bytes &data, u32 blockSize) {
  Buf out;
  u8 hash[SHA256_HASH_SIZE];
  for (size_t i = 0; i < data.size(); i += blockSize) {
    const size_t n = std::min<size_t>(blockSize, data.size() - i);
    sha256CalculateHash(hash, data.data() + i, n);
    out.write(hash, sizeof(hash));
  }
  return out.bytes;
}

Bytes sha256Of(const Bytes &data) {
  Bytes h(SHA256_HASH_SIZE);
  sha256CalculateHash(h.data(), data.data(), data.size());
  return h;
}

// ---- NCA sections --------------------------------------------------------------

void setSection(NcaHeader &h, u8 index, u64 start, u64 end) {
  auto &s = h.fsTable[index];
  s.mediaStart = static_cast<u32>(start / 0x200);
  s.mediaEnd = static_cast<u32>(end / 0x200);
  s.unk8[0] = 1;
}

void writePfs0Section(NcaHeader &h, u8 index, const FileEntries &entries, u32 blockSize, Buf &buf) {
  const Bytes pfs0 = buildPfs0(entries);
  const Bytes hashTable = sha256Blocks(pfs0, blockSize);
  const Bytes masterHash = sha256Of(hashTable);

  buf.write(hashTable);
  const u64 padding = writePadding(buf, hashTable.size(), kPfs0Padding);

  auto &fs = h.fsHeader[index];
  fs.hash.sha256.pfs0Layer.offset = hashTable.size() + padding;
  fs.hash.sha256.pfs0Layer.size = pfs0.size();
  buf.write(pfs0);
  writePadding(buf, buf.tell(), 0x200);

  const u64 start = index == 0 ? sizeof(NcaHeader) : u64(h.fsTable[index - 1].mediaEnd) * 0x200;
  setSection(h, index, start, buf.tell());

  fs.hashType = HashTypeHierarchicalSha256;
  fs.fsType = FsTypePfs0;
  fs.version = 2;
  fs.hash.sha256.layerCount = 2;
  fs.hash.sha256.blockSize = blockSize;
  fs.encryptionType = EncryptionNone;
  fs.hash.sha256.hashLayer.size = hashTable.size();
  std::memcpy(fs.hash.sha256.masterHash, masterHash.data(), masterHash.size());
  sha256CalculateHash(h.fsHeaderHash[index], &fs, sizeof(fs));
}

Bytes ivfcLevel(const Bytes &src) {
  Buf out;
  u8 hash[SHA256_HASH_SIZE];
  for (size_t i = 0; i < src.size(); i += kIvfcBlock) {
    const size_t n = std::min<size_t>(kIvfcBlock, src.size() - i);
    sha256CalculateHash(hash, src.data() + i, n);
    out.write(hash, sizeof(hash));
  }
  writePadding(out, out.tell(), kIvfcBlock);
  return out.bytes;
}

void writeRomfsSection(NcaHeader &h, u8 index, const FileEntries &entries, Buf &buf) {
  auto &fs = h.fsHeader[index];
  auto &meta = fs.hash.integrity;
  auto &levels = meta.infoLevelHash;

  Bytes ivfc[6];
  {
    Bytes romfs = buildRomfs(entries);
    levels.levels[5].hashDataSize = romfs.size();
    Buf padded;
    padded.write(romfs);
    padded.seek(padded.bytes.size());
    writePadding(padded, padded.tell(), kIvfcBlock);
    ivfc[5] = std::move(padded.bytes);
  }
  for (int b = 4; b >= 0; b--) {
    ivfc[b] = ivfcLevel(ivfc[b + 1]);
    levels.levels[b].hashDataSize = ivfc[b].size();
    levels.levels[b].blockSizeLog2 = 0x0E; // 0x4000
  }
  levels.levels[5].blockSizeLog2 = 0x0E;
  levels.levels[0].logicalOffset = 0;
  for (int i = 1; i <= 5; i++)
    levels.levels[i].logicalOffset = levels.levels[i - 1].logicalOffset + levels.levels[i - 1].hashDataSize;

  for (const auto &level : ivfc)
    buf.write(level);
  writePadding(buf, buf.tell(), 0x200);

  const Bytes master = sha256Of(ivfc[0]);
  std::memcpy(meta.masterHash, master.data(), sizeof(meta.masterHash));

  const u64 start = index == 0 ? sizeof(NcaHeader) : u64(h.fsTable[index - 1].mediaEnd) * 0x200;
  setSection(h, index, start, buf.tell());

  fs.hashType = HashTypeHierarchicalIntegrity;
  fs.fsType = FsTypeRomFs;
  fs.version = 2;
  meta.magic = 0x43465649; // "IVFC"
  meta.version = 0x20000;
  meta.masterHashSize = SHA256_HASH_SIZE;
  levels.maxLayers = 7;
  fs.encryptionType = EncryptionNone;
  sha256CalculateHash(h.fsHeaderHash[index], &fs, sizeof(fs));
}

void encryptHeader(NcaHeader *h, const u8 key[0x20]) {
  Aes128XtsContext ctx{};
  aes128XtsContextCreate(&ctx, key, key + 0x10, true);
  u64 sector = 0;
  for (u64 pos = 0; pos < 0xC00; pos += 0x200) {
    aes128XtsContextResetSector(&ctx, sector++, true);
    aes128XtsEncrypt(&ctx, reinterpret_cast<u8 *>(h) + pos, reinterpret_cast<const u8 *>(h) + pos, 0x200);
  }
}

// Finishes the NCA: header fields, encryption, written at offset 0
void finishNca(NcaHeader &h, u64 tid, u8 contentType, const u8 headerKey[0x20], Buf &buf) {
  h.magic = kNca3Magic;
  h.distributionType = 0; // system
  h.contentType = contentType;
  h.programId = tid;
  h.sdkVersion = 0x000C1100;
  h.size = buf.tell();
  encryptHeader(&h, headerKey);
  buf.seek(0);
  buf.write(&h, sizeof(h));
}

Content makeContent(Buf &buf, u8 type) {
  Content c;
  c.data = std::move(buf.bytes);
  u8 hash[SHA256_HASH_SIZE];
  sha256CalculateHash(hash, c.data.data(), c.data.size());
  std::memcpy(&c.id, hash, sizeof(c.id));
  c.type = type;
  return c;
}

// ---- NPDM patch -----------------------------------------------------------------

bool patchNpdm(Bytes &npdm, u64 tid, std::string &error) {
  // META (0x80 bytes): aci0_offset @0x70, acid_offset @0x78; title name @0x20
  if (npdm.size() < 0x80 || std::memcmp(npdm.data(), "META", 4) != 0) {
    error = "main.npdm: bad META";
    return false;
  }
  u32 aci0Off, acidOff;
  std::memcpy(&aci0Off, npdm.data() + 0x70, 4);
  std::memcpy(&acidOff, npdm.data() + 0x78, 4);
  if (aci0Off + 0x20 > npdm.size() || acidOff + 0x220 > npdm.size() ||
      std::memcmp(npdm.data() + aci0Off, "ACI0", 4) != 0 ||
      std::memcmp(npdm.data() + acidOff + 0x200, "ACID", 4) != 0) {
    error = "main.npdm: bad ACI0/ACID";
    return false;
  }
  char title[0x10] = "Application";
  std::memcpy(npdm.data() + 0x20, title, sizeof(title));
  std::memcpy(npdm.data() + aci0Off + 0x10, &tid, 8);      // ACI0.program_id
  std::memcpy(npdm.data() + acidOff + 0x210, &tid, 8);     // ACID.program_id_min
  std::memcpy(npdm.data() + acidOff + 0x218, &tid, 8);     // ACID.program_id_max
  return true;
}

} // namespace

Result deriveHeaderKey(u8 outKey[0x20]) {
  u8 kek[0x20];
  Result rc = splCryptoGenerateAesKek(kHeaderKekSrc, 0, 0, kek);
  if (R_FAILED(rc))
    return rc;
  rc = splCryptoGenerateAesKey(kek, kHeaderKeySrc, outKey);
  if (R_FAILED(rc))
    return rc;
  return splCryptoGenerateAesKey(kek, kHeaderKeySrc + 0x10, outKey + 0x10);
}

std::vector<u8> buildNacp(const std::string &name, u64 titleId) {
  Bytes raw(sizeof(NacpStruct), 0);
  static_assert(sizeof(NacpStruct) == 0x4000);

  // 16 language slots: name (0x200) + publisher (0x100)
  for (int i = 0; i < 16; i++) {
    u8 *slot = raw.data() + i * 0x300;
    std::strncpy(reinterpret_cast<char *>(slot), name.c_str(), 0x1FF);
    std::strncpy(reinterpret_cast<char *>(slot + 0x200), "vkpcnx", 0xFF);
  }

  auto *nacp = reinterpret_cast<NacpStruct *>(raw.data());
  nacp->startup_user_account = 0;       // start without picking a profile
  nacp->user_account_switch_lock = 0;
  nacp->add_on_content_registration_type = 1;
  nacp->supported_language_flag = 1;    // American English slot
  nacp->screenshot = 0;                 // allowed
  nacp->video_capture = 2;              // automatic
  nacp->data_loss_confirmation = 0;
  nacp->play_log_policy = 0;
  nacp->presence_group_id = titleId;
  std::snprintf(nacp->display_version, sizeof(nacp->display_version), "1.0.0");
  nacp->add_on_content_base_id = titleId ^ 0x1000;
  nacp->save_data_owner_id = titleId;
  nacp->logo_type = 2;                  // "Nintendo" logo type (no logo section is shipped)
  nacp->logo_handling = 0;
  nacp->pseudo_device_id_seed = titleId;
  nacp->required_network_service_license_on_launch = 0;
  nacp->application_error_code_category = 0;
  std::memcpy(&nacp->application_error_code_category, "vkpcnx", 6);
  for (auto &id : nacp->local_communication_id)
    id = titleId;
  nacp->play_log_query_capability = 0;
  return raw;
}

bool buildPackage(const PackageInput &in, const u8 headerKey[0x20], Package &out, std::string &error) {
  if (in.exefsMain.empty() || in.exefsNpdm.empty() || in.nacp.size() != 0x4000 || in.iconJpeg.empty()) {
    error = "bad forwarder input";
    return false;
  }
  const u64 tid = in.titleId;
  out = Package{};

  // -- program NCA: exefs (loader) + a romfs with the target for reference
  {
    Bytes npdm = in.exefsNpdm;
    if (!patchNpdm(npdm, tid, error))
      return false;

    FileEntries exefs;
    addFile(exefs, "main", in.exefsMain);
    addFile(exefs, "main.npdm", npdm);

    FileEntries romfs;
    addFile(romfs, "/nextArgv", in.argv.data(), in.argv.size());
    addFile(romfs, "/nextNroPath", in.nroPath.data(), in.nroPath.size());

    NcaHeader header{};
    Buf buf;
    buf.write(&header, sizeof(header));
    writePfs0Section(header, 0, exefs, kPfs0ExefsBlock, buf);
    writeRomfsSection(header, 1, romfs, buf);
    finishNca(header, tid, ContentTypeProgram, headerKey, buf);
    out.contents.push_back(makeContent(buf, NcmContentType_Program));
  }

  // -- control NCA: NACP + icon
  {
    FileEntries romfs;
    addFile(romfs, "/control.nacp", in.nacp);
    addFile(romfs, "/icon_AmericanEnglish.dat", in.iconJpeg);

    NcaHeader header{};
    Buf buf;
    buf.write(&header, sizeof(header));
    writeRomfsSection(header, 0, romfs, buf);
    finishNca(header, tid, ContentTypeControl, headerKey, buf);
    out.contents.push_back(makeContent(buf, NcmContentType_Control));
  }

  // -- meta NCA: the CNMT describing program + control
  {
    CnmtHeader cnmt{};
    NcmApplicationMetaExtendedHeader ext{};
    NcmPackagedContentInfo packed[2]{};
    u8 digest[0x20]{};

    cnmt.titleId = tid;
    cnmt.titleVersion = 0;
    cnmt.metaType = NcmContentMetaType_Application;
    cnmt.metaHeader.extended_header_size = sizeof(ext);
    cnmt.metaHeader.content_count = 2; // program + control
    cnmt.metaHeader.content_meta_count = 1;
    cnmt.metaHeader.attributes = 0;
    cnmt.metaHeader.storage_id = in.storageId;
    ext.patch_id = tid | 0x800;

    for (int i = 0; i < 2; i++) {
      const Content &c = out.contents[i];
      u8 hash[SHA256_HASH_SIZE];
      sha256CalculateHash(hash, c.data.data(), c.data.size());
      std::memcpy(packed[i].hash, hash, sizeof(packed[i].hash));
      packed[i].info.content_id = c.id;
      packed[i].info.content_type = c.type;
      ncmU64ToContentInfoSize(c.data.size(), &packed[i].info);
    }

    Buf cnmtBuf;
    cnmtBuf.write(&cnmt, sizeof(cnmt));
    cnmtBuf.write(&ext, sizeof(ext));
    cnmtBuf.write(packed, sizeof(packed));
    cnmtBuf.write(digest, sizeof(digest));

    FileEntries cnmtFiles;
    char cnmtName[40];
    std::snprintf(cnmtName, sizeof(cnmtName), "Application_%016llX.cnmt", static_cast<unsigned long long>(tid));
    addFile(cnmtFiles, cnmtName, cnmtBuf.bytes);

    NcaHeader header{};
    Buf buf;
    buf.write(&header, sizeof(header));
    writePfs0Section(header, 0, cnmtFiles, kPfs0MetaBlock, buf);
    finishNca(header, tid, ContentTypeMeta, headerKey, buf);
    out.contents.push_back(makeContent(buf, NcmContentType_Meta));

    // database entries
    NcmContentMetaHeader metaHeader = cnmt.metaHeader;
    metaHeader.content_count++; // + the meta NCA itself
    metaHeader.storage_id = 0;

    out.metaKey.id = tid;
    out.metaKey.version = cnmt.titleVersion;
    out.metaKey.type = cnmt.metaType;
    out.metaKey.install_type = NcmContentInstallType_Full;
    std::memset(out.metaKey.padding, 0, sizeof(out.metaKey.padding));

    out.record.key = out.metaKey;
    out.record.storageId = static_cast<u8>(in.storageId);
    std::memset(out.record.padding, 0, sizeof(out.record.padding));

    out.metaData.header = metaHeader;
    out.metaData.extended = ext;
    const Content &metaContent = out.contents[2];
    out.metaData.infos[0].content_id = metaContent.id;
    out.metaData.infos[0].content_type = metaContent.type;
    out.metaData.infos[0].attr = 0;
    ncmU64ToContentInfoSize(metaContent.data.size(), &out.metaData.infos[0]);
    out.metaData.infos[0].id_offset = 0;
    out.metaData.infos[1] = packed[0].info;
    out.metaData.infos[2] = packed[1].info;
  }
  return true;
}

} // namespace vkpcnx::forwarder
#endif
