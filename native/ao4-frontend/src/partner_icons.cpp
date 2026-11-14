#include "partner_icons.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

namespace ao4icons {
namespace {

using Bytes = std::vector<uint8_t>;

bool ReadFile(const std::wstring& path, Bytes& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  out.resize(size_t(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
  return bool(in);
}

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
size_t Align16(size_t v) { return (v + 15) & ~size_t(15); }

// Frostbite payload: blocks of (uncompressed size, compressed size, zlib data), big endian.
Bytes Inflate(const uint8_t* data, size_t size) {
  Bytes out;
  size_t p = 0;
  while (p + 8 <= size) {
    const uint32_t usize = BE32(data + p), csize = BE32(data + p + 4);
    p += 8;
    if (p + csize > size) break;
    int len = 0;
    char* dec = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(data + p), int(csize),
                                                             int(usize ? usize : 65536), &len, 1);
    if (dec) {
      out.insert(out.end(), dec, dec + len);
      STBI_FREE(dec);
    } else {
      out.insert(out.end(), data + p, data + p + csize);  // stored block
    }
    p += csize;
  }
  return out;
}

// ---- noncas bundles: res payloads by name ---------------------------------------------
struct Span { size_t offset = 0, size = 0; };

std::unordered_map<std::string, Span> ScanResources(const Bytes& sb) {
  std::unordered_map<std::string, Span> res;
  for (size_t p = 4; p + 36 <= sb.size(); p += 4) {
    if (BE32(&sb[p]) != 0x970D1C13) continue;
    const size_t start = p - 4, meta_start = p;
    const uint32_t meta_size = BE32(&sb[start]);
    const uint8_t* h = &sb[meta_start];
    const uint32_t num = BE32(h + 4), nebx = BE32(h + 8), nres = BE32(h + 12), nchunks = BE32(h + 16);
    const uint32_t off_str = BE32(h + 20);
    if (num != nebx + nres + nchunks || meta_size > (64u << 20) || meta_start + meta_size > sb.size()) continue;
    size_t q = meta_start + 32 + size_t(20) * num;
    const size_t ebx_at = q;
    q += size_t(12) * nebx;
    const size_t res_at = q;
    q += size_t(12) * nres + size_t(20) * nres;
    const size_t chunk_at = q;
    if (chunk_at + size_t(28) * nchunks > sb.size()) continue;
    size_t pos = meta_start + meta_size;
    bool ok = true;
    for (uint32_t i = 0; i < nebx && ok; ++i) {
      pos = Align16(pos) + BE32(&sb[ebx_at + 12 * i + 4]);
      ok = pos <= sb.size();
    }
    for (uint32_t i = 0; i < nres && ok; ++i) {
      const uint8_t* e = &sb[res_at + 12 * i];
      pos = Align16(pos);
      const size_t name_at = meta_start + off_str + BE32(e);
      if (name_at >= sb.size()) { ok = false; break; }
      const char* name = reinterpret_cast<const char*>(&sb[name_at]);
      const size_t len = strnlen(name, std::min<size_t>(512, sb.size() - name_at));
      res[std::string(name, len)] = {pos, BE32(e + 4)};
      pos += BE32(e + 4);
      ok = pos <= sb.size();
    }
    for (uint32_t i = 0; i < nchunks && ok; ++i) {
      const uint8_t* c = &sb[chunk_at + 28 * i];
      pos = Align16(pos) + (BE32(c + 20) - BE32(c + 16));
      ok = pos <= sb.size();
    }
  }
  return res;
}

// ---- toc: XOR-obfuscated binary object file, "chunks" = [{id, offset, size}] ------------
struct ChunkRef { uint64_t offset = 0; uint32_t size = 0; bool compressed = false; };

struct Node {
  uint8_t type = 0;
  std::string name;
  uint64_t num = 0;
  std::string bytes;
  std::vector<Node> kids;
  const Node* Kid(const char* n) const {
    for (const auto& k : kids) if (k.name == n) return &k;
    return nullptr;
  }
};

struct TocParser {
  const Bytes& d;
  size_t p = 0;
  bool fail = false;
  uint64_t Varint() {
    uint64_t r = 0;
    for (int shift = 0; p < d.size() && shift < 64; shift += 7) {
      const uint8_t b = d[p++];
      r |= uint64_t(b & 0x7F) << shift;
      if (!(b & 0x80)) return r;
    }
    fail = true;
    return 0;
  }
  bool Need(size_t n) {
    if (p + n > d.size()) fail = true;
    return !fail;
  }
  std::string Take(size_t n) {
    if (!Need(n)) return {};
    std::string r(reinterpret_cast<const char*>(&d[p]), n);
    p += n;
    return r;
  }
  void Parse(Node& n, int depth = 0) {
    if (fail || depth > 32 || !Need(1)) { fail = true; return; }
    const uint8_t header = d[p++];
    n.type = header & 0x1F;
    if (!((header >> 5) & 4)) {
      const size_t s = p;
      while (p < d.size() && d[p]) ++p;
      n.name.assign(reinterpret_cast<const char*>(&d[s]), p - s);
      ++p;
    }
    switch (n.type) {
      case 1: case 2: {  // array / object, both end with a null byte
        const size_t len = size_t(Varint());  // read first: it advances p
        const size_t end = p + len;
        if (end > d.size()) { fail = true; return; }
        while (!fail && p + 1 < end) {
          n.kids.emplace_back();
          Parse(n.kids.back(), depth + 1);
        }
        p = end;
        return;
      }
      case 4: return;
      case 5: Take(12); return;
      case 6: Take(1); return;
      case 7: case 19: n.bytes = Take(size_t(Varint())); return;
      case 8: { const std::string v = Take(4); if (v.size() == 4) { uint32_t x; std::memcpy(&x, v.data(), 4); n.num = x; } return; }
      case 9: { const std::string v = Take(8); if (v.size() == 8) std::memcpy(&n.num, v.data(), 8); return; }
      case 10: case 21: n.num = Varint(); return;
      case 11: Take(4); return;
      case 12: case 13: Take(8); return;
      case 14: Take(6); return;
      case 15: n.bytes = Take(16); return;  // GUID: first three fields little endian
      case 16: case 20: Take(20); return;
      case 17: Take(64); return;
      case 18: Take(16); return;
      default: fail = true; return;
    }
  }
};

bool ReadToc(const std::wstring& path, std::map<std::string, ChunkRef>& chunks) {
  Bytes raw;
  if (!ReadFile(path, raw) || raw.size() < 4) return false;
  Bytes data;
  if (raw[0] == 0x00 && raw[1] == 0xD1 && raw[2] == 0xCE && (raw[3] == 0x00 || raw[3] == 0x01)) {
    if (raw.size() < 296 + 260) return false;
    uint8_t key[260];
    for (int i = 0; i < 260; ++i) key[i] = raw[296 + i] ^ 0x7B;
    data.resize(raw.size() - 556);
    for (size_t i = 0; i < data.size(); ++i) data[i] = key[i % 257] ^ raw[556 + i];
  } else {
    data = std::move(raw);
  }
  TocParser parser{data};
  Node root;
  parser.Parse(root);
  if (getenv("AO4_ICON_DEBUG"))
    fprintf(stderr, "toc size=%zu fail=%d pos=%zu kids=%zu first=%s type=%d\n", data.size(), int(parser.fail), parser.p,
            root.kids.size(), root.kids.empty() ? "" : root.kids[0].name.c_str(), int(root.type));
  if (getenv("AO4_ICON_DEBUG"))
    for (const auto& k : root.kids) {
      fprintf(stderr, "  kid '%s' type=%d n=%zu\n", k.name.c_str(), int(k.type), k.kids.size());
      if (!k.kids.empty())
        for (const auto& f : k.kids[0].kids)
          fprintf(stderr, "    field '%s' type=%d bytes=%zu num=%llu\n", f.name.c_str(), int(f.type), f.bytes.size(),
                  (unsigned long long)f.num);
    }
  const Node* list = root.Kid("chunks");
  if (!list) return false;
  for (const Node& e : list->kids) {
    const Node* id = e.Kid("id");
    const Node* off = e.Kid("offset");
    const Node* size = e.Kid("size");
    if (!id || id->bytes.size() != 16 || !off || !size) continue;
    std::string g = id->bytes;  // to the byte order of texture headers (big endian fields)
    std::swap(g[0], g[3]); std::swap(g[1], g[2]); std::swap(g[4], g[5]); std::swap(g[6], g[7]);
    chunks[g] = {off->num, uint32_t(size->num), (uint8_t(g[15]) & 1) != 0};
  }
  return !chunks.empty();
}

// ---- Xbox 360 tiled BC3 -> linear (XGAddress2DTiled, as tools/export_xenon_textures.py) ----
Bytes Untile(const Bytes& data, int width, int height) {
  const int64_t pitch = 16, log = 4;
  const int64_t bw = std::max(1, (width + 3) / 4), bh = std::max(1, (height + 3) / 4);
  const int64_t aw = (bw + 31) & ~int64_t(31);
  Bytes out(size_t(bw * bh * pitch), 0);
  const int64_t count = int64_t(data.size()) / pitch;
  for (int64_t idx = 0; idx < count; ++idx) {
    const int64_t ob = idx << log;
    const int64_t ot = ((ob & ~int64_t(4095)) >> 3) + ((ob & 1792) >> 2) + (ob & 63);
    const int64_t om = ot >> (7 + log);
    int64_t x = ((((om % (aw >> 5)) << 2) + ((((ot >> (5 + log)) & 2) + (ob >> 6)) & 3)) << 3);
    x += ((((ot >> 1) & ~int64_t(15)) + (ot & 15)) & ((pitch << 3) - 1)) >> log;
    int64_t y = (((om / (aw >> 5)) << 2) + ((ot >> (6 + log)) & 1) + ((ob & 2048) >> 10)) << 3;
    y += ((((ot & (((pitch << 6) - 1) & ~int64_t(31))) + ((ot & 15) << 1)) >> (3 + log)) & ~int64_t(1)) + ((ot & 16) >> 4);
    if (x < bw && y < bh) std::memcpy(&out[size_t((y * bw + x) * pitch)], &data[size_t(idx * pitch)], size_t(pitch));
  }
  for (size_t i = 0; i + 1 < out.size(); i += 2) std::swap(out[i], out[i + 1]);  // 16-bit endian swap
  return out;
}

// BC3 (DXT5) -> RGBA8
Bytes DecodeBc3(const Bytes& blocks, int width, int height) {
  Bytes rgba(size_t(width) * height * 4, 0);
  const int bw = (width + 3) / 4, bh = (height + 3) / 4;
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* b = &blocks[size_t(by * bw + bx) * 16];
      uint8_t alpha[8];
      alpha[0] = b[0];
      alpha[1] = b[1];
      if (alpha[0] > alpha[1]) {
        for (int i = 1; i < 7; ++i) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1] + 3) / 7);
      } else {
        for (int i = 1; i < 5; ++i) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1] + 2) / 5);
        alpha[6] = 0;
        alpha[7] = 255;
      }
      uint64_t abits = 0;
      for (int i = 0; i < 6; ++i) abits |= uint64_t(b[2 + i]) << (8 * i);
      const uint16_t c0 = uint16_t(b[8] | b[9] << 8), c1 = uint16_t(b[10] | b[11] << 8);
      uint8_t col[4][3];
      auto expand = [](uint16_t c, uint8_t* o) {
        o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
        o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        o[2] = uint8_t((c & 31) * 255 / 31);
      };
      expand(c0, col[0]);
      expand(c1, col[1]);
      for (int k = 0; k < 3; ++k) {
        col[2][k] = uint8_t((2 * col[0][k] + col[1][k] + 1) / 3);
        col[3][k] = uint8_t((col[0][k] + 2 * col[1][k] + 1) / 3);
      }
      const uint32_t cbits = uint32_t(b[12]) | uint32_t(b[13]) << 8 | uint32_t(b[14]) << 16 | uint32_t(b[15]) << 24;
      for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
          const int x = bx * 4 + px, y = by * 4 + py;
          if (x >= width || y >= height) continue;
          const int i = py * 4 + px;
          uint8_t* o = &rgba[(size_t(y) * width + x) * 4];
          const uint8_t* c = col[(cbits >> (2 * i)) & 3];
          o[0] = c[0];
          o[1] = c[1];
          o[2] = c[2];
          o[3] = alpha[(abits >> (3 * i)) & 7];
        }
      }
    }
  }
  return rgba;
}

// ---- minimal PNG writer (stored deflate blocks) -------------------------------------------
uint32_t Crc(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
  static uint32_t table[256] = {};
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
  }
  for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c;
}
void Put32(Bytes& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s)); }
void Chunk(Bytes& o, const char* type, const Bytes& body) {
  Put32(o, uint32_t(body.size()));
  const size_t at = o.size();
  o.insert(o.end(), type, type + 4);
  o.insert(o.end(), body.begin(), body.end());
  Put32(o, Crc(&o[at], o.size() - at) ^ 0xFFFFFFFFu);
}
bool WritePng(const std::wstring& path, int w, int h, const Bytes& rgba) {
  Bytes raw;
  raw.reserve(size_t(h) * (w * 4 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
  }
  Bytes z = {0x78, 0x01};
  for (size_t p = 0; p < raw.size(); p += 65535) {
    const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - p));
    z.push_back(p + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(uint16_t(~n) >> 8));
    z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
  }
  uint32_t a = 1, b = 0;
  for (const uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  Put32(z, b << 16 | a);
  Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes ihdr;
  Put32(ihdr, uint32_t(w));
  Put32(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  const std::wstring tmp = path + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    if (!out) return false;
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}  // namespace

std::wstring IconPath(const std::wstring& out_dir, uint32_t id) { return out_dir + L"\\" + std::to_wstring(id) + L".png"; }

int Build(const std::wstring& disc_dir, const std::wstring& out_dir, const std::vector<Request>& items,
          std::wstring& error) {
  CreateDirectoryW(out_dir.c_str(), nullptr);
  int present = 0;
  std::vector<const Request*> todo;
  for (const auto& it : items) {
    if (GetFileAttributesW(IconPath(out_dir, it.id).c_str()) != INVALID_FILE_ATTRIBUTES) ++present;
    else todo.push_back(&it);
  }
  if (todo.empty()) return present;
  Bytes frontend, tod;
  const std::wstring xenon = disc_dir + L"\\Data\\Xenon";
  if (!ReadFile(xenon + L"\\AO4_Levels\\AO4Frontend\\AO4Frontend.sb", frontend) ||
      !ReadFile(xenon + L"\\TexturesOnDemand.sb", tod)) {
    error = L"game files missing (AO4Frontend.sb / TexturesOnDemand.sb)";
    return present;
  }
  std::map<std::string, ChunkRef> chunks;
  if (!ReadToc(xenon + L"\\TexturesOnDemand.toc", chunks)) {
    error = L"TexturesOnDemand.toc unreadable";
    return present;
  }
  const auto res = ScanResources(frontend);
  for (const Request* it : todo) {
    const auto r = res.find(it->icon);
    if (r == res.end()) continue;
    const Bytes hdr = Inflate(&frontend[r->second.offset], r->second.size);
    if (hdr.size() < 48) continue;
    const uint32_t fmt = BE32(&hdr[8]);
    const int w = BE16(&hdr[16]), h = BE16(&hdr[18]);
    const std::string guid(reinterpret_cast<const char*>(&hdr[28]), 16);
    const uint32_t size = BE32(&hdr[44]);
    const auto c = chunks.find(guid);
    if (fmt != 2 || c == chunks.end() || c->second.offset + c->second.size > tod.size()) continue;
    Bytes pixels = c->second.compressed ? Inflate(&tod[size_t(c->second.offset)], c->second.size)
                                        : Bytes(tod.begin() + c->second.offset, tod.begin() + c->second.offset + c->second.size);
    if (pixels.size() < size) continue;
    pixels.resize(size);
    if (WritePng(IconPath(out_dir, it->id), w, h, DecodeBc3(Untile(pixels, w, h), w, h))) ++present;
  }
  if (present < int(items.size()) && error.empty()) error = L"some icons were not found";
  return present;
}

bool LoadRgba(const std::wstring& file, int& width, int& height, std::vector<uint8_t>& rgba) {
  Bytes png;
  if (!ReadFile(file, png)) return false;
  int n = 0;
  stbi_uc* px = stbi_load_from_memory(png.data(), int(png.size()), &width, &height, &n, 4);
  if (!px) return false;
  rgba.assign(px, px + size_t(width) * height * 4);
  stbi_image_free(px);
  return true;
}

}  // namespace ao4icons
