// Character role swap research (Alpha/Bravo). The original getType functions always run.
//   AO4_SOLDIER_TRACE: read-only log of the character spawners and their identity data.
//   AO4_ROLE_SWAP:     in guest memory only, exchange the identity (CustomizeSoldierData:
//                      appearance, mask, weapons; VocalizationPackage: voice) between the
//                      player's spawn template and the AI partner's spawn template. The AI
//                      behaviour stays with the partner. Game files are never modified.
#include "generated/ao4.main/ao4_main_init.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <windows.h>
#include "ao4_readable.h"
#include <thread>
#include <algorithm>
#include <cctype>
#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_set>
#include <map>
#include <tuple>
#include "partner_appearance_state.h"
#include "applied_appearance_policy.h"
#include "look_template_state.h"
#include "qol_settings.h"

namespace {
// CharacterSpawnReferenceObjectData (vtable 0x82A627AC, getType sub_83F33C50) and its
// subclass AO2BotSpawnReferenceObjectData (vtable 0x82ACFD28, getType sub_84005168).
constexpr uint32_t kCharacterSpawnVtable = 0x82A627AC;
constexpr uint32_t kBotSpawnVtable = 0x82ACFD28;
// Field offsets from the Frostbite reflection data.
constexpr uint32_t kAssetName = 0x08;            // Asset::Name
constexpr uint32_t kBlueprint = 0x50;            // ReferenceObjectData::Blueprint
constexpr uint32_t kPlayerType = 0x180;          // CharacterSpawnReferenceObjectData::PlayerType
constexpr uint32_t kTemplate = 0x18C;            // CharacterSpawnReferenceObjectData::Template
constexpr uint32_t kCustomizeSoldierData = 0x0C; // SoldierSpawnTemplateData
constexpr uint32_t kBehaviour = 0x14;            // SoldierSpawnTemplateData
constexpr uint32_t kVocalizationPackage = 0x24;  // SoldierSpawnTemplateData (string)
constexpr uint32_t kMaskAsset = 0x5C;            // CustomizeSoldierData
// Spawn templates of the player (Alpha) and of the AI partner (Bravo).
constexpr char kPlayerTemplate[] = "Gameplay/Spawners/AO2SoldierSpawnTemplate";
constexpr char kPartnerTemplate[] = "OldObjects/Soldiers/Templates/SpawnTemplate_Partner";

// Whether a guest range is committed host memory (reads must never fault).
bool IsReadable(uint8_t* base, uint32_t address, uint32_t length) {
  if (address < 0x10000 || uint64_t(address) + length > 0x100000000ull) {
    return false;
  }
  return Ao4CommittedRange(reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(address)), length);
}

// Copy diagnostic/customization reads: a level may decommit between VirtualQuery
// and a raw load. Failed reads leave the original game routine in control.
bool ReadLookMemory(uint8_t* base, uint32_t address, void* output, size_t length) {
  if (address < 0x10000 || uint64_t(address)+length > 0x100000000ull) return false;
  SIZE_T copied=0;
  return ReadProcessMemory(GetCurrentProcess(),REX_RAW_ADDR(address),output,length,&copied) &&
      copied==length;
}

// Guest C string at address, or empty if it does not look like one.
std::string GuestString(uint8_t* base, uint32_t address) {
  std::array<uint8_t,96> bytes{};
  if (!ReadLookMemory(base, address, bytes.data(), bytes.size())) {
    return {};
  }
  std::string text;
  for (uint32_t i = 0; i < 96; ++i) {
    const uint8_t c = bytes[i];
    if (!c) {
      break;
    }
    if (c < 0x20 || c >= 0x7F) {
      return {};
    }
    text.push_back(char(c));
  }
  return text.size() >= 4 ? text : std::string();
}

std::string AssetName(uint8_t* base, uint32_t asset) {
  uint32_t text=0;
  return asset<=0xFFFFFFF3u && Ao4LookWord(asset+kAssetName,text,
      [&](uint32_t a,void* p,size_t n){return ReadLookMemory(base,a,p,n);})
      ? GuestString(base,text) : std::string();
}

void LogTemplate(uint8_t* base, uint32_t spawn_template) {
  const uint32_t customize = REX_LOAD_U32(spawn_template + kCustomizeSoldierData);
  const uint32_t mask = IsReadable(base, customize, 0x88) ? REX_LOAD_U32(customize + kMaskAsset) : 0;
  REXLOG_WARN("AO4_SPAWNER_TPL {:08X} {} customize={:08X} {} mask={} behaviour={} voice={}",
              spawn_template, AssetName(base, spawn_template), customize, AssetName(base, customize),
              AssetName(base, mask), AssetName(base, REX_LOAD_U32(spawn_template + kBehaviour)),
              GuestString(base, REX_LOAD_U32(spawn_template + kVocalizationPackage)));
}

// One-line dump of a data object: vtable, asset name and the first words, with the asset
// name of every word that points at a named asset.
void DumpObject(uint8_t* base, const char* label, uint32_t object, uint32_t words) {
  if (!IsReadable(base, object, words * 4)) {
    REXLOG_WARN("AO4_DUMP {} {:08X} unreadable", label, object);
    return;
  }
  std::string text;
  for (uint32_t i = 0; i < words; ++i) {
    const uint32_t value = REX_LOAD_U32(object + i * 4);
    text += fmt::format(" +{:X}={:08X}", i * 4, value);
    const std::string name = AssetName(base, value);
    if (!name.empty()) {
      text += "(" + name + ")";
    }
  }
  REXLOG_WARN("AO4_DUMP {} {:08X} vt={:08X} name={}:{}", label, object, REX_LOAD_U32(object),
              AssetName(base, object), text);
}

// Frostbite array: pointer to the first element, element count just before it.
void DumpArray(uint8_t* base, const char* label, uint32_t array, uint32_t words) {
  if (!IsReadable(base, array - 4, 4)) {
    return;
  }
  const uint32_t count = REX_LOAD_U32(array - 4);
  REXLOG_WARN("AO4_DUMP {} array {:08X} count={}", label, array, count);
  for (uint32_t i = 0; i < count && i < 16; ++i) {
    const uint32_t element = REX_LOAD_U32(array + i * 4);
    DumpObject(base, label, element, words);
    // VisualGroup-like elements: dump arrays they point at one level deeper.
    for (uint32_t w = 2; w < 8 && IsReadable(base, element + w * 4, 4); ++w) {
      const uint32_t inner = REX_LOAD_U32(element + w * 4);
      if (IsReadable(base, inner - 4, 4) && REX_LOAD_U32(inner - 4) > 0 &&
          REX_LOAD_U32(inner - 4) < 64 && IsReadable(base, inner, 4) &&
          !AssetName(base, REX_LOAD_U32(inner)).empty()) {
        for (uint32_t k = 0; k < REX_LOAD_U32(inner - 4) && k < 16; ++k) {
          DumpObject(base, "  inner", REX_LOAD_U32(inner + k * 4), 12);
        }
      }
    }
  }
}

// Recursive dump: object words, then arrays (count just before the first element) of
// pointers found in those words, depth-limited.
void DumpTree(uint8_t* base, const std::string& label, uint32_t object, int depth,
              std::unordered_set<uint32_t>& visited) {
  if (depth < 0 || !visited.insert(object).second || visited.size() > 400) {
    return;
  }
  DumpObject(base, label.c_str(), object, 20);
  for (uint32_t w = 1; w < 20; ++w) {
    const uint32_t array = REX_LOAD_U32(object + w * 4);
    if (!IsReadable(base, array - 4, 8)) {
      continue;
    }
    const uint32_t count = REX_LOAD_U32(array - 4);
    if (count == 0 || count > 256 || !IsReadable(base, array, count * 4)) {
      continue;
    }
    const uint32_t first = REX_LOAD_U32(array);
    if (!IsReadable(base, first, 8)) {
      continue;
    }
    REXLOG_WARN("AO4_DUMP {}+{:X} array {:08X} count={}", label, w * 4, array, count);
    for (uint32_t i = 0; i < count && i < 64; ++i) {
      const uint32_t element = REX_LOAD_U32(array + i * 4);
      if (IsReadable(base, element, 20 * 4)) {
        DumpTree(base, fmt::format("{}+{:X}[{}]", label, w * 4, i), element, depth - 1, visited);
      }
    }
  }
}

// Committed guest memory, captured once for searches that test many pointers (a VirtualQuery
// per pointer is far too slow there).
class GuestRegions {
 public:
  explicit GuestRegions(uint8_t* base) {
    for (uint64_t address = 0x40000000; address < 0xE0000000;) {
      MEMORY_BASIC_INFORMATION info = {};
      if (!VirtualQuery(base + address, &info, sizeof(info))) {
        break;
      }
      const uint64_t end = std::min<uint64_t>(
          uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base) + info.RegionSize, 0xE0000000);
      if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        if (!ranges_.empty() && ranges_.back().second == address) {
          ranges_.back().second = end;
        } else {
          ranges_.emplace_back(address, end);
        }
      }
      address = end;
    }
  }
  bool Readable(uint32_t address, uint32_t length) const {
    auto it = std::upper_bound(ranges_.begin(), ranges_.end(), uint64_t(address),
                               [](uint64_t a, const auto& r) { return a < r.first; });
    if (it == ranges_.begin()) {
      return false;
    }
    --it;
    return uint64_t(address) + length <= it->second;
  }

 private:
  std::vector<std::pair<uint64_t, uint64_t>> ranges_;
};

// Every ShaderParameterComponentData (vtable 82A6D300) reachable from a blueprint with its
// depth: breadth first over pointer fields and Frostbite pointer arrays, bounded.
std::vector<std::pair<uint32_t, uint32_t>> FindShaderParameterData(uint8_t* base, uint32_t root,
                                                                    size_t* visited_count) {
  constexpr uint32_t kShaderParameterData = 0x82A6D300;
  const GuestRegions regions(base);
  auto is_object = [&](uint32_t p) {
    if (!regions.Readable(p, 0x100)) {
      return false;
    }
    const uint32_t vtable = REX_LOAD_U32(p);
    return vtable >= 0x82000000 && vtable < 0x83000000;
  };
  std::vector<std::pair<uint32_t, uint32_t>> found, queue{{root, 0}};
  std::unordered_set<uint32_t> visited{root};
  for (size_t next = 0; next < queue.size() && visited.size() < 20000; ++next) {
    const auto [object, depth] = queue[next];
    if (REX_LOAD_U32(object) == kShaderParameterData) {
      found.emplace_back(object, depth);
    }
    for (uint32_t w = 1; w < 64; ++w) {
      const uint32_t p = REX_LOAD_U32(object + w * 4);
      if (is_object(p)) {
        if (visited.insert(p).second) {
          queue.emplace_back(p, depth + 1);
        }
        continue;
      }
      if (!regions.Readable(p - 4, 8)) {
        continue;
      }
      const uint32_t count = REX_LOAD_U32(p - 4);
      if (count == 0 || count > 256 || !regions.Readable(p, count * 4)) {
        continue;
      }
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t element = REX_LOAD_U32(p + i * 4);
        if (is_object(element) && visited.insert(element).second) {
          queue.emplace_back(element, depth + 1);
        }
      }
    }
  }
  *visited_count = visited.size();
  return found;
}

std::string ShaderTextureNames(uint8_t* base, uint32_t data) {
  const uint32_t names = REX_LOAD_U32(data + 0x64);
  std::string list;
  const uint32_t count = IsReadable(base, names - 4, 4) ? REX_LOAD_U32(names - 4) : 0;
  for (uint32_t k = 0; k < count && k < 16; ++k) {
    list += " '" + GuestString(base, REX_LOAD_U32(names + k * 4)) + "'";
  }
  return fmt::format("count={}:{}", count, list);
}

uint32_t NewGuestString(uint8_t* base, const std::string& text) {
  const uint32_t block = rex::system::kernel_memory()->SystemHeapAlloc(uint32_t(text.size()) + 1);
  if (block) {
    for (size_t i = 0; i <= text.size(); ++i) {
      REX_STORE_U8(block + uint32_t(i), i < text.size() ? uint8_t(text[i]) : 0);
    }
  }
  return block;
}

uint32_t NewGuestArray(uint8_t* base, const std::vector<uint32_t>& elements);

// AO4_AI_ALPHA_OUTFIT: the AISoldier blueprint has no dynamic shader texture slots (mask,
// tattoo, skin), so Alpha's outfit on the AI partner renders with white arms. Before the
// partner spawns, the blueprint's own shader parameter data (the nearest ones, not those of
// referenced blueprints) gets the player's slot names in the player's order (0 mask,
// 1 tattoo, 2 skin). Shared by every AISoldier; their materials do not use these slots.
void MaybeAddAiShaderSlots(uint8_t* base, uint32_t blueprint, const std::string& name) {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_AI_ALPHA_OUTFIT", nullptr, 0) != 0;
  // A reloaded level (checkpoint, mission restart) can put the new AISoldier blueprint at the
  // same address, so "done" is checked against the patched data still carrying the names.
  static std::map<uint32_t, std::vector<uint32_t>> done;
  static uint32_t names = 0;
  if (!enabled || !blueprint || name != "Characters/Soldiers/AISoldier") {
    return;
  }
  if (auto it = done.find(blueprint); it != done.end() && !it->second.empty() && names &&
      std::all_of(it->second.begin(), it->second.end(), [&](uint32_t data) {
        return IsReadable(base, data, 0x68) && REX_LOAD_U32(data) == 0x82A6D300 &&
               REX_LOAD_U32(data + 0x64) == names;
      })) {
    return;
  }
  auto& patched = done[blueprint];
  patched.clear();
  if (!names) {
    std::vector<uint32_t> strings;
    for (const char* text : {"AO4SoldierMaskTexture", "Tattoo_Map", "Skin_Diffuse_Map"}) {
      strings.push_back(NewGuestString(base, text));
    }
    if (std::find(strings.begin(), strings.end(), 0u) == strings.end()) {
      names = NewGuestArray(base, strings);
    }
  }
  const ULONGLONG start = GetTickCount64();
  size_t visited = 0;
  const auto found = FindShaderParameterData(base, blueprint, &visited);
  uint32_t nearest = UINT32_MAX;
  for (const auto& [data, depth] : found) {
    nearest = std::min(nearest, depth);
    REXLOG_WARN("AO4_AI_SLOTS data {:08X} depth={} {}", data, depth, ShaderTextureNames(base, data));
  }
  for (const auto& [data, depth] : found) {
    const uint32_t current = REX_LOAD_U32(data + 0x64);
    if (names && depth == nearest && IsReadable(base, current - 4, 4) &&
        REX_LOAD_U32(current - 4) == 0) {
      REX_STORE_U32(data + 0x64, names);
      REXLOG_WARN("AO4_AI_SLOTS data {:08X} now {}", data, ShaderTextureNames(base, data));
    }
    if (names && depth == nearest && REX_LOAD_U32(data + 0x64) == names) {
      patched.push_back(data);
    }
  }
  REXLOG_WARN("AO4_AI_SLOTS ai blueprint {:08X}: {} data, {} objects visited, {} ms", blueprint,
              found.size(), visited, GetTickCount64() - start);
}

void DumpCustomize(uint8_t* base, uint32_t spawn_template) {
  static const bool dump = GetEnvironmentVariableW(L"AO4_SKIN_DUMP", nullptr, 0) != 0;
  static std::unordered_set<uint32_t> seen;
  const uint32_t customize = REX_LOAD_U32(spawn_template + kCustomizeSoldierData);
  if (!dump || !seen.insert(customize).second || !IsReadable(base, customize, 0x88)) {
    return;
  }
  DumpObject(base, "customize", customize, 0x88 / 4);
  DumpArray(base, "visualgroups", REX_LOAD_U32(customize + 0x0C), 16);
  // AO4CustomizationAsset (same chain as sub_832EB590).
  const uint32_t root = REX_LOAD_U32(0x841350DC);
  if (IsReadable(base, root + 16, 4)) {
    const uint32_t a = REX_LOAD_U32(root + 16);
    const uint32_t b = IsReadable(base, a + 96, 4) ? REX_LOAD_U32(a + 96) : 0;
    const uint32_t asset = IsReadable(base, b + 212, 4) ? REX_LOAD_U32(b + 212) : 0;
    DumpObject(base, "customization_asset", asset, 0x6C / 4);
    static bool lists_dumped = false;
    if (!lists_dumped && IsReadable(base, asset + 0x68, 4)) {
      lists_dumped = true;
      std::unordered_set<uint32_t> visited;
      DumpTree(base, "outfits", REX_LOAD_U32(asset + 0x68), 3, visited);
      DumpTree(base, "tattoos", REX_LOAD_U32(asset + 0x5C), 3, visited);
    }
    for (uint32_t offset : {0x54u, 0x58u}) {
      if (IsReadable(base, asset + offset, 4)) {
        DumpObject(base, offset == 0x54 ? "alpha_skin" : "bravo_skin",
                   REX_LOAD_U32(asset + offset), 24);
      }
    }
  }
}

// Finds a loaded asset by name: scans committed guest heap memory for the name string, then
// for an object whose Asset::Name points at it. Slow, run once.
std::vector<uint32_t> FindAssetsByName(uint8_t* base, const std::string& name) {
  const std::string needle = name + std::string(1, char(0));
  std::vector<uint32_t> strings;
  for (uint32_t address = 0x40000000; address < 0x80000000 && strings.size() < 8;) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(base + address, &info, sizeof(info))) {
      break;
    }
    const uint32_t region_end = address + uint32_t(info.RegionSize);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      const char* begin = reinterpret_cast<const char*>(base + address);
      const char* end = begin + info.RegionSize;
      for (const char* hit = std::search(begin, end, needle.begin(), needle.end()); hit != end;
           hit = std::search(hit + 1, end, needle.begin(), needle.end())) {
        strings.push_back(address + uint32_t(hit - begin));
      }
    }
    address = region_end;
  }
  // Asset objects sit right before their name in the same allocation in practice; scan a
  // window before each string for an object whose +8 points at it.
  std::vector<uint32_t> objects;
  for (uint32_t text : strings) {
    for (uint32_t object = (text & ~3u) - 4; object + 0x400 > text && object > 0x40000000;
         object -= 4) {
      if (IsReadable(base, object, 12) && REX_LOAD_U32(object + kAssetName) == text) {
        objects.push_back(object);
        break;
      }
    }
  }
  if (objects.empty()) {
    REXLOG_WARN("AO4_LOOK_SWAP asset {} not found ({} name strings)", name, strings.size());
  }
  return objects;
}

uint32_t FindAssetByName(uint8_t* base, const std::string& name) {
  const auto objects = FindAssetsByName(base, name);
  return objects.empty() ? 0 : objects.front();
}

// Replaces every pointer to `from` with `to` in a Frostbite array of asset pointers.
void ReplaceInArray(uint8_t* base, uint32_t array, uint32_t from, uint32_t to) {
  if (!from || !to || !IsReadable(base, array - 4, 4)) {
    return;
  }
  for (uint32_t i = 0; i < REX_LOAD_U32(array - 4) && i < 16; ++i) {
    if (REX_LOAD_U32(array + i * 4) == from) {
      REX_STORE_U32(array + i * 4, to);
    }
  }
}

// AO4CustomizationAsset (same chain as sub_832EB590), or 0.
uint32_t CustomizationAsset(uint8_t* base) {
  const uint32_t root = REX_LOAD_U32(0x841350DC);
  const uint32_t a = IsReadable(base, root + 16, 4) ? REX_LOAD_U32(root + 16) : 0;
  const uint32_t b = IsReadable(base, a + 96, 4) ? REX_LOAD_U32(a + 96) : 0;
  return IsReadable(base, b + 212, 4) ? REX_LOAD_U32(b + 212) : 0;
}

// Description lists (outfits, tattoos) hold entries {bundle id, description, unlock asset}.
uint32_t UnlockForBundle(uint8_t* base, uint32_t list, uint32_t bundle_id) {
  const uint32_t array = IsReadable(base, list + 0x0C, 4) ? REX_LOAD_U32(list + 0x0C) : 0;
  if (!IsReadable(base, array - 4, 4)) {
    return 0;
  }
  const uint32_t count = REX_LOAD_U32(array - 4);
  // `count` is the number of 12-byte entries.
  for (uint32_t i = 0; i < count && i < 256 && IsReadable(base, array + i * 12, 12); ++i) {
    if (REX_LOAD_U32(array + i * 12) == bundle_id) {
      return REX_LOAD_U32(array + i * 12 + 8);
    }
  }
  return 0;
}

// New Frostbite-style array (count word, then elements) in the guest system heap.
uint32_t NewGuestArray(uint8_t* base, const std::vector<uint32_t>& elements) {
  const uint32_t size = 4 + uint32_t(elements.size()) * 4;
  const uint32_t block = rex::system::kernel_memory()->SystemHeapAlloc(size + 16);
  if (!block) {
    return 0;
  }
  REX_STORE_U32(block + 12, uint32_t(elements.size()));
  for (size_t i = 0; i < elements.size(); ++i) {
    REX_STORE_U32(block + 16 + uint32_t(i) * 4, elements[i]);
  }
  return block + 16;
}

// AO4_OUTFIT_DUMP: lists every loaded BlueprintAndVariationPair (vtable 82A7B8D8, the part an
// UnlockAsset's LinkedTo points at: +C mesh, +10 skin texture) whose mesh is a character
// mesh, plus the UnlockAssets linking to it. Runs once, a while after the soldiers spawned,
// on its own thread (reads only).
// AO4_UNLOCK_DUMP: every loaded unlock asset (UnlockAssetBase and subclasses) with its
// unlock condition, value, licenses and flags, to find the demo/DLC-bound content. Reads only.
// UnlockAssetBase: +0C UnlockConditionValue, +14 UnlockCondition (0 None, 1 Rank, 2 Contract,
// 3 Level, 4 CampaignComplete, order to be confirmed), +1C Licenses, +28 AutoAvailable,
// +29 HiddenUntilUnlocked.
void ScanUnlocks(uint8_t* base) {
  static const std::pair<uint32_t, const char*> kTypes[] = {
      {0x82A7C0C4, "UnlockAssetBase"},      {0x82A7C0CC, "UnlockAsset"},
      {0x82A7C0D4, "RandomUnlockAsset"},    {0x82A7C0DC, "SoldierWeaponUnlockAsset"},
      {0x82A7C0E4, "ValueUnlockAsset"},     {0x82A9F244, "UnlockableSoldierAbilityAsset"}};
  std::vector<std::pair<uint32_t, const char*>> found;
  for (uint32_t address = 0x40000000; address < 0x80000000;) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(base + address, &info, sizeof(info))) {
      break;
    }
    const uint32_t region_end = address + uint32_t(info.RegionSize);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      for (uint32_t a = address; a + 0x40 <= region_end && found.size() < 8192; a += 4) {
        const uint32_t value = REX_LOAD_U32(a);
        for (const auto& [vtable, type] : kTypes) {
          if (value == vtable) {
            found.emplace_back(a, type);
          }
        }
      }
    }
    address = region_end;
  }
  REXLOG_WARN("AO4_UNLOCK scan: {} unlock assets", found.size());
  for (const auto& [object, type] : found) {
    const std::string name = AssetName(base, object);
    if (name.empty()) {
      continue;
    }
    std::string licenses;
    const uint32_t array = REX_LOAD_U32(object + 0x1C);
    if (IsReadable(base, array - 4, 4)) {
      for (uint32_t i = 0; i < REX_LOAD_U32(array - 4) && i < 8; ++i) {
        licenses += " '" + GuestString(base, REX_LOAD_U32(array + i * 4)) + "'";
      }
    }
    std::string words;
    for (uint32_t w = 3; w < 16; ++w) {
      words += fmt::format(" +{:X}={:08X}", w * 4, REX_LOAD_U32(object + w * 4));
    }
    REXLOG_WARN("AO4_UNLOCK {:08X} {} '{}' cond={} value={} auto={} hidden={} licenses=[{}]{}",
                object, type, name, REX_LOAD_U32(object + 0x14), REX_LOAD_U32(object + 0x0C),
                REX_LOAD_U8(object + 0x28), REX_LOAD_U8(object + 0x29), licenses, words);
  }
}

void ScanCharacterParts(uint8_t* base) {
  constexpr uint32_t kPairVtable = 0x82A7B8D8;
  constexpr uint32_t kUnlockVtable = 0x82A7C0CC;
  std::vector<uint32_t> pairs, unlocks;
  for (uint32_t address = 0x40000000; address < 0x80000000;) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(base + address, &info, sizeof(info))) {
      break;
    }
    const uint32_t region_end = address + uint32_t(info.RegionSize);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      for (uint32_t a = address; a + 0x30 <= region_end; a += 4) {
        const uint32_t value = REX_LOAD_U32(a);
        if (value == kPairVtable && pairs.size() < 4096) {
          pairs.push_back(a);
        } else if (value == kUnlockVtable && unlocks.size() < 16384) {
          unlocks.push_back(a);
        }
      }
    }
    address = region_end;
  }
  REXLOG_WARN("AO4_PARTS scan: {} pairs, {} unlocks", pairs.size(), unlocks.size());
  // Every loaded asset whose name is a character path (objects keep their name at +8).
  std::vector<std::pair<uint32_t, std::string>> names;
  for (uint32_t address = 0x40000000; address < 0x80000000 && names.size() < 2048;) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(base + address, &info, sizeof(info))) {
      break;
    }
    const uint32_t region_end = address + uint32_t(info.RegionSize);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      const char* begin = reinterpret_cast<const char*>(base + address);
      const char* end = begin + info.RegionSize;
      for (const char* key : {"haracters/Primary/", "haracters/primary/", "haracters/Outfits/",
                              "haracters/outfits/"}) {
        const std::string needle(key);
        for (const char* hit = std::search(begin, end, needle.begin(), needle.end()); hit != end;
             hit = std::search(hit + 1, end, needle.begin(), needle.end())) {
          const char* start = hit;
          while (start > begin && start[-1] >= 0x20 && start[-1] < 0x7F) {
            --start;
          }
          const char* stop = hit;
          while (stop < end && *stop >= 0x20 && *stop < 0x7F) {
            ++stop;
          }
          names.emplace_back(address + uint32_t(start - begin), std::string(start, stop));
        }
      }
    }
    address = region_end;
  }
  std::unordered_set<std::string> logged;
  for (auto& [text, name] : names) {
    for (uint32_t object = (text & ~3u) - 4; object + 0x400 > text && object > 0x40000000;
         object -= 4) {
      if (IsReadable(base, object, 12) && REX_LOAD_U32(object + 8) == text) {
        if (logged.insert(fmt::format("{:08X}", object)).second) {
          REXLOG_WARN("AO4_PARTS asset {:08X} vt={:08X} {}", object, REX_LOAD_U32(object), name);
        }
        break;
      }
    }
  }
  for (uint32_t pair : pairs) {
    const std::string mesh = AssetName(base, REX_LOAD_U32(pair + 0x0C));
    if (mesh.find("haracters") == std::string::npos) {
      continue;
    }
    std::string owners;
    for (uint32_t unlock : unlocks) {
      const uint32_t linked = REX_LOAD_U32(unlock + 0x2C);
      if (!IsReadable(base, linked - 4, 4)) {
        continue;
      }
      for (uint32_t k = 0; k < REX_LOAD_U32(linked - 4) && k < 8; ++k) {
        if (IsReadable(base, linked + k * 4, 4) && REX_LOAD_U32(linked + k * 4) == pair) {
          owners += fmt::format(" {:08X}({})", unlock, AssetName(base, unlock));
        }
      }
    }
    REXLOG_WARN("AO4_PARTS pair {:08X} mesh={:08X}({}) skin={:08X}({}) owners:{}", pair,
                REX_LOAD_U32(pair + 0x0C), mesh, REX_LOAD_U32(pair + 0x10),
                AssetName(base, REX_LOAD_U32(pair + 0x10)), owners);
  }
}

// AO4_LOOK_PARTS: comma-separated subset of "head,mask,voice,playermask" (default:
// head,mask,voice), to find which
// part of the look swap breaks the prologue cutscene after Fiona_Young spawns.
bool LookPart(const char* part) {
  static const std::string parts = [] {
    wchar_t text[64] = {};
    const DWORD length = GetEnvironmentVariableW(L"AO4_LOOK_PARTS", text, 64);
    std::string value = "head,mask,voice";
    if (length && length < 64) {
      value.clear();
      for (DWORD i = 0; i < length; ++i) {
        value.push_back(char(text[i]));
      }
    }
    return "," + value + ",";
  }();
  return parts.find("," + std::string(part) + ",") != std::string::npos;
}

// Template writes of MaybeSwapLook, undone in reverse order when the Bravo mode changes while
// the level data stays loaded (checkpoint reload). Data that no longer holds the written
// values was reloaded and is not touched again.
struct LookJournal {
  struct Write {
    uint32_t address, old, now;
    bool partner;  // written into the partner's template/customize data
  };
  std::vector<Write> writes;
  static bool ReadWord(uint8_t* base,uint32_t address,uint32_t& value) {
    return Ao4LookWord(address,value,[&](uint32_t a,void* p,size_t n){return ReadLookMemory(base,a,p,n);});
  }
  void Store(uint8_t* base, uint32_t address, uint32_t value, bool partner) {
    uint32_t old=0;
    if (!ReadWord(base,address,old)) return;
    writes.push_back({address, old, value, partner});
    REX_STORE_U32(address, value);
  }
  void Replace(uint8_t* base, uint32_t array, uint32_t from, uint32_t to, bool partner) {
    std::vector<uint32_t> values;
    if (!from || !to || !Ao4LookArray(array,16,values,
        [&](uint32_t a,void* p,size_t n){return ReadLookMemory(base,a,p,n);})) return;
    for (uint32_t i = 0; i < values.size(); ++i) {
      if (values[i] == from) Store(base, array + i * 4, to, partner);
    }
  }
  bool Intact(uint8_t* base) const {
    for (const auto& w : writes) {
      uint32_t value=0;
      if (!ReadWord(base,w.address,value) || value != w.now) return false;
    }
    return true;
  }
  void Undo(uint8_t* base) {
    for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
      REX_STORE_U32(it->address, it->old);
    }
    writes.clear();
  }
  // A checkpoint reload replaces one side's data, while the other side's data survives and
  // still points at the swapped-in assets of the freed side. Restore a side only if all of
  // its writes are still in place (never write into reloaded/freed memory), then forget all.
  uint32_t UndoSurvivors(uint8_t* base, bool player_side, bool partner_side) {
    auto side_intact = [&](bool partner) {
      for (const auto& w : writes) {
        uint32_t value=0;
        if (w.partner == partner && (!ReadWord(base,w.address,value) || value != w.now)) return false;
      }
      return true;
    };
    const bool restore[2] = {player_side && side_intact(false), partner_side && side_intact(true)};
    uint32_t restored = 0;
    for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
      if (!restore[it->partner]) continue;
      REX_STORE_U32(it->address, it->old);
      ++restored;
    }
    writes.clear();
    return restored;
  }
};

// AO4_LOOK_SWAP, Bravo mode on: the AI partner gets Alpha's head, body and mask, the player
// Bravo's head and voice (the player's body comes from the profile outfit and is not touched).
// Bravo mode off: only the partner's Bravo body group is dropped (AO4_AI_ALPHA_OUTFIT), its
// outfit comes at runtime from role_observer.cpp (Bravo's default or the custom gear).
// Called on every observation of the templates (the game asks for them all the time), so the
// templates follow the Bravo mode in effect, which changes only while no soldiers exist
// (main menu, loading, checkpoint reload; role_observer.cpp).
struct LookPair {
  uint32_t player_customize = 0, partner_customize = 0;
  uint32_t player_template = 0, partner_template = 0;
  int applied = -1;  // -1 nothing, 0 plain (Bravo mode off), 1 Bravo mode
  LookJournal journal;
};
LookPair look_pairs[4];  // standard, RR, RRChopper, Tutorial
std::recursive_mutex look_lock;  // spawner observation and customize application threads

void MaybeSwapLook(uint8_t* base, uint32_t spawn_template, const std::string& name) {
  static const bool swap = GetEnvironmentVariableW(L"AO4_LOOK_SWAP", nullptr, 0) != 0;
  if (!swap) return;
  std::lock_guard<std::recursive_mutex> look_guard(look_lock);
  const bool rr = name == "Gameplay/Spawners/AO2SoldierSpawnTemplate_RR" ||
                  name == "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RR";
  const bool chopper = name == "Gameplay/Spawners/AO2SoldierSpawnTemplate_RRChopper" ||
                       name == "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RRChopper";
  const bool tutorial = name == "Gameplay/Spawners/AO2SoldierSpawnTemplate_Tutorial" ||
                        name == "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_Tutorial";
  const bool is_player = name == kPlayerTemplate ||
                        name == "Gameplay/Spawners/AO2SoldierSpawnTemplate_RR" ||
                        name == "Gameplay/Spawners/AO2SoldierSpawnTemplate_RRChopper" ||
                        name == "Gameplay/Spawners/AO2SoldierSpawnTemplate_Tutorial";
  const bool is_partner = name == kPartnerTemplate ||
                         name == "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RR" ||
                         name == "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RRChopper" ||
                         name == "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_Tutorial";
  if (!is_player && !is_partner) return;
  const auto& variant = kAo4LookVariants[tutorial ? 3 : chopper ? 2 : rr ? 1 : 0];
  auto& pair = look_pairs[tutorial ? 3 : chopper ? 2 : rr ? 1 : 0];
  auto read = [&](uint32_t a,void* p,size_t n){return ReadLookMemory(base,a,p,n);};
  auto asset_name = [&](uint32_t a){return AssetName(base,a);};
  auto current = [&](bool partner,uint32_t tpl,uint32_t data) {
    return Ao4LookSideCurrent(variant,partner,tpl,data,read,asset_name);
  };
  uint32_t customize=0;
  if (!Ao4LookWord(spawn_template+kCustomizeSoldierData,customize,read) ||
      !current(is_partner,spawn_template,customize)) return;
  // Validate BOTH cached sides on every observation, including the unchanged-address
  // reload case. The crash dump had a live partner but a reused player allocation:
  // player_customize+12 contained 5F506879. Never traverse that cached object.
  const bool player_current=current(false,pair.player_template,pair.player_customize) &&
      (!is_player || (pair.player_template==spawn_template && pair.player_customize==customize));
  const bool partner_current=current(true,pair.partner_template,pair.partner_customize) &&
      (!is_partner || (pair.partner_template==spawn_template && pair.partner_customize==customize));
  if ((pair.player_customize && !player_current) || (pair.partner_customize && !partner_current)) {
    const uint32_t restored=pair.journal.UndoSurvivors(base,player_current,partner_current);
    REXLOG_WARN("AO4_LOOK_SWAP stale templates invalidated player={} partner={} surviving writes restored={}",
                !player_current,!partner_current,restored);
    if (!player_current) {pair.player_customize=0;pair.player_template=0;}
    if (!partner_current) {pair.partner_customize=0;pair.partner_template=0;}
    pair.applied=-1;
  }
  if (is_player) {pair.player_customize=customize;pair.player_template=spawn_template;}
  else {pair.partner_customize=customize;pair.partner_template=spawn_template;}
  if (!pair.player_customize || !pair.partner_customize) return;
  const int wanted = ao4_bravo_mode ? 1 : 0;
  if (pair.applied >= 0 && !pair.journal.Intact(base)) {
    // Reloaded data has its original values back; a side that survived is restored.
    const uint32_t restored = pair.journal.UndoSurvivors(base, true, true);
    REXLOG_WARN("AO4_LOOK_SWAP data reloaded, surviving writes restored={}", restored);
    pair.applied = -1;
  }
  if (pair.applied == wanted) {
    return;
  }
  const bool fresh = pair.applied < 0;  // new or reloaded data, not just a mode change
  if (pair.applied >= 0) {
    pair.journal.Undo(base);
    REXLOG_WARN("AO4_LOOK_SWAP {} variant undone", pair.applied ? "Bravo" : "plain");
    pair.applied = -1;
  }
  const uint32_t player_customize = pair.player_customize, partner_customize = pair.partner_customize;
  const uint32_t player_template = pair.player_template, partner_template = pair.partner_template;
  // Group arrays: player group 0 = [AlphaHead, BravoHead]; partner = [BravoHead], [BravoMesh].
  uint32_t player_groups=0,partner_groups=0,alpha_mask=0,bravo_mask=0;
  std::vector<uint32_t> players,heads,partners;
  std::vector<std::vector<uint32_t>> partner_parts;
  if (!Ao4LookWord(player_customize+12,player_groups,read) ||
      !Ao4LookWord(partner_customize+12,partner_groups,read) ||
      !Ao4LookWord(player_customize+kMaskAsset,alpha_mask,read) ||
      !Ao4LookWord(partner_customize+kMaskAsset,bravo_mask,read) ||
      !Ao4LookArray(player_groups,8,players,read) ||
      !Ao4LookArray(players[0],16,heads,read) || heads.size()<2 ||
      !Ao4LookArray(partner_groups,8,partners,read)) return;
  for (uint32_t group : partners) {
    std::vector<uint32_t> parts;
    if (!Ao4LookArray(group,16,parts,read)) return;
    partner_parts.push_back(std::move(parts));
  }
  const uint32_t player_heads=players[0],alpha_head=heads[0],bravo_head=heads[1];
  if (AssetName(base, alpha_head) != "Gameplay/Spawners/AlphaHeadMeshUnlockAsset" ||
      AssetName(base, bravo_head) != "Gameplay/Spawners/BravoHeadMeshUnlockAsset") {
    REXLOG_WARN("AO4_LOOK_SWAP unexpected player head group, deferred");
    return;
  }
  pair.applied = wanted;
  if (fresh) {
    // A fresh level or checkpoint load: no mission stage yet. The Drogenlabor's own stages
    // (Scavenge customize data) follow shortly; until the gear comes back the AI gear may only
    // change tattoo and, in the mask stage, the mask.
    ao4_scavenge_level =
        FindAssetByName(base, "Gameplay/Spawners/AO2_Scavenge_Partner_CustomizeData") != 0;
    ao4_scavenge_appearance = false;
    ao4_mission_appearance_revision.fetch_add(1);
    REXLOG_WARN("AO4_LOOK_SWAP fresh templates, scavenge level={}", ao4_scavenge_level.load());
  }
  REXLOG_WARN("AO4_LOOK_SWAP matched templates variant={} player={:08X} partner={:08X} mode={}",
              tutorial ? "Tutorial" : chopper ? "RRChopper" : rr ? "RR" : "standard",
              player_template, partner_template, wanted ? "Bravo" : "plain");
  uint32_t bravo_body = 0;
  for (const auto& parts : partner_parts) {
    for (uint32_t part : parts) {
      const std::string body_name = AssetName(base, part);
      if (body_name == "Gameplay/Spawners/BravoMeshUnlockAsset" ||
          (tutorial && body_name == "Gameplay/Spawners/BravoMesh_Tutorial_UnlockAsset")) bravo_body=part;
    }
  }
  // Tutorial: each has an own training body in its own group (player AlphaMesh_Tutorial,
  // partner BravoMesh_Tutorial); there is no AlphaMeshUnlockAsset to find.
  uint32_t alpha_body = 0;
  std::vector<uint32_t> player_body_groups;
  if (tutorial) {
    for (uint32_t group : players) {
      std::vector<uint32_t> parts;
      if (!Ao4LookArray(group,16,parts,read)) continue;
      for (uint32_t part : parts) {
        if (AssetName(base, part) == "Gameplay/Spawners/AlphaMesh_Tutorial_UnlockAsset") {
          alpha_body = part;
          player_body_groups.push_back(group);
        }
      }
    }
  } else {
    alpha_body = FindAssetByName(base, "Gameplay/Spawners/AlphaMeshUnlockAsset");
  }
  if (wanted && LookPart("head")) {
    for (uint32_t group : partners) {
      pair.journal.Replace(base, group, bravo_head, alpha_head, true);
      pair.journal.Replace(base, group, bravo_body, alpha_body, true);
    }
    pair.journal.Replace(base, player_heads, alpha_head, bravo_head, false);
    // Bravo mode in the tutorial: the human (Bravo) wears Bravo's training body.
    for (uint32_t group : player_body_groups) {
      pair.journal.Replace(base, group, alpha_body, bravo_body, false);
    }
  }
  // AO4_AI_ALPHA_OUTFIT (role_observer.cpp) puts the outfit on the AI partner at runtime (Alpha's
  // or Bravo's default, or the custom gear), like the player's body comes from the profile
  // outfit. Drop the Bravo body group from the partner's visual groups, otherwise both bodies
  // render inside each other.
  // Not in the tutorial: there both keep (swapped) training bodies, no outfit is added.
  if (GetEnvironmentVariableW(L"AO4_AI_ALPHA_OUTFIT", nullptr, 0) && bravo_body && !tutorial) {
    std::vector<uint32_t> kept;
    const uint32_t count = uint32_t(partners.size());
    for (uint32_t i=0;i<count;++i) {
      // Use the pre-write snapshot: Replace above may have changed Bravo to Alpha.
      bool has_body=false;
      for (uint32_t part : partner_parts[i]) has_body |= part==bravo_body || (alpha_body && part==alpha_body);
      if (!has_body) kept.push_back(partners[i]);
    }
    if (!kept.empty() && kept.size() < count) {
      const uint32_t groups = NewGuestArray(base, kept);
      if (groups) {
        pair.journal.Store(base, partner_customize + 0x0C, groups, true);
        REXLOG_WARN("AO4_LOOK_SWAP partner body group removed ({} of {} groups kept)", kept.size(),
                    count);
      }
    }
  }
  if (!wanted) {
    REXLOG_WARN("AO4_LOOK_SWAP done (Bravo mode off): bravo_body={:08X}", bravo_body);
    return;
  }
  // Alpha's default outfit and tattoo for the AI partner: its body group becomes
  // [outfit unlock, tattoo unlock] (the player's body comes from these through the profile).
  // The global pointer chain is not set up yet when the soldiers spawn; fall back to the
  // asset's name.
  uint32_t asset = CustomizationAsset(base);
  if (!asset) {
    asset = FindAssetByName(base, "AO4_Customization/AO4CustomizationAsset");
  }
  const uint32_t alpha_outfit =
      asset ? UnlockForBundle(base, REX_LOAD_U32(asset + 0x68), REX_LOAD_U32(asset + 0x4C)) : 0;
  const uint32_t alpha_tattoo =
      asset ? UnlockForBundle(base, REX_LOAD_U32(asset + 0x5C), REX_LOAD_U32(asset + 0x60)) : 0;
  REXLOG_WARN("AO4_LOOK_SWAP customization asset={:08X} outfits={:08X} array={:08X} id={:08X} "
              "tattoos={:08X} id={:08X}",
              asset, asset ? REX_LOAD_U32(asset + 0x68) : 0,
              asset ? REX_LOAD_U32(REX_LOAD_U32(asset + 0x68) + 0x0C) : 0,
              asset ? REX_LOAD_U32(asset + 0x4C) : 0, asset ? REX_LOAD_U32(asset + 0x5C) : 0,
              asset ? REX_LOAD_U32(asset + 0x60) : 0);
  REXLOG_WARN("AO4_LOOK_SWAP alpha outfit={:08X} ({}) tattoo={:08X} ({})", alpha_outfit,
              AssetName(base, alpha_outfit), alpha_tattoo, AssetName(base, alpha_tattoo));
  // AO4_OUTFIT_DUMP: what the outfit unlocks and the AI's body unlock point at (LinkedTo),
  // to find the parts that give the AI partner Alpha's body.
  if (GetEnvironmentVariableW(L"AO4_OUTFIT_DUMP", nullptr, 0)) {
    std::thread([base] {
      Sleep(60000);
      ScanCharacterParts(base);
    }).detach();
    constexpr uint32_t kBravoOutfitBundle = 0xB5CC544E;
    const uint32_t bravo_outfit =
        asset ? UnlockForBundle(base, REX_LOAD_U32(asset + 0x68), kBravoOutfitBundle) : 0;
    std::unordered_set<uint32_t> visited;
    for (auto [label, object] : {std::pair{"alpha_outfit", alpha_outfit},
                                 std::pair{"bravo_outfit", bravo_outfit},
                                 std::pair{"bravo_body", bravo_body},
                                 std::pair{"alpha_head", alpha_head},
                                 std::pair{"bravo_head", bravo_head},
                                 std::pair{"alpha_tattoo", alpha_tattoo}}) {
      if (object) {
        DumpTree(base, label, object, 3, visited);
      }
    }
  }
  // Optional (AO4_LOOK_OUTFIT): replacing the base body with the outfit alone left the AI
  // invisible, so the variant keeps the base body and adds outfit and tattoo on top.
  static const bool outfit = GetEnvironmentVariableW(L"AO4_LOOK_OUTFIT", nullptr, 0) != 0;
  if (outfit && alpha_outfit && alpha_tattoo) {
    for (uint32_t i=0;i<partners.size();++i) {
      if (partner_parts[i].size()==1 && partner_parts[i][0]==bravo_body) {
        const uint32_t body=NewGuestArray(base,{bravo_body,alpha_outfit,alpha_tattoo});
        if (body) pair.journal.Store(base,partner_groups+i*4,body,true);
      }
    }
  }
  // The AI partner wears Alpha's mask. The player keeps the mask from the profile (it can be
  // changed in the menu); "playermask" restores the old default mask swap for the player.
  if (LookPart("mask")) {
    pair.journal.Store(base, partner_customize + kMaskAsset, alpha_mask, true);
  }
  if (LookPart("playermask")) {
    pair.journal.Store(base, player_customize + kMaskAsset, bravo_mask, false);
  }
  // Voice: the player speaks as Bravo, the AI partner as Alpha ("Alpha" is an image string).
  constexpr uint32_t kAlphaString = 0x82A62DFC;
  REXLOG_WARN("AO4_LOOK_SWAP voice before: player='{}' partner='{}' player_vo='{}' partner_vo='{}'",
              GuestString(base, REX_LOAD_U32(player_template + kVocalizationPackage)),
              GuestString(base, REX_LOAD_U32(partner_template + kVocalizationPackage)),
              GuestString(base, REX_LOAD_U32(player_customize + 0x30)),
              GuestString(base, REX_LOAD_U32(partner_customize + 0x30)));
  // Chopper and tutorial player templates have empty packages in the original data.
  // Do not turn an intentional silent template into an invented voice assignment.
  if (!chopper && !tutorial && LookPart("voice") && GuestString(base, kAlphaString) == "Alpha") {
    const uint32_t partner_voice = REX_LOAD_U32(partner_template + kVocalizationPackage);
    pair.journal.Store(base, player_template + kVocalizationPackage, partner_voice, false);
    pair.journal.Store(base, partner_template + kVocalizationPackage, kAlphaString, true);
  }
  REXLOG_WARN("AO4_LOOK_SWAP parts: head={} mask={} voice={} playermask={}", LookPart("head"),
              LookPart("mask"), LookPart("voice"), LookPart("playermask"));
  REXLOG_WARN("AO4_LOOK_SWAP done: alpha_head={:08X} bravo_head={:08X} alpha_body={:08X} "
              "bravo_body={:08X} alpha_mask={:08X} bravo_mask={:08X}",
              alpha_head, bravo_head, alpha_body, bravo_body, alpha_mask, bravo_mask);
}

// A level transition can spawn the soldiers before their spawner's template was observed
// (ObserveSpawner runs on another thread), so the meshes were chosen from unswapped data.
// The original customize application (8313DE78) is the last point before that choice: make
// sure the swap covers the template owning this customize data, finding it by name if needed.
void EnsureLookSwapForCustomize(uint8_t* base, uint32_t data, const std::string& name) {
  static const bool swap = GetEnvironmentVariableW(L"AO4_LOOK_SWAP", nullptr, 0) != 0;
  if (!swap) return;
  struct Variant { const char* customize; const char* spawn_template; };
  static const Variant variants[8] = {
      {"Gameplay/Spawners/AO2PlayerCustomizeData", kPlayerTemplate},
      {"AO4JawBreaker/Gameplay/Spawners/AO2DefaultPlayerCustomizeData_RR",
       "Gameplay/Spawners/AO2SoldierSpawnTemplate_RR"},
      {"Gameplay/Spawners/AO2DefaultPlayerCustomizeData_RRChopper",
       "Gameplay/Spawners/AO2SoldierSpawnTemplate_RRChopper"},
      {"Gameplay/Spawners/AO2PlayerCustomizeData_Tutorial",
       "Gameplay/Spawners/AO2SoldierSpawnTemplate_Tutorial"},
      {"Gameplay/Spawners/PartnerCustomizeAIData", kPartnerTemplate},
      {"Gameplay/Spawners/PartnerCustomizeAIData_RR",
       "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RR"},
      {"Gameplay/Spawners/PartnerCustomizeAIData_RRChopper",
       "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RRChopper"},
      {"Gameplay/Spawners/PAI_CustomizeAIData_Tutorial",
       "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_Tutorial"}};
  int index = -1;
  for (int i = 0; i < 8; ++i) {
    if (name == variants[i].customize) index = i;
  }
  if (index < 0) return;
  std::lock_guard<std::recursive_mutex> look_guard(look_lock);
  const bool partner = index >= 4;
  LookPair& pair = look_pairs[index % 4];
  const uint32_t known = partner ? pair.partner_customize : pair.player_customize;
  const auto& variant=kAo4LookVariants[index%4];
  auto read=[&](uint32_t a,void* p,size_t n){return ReadLookMemory(base,a,p,n);};
  auto asset_name=[&](uint32_t a){return AssetName(base,a);};
  if (known == data && pair.applied >= 0 &&
      Ao4LookSideCurrent(variant,false,pair.player_template,pair.player_customize,read,asset_name) &&
      Ao4LookSideCurrent(variant,true,pair.partner_template,pair.partner_customize,read,asset_name) &&
      pair.journal.Intact(base)) return;
  // A side counts as current if its template still names the expected template/customize.
  auto current = [&](const Variant& v, uint32_t spawn_template) {
    return spawn_template && IsReadable(base, spawn_template, 0x2C) &&
           AssetName(base, spawn_template) == v.spawn_template &&
           AssetName(base, REX_LOAD_U32(spawn_template + kCustomizeSoldierData)) == v.customize;
  };
  // The template owning exactly this customize data.
  const Variant& own = variants[index];
  uint32_t own_template = 0;
  for (uint32_t candidate : FindAssetsByName(base, own.spawn_template)) {
    if (IsReadable(base, candidate, 0x2C) &&
        REX_LOAD_U32(candidate + kCustomizeSoldierData) == data) own_template = candidate;
  }
  if (!own_template) {
    REXLOG_WARN("AO4_LOOK_SWAP spawn-time: no template for {} data={:08X}", name, data);
    return;
  }
  // The other side: keep the observed one if still valid, else a unique loaded match.
  const Variant& other = variants[partner ? index - 4 : index + 4];
  uint32_t other_template = partner ? pair.player_template : pair.partner_template;
  if (!current(other, other_template)) {
    other_template = 0;
    uint32_t matches = 0;
    for (uint32_t candidate : FindAssetsByName(base, other.spawn_template)) {
      if (current(other, candidate)) { other_template = candidate; ++matches; }
    }
    if (matches != 1) {
      REXLOG_WARN("AO4_LOOK_SWAP spawn-time: {} candidates for {}, not swapped",
                  matches, other.spawn_template);
      return;
    }
  }
  REXLOG_WARN("AO4_LOOK_SWAP spawn-time: {} data={:08X} template={:08X} other={:08X}", name,
              data, own_template, other_template);
  // A side whose template changed resets the pair (dropping the other side), so register own,
  // then other, then own again: afterwards both are registered and the swap is applied.
  MaybeSwapLook(base, own_template, own.spawn_template);
  MaybeSwapLook(base, other_template, other.spawn_template);
  MaybeSwapLook(base, own_template, own.spawn_template);
}

// 8313DE78 also receives profile/spawn COPIES, not the template's customize pointer.
// Normalize their actual visual groups before the original chooses the meshes. Use private
// arrays: changing a shared source group here would invalidate the template rollback journal.
void NormalizeAppliedLook(uint8_t* base, uint32_t data, const std::string& name) {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_LOOK_SWAP", nullptr, 0) != 0;
  if (!enabled || !IsReadable(base, data, kMaskAsset + 4)) return;
  const bool partner = name == "Gameplay/Spawners/PartnerCustomizeAIData" ||
      name == "Gameplay/Spawners/PartnerCustomizeAIData_RR" ||
      name == "Gameplay/Spawners/PartnerCustomizeAIData_RRChopper" ||
      name == "Gameplay/Spawners/PAI_CustomizeAIData_Tutorial";
  const bool player = name == "Gameplay/Spawners/AO2PlayerCustomizeData" ||
      name == "AO4JawBreaker/Gameplay/Spawners/AO2DefaultPlayerCustomizeData_RR" ||
      name == "Gameplay/Spawners/AO2DefaultPlayerCustomizeData_RRChopper" ||
      name == "Gameplay/Spawners/AO2PlayerCustomizeData_Tutorial";
  if (!partner && !player) return;  // retain story-controlled DrugLab stages and FrontEnd
  std::lock_guard<std::recursive_mutex> guard(look_lock);
  // Canonical templates remain under the existing transactional rollback policy.
  for (const auto& pair : look_pairs)
    if (data == pair.player_customize || data == pair.partner_customize) return;
  const uint32_t groups = REX_LOAD_U32(data + 12);
  if (!IsReadable(base, groups - 4, 4)) return;
  const uint32_t count = REX_LOAD_U32(groups - 4);
  if (!count || count > 8 || !IsReadable(base, groups, count * 4)) return;
  const bool bravo = ao4_bravo_mode.load();
  const bool strip_body = partner &&
      GetEnvironmentVariableW(L"AO4_AI_ALPHA_OUTFIT", nullptr, 0) != 0;
  std::map<std::string, uint32_t> replacements;
  auto resolve = [&](const std::string& target) {
    auto [it, inserted] = replacements.try_emplace(target, 0);
    if (inserted) it->second = FindAssetByName(base, target);
    return it->second;
  };
  bool changed = false;
  std::vector<std::vector<uint32_t>> visuals;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t group = REX_LOAD_U32(groups + i * 4);
    if (!IsReadable(base, group - 4, 4)) return;
    const uint32_t n = REX_LOAD_U32(group - 4);
    if (!n || n > 16 || !IsReadable(base, group, n * 4)) return;
    std::vector<uint32_t> elements;
    for (uint32_t j = 0; j < n; ++j) {
      uint32_t asset = REX_LOAD_U32(group + j * 4);
      const std::string current = AssetName(base, asset);
      if (strip_body && Ao4DefaultPartnerBody(current)) { changed = true; continue; }
      const std::string target(Ao4AppliedIdentity(current, partner, bravo));
      if (target != current && LookPart("head")) {
        const uint32_t replacement = resolve(target);
        if (!replacement) return;  // publish nothing if a required identity asset is absent
        asset = replacement; changed = true;
      }
      elements.push_back(asset);
    }
    if (!elements.empty()) visuals.push_back(std::move(elements));
  }
  uint32_t replacement_groups = groups;
  if (changed) {
    if (visuals.empty()) return;
    std::vector<uint32_t> new_groups;
    for (const auto& elements : visuals) {
      const uint32_t group = NewGuestArray(base, elements);
      if (!group) return;
      new_groups.push_back(group);
    }
    replacement_groups = NewGuestArray(base, new_groups);
    if (!replacement_groups) return;
  }
  const uint32_t mask = REX_LOAD_U32(data + kMaskAsset);
  const std::string mask_name = AssetName(base, mask);
  const std::string mask_target(Ao4AppliedIdentity(mask_name, partner, bravo));
  uint32_t replacement_mask = mask;
  if (partner && LookPart("mask") && mask_target != mask_name) {
    replacement_mask = resolve(mask_target);
    if (!replacement_mask) return;
  }
  if (replacement_groups == groups && replacement_mask == mask) return;
  REX_STORE_U32(data + 12, replacement_groups);
  REX_STORE_U32(data + kMaskAsset, replacement_mask);
  REXLOG_WARN("AO4_LOOK_APPLIED source={} data={:08X} partner={} bravo={} groups={:08X}->{:08X} "
              "mask={:08X}->{:08X}; private visual arrays, profile/story fields retained",
              name, data, partner, bravo, groups, replacement_groups, mask, replacement_mask);
}

// Swaps the identity fields once both templates are known.
void MaybeSwapIdentity(uint8_t* base, uint32_t spawn_template, const std::string& name) {
  static const bool swap = GetEnvironmentVariableW(L"AO4_ROLE_SWAP", nullptr, 0) != 0;
  static uint32_t player_template = 0, partner_template = 0;
  static bool swapped = false;
  if (!swap || swapped) {
    return;
  }
  if (name == kPlayerTemplate) {
    player_template = spawn_template;
  } else if (name == kPartnerTemplate) {
    partner_template = spawn_template;
  }
  if (!player_template || !partner_template) {
    return;
  }
  for (uint32_t offset : {kCustomizeSoldierData, kVocalizationPackage}) {
    const uint32_t player_value = REX_LOAD_U32(player_template + offset);
    REX_STORE_U32(player_template + offset, REX_LOAD_U32(partner_template + offset));
    REX_STORE_U32(partner_template + offset, player_value);
  }
  swapped = true;
  REXLOG_WARN("AO4_ROLE_SWAP identity swapped: player template {:08X} <-> partner template {:08X}",
              player_template, partner_template);
  LogTemplate(base, player_template);
  LogTemplate(base, partner_template);
}

// AO4_UNLOCK_ALL, missions: UICoopLevelDescription (vtable 82C02D1C) +0x10 UnlockedByLevels
// (the levels whose completion unlocks this one) is emptied, so no level waits for another.
// A background thread scans the guest heap every 30 s (menus and levels load new data); every
// change is logged once. (The engine's developer switch SyncedAO4Settings.AllUnlocksUnlocked
// +0x18 is dead in this build: only the network sync copies it, nothing reads it. Licensed
// unlocks are handled by the sub_83272A38 hook below.)
// AO4_COSMETICS_BUYABLE (launcher option): masks, outfits and tattoos can be bought from the
// start regardless of rank or campaign progress; they are cosmetic. See the sub_83301E48 hook.
bool CosmeticUnlockName(const std::string& name) {
  std::string lower = name;
  for (char& c : lower) c = char(std::tolower(uint8_t(c)));
  return lower.rfind("ao4_customization/masks/", 0) == 0 || lower.rfind("ao4_customization/outfits/", 0) == 0 ||
         lower.rfind("ao4_customization/tattoos/", 0) == 0;
}

// AO4_UNLOCK_ALL levels and AO4_COSMETICS_BUYABLE share one background scan of the guest heap
// every 30 s (menus and levels load new data); every change is logged once.
void UnlockScan(uint8_t* base, bool levels_on) {
  constexpr uint32_t kCoopLevelDescription = 0x82C02D1C;
  for (;;) {
    uint32_t levels = 0;
    for (uint32_t address = 0x40000000; address < 0x80000000;) {
      MEMORY_BASIC_INFORMATION info = {};
      if (!VirtualQuery(base + address, &info, sizeof(info))) {
        break;
      }
      const uint32_t region_end = address + uint32_t(info.RegionSize);
      if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        for (uint32_t a = address; a + 0x30 <= region_end; a += 4) {
          const uint32_t vtable = REX_LOAD_U32(a);
          (void)levels_on;
          if (!levels_on || vtable != kCoopLevelDescription) {
            continue;
          }
          const uint32_t array = REX_LOAD_U32(a + 0x10);
          const uint32_t count = IsReadable(base, array - 4, 4) ? REX_LOAD_U32(array - 4) : 0;
          if (count && count < 64) {
            std::string list;
            for (uint32_t i = 0; i < count && i < 16; ++i) {
              list += " '" + GuestString(base, REX_LOAD_U32(array + i * 4)) + "'";
            }
            REXLOG_WARN("AO4_UNLOCK_ALL level {:08X} debriefing='{}' unlocked_by={}:{} -> 0", a,
                        GuestString(base, REX_LOAD_U32(a + 0x08)), count, list);
            REX_STORE_U32(array - 4, 0);
            ++levels;
          }
        }
      }
      address = region_end;
    }
    if (levels) {
      REXLOG_WARN("AO4_UNLOCK_ALL pass: {} levels unlocked", levels);
    }
    Sleep(30000);
  }
}

void ObserveSpawner(uint32_t instance, uint32_t caller, uint8_t* base) {
  static std::once_flag unlock_scan;
  const bool levels_on = GetEnvironmentVariableW(L"AO4_UNLOCK_ALL", nullptr, 0) != 0 ||
                         GetEnvironmentVariableW(L"AO4_UNLOCK_MISSIONS", nullptr, 0) != 0;
  if (levels_on) {
    std::call_once(unlock_scan, [base] { std::thread(UnlockScan, base, true).detach(); });
  }
  static std::once_flag unlock_dump;
  if (GetEnvironmentVariableW(L"AO4_UNLOCK_DUMP", nullptr, 0)) {
    std::call_once(unlock_dump, [base] {
      std::thread([base] {
        Sleep(30000);
        ScanUnlocks(base);
      }).detach();
    });
  }
  static const bool enabled = GetEnvironmentVariableW(L"AO4_SOLDIER_TRACE", nullptr, 0) != 0 ||
                              GetEnvironmentVariableW(L"AO4_LOOK_SWAP", nullptr, 0) != 0 ||
                              GetEnvironmentVariableW(L"AO4_ROLE_SWAP", nullptr, 0) != 0;
  if ((!enabled && !Ao4Qol().gear_reload_fix) || instance < 0x40000000) {
    return;
  }
  const uint32_t vtable = REX_LOAD_U32(instance);
  if (vtable != kCharacterSpawnVtable && vtable != kBotSpawnVtable) {
    return;
  }
  // Human player spawn: the load context for the QoL equipment check (also without mods).
  if (vtable == kCharacterSpawnVtable && REX_LOAD_U32(instance + kPlayerType) == 0) {
    const uint32_t tpl = REX_LOAD_U32(instance + kTemplate);
    if (IsReadable(base, tpl, 0x10)) {
      const std::string name = AssetName(base, tpl);
      if (name.find("FrontEnd") == std::string::npos) {
        ao4_tutorial_level = name == "Gameplay/Spawners/AO2SoldierSpawnTemplate_Tutorial";
        ao4_player_spawn_epoch.fetch_add(1);
      }
    }
  }
  if (!enabled) {
    return;
  }
  // Main menu or level, for the status overlay: the menu spawns only the FrontEnd template.
  if (const uint32_t tpl = REX_LOAD_U32(instance + kTemplate); IsReadable(base, tpl, 0x10)) {
    const std::string tpl_name = AssetName(base, tpl);
    if (!tpl_name.empty()) ao4_frontend = tpl_name.find("FrontEnd") != std::string::npos;
  }
  static std::mutex lock;
  static std::map<uint32_t, std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> seen;
  std::lock_guard<std::mutex> guard(lock);
  const uint32_t spawn_template = REX_LOAD_U32(instance + kTemplate);
  // Every time: reloaded data with the same signature must be patched again.
  if (IsReadable(base, spawn_template, 0x2C)) {
    MaybeSwapLook(base, spawn_template, AssetName(base, spawn_template));
  }
  auto signature = [&]() {
    const uint32_t customize = IsReadable(base, spawn_template, 0x2C)
        ? REX_LOAD_U32(spawn_template + kCustomizeSoldierData) : 0;
    const uint32_t groups = IsReadable(base, customize, 16) ? REX_LOAD_U32(customize + 12) : 0;
    const uint32_t head_group = IsReadable(base, groups, 4) ? REX_LOAD_U32(groups) : 0;
    const uint32_t head = IsReadable(base, head_group, 4) ? REX_LOAD_U32(head_group) : 0;
    const uint32_t voice = IsReadable(base, spawn_template, 0x2C)
        ? REX_LOAD_U32(spawn_template + kVocalizationPackage) : 0;
    return std::make_tuple(spawn_template, customize, head, voice);
  };
  const auto before = signature();
  auto it = seen.find(instance);
  if (it != seen.end() && it->second == before) return;
  REXLOG_WARN("AO4_SPAWNER data={:08X} kind={} player_type={} blueprint={} caller={:08X}", instance,
              vtable == kBotSpawnVtable ? "bot" : "character", REX_LOAD_U32(instance + kPlayerType),
              AssetName(base, REX_LOAD_U32(instance + kBlueprint)), caller);
  MaybeAddAiShaderSlots(base, REX_LOAD_U32(instance + kBlueprint),
                        AssetName(base, REX_LOAD_U32(instance + kBlueprint)));
  if (IsReadable(base, spawn_template, 0x2C)) {
    LogTemplate(base, spawn_template);
    DumpCustomize(base, spawn_template);
    MaybeSwapIdentity(base, spawn_template, AssetName(base, spawn_template));
  }
  seen[instance] = signature();
}
}  // namespace

// CharacterSpawnReferenceObjectData::getType
REX_HOOK_RAW(sub_83F33C50) {
  ObserveSpawner(ctx.r3.u32, uint32_t(ctx.lr), base);
  __imp__sub_83F33C50(ctx, base);
}

namespace {
// Called at the original ServerCustomizeSoldierEntity event boundary, not while
// merely enumerating assets. Copy visual arrays; keep story flags and weapons.
bool SwapScavengeVisuals(uint8_t* base, uint32_t data, const std::string& name) {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_LOOK_SWAP", nullptr, 0) != 0;
  if (!enabled || !IsReadable(base, data, 16)) return false;
  const bool partner = name == "Gameplay/Spawners/AO2_Scavenge_Partner_CustomizeData" ||
                       name == "Gameplay/Spawners/AO2_Scavenge_Mask_Partner_CustomizeData";
  const bool player = name == "Gameplay/Spawners/AO2_Scavenge_CustomizeData" ||
                      name == "Gameplay/Spawners/AO2_Scavenge_Mask_CustomizeData";
  if (!partner && !player) return false;
  // data -> (original groups, replacement). Without Bravo mode a swap left over from before
  // (checkpoint reload keeps the mission data) is put back, so the mission meshes are original.
  static std::mutex lock;
  static std::map<uint32_t, std::pair<uint32_t, uint32_t>> patched;
  std::lock_guard<std::mutex> guard(lock);
  const uint32_t groups = REX_LOAD_U32(data + 12);
  const auto it = patched.find(data);
  if (!ao4_bravo_mode) {
    if (it != patched.end() && groups == it->second.second) {
      REX_STORE_U32(data + 12, it->second.first);
      REXLOG_WARN("AO4_SCAVENGE_LOOK source={} data={:08X} swap undone (Bravo mode off)", name, data);
    }
    if (it != patched.end()) patched.erase(it);
    return false;
  }
  if (it != patched.end() && it->second.second == groups) return true;
  if (!IsReadable(base, groups - 4, 4)) return false;
  const uint32_t count = REX_LOAD_U32(groups - 4);
  if (!count || count > 8 || !IsReadable(base, groups, count * 4)) return false;
  const char* names[] = {"Gameplay/Spawners/Alpha_druglabUnlockAsset",
                        "Gameplay/Spawners/Bravo_druglabUnlockAsset",
                        "Gameplay/Spawners/Alpha_druglab_harnessUnlockAsset",
                        "Gameplay/Spawners/Bravo_druglab_harnessUnlockAsset"};
  uint32_t assets[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    assets[i] = FindAssetByName(base, names[i]);
    if (!assets[i]) return false;  // keep original appearance while unavailable
  }
  std::vector<std::vector<uint32_t>> visuals;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t group = REX_LOAD_U32(groups + i * 4);
    if (!IsReadable(base, group - 4, 4)) return false;
    const uint32_t n = REX_LOAD_U32(group - 4);
    if (!n || n > 8 || !IsReadable(base, group, n * 4)) return false;
    std::vector<uint32_t> elements;
    for (uint32_t j = 0; j < n; ++j) {
      uint32_t value = REX_LOAD_U32(group + j * 4);
      for (uint32_t k = 0; k < 4; ++k) {
        if (value == assets[k]) { value = assets[k ^ 1]; break; }
      }
      elements.push_back(value);
    }
    visuals.push_back(std::move(elements));
  }
  // Druglab meshes already contain their own head, eyes and arms. Normal
  // Alpha/Bravo head meshes also include harness geometry, so adding one here
  // duplicates heads and equips a harness before the original mask event.
  // Keep exactly the mission's body/harness groups, exchanging their roles.
  std::vector<uint32_t> new_groups;
  for (const auto& elements : visuals) {
    const uint32_t group = NewGuestArray(base, elements);
    if (!group) return false;
    new_groups.push_back(group);
  }
  const uint32_t replacement = NewGuestArray(base, new_groups);
  if (!replacement) return false;
  REX_STORE_U32(data + 12, replacement);
  patched[data] = {groups, replacement};
  REXLOG_WARN("AO4_SCAVENGE_LOOK source={} data={:08X} groups={} mission meshes swapped; story flags kept",
              name, data, new_groups.size());
  return true;
}
}  // namespace

namespace {
thread_local bool t_observe_scavenge_visuals = false;
thread_local uint32_t t_scavenge_soldier = 0;
thread_local uint32_t t_observe_client_component = 0;

bool ClientVisualTraceEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_SOLDIER_TRACE", nullptr, 0) != 0;
  return enabled;
}

using MeshGuid = std::array<uint32_t, 4>;

bool ReadMeshGuid(uint8_t* base, uint32_t mesh, MeshGuid& guid) {
  // Same GUID location/HasGuid bit as original sub_82DAA4A0.
  if (!IsReadable(base, mesh - 16, 24) || REX_LOAD_U32(mesh) != 0x82A23CB4 ||
      !(REX_LOAD_U16(mesh + 4) & 0x100)) return false;
  for (uint32_t i = 0; i < 4; ++i) guid[i] = REX_LOAD_U32(mesh - 16 + i * 4);
  return std::any_of(guid.begin(), guid.end(), [](uint32_t w) { return w != 0; });
}

bool SocketGuidEquals(uint8_t* base, uint32_t object, const MeshGuid& guid) {
  if (!IsReadable(base, object + 48, 16)) return false;
  for (uint32_t i = 0; i < 4; ++i)
    if (REX_LOAD_U32(object + 48 + i * 4) != guid[i]) return false;
  return true;
}

void AddScavengeAiSocketAliases(uint8_t* base, uint32_t component) {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_LOOK_SWAP", nullptr, 0) != 0;
  if (!enabled || !ao4_bravo_mode || !IsReadable(base, component, 8) ||
      REX_LOAD_U32(component) != 0x82A588C0) return;
  const uint32_t data = REX_LOAD_U32(component + 4);
  const uint32_t list = IsReadable(base, data, 100) ? REX_LOAD_U32(data + 96) : 0;
  if (AssetName(base, list) != "Characters/Soldiers/Customizations/AISocketList") return;
  const uint32_t sockets = IsReadable(base, list, 24) ? REX_LOAD_U32(list + 20) : 0;
  const uint32_t count = IsReadable(base, sockets - 4, 4) ? REX_LOAD_U32(sockets - 4) : 0;
  if (!count || count > 16 || !IsReadable(base, sockets, count * 4)) return;
  static std::mutex lock;
  std::lock_guard<std::mutex> guard(lock);
  static std::map<std::string, uint32_t> meshes;
  auto find_mesh = [&](const char* name) {
    uint32_t& mesh = meshes[name];
    if (AssetName(base, mesh) != name) mesh = FindAssetByName(base, name);
    return mesh;
  };
  MeshGuid alpha_head{};
  const bool has_alpha_head = ReadMeshGuid(base,
      find_mesh("ao4_assets/characters/primary/alpha/alpha_Mesh"), alpha_head);
  uint32_t alpha_face = UINT32_MAX;
  if (has_alpha_head) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t socket = REX_LOAD_U32(sockets + i * 4);
      if (!IsReadable(base, socket, 172)) continue;
      const uint32_t objects = REX_LOAD_U32(socket + 168);
      const uint32_t n = IsReadable(base, objects - 4, 4) ? REX_LOAD_U32(objects - 4) : 0;
      if (n > 256 || !IsReadable(base, objects, n * 4)) continue;
      for (uint32_t j = 0; j < n; ++j) {
        const uint32_t object = REX_LOAD_U32(objects + j * 4);
        if (IsReadable(base, object, 84) && REX_LOAD_U32(object) == 0x82A6D338 &&
            SocketGuidEquals(base, object, alpha_head)) alpha_face = REX_LOAD_U32(object + 80);
      }
    }
  }
  for (auto [from_name, to_name] : {
      std::pair{"ao4_assets/characters/primary/bravo_druglab/bravo_druglab_Mesh",
                "ao4_assets/characters/primary/alpha_druglab/alpha_druglab_Mesh"},
      std::pair{"ao4_assets/characters/primary/bravo_druglab/bravo_druglab_harness_Mesh",
                "ao4_assets/characters/primary/alpha_druglab/alpha_druglab_harness_Mesh"}}) {
    MeshGuid from{}, to{};
    if (!ReadMeshGuid(base, find_mesh(from_name), from) ||
        !ReadMeshGuid(base, find_mesh(to_name), to)) {
      REXLOG_WARN("AO4_SCAVENGE_SOCKET_MAP mesh GUID unavailable: {} -> {}", from_name, to_name);
      continue;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t socket = REX_LOAD_U32(sockets + i * 4);
      if (!IsReadable(base, socket, 172)) continue;
      const uint32_t objects = REX_LOAD_U32(socket + 168);
      const uint32_t n = IsReadable(base, objects - 4, 4) ? REX_LOAD_U32(objects - 4) : 0;
      if (!n || n >= 256 || !IsReadable(base, objects, n * 4)) continue;
      uint32_t source = 0;
      bool present = false;
      std::vector<uint32_t> entries;
      for (uint32_t j = 0; j < n; ++j) {
        const uint32_t object = REX_LOAD_U32(objects + j * 4);
        entries.push_back(object);
        if (!IsReadable(base, object, 84) || REX_LOAD_U32(object) != 0x82A6D338) continue;
        if (SocketGuidEquals(base, object, from)) source = object;
        if (SocketGuidEquals(base, object, to)) present = true;
      }
      if (!source || present || !IsReadable(base, source - 16, 100)) continue;
      // Exact original SkinnedSocketObjectData fields through FacePoserLibrary
      // (+80, total 84 bytes). Preserve the source's materials/variation fields.
      const uint32_t allocation = rex::system::kernel_memory()->SystemHeapAlloc(100);
      if (!allocation) continue;
      const uint32_t clone = allocation + 16;
      for (uint32_t j = 0; j < 100; j += 4)
        REX_STORE_U32(allocation + j, REX_LOAD_U32(source - 16 + j));
      for (uint32_t j = 0; j < 4; ++j) REX_STORE_U32(clone + 48 + j * 4, to[j]);
      // The body has a face library; use the existing Alpha-head entry rather
      // than carrying Bravo's face library onto Alpha. Harness (-1) stays -1.
      if (REX_LOAD_U32(source + 80) != UINT32_MAX && alpha_face != UINT32_MAX)
        REX_STORE_U32(clone + 80, alpha_face);
      entries.push_back(clone);
      const uint32_t replacement = NewGuestArray(base, entries);
      if (!replacement) continue;
      REX_STORE_U32(socket + 168, replacement);
      REXLOG_WARN("AO4_SCAVENGE_SOCKET_MAP component={:08X} list={:08X} socket={} "
                  "source={:08X} alias={:08X} mesh={} objects={} face={:08X}",
                  component, list, i, source, clone, to_name, entries.size(),
                  REX_LOAD_U32(clone + 80));
    }
  }
}

// Original 830064E8 consumes a fixed packet: body assets at +128, their
// variations at +192 and per-mesh flags at +256. It rebuilds component+352.
// Read only; never remove a mesh or alter an outfit flag to make a test pass.
void LogClientVisualState(uint8_t* base, const char* label, uint32_t component,
                         uint32_t caller) {
  if (!IsReadable(base, component, 3380)) return;
  const uint32_t owner = REX_LOAD_U32(component + 8);
  const uint32_t soldier = IsReadable(base, owner, 4) ? REX_LOAD_U32(owner) : 0;
  const uint32_t begin = REX_LOAD_U32(component + 352);
  const uint32_t end = REX_LOAD_U32(component + 356);
  std::string meshes;
  if (end >= begin && (end - begin) % 4 == 0 && end - begin <= 64 &&
      IsReadable(base, begin, end - begin)) {
    for (uint32_t entry = begin; entry < end; entry += 4) {
      const uint32_t mesh = REX_LOAD_U32(entry);
      const uint32_t resource = IsReadable(base, mesh, 4) ? REX_LOAD_U32(mesh) : 0;
      meshes += fmt::format(" [{:08X} resource={:08X}]", mesh, resource);
    }
  }
  REXLOG_WARN("AO4_CLIENT_VISUAL_STATE {} component={:08X} soldier={:08X} caller={:08X} "
              "begin={:08X} end={:08X} limits={}/{} outfit_enabled={} outfit_mesh={} outfit={:08X} meshes:{}",
              label, component, soldier, caller, begin, end,
              REX_LOAD_U32(component + 88), REX_LOAD_U32(component + 92),
              REX_LOAD_U8(component + 3369), REX_LOAD_U8(component + 3370),
              REX_LOAD_U32(component + 3372), meshes);
}
}

// Server selection matches a MeshAsset GUID against AISocketList AvailableObjects.
// The original list has Bravo druglab body/harness entries, but no Alpha equivalents.
// Add aliases in those same slots before the original matcher; adding an option
// does not equip it or advance the mission's mask/harness progression.
REX_HOOK_RAW(sub_831593F0) {
  if (t_observe_scavenge_visuals) AddScavengeAiSocketAliases(base, ctx.r3.u32);
  __imp__sub_831593F0(ctx, base);
}

REX_HOOK_RAW(sub_830064E8) {
  const uint32_t component = ctx.r3.u32, packet = ctx.r4.u32, caller = uint32_t(ctx.lr);
  bool primary = false;
  if (ClientVisualTraceEnabled() && IsReadable(base, packet, 664)) {
    for (uint32_t i = 0; i < 16; ++i) {
      const uint32_t asset = REX_LOAD_U32(packet + 128 + i * 4);
      if (!asset) break;
      if (AssetName(base, asset).find("ao4_assets/characters/primary/") == 0) primary = true;
    }
  }
  static std::atomic<uint32_t> calls{0};
  const bool observe = primary && calls.fetch_add(1) < 256;
  const uint32_t outer = t_observe_client_component;
  if (observe) {
    t_observe_client_component = component;
    LogClientVisualState(base, "packet-before", component, caller);
    REXLOG_WARN("AO4_CLIENT_VISUAL_PACKET component={:08X} packet={:08X} flags={:08X}",
                component, packet, REX_LOAD_U32(packet + 660));
    for (uint32_t i = 0; i < 16; ++i) {
      const uint32_t asset = REX_LOAD_U32(packet + 128 + i * 4);
      if (!asset) break;
      REXLOG_WARN("AO4_CLIENT_VISUAL_ITEM component={:08X} slot={} asset={:08X} name={} "
                  "variation={:08X} flags={:02X}", component, i, asset, AssetName(base, asset),
                  REX_LOAD_U32(packet + 192 + i * 4), REX_LOAD_U8(packet + 256 + i));
    }
  }
  __imp__sub_830064E8(ctx, base);
  if (observe) LogClientVisualState(base, "packet-after", component, caller);
  t_observe_client_component = outer;
}

REX_HOOK_RAW(sub_830A0CC0) {
  const uint32_t mesh = ctx.r3.u32, asset = ctx.r4.u32, caller = uint32_t(ctx.lr);
  if (t_observe_client_component) {
    REXLOG_WARN("AO4_CLIENT_VISUAL_BIND component={:08X} mesh={:08X} asset={:08X} name={} "
                "caller={:08X}", t_observe_client_component, mesh, asset, AssetName(base, asset), caller);
  }
  __imp__sub_830A0CC0(ctx, base);
}

REX_HOOK_RAW(sub_83004BD0) {
  const uint32_t component = ctx.r3.u32, caller = uint32_t(ctx.lr);
  static std::atomic<uint32_t> calls{0};
  const bool observe = ClientVisualTraceEnabled() &&
      (t_observe_client_component || ao4_scavenge_appearance.load()) && calls.fetch_add(1) < 256;
  if (observe) LogClientVisualState(base, "outfit-clear-before", component, caller);
  __imp__sub_83004BD0(ctx, base);
  if (observe) LogClientVisualState(base, "outfit-clear-after", component, caller);
}

REX_HOOK_RAW(sub_83006B38) {
  const uint32_t component = ctx.r3.u32, info = ctx.r4.u32, caller = uint32_t(ctx.lr);
  static std::atomic<uint32_t> calls{0};
  const bool observe = ClientVisualTraceEnabled() &&
      (t_observe_client_component || ao4_scavenge_appearance.load()) && calls.fetch_add(1) < 256;
  if (observe) {
    REXLOG_WARN("AO4_CLIENT_OUTFIT_APPLY component={:08X} info={:08X} name={} caller={:08X}",
                component, info, AssetName(base, info), caller);
    LogClientVisualState(base, "outfit-apply-before", component, caller);
  }
  __imp__sub_83006B38(ctx, base);
  if (observe) LogClientVisualState(base, "outfit-apply-after", component, caller);
}

REX_HOOK_RAW(sub_8311B0E0) {
  const uint32_t entity = ctx.r3.u32, event = ctx.r4.u32;
  bool notify = false, scavenge = false, masked = false;
  if (IsReadable(base, entity, 20) && IsReadable(base, event, 8) &&
      REX_LOAD_U32(event + 4) == 0x0C487271) {
    const uint32_t entity_data = REX_LOAD_U32(entity + 16);
    const uint32_t data = IsReadable(base, entity_data, 20)
        ? REX_LOAD_U32(entity_data + 16) : 0;
    const std::string name = AssetName(base, data);
    REXLOG_WARN("AO4_MISSION_CUSTOMIZE entity={:08X} data={:08X} source={}",
                entity, data, name);
    // The mission's body/harness stage counts in both modes (the AI gear waits for it); the
    // Alpha/Bravo swap of those meshes only happens in Bravo mode.
    scavenge = name == "Gameplay/Spawners/AO2_Scavenge_Partner_CustomizeData" ||
               name == "Gameplay/Spawners/AO2_Scavenge_Mask_Partner_CustomizeData" ||
               name == "Gameplay/Spawners/AO2_Scavenge_CustomizeData" ||
               name == "Gameplay/Spawners/AO2_Scavenge_Mask_CustomizeData";
    if (scavenge) SwapScavengeVisuals(base, data, name);
    masked = name.find("AO2_Scavenge_Mask_") != std::string::npos;
    notify = scavenge || name == "Gameplay/Spawners/AO2PlayerCustomizeData" ||
                        name == "Gameplay/Spawners/PartnerCustomizeAIData";
  }
  const bool outer_observation = t_observe_scavenge_visuals;
  if (scavenge) t_observe_scavenge_visuals = true;
  __imp__sub_8311B0E0(ctx, base);
  t_observe_scavenge_visuals = outer_observation;
  if (notify) {
    if (!scavenge) ao4_scavenge_level = false;
    ao4_scavenge_has_mask.store(masked);
    ao4_scavenge_appearance.store(scavenge);
    ao4_mission_appearance_revision.fetch_add(1);
  }
}

// Observe the actual server customization application, after any profile copy.
REX_HOOK_RAW(sub_8313DE78) {
  const uint32_t player = ctx.r3.u32, soldier = ctx.r4.u32, data = ctx.r5.u32;
  const std::string name = AssetName(base, data);
  EnsureLookSwapForCustomize(base, data, name);
  NormalizeAppliedLook(base, data, name);
  const bool observe = t_observe_scavenge_visuals ||
                       name.find("Gameplay/Spawners/AO2_Scavenge_") == 0;
  const bool outer = t_observe_scavenge_visuals;
  const uint32_t previous_soldier = t_scavenge_soldier;
  if (observe) {
    t_observe_scavenge_visuals = true;
    t_scavenge_soldier = soldier;
    REXLOG_WARN("AO4_SCAVENGE_APPLY source={} data={:08X} player={:08X} soldier={:08X} "
                "selection={} variation={}", name, data, player, soldier, ctx.r6.s32, ctx.r7.s32);
    const uint32_t groups = IsReadable(base, data, 16) ? REX_LOAD_U32(data + 12) : 0;
    const uint32_t count = IsReadable(base, groups - 4, 4) ? REX_LOAD_U32(groups - 4) : 0;
    for (uint32_t i = 0; i < count && i < 8 && IsReadable(base, groups + i * 4, 4); ++i) {
      const uint32_t group = REX_LOAD_U32(groups + i * 4);
      const uint32_t n = IsReadable(base, group - 4, 4) ? REX_LOAD_U32(group - 4) : 0;
      for (uint32_t j = 0; j < n && j < 8 && IsReadable(base, group + j * 4, 4); ++j) {
        const uint32_t asset = REX_LOAD_U32(group + j * 4);
        const uint32_t vt = IsReadable(base, asset, 4) ? REX_LOAD_U32(asset) : 0;
        const uint32_t linked = IsReadable(base, asset, 48) ? REX_LOAD_U32(asset + 44) : 0;
        const uint32_t links = IsReadable(base, linked - 4, 4) ? REX_LOAD_U32(linked - 4) : 0;
        REXLOG_WARN("AO4_SCAVENGE_GROUP soldier={:08X} group={} item={} asset={:08X} vt={:08X} "
                    "name={} linked={:08X} count={}", soldier, i, j, asset, vt,
                    AssetName(base, asset), linked, links);
      }
    }
  }
  __imp__sub_8313DE78(ctx, base);
  t_observe_scavenge_visuals = outer;
  t_scavenge_soldier = previous_soldier;
}

// Body visual references use this map insertion. Capture the actual MeshAsset
// key and original variation before insertion (not a BlueprintAndVariationPair).
REX_HOOK_RAW(sub_83179468) {
  if (t_observe_scavenge_visuals && uint32_t(ctx.lr) == 0x8313E6B0 &&
      IsReadable(base, ctx.r4.u32, 4)) {
    const uint32_t pair = REX_LOAD_U32(ctx.r4.u32);
    const uint32_t vt = IsReadable(base, pair, 4) ? REX_LOAD_U32(pair) : 0;
    REXLOG_WARN("AO4_SCAVENGE_BODY_SELECTED soldier={:08X} pair={:08X} vt={:08X} "
                "name={} variation={:08X}", t_scavenge_soldier, pair, vt,
                AssetName(base, pair), ctx.r28.u32);
  }
  __imp__sub_83179468(ctx, base);
}

// This original helper registers the selected visual/unlock reference in the
// customization list. Do not infer it merely from the source group array.
REX_HOOK_RAW(sub_830DF688) {
  if (t_observe_scavenge_visuals && IsReadable(base, ctx.r5.u32, 4)) {
    const uint32_t asset = REX_LOAD_U32(ctx.r5.u32);
    static std::atomic<uint32_t> count{0};
    if (count.fetch_add(1) < 512) {
      REXLOG_WARN("AO4_SCAVENGE_SELECTED soldier={:08X} asset={:08X} name={} caller={:08X}",
                  t_scavenge_soldier, asset, AssetName(base, asset), uint32_t(ctx.lr));
    }
  }
  __imp__sub_830DF688(ctx, base);
}

// AO2BotSpawnReferenceObjectData::getType
REX_HOOK_RAW(sub_84005168) {
  ObserveSpawner(ctx.r3.u32, uint32_t(ctx.lr), base);
  __imp__sub_84005168(ctx, base);
}

// AO4_COSMETICS_BUYABLE: sub_83301E48(player data, UnlockAsset, player index) returns an
// unlock's state for the menus and the shop. The shop maps it in sub_833033E8: 0x1 owned -> 1,
// 0x2 new ("!", cleared when viewed by sub_83303328) -> 2, 0x4 available for purchase (rank or
// campaign condition reached, or license owned) -> 3, else locked/hidden (0x8). Only state 3 is
// sold by the purchase handlers (sub_834D1650 ...: WalletCash minus cost). For masks, outfits and
// tattoos the switch reports "available, not hidden", so they are sold for their normal price. Nothing is
// written to the save: without the switch the original state comes back. Items whose license
// is missing (DLC/demo extras switched off) stay as they are.
REX_HOOK_RAW(sub_83301E48) {
  static const bool on = [] {
    wchar_t v[4] = {};
    return GetEnvironmentVariableW(L"AO4_COSMETICS_BUYABLE", v, 4) && v[0] == L'1';
  }();
  const uint32_t asset = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_83301E48(ctx, base);
  if (!on || !IsReadable(base, asset, 0x2C)) return;
  // The two writers read the state, change one bit and store the whole value in the profile:
  // sub_83303328 (set/clear "unlocked") and sub_83303230 (purchase: pay, set "owned"). They get
  // the real state, so the save never contains the switch's "unlocked/visible" bits.
  if (caller == 0x83303348 || caller == 0x833032B8) return;
  const uint32_t state = ctx.r3.u32;
  const uint32_t licenses = REX_LOAD_U32(asset + 0x1C);
  const uint32_t license_count = IsReadable(base, licenses - 4, 4) ? REX_LOAD_U32(licenses - 4) : 0;
  if (license_count && state == 8) return;  // licensed extra not owned (DLC/demo extras off)
  if ((state & 4) && !(state & 8)) return;  // already available and visible
  const uint32_t name_ptr = REX_LOAD_U32(asset + 0x08);
  if (!IsReadable(base, name_ptr, 8)) return;
  const std::string name = GuestString(base, name_ptr);
  if (!CosmeticUnlockName(name)) return;
  ctx.r3.u64 = (state | 4) & ~8u;
  static std::mutex lock;
  static std::unordered_set<uint32_t> logged;
  std::lock_guard<std::mutex> guard(lock);
  if (logged.insert(asset).second) {
    REXLOG_WARN("AO4_COSMETICS_BUYABLE '{}' state={:X} -> {:X} cost={}", name, state, ctx.r3.u32,
                REX_LOAD_U32(asset + 0x20));
  }
}

// Shop purchase sub_83303230(player data, UnlockAsset, player index): reads the bank from the
// player's profile container (+400) under the key at *(0x84080A64), subtracts UnlockCost (+0x20)
// if it is enough, stores it and sets the item owned. Logged per purchase with the bank before
// and after (read through the same getter) to follow "money is not deducted" reports.
REX_HOOK_RAW(sub_83303230) {
  const uint32_t asset = ctx.r4.u32;
  uint32_t container = 0, key = 0;
  if (IsReadable(base, ctx.r3.u32 + 4, 4)) {
    container = REX_LOAD_U32(0x8413870C) + REX_LOAD_U32(ctx.r3.u32 + 4) * 3616 + 400;
    key = REX_LOAD_U32(0x84080A64);
  }
  auto bank = [&]() -> int32_t {
    if (!container || !key) return -1;
    PPCContext c = ctx;
    c.r3.u64 = container;
    c.r4.u64 = key;
    c.r5.u64 = 0;
    sub_82F7C0B0(c, base);  // hooked getter: only "Unlock<hex>" keys differ
    return c.r3.s32;
  };
  const int32_t before = bank();
  __imp__sub_83303230(ctx, base);
  const int32_t after = bank();
  REXLOG_WARN("AO4_SHOP purchase '{}' cost={} key='{}' bank {} -> {} result={}",
              IsReadable(base, asset, 0x24) ? GuestString(base, REX_LOAD_U32(asset + 8)) : std::string("?"),
              IsReadable(base, asset, 0x24) ? REX_LOAD_U32(asset + 0x20) : 0, key ? GuestString(base, key) : std::string(),
              before, after, ctx.r3.u32 & 0xFF);
}

// AO4_UNLOCK_ALL, licensed content: sub_83272A38(&Licenses, owned license set) answers whether
// the player owns an unlock's licenses (demo0, po_gamestop, DLC); its callers are the
// customization checks sub_832EDF40/sub_832EDEE0 (masks, outfits, tattoos) and weapon/unlock
// code. With the switch every license counts as owned, from the start, so the menus build
// their lists consistently. Rank and campaign conditions are separate and stay. Each caller is
// logged once.
REX_HOOK_RAW(sub_83272A38) {
  // AO4_UNLOCK_LICENSES (launcher: DLC/demo/pre-order extras) or AO4_UNLOCK_ALL (old starters).
  static const bool unlock_all = GetEnvironmentVariableW(L"AO4_UNLOCK_ALL", nullptr, 0) != 0 ||
                                 GetEnvironmentVariableW(L"AO4_UNLOCK_LICENSES", nullptr, 0) != 0;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_83272A38(ctx, base);
  if (!unlock_all) {
    return;
  }
  const uint32_t owned = ctx.r3.u32 & 0xFF;
  ctx.r3.u64 = 1;
  static std::mutex lock;
  static std::unordered_set<uint32_t> logged;
  std::lock_guard<std::mutex> guard(lock);
  if (logged.insert(caller).second) {
    REXLOG_WARN("AO4_UNLOCK_ALL license check from {:08X}: owned={} -> 1", caller, owned);
  }
}

// AO4_PROFILE_KEYS (research): sub_82F7A610(out, map, &key) is the lookup of the string-keyed
// profile options map, used by all profile getters and setters. Logs every key the first time
// it is looked up from each caller (lr), to see which keys the mission select reads.
// AO4_PROFILE_WRITE_TRACE (diagnosis, off by default): sub_82F7E2B8(container, key, value) stores
// an int in the string-keyed profile map (bank, unlock states, ...). Logs key, value and caller,
// to find the shop's real purchase path.
REX_HOOK_RAW(sub_82F7E2B8) {
  static const bool trace = GetEnvironmentVariableW(L"AO4_PROFILE_WRITE_TRACE", nullptr, 0) != 0;
  if (trace && IsReadable(base, ctx.r4.u32, 4)) {
    REXLOG_WARN("AO4_PROFILE_WRITE key='{}' value={} container={:08X} lr={:08X}", GuestString(base, ctx.r4.u32),
                ctx.r5.s32, ctx.r3.u32, uint32_t(ctx.lr));
  }
  __imp__sub_82F7E2B8(ctx, base);
}

// AO4_PROFILE_WRITE_TRACE also follows the shop UI: the five purchase handlers (they call
// sub_83303230 through sub_83304A28/A30) and their state check sub_834D83E8 (3 = buyable).
#define AO4_SHOP_HANDLER(fn)                                                                           \
  REX_HOOK_RAW(fn) {                                                                                 \
    static const bool trace = GetEnvironmentVariableW(L"AO4_PROFILE_WRITE_TRACE", nullptr, 0) != 0; \
    if (trace)                                                                                       \
      REXLOG_WARN("AO4_SHOP_UI " #fn " this={:08X} +128={:08X} +132={:08X} +60={:08X} lr={:08X}",   \
                  ctx.r3.u32, IsReadable(base, ctx.r3.u32, 136) ? REX_LOAD_U32(ctx.r3.u32 + 128) : 0, \
                  IsReadable(base, ctx.r3.u32, 136) ? REX_LOAD_U32(ctx.r3.u32 + 132) : 0,             \
                  IsReadable(base, ctx.r3.u32, 136) ? REX_LOAD_U32(ctx.r3.u32 + 60) : 0, uint32_t(ctx.lr)); \
    __imp__##fn(ctx, base);                                                                          \
  }
AO4_SHOP_HANDLER(sub_834D14C8)
AO4_SHOP_HANDLER(sub_834D1650)
AO4_SHOP_HANDLER(sub_834D2960)
AO4_SHOP_HANDLER(sub_834DD438)
AO4_SHOP_HANDLER(sub_834DD5C8)
#undef AO4_SHOP_HANDLER
REX_HOOK_RAW(sub_834C95B8) {
  static const bool trace = GetEnvironmentVariableW(L"AO4_PROFILE_WRITE_TRACE", nullptr, 0) != 0;
  const uint32_t id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_834C95B8(ctx, base);
  static std::atomic<uint64_t> last{0};
  const uint64_t key = (uint64_t(id) << 32) ^ (uint64_t(lr) << 4) ^ uint64_t(ctx.r3.u32 & 0xF);
  if (trace && last.exchange(key) != key)
    REXLOG_WARN("AO4_SHOP_UI item id={:08X} -> state {} lr={:08X}", id, ctx.r3.s32, lr);
}
REX_HOOK_RAW(sub_834D83E8) {
  static const bool trace = GetEnvironmentVariableW(L"AO4_PROFILE_WRITE_TRACE", nullptr, 0) != 0;
  const uint32_t a4 = ctx.r4.u32, a5 = ctx.r5.u32, lr = uint32_t(ctx.lr);
  __imp__sub_834D83E8(ctx, base);
  static std::atomic<uint64_t> last{0};
  const uint64_t key = (uint64_t(a4) << 32) ^ (uint64_t(a5) << 8) ^ uint64_t(ctx.r3.u32 & 0xFF);
  if (trace && last.exchange(key) != key)
    REXLOG_WARN("AO4_SHOP_UI state r4={:08X} r5={:08X} -> {} lr={:08X}", a4, a5, ctx.r3.s32, lr);
}

REX_HOOK_RAW(sub_82F7A610) {
  static const bool trace = GetEnvironmentVariableW(L"AO4_PROFILE_KEYS", nullptr, 0) != 0;
  if (trace && IsReadable(base, ctx.r5.u32, 4)) {
    const std::string key = GuestString(base, REX_LOAD_U32(ctx.r5.u32));
    static std::mutex lock;
    static std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> guard(lock);
    if (!key.empty() && seen.size() < 8192 &&
        seen.insert(key + fmt::format("@{:08X}", uint32_t(ctx.lr))).second) {
      REXLOG_WARN("AO4_PROFILE_KEY '{}' map={:08X} lr={:08X}", key, ctx.r4.u32,
                  uint32_t(ctx.lr));
    }
  }
  __imp__sub_82F7A610(ctx, base);
}

// AO4_UNLOCK_ALL, missions and checkpoints: the profile entry "Unlock<hex chapter id>" (own
// container at profile +784) is a bit mask of the chapter's unlocked checkpoints, bit n =
// checkpoint n; sub_832FE4D0(chapter, checkpoint) tests it through the getter
// sub_82F7C0B0(container, key, default). With the switch these keys answer all bits set, so
// every chapter and checkpoint is selectable. Not when sub_832FEC18 (sets a checkpoint's bit
// when it is reached: reads the mask, ORs the bit, stores it) reads it, so the save keeps
// only the real progress. Each key is logged once with its stored mask.
REX_HOOK_RAW(sub_82F7C0B0) {
  // AO4_UNLOCK_MISSIONS (launcher: story missions/checkpoints) or AO4_UNLOCK_ALL (old starters).
  static const bool unlock_all = GetEnvironmentVariableW(L"AO4_UNLOCK_ALL", nullptr, 0) != 0 ||
                                 GetEnvironmentVariableW(L"AO4_UNLOCK_MISSIONS", nullptr, 0) != 0;
  const uint32_t key_address = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_82F7C0B0(ctx, base);
  if (!unlock_all || (caller >= 0x832FEC18 && caller < 0x832FECD0)) {
    return;
  }
  const std::string key = GuestString(base, key_address);
  if (key.size() <= 6 || key.compare(0, 6, "Unlock") != 0 ||
      key.find_first_not_of("0123456789ABCDEFabcdef", 6) != std::string::npos) {
    return;
  }
  const uint32_t stored = ctx.r3.u32;
  ctx.r3.u64 = 0xFFFFFFFF;
  static std::mutex lock;
  static std::unordered_set<std::string> logged;
  std::lock_guard<std::mutex> guard(lock);
  if (logged.insert(key).second) {
    REXLOG_WARN("AO4_UNLOCK_ALL chapter key '{}' stored mask={:X} -> all", key, stored);
  }
}
