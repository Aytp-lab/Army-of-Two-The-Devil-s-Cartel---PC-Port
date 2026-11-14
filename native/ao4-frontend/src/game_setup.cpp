#include "game_setup.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

#pragma comment(lib, "bcrypt.lib")

namespace ao4setup {
namespace {

constexpr uint64_t kSector = 2048;
// Game partition offsets of the Xbox 360 disc formats (XGD2, XGD3, XGD3 variant) and plain XISO.
constexpr uint64_t kPartitionOffsets[] = {0x2080000ull, 0xFD90000ull, 0x18300000ull, 0ull};
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

// Known good build (Europe En/De/It), the version the native port was recompiled from.
constexpr char kDefaultXexSha[] = "a99a063bb9089a9892d73aca66d5176ac20dbce8bc64e08542edef208f62ef55";
constexpr char kMainXexSha[] = "7756da07fe3fb481f58873313c1a1534fecab8d4ee983221208141e09e14d24b";
constexpr char kHdPackage[] = "Content/0000000000000000/454109AB/00000002/454109AB00000000";

struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
  bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

bool ReadAt(HANDLE h, uint64_t offset, void* buffer, uint32_t size) {
  LARGE_INTEGER pos;
  pos.QuadPart = int64_t(offset);
  if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
  DWORD read = 0;
  return ReadFile(h, buffer, size, &read, nullptr) && read == size;
}

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::wstring JoinPath(const std::wstring& dir, const std::string& rel) {
  std::wstring w = Widen(rel);
  std::replace(w.begin(), w.end(), L'/', L'\\');
  return dir + L"\\" + w;
}

bool MakeDirs(const std::wstring& path) {
  // Creates every parent directory of a file path.
  for (size_t i = 3; i < path.size(); ++i) {
    if (path[i] == L'\\') {
      const std::wstring part = path.substr(0, i);
      if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
  }
  return true;
}

uint64_t FileSizeOf(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return UINT64_MAX;
  return (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// Walks an XDVDFS directory (binary tree of entries, offsets in 4-byte units).
bool WalkDirectory(HANDLE h, uint64_t partition, uint32_t sector, uint32_t size, const std::string& prefix,
                   std::vector<IsoFile>& out, int depth) {
  if (size == 0) return true;
  if (depth > 32 || size > 64u * 1024 * 1024) return false;
  std::vector<uint8_t> dir(size);
  if (!ReadAt(h, partition + uint64_t(sector) * kSector, dir.data(), size)) return false;
  std::vector<uint32_t> stack{0};
  std::vector<bool> seen(size / 4 + 1, false);
  while (!stack.empty()) {
    const uint32_t off = stack.back();
    stack.pop_back();
    if (off + 14 > size || seen[off / 4]) continue;
    seen[off / 4] = true;
    const uint8_t* e = dir.data() + off;
    if (Le16(e) == 0xFFFF && Le16(e + 2) == 0xFFFF) continue;  // sector padding
    const uint16_t left = Le16(e), right = Le16(e + 2);
    const uint32_t start = Le32(e + 4), length = Le32(e + 8);
    const uint8_t attr = e[12], name_len = e[13];
    if (off + 14 + name_len > size) return false;
    const std::string name(reinterpret_cast<const char*>(e + 14), name_len);
    const std::string path = prefix.empty() ? name : prefix + "/" + name;
    if (attr & 0x10) {
      if (!WalkDirectory(h, partition, start, length, path, out, depth + 1)) return false;
    } else {
      out.push_back({path, partition + uint64_t(start) * kSector, length});
    }
    if (left) stack.push_back(uint32_t(left) * 4);
    if (right) stack.push_back(uint32_t(right) * 4);
  }
  return true;
}

bool Wanted(const std::string& path) {
  if (path.rfind("$SystemUpdate/", 0) == 0) return false;
  if (path.rfind("Content/", 0) == 0) return false;  // HD package, extracted separately
  return true;
}

// Copies [offset, offset+size) of the ISO into a file.
bool CopyRange(HANDLE src, uint64_t offset, uint64_t size, const std::wstring& dst, uint64_t& done,
               uint64_t total, const std::wstring& label, const Progress& progress, std::wstring& error) {
  if (!MakeDirs(dst)) { error = L"Ordner kann nicht angelegt werden: " + dst; return false; }
  Handle out;
  out.h = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!out.ok()) { error = L"Datei kann nicht geschrieben werden: " + dst; return false; }
  constexpr uint32_t kChunk = 4u * 1024 * 1024;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
  uint64_t left = size;
  while (left) {
    const uint32_t n = uint32_t(std::min<uint64_t>(left, kChunk));
    if (!ReadAt(src, offset + (size - left), buf.get(), n)) { error = L"Lesefehler in der ISO bei " + label; return false; }
    DWORD written = 0;
    if (!WriteFile(out.h, buf.get(), n, &written, nullptr) || written != n) { error = L"Schreibfehler (Speicherplatz?): " + dst; return false; }
    left -= n;
    done += n;
    if (progress && !progress(done, total, label)) { error = L"cancelled"; return false; }
  }
  return true;
}

// ---- STFS (HD package inside the ISO, at base offset) ------------------------------------
struct Stfs {
  HANDLE h;
  uint64_t base;
  uint64_t length;
  uint32_t first_hash_table = 0;
  int shift = 0;

  uint32_t BackingDataBlock(uint32_t block) const {
    uint32_t r = (((block + 0xAA) / 0xAA) << shift) + block;
    if (block < 0xAA) return r;
    if (block < 0x70E4) return r + (((block + 0x70E4) / 0x70E4) << shift);
    return (1u << shift) + r + (((block + 0x70E4) / 0x70E4) << shift);
  }
  uint64_t BlockAddress(uint32_t block) const { return (uint64_t(BackingDataBlock(block)) << 12) + first_hash_table; }
  uint32_t Level0HashBlock(uint32_t block) const {
    if (block < 0xAA) return 0;
    uint32_t num = (block / 0xAA) * (shift == 0 ? 0xABu : 0xACu);
    num += ((block / 0x70E4) + 1) << shift;
    if (block / 0x70E4 == 0) return num;
    return num + (1u << shift);
  }
  bool Read(uint64_t off, void* buf, uint32_t n) const { return off + n <= length && ReadAt(h, base + off, buf, n); }
  int64_t NextBlock(uint32_t block) const {
    const uint64_t record = (uint64_t(Level0HashBlock(block)) << 12) + first_hash_table + (block % 0xAA) * 0x18;
    uint8_t b[4];
    if (!Read(record + 0x14, b, 4)) return -1;
    const uint32_t next = Be24(b + 1);
    if (next >= 0xFFFFFE) return -1;
    return next;
  }
};

struct StfsEntry {
  std::string name;
  bool dir = false, consecutive = false;
  uint32_t blocks = 0, first = 0, size = 0;
  int parent = -1;
};

bool ReadStfs(Stfs& s, std::vector<StfsEntry>& entries, std::wstring& error) {
  uint8_t head[0x500];
  if (!s.Read(0, head, sizeof(head))) { error = L"HD-Paket nicht lesbar"; return false; }
  if (std::memcmp(head, "CON ", 4) && std::memcmp(head, "LIVE", 4) && std::memcmp(head, "PIRS", 4)) {
    error = L"HD-Paket hat kein STFS-Format";
    return false;
  }
  const uint32_t header_size = Be32(head + 0x340);
  s.first_hash_table = (header_size + 0xFFF) & 0xFFFFF000u;
  const uint8_t block_sep = head[0x37B];
  s.shift = (block_sep & 1) ? 0 : 1;
  const uint32_t table_blocks = Le16(head + 0x37C);
  uint32_t block = Le24(head + 0x37E);
  for (uint32_t read = 0; read < table_blocks; ++read) {
    uint8_t tbl[0x1000];
    if (!s.Read(s.BlockAddress(block), tbl, sizeof(tbl))) { error = L"HD-Dateitabelle nicht lesbar"; return false; }
    for (int i = 0; i < 64; ++i) {
      const uint8_t* e = tbl + i * 0x40;
      const uint8_t len = e[0x28] & 0x3F;
      if (!len) continue;
      bool printable = true;
      for (int k = 0; k < len; ++k) if (e[k] < 0x20 || e[k] > 0x7E) printable = false;
      if (!printable) continue;
      StfsEntry x;
      x.name.assign(reinterpret_cast<const char*>(e), len);
      x.dir = (e[0x28] & 0x80) != 0;
      x.consecutive = (e[0x28] & 0x40) != 0;
      x.blocks = Le24(e + 0x29);
      x.first = Le24(e + 0x2F);
      const uint16_t p = uint16_t((e[0x32] << 8) | e[0x33]);
      x.parent = p == 0xFFFF ? -1 : int(p);
      x.size = Be32(e + 0x34);
      entries.push_back(x);
    }
    if (read + 1 < table_blocks) {
      const int64_t next = s.NextBlock(block);
      if (next < 0) break;
      block = uint32_t(next);
    }
  }
  return true;
}

std::string StfsPath(const std::vector<StfsEntry>& entries, size_t i) {
  std::string path = entries[i].name;
  int p = entries[i].parent;
  for (int guard = 0; p >= 0 && size_t(p) < entries.size() && guard < 64; ++guard) {
    path = entries[size_t(p)].name + "/" + path;
    p = entries[size_t(p)].parent;
  }
  return path;
}

bool ExtractStfsFile(const Stfs& s, const StfsEntry& e, const std::wstring& dst, uint64_t& done, uint64_t total,
                     const std::wstring& label, const Progress& progress, std::wstring& error) {
  if (!MakeDirs(dst)) { error = L"Ordner kann nicht angelegt werden: " + dst; return false; }
  Handle out;
  out.h = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!out.ok()) { error = L"Datei kann nicht geschrieben werden: " + dst; return false; }
  uint32_t block = e.first, left = e.size;
  uint8_t buf[0x1000];
  for (uint32_t n = 0; left && n < e.blocks; ++n) {
    const uint32_t take = std::min<uint32_t>(left, 0x1000);
    if (!s.Read(s.BlockAddress(block), buf, take)) { error = L"HD-Paket: Lesefehler in " + label; return false; }
    DWORD written = 0;
    if (!WriteFile(out.h, buf, take, &written, nullptr) || written != take) { error = L"Schreibfehler (Speicherplatz?): " + dst; return false; }
    left -= take;
    done += take;
    if ((n & 255) == 0 && progress && !progress(done, total, label)) { error = L"cancelled"; return false; }
    if (left) {
      if (e.consecutive) {
        ++block;
      } else {
        const int64_t next = s.NextBlock(block);
        if (next < 0) { error = L"HD-Paket: Blockkette endet zu früh in " + label; return false; }
        block = uint32_t(next);
      }
    }
  }
  if (left) { error = L"HD-Paket: Datei unvollständig: " + label; return false; }
  return true;
}

}  // namespace

std::string Sha256File(const std::wstring& path) {
  Handle f;
  f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (!f.ok()) return {};
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  std::unique_ptr<uint8_t[]> buf(new uint8_t[1 << 20]);
  DWORD read = 0;
  while (ReadFile(f.h, buf.get(), 1 << 20, &read, nullptr) && read) BCryptHashData(hash, buf.get(), read, 0);
  uint8_t digest[32];
  BCryptFinishHash(hash, digest, 32, 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : digest) { s += hex[b >> 4]; s += hex[b & 15]; }
  return s;
}

namespace {
std::string Sha256Range(HANDLE h, uint64_t offset, uint64_t size) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  std::unique_ptr<uint8_t[]> buf(new uint8_t[1 << 20]);
  bool ok = true;
  for (uint64_t done = 0; done < size && ok;) {
    const uint32_t n = uint32_t(std::min<uint64_t>(size - done, 1 << 20));
    ok = ReadAt(h, offset + done, buf.get(), n);
    if (ok) BCryptHashData(hash, buf.get(), n, 0);
    done += n;
  }
  uint8_t digest[32];
  BCryptFinishHash(hash, digest, 32, 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  if (!ok) return {};
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : digest) { s += hex[b >> 4]; s += hex[b & 15]; }
  return s;
}
const IsoFile* Find(const IsoInfo& info, const char* path) {
  for (const auto& f : info.files) if (_stricmp(f.path.c_str(), path) == 0) return &f;
  return nullptr;
}
}  // namespace

bool ReadIso(const std::wstring& iso, IsoInfo& info, std::wstring& error) {
  Handle h;
  h.h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!h.ok()) { error = L"ISO kann nicht geöffnet werden."; return false; }
  for (uint64_t partition : kPartitionOffsets) {
    uint8_t vd[0x800];
    if (!ReadAt(h.h, partition + 32 * kSector, vd, sizeof(vd))) continue;
    if (std::memcmp(vd, kMagic, 20) != 0) continue;
    info = {};
    info.partition_offset = partition;
    if (!WalkDirectory(h.h, partition, Le32(vd + 20), Le32(vd + 24), "", info.files, 0)) {
      error = L"Dateisystem der ISO ist beschädigt.";
      return false;
    }
    return true;
  }
  error = L"Keine Xbox-360-Spiel-ISO (XDVDFS nicht gefunden).";
  return false;
}

VersionCheck CheckVersion(const std::wstring& iso, const IsoInfo& info) {
  VersionCheck v;
  const IsoFile* def = Find(info, "Default.xex");
  const IsoFile* main = Find(info, "AO4.Main.xex");
  if (!def || !main) {
    v.message = L"Das ist nicht Army of Two: The Devil's Cartel (Default.xex/AO4.Main.xex fehlen).";
    return v;
  }
  Handle h;
  h.h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!h.ok()) { v.message = L"ISO kann nicht geöffnet werden."; return v; }
  v.default_xex_sha256 = Sha256Range(h.h, def->offset, def->size);
  v.main_xex_sha256 = Sha256Range(h.h, main->offset, main->size);
  v.ok = v.default_xex_sha256 == kDefaultXexSha && v.main_xex_sha256 == kMainXexSha;
  v.message = v.ok ? L"Unterstützte Version erkannt (Europa, En/De/It)."
                   : L"Andere Spielversion. Der Port unterstützt nur die Europa-Version (En/De/It).";
  return v;
}

bool Extract(const std::wstring& iso, const IsoInfo& info, const std::wstring& disc_dir,
             const std::wstring& hd_dir, const Progress& progress, std::wstring& error) {
  Handle h;
  h.h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (!h.ok()) { error = L"ISO kann nicht geöffnet werden."; return false; }
  const IsoFile* hd = Find(info, kHdPackage);
  if (!hd) { error = L"HD-Paket fehlt auf der Disc."; return false; }

  Stfs s{h.h, hd->offset, hd->size};
  std::vector<StfsEntry> entries;
  if (!ReadStfs(s, entries, error)) return false;

  uint64_t total = 0, done = 0;
  for (const auto& f : info.files) if (Wanted(f.path)) total += f.size;
  for (const auto& e : entries) if (!e.dir) total += e.size;

  for (const auto& f : info.files) {
    if (!Wanted(f.path)) continue;
    const std::wstring dst = JoinPath(disc_dir, f.path);
    if (FileSizeOf(dst) == f.size) { done += f.size; continue; }  // resume
    if (!CopyRange(h.h, f.offset, f.size, dst, done, total, Widen(f.path), progress, error)) return false;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    if (e.dir) continue;
    const std::string rel = StfsPath(entries, i);
    const std::wstring dst = JoinPath(hd_dir, rel);
    if (FileSizeOf(dst) == e.size) { done += e.size; continue; }
    if (!ExtractStfsFile(s, e, dst, done, total, L"HD/" + Widen(rel), progress, error)) return false;
  }
  if (progress) progress(total, total, L"");
  return true;
}

bool IsInstalled(const std::wstring& disc_dir, const std::wstring& hd_dir) {
  return FileSizeOf(disc_dir + L"\\Default.xex") == 475136 && FileSizeOf(disc_dir + L"\\AO4.Main.xex") == 10051584 &&
         FileSizeOf(disc_dir + L"\\Data\\layout.toc") != UINT64_MAX && FileSizeOf(hd_dir + L"\\package.mft") != UINT64_MAX &&
         FileSizeOf(hd_dir + L"\\Data\\Xenon\\HD.sb") != UINT64_MAX;
}

}  // namespace ao4setup
