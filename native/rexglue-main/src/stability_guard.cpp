// Diagnostic fallback for invalid texture objects. Their lifetime failure is not yet proven.
//   Texture binding (always on; AO4_NO_TEXTURE_GUARD disables it): sub_82E73440(context,
//   slot, texture, srgb) binds a texture to a shader slot and calls texture->vtable[14]. At
//   some streaming points (a load door in 04_Church, rarely in the prologue) the texture is
//   invalid, its vtable is garbage and the call ends in "Call to invalid or unregistered
//   function at guest address 0x1A200152". If the texture's vtable or vtable[14] does not point
//   into the game image, the guard binds no texture (0, the game's own "default texture" path)
//   instead and logs the case (slot, texture, caller) once per texture and caller.
#include "generated/ao4.main/ao4_main_init.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <set>
#include <utility>
#include <string>
#include <cstring>

void Ao4ObserveMaterialTexture(PPCContext& ctx, uint8_t* base, uint32_t object,
                               const char* label);
void Ao4DumpTextureLifetime(uint8_t* base, uint32_t object, uint32_t caller);
void Ao4ObserveTextureParameter(PPCContext& ctx, uint8_t* base, uint32_t object);

namespace {
constexpr uint32_t kImageBegin = 0x82A00000;  // image base, size 0x19A0000
constexpr uint32_t kImageEnd = 0x843A0000;

bool InImage(uint32_t address) { return address >= kImageBegin && address < kImageEnd; }

// Reads a guest word; false if the address faults (no VirtualQuery, this runs per bind).
bool TryLoad32(uint8_t* base, uint32_t address, uint32_t& value) {
  __try {
    value = REX_LOAD_U32(address);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// True if the object's vtable and vtable[slot] point into the game image.
bool HasValidVirtual(uint8_t* base, uint32_t object, uint32_t slot, uint32_t& vtable,
                     uint32_t& function) {
  vtable = function = 0;
  if (!TryLoad32(base, object, vtable) || !InImage(vtable)) {
    return false;
  }
  return TryLoad32(base, vtable + slot * 4, function) && InImage(function);
}

std::string Snapshot(uint8_t* base, uint32_t address, uint32_t words) {
  std::string result;
  if (!address) return " null";
  for (uint32_t i = 0; i < words; ++i) {
    uint32_t value = 0;
    if (!TryLoad32(base, address + i * 4, value)) {
      result += fmt::format(" +{:X}=unreadable", i * 4);
      break;
    }
    result += fmt::format(" +{:X}={:08X}", i * 4, value);
  }
  return result;
}

bool TryReadLabel(uint8_t* base, uint32_t address, char* label, size_t size) {
  if (address < 0x10000 || size < 2) return false;
  // SDK exception handling may report guest AVs before this observer's SEH runs.
  // Inspect committed host memory first instead of probing arbitrary event values.
  const auto host = reinterpret_cast<uintptr_t>(REX_RAW_ADDR(address));
  MEMORY_BASIC_INFORMATION memory = {};
  if (!VirtualQuery(reinterpret_cast<void*>(host), &memory, sizeof(memory)) ||
      memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    return false;
  const auto end = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
  __try {
    for (size_t i = 0; i < size - 1; ++i) {
      if (host + i >= end || uint64_t(address) + i > UINT32_MAX) return false;
      label[i] = static_cast<char>(REX_LOAD_U8(address + uint32_t(i)));
      if (!label[i]) return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}  // namespace

REX_HOOK_RAW(sub_82E73440) {
  static const bool disabled = GetEnvironmentVariableW(L"AO4_NO_TEXTURE_GUARD", nullptr, 0) != 0;
  static const bool announced = [] {
    REXLOG_INFO("AO4_TEXTURE_GUARD hook active, fallback {}",
                disabled ? "disabled" : "enabled");
    return true;
  }();
  (void)announced;
  const uint32_t texture = ctx.r5.u32;
  if (uint32_t(ctx.lr) == 0x82E9FE34)
    Ao4ObserveTextureParameter(ctx, base, texture);
  uint32_t vtable = 0, function = 0;
  // Opt-in evidence only: distinguish an explicit/default material texture from
  // the invalid-object fallback. Keep the original guard and all guest data.
  static const bool material_trace =
      GetEnvironmentVariableW(L"AO4_MATERIAL_BIND_TRACE", nullptr, 0) != 0;
  if (material_trace && uint32_t(ctx.lr) == 0x82E9FE34 && ctx.r4.u32 == 0) {
    static std::mutex trace_lock;
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    const uint32_t material = ctx.r28.u32;
    bool first = false;
    {
      std::lock_guard<std::mutex> guard(trace_lock);
      first = seen.size() < 2048 && seen.emplace(material, texture).second;
    }
    if (first) {
      uint32_t observed_vtable = 0, observed_function = 0;
      const bool valid = texture &&
          HasValidVirtual(base, texture, 14, observed_vtable, observed_function);
      const uint32_t entry = ctx.r29.u32 - 39;
      char label[64] = {};
      const bool label_ok = TryReadLabel(base, entry, label, sizeof(label));
      if (label_ok && valid) Ao4ObserveMaterialTexture(ctx, base, texture, label);
      REXLOG_INFO("AO4_MATERIAL_BIND material={:08X} context={:08X} slot=0 "
                  "texture={:08X} source={} srgb={} entry={:08X} label={} "
                  "vtable={:08X} fn14={:08X} texture_words:{}",
                  material, ctx.r3.u32, texture,
                  !texture ? "explicit-default" : valid ? "valid-object" :
                      disabled ? "invalid-unguarded" : "guard-default",
                  ctx.r6.u32 & 0xFF, entry, label_ok ? label : "unreadable",
                  observed_vtable, observed_function, Snapshot(base, texture, 20));
    }
  }
  if (!disabled && texture && !HasValidVirtual(base, texture, 14, vtable, function)) {
    static std::atomic<uint32_t> count{0};
    const uint32_t total = ++count;
    static std::mutex lock;
    static std::set<std::pair<uint32_t, uint32_t>> logged;
    const uint32_t caller = uint32_t(ctx.lr);
    {
      std::lock_guard<std::mutex> guard(lock);
      if (logged.size() < 256 && logged.emplace(texture, caller).second) {
        Ao4DumpTextureLifetime(base, texture, caller);
        REXLOG_WARN("AO4_TEXTURE_GUARD invalid texture {:08X} (vtable {:08X}, [14] {:08X}) slot={} "
                    "srgb={} context={:08X} caller={:08X}, bound default instead (#{})",
                    texture, vtable, function, ctx.r4.u32, ctx.r6.u32 & 0xFF, ctx.r3.u32, caller,
                    total);
        if (caller == 0x82E9FE34) {
          const uint32_t material = ctx.r28.u32;
          const uint32_t entry = ctx.r29.u32 - 39;
          const uint32_t parameter = ctx.r9.u32;
          uint32_t linked_texture = 0;
          const bool matched = parameter && TryLoad32(base, parameter + 16, linked_texture) &&
                               linked_texture == texture;
          REXLOG_WARN("AO4_TEXTURE_ORIGIN material={:08X} entry={:08X} parameter={:08X} "
                      "parameter_matches={} texture={:08X} material_tail:{} entry_words:{} "
                      "parameter_words:{} texture_words:{}",
                      material, entry, parameter, matched, texture,
                      Snapshot(base, material + 688, 12), Snapshot(base, entry, 10),
                      matched ? Snapshot(base, parameter, 8) : " not proven",
                      Snapshot(base, texture, 20));
        }
      }
    }
    ctx.r5.u64 = 0;
  }
  __imp__sub_82E73440(ctx, base);
}

// Observe the original TCR/UI message parser without replacing its result.
REX_HOOK_RAW(sub_83234888) {
  const uint32_t owner = ctx.r3.u32, text = ctx.r4.u32;
  char label[96] = {};
  const bool readable = TryReadLabel(base, text, label, sizeof(label));
  if (readable && (std::strstr(label, "WAIT") || std::strstr(label, "LOADING"))) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 64) {
      REXLOG_INFO("AO4_WAIT_MESSAGE label={} owner={:08X} count={} args={:08X} caller={:08X}",
                  label, owner, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
    }
  }
  __imp__sub_83234888(ctx, base);
}

// The HUD event handler references ID_UI_WAIT_FOR_PARTNER. Record original
// events and the small owner header, without interpreting unknown fields.
REX_HOOK_RAW(sub_834A4560) {
  const uint32_t owner = ctx.r3.u32, event = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  static std::atomic<uint32_t> count{0};
  uint32_t event_class = 0, event_id = 0, text_pointer = 0;
  TryLoad32(base, event + 4, event_class);
  TryLoad32(base, event + 8, event_id);
  const bool handled = event_class == 0x005973B9 &&
      (event_id == 0xB88B0D98 || event_id == 0xCE6D9E2A ||
       event_id == 0x25514861 || event_id == 0x385F2387 || event_id == 0x5E75ED67);
  bool record = false;
  char text_label[96] = {};
  if (handled) {
    // Only the update event has a text pointer at +48. Removal events carry
    // shorter payloads; reading beyond them interpreted the value 3 as a pointer.
    if (event_id == 0x5E75ED67) {
      TryLoad32(base, event + 48, text_pointer);
      TryReadLabel(base, text_pointer, text_label, sizeof(text_label));
    }
    // The update event is sent every frame. Deduplicate only its relevant
    // payload (40..72), not arbitrary caller-stack bytes following the event.
    static std::mutex seen_lock;
    static std::set<std::string> seen;
    const std::string signature = fmt::format("{:08X}:{:08X}:{}:{}", owner, event_id,
                                             Snapshot(base, event + 40, 9), text_label);
    std::lock_guard<std::mutex> guard(seen_lock);
    if (seen.size() < 1024 && seen.emplace(signature).second)
      record = count.fetch_add(1) < 1024;
  }
  if (record) {
    REXLOG_INFO("AO4_HUD_WAIT_EVENT owner={:08X} event={:08X} caller={:08X} label={} event_words:{} before:{} entries:{}",
                owner, event, caller, text_label, Snapshot(base, event, 19), Snapshot(base, owner, 10),
                [&] { uint32_t entries = 0; TryLoad32(base, owner + 4, entries); return Snapshot(base, entries, 24); }());
  }
  __imp__sub_834A4560(ctx, base);
  if (record) {
    REXLOG_INFO("AO4_HUD_WAIT_EVENT_AFTER owner={:08X} words:{}", owner, Snapshot(base, owner, 10));
  }
}

REX_HOOK_RAW(sub_83227438) {
  const uint32_t code = ctx.r3.u32, caller = uint32_t(ctx.lr);
  __imp__sub_83227438(ctx, base);
  char label[96] = {};
  if (TryReadLabel(base, ctx.r3.u32, label, sizeof(label)) && std::strstr(label, "WAIT")) {
    static std::atomic<uint32_t> count{0};
    if (count.fetch_add(1) < 64)
      REXLOG_INFO("AO4_WAIT_ID code={} label={} caller={:08X}", code, label, caller);
  }
}

// Observe the selection-to-UI boundary, including the original no-selection
// transition. No synthetic text update or visibility change is issued.
REX_HOOK_RAW(sub_83497DF0) {
  const uint32_t owner = ctx.r3.u32, id = ctx.r4.u32, refresh = ctx.r5.u32;
  uint32_t previous = 0, selected = 0, revision = 0;
  TryLoad32(base, owner + 120, previous);
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_83497DF0(ctx, base);
  TryLoad32(base, owner + 120, selected);
  TryLoad32(base, owner + 132, revision);
  if (previous != selected) {
    static std::atomic<uint32_t> count{0};
    if (count.fetch_add(1) < 256)
      REXLOG_INFO("AO4_HUD_SELECTION owner={:08X} caller={:08X} event_id={:08X} refresh={} before={:08X} after={:08X} revision={} ui:{}",
                  owner, caller, id, refresh, previous, selected, revision,
                  Snapshot(base, owner + 48, 10));
  }
}

REX_HOOK_RAW(sub_830A54C0) {
  char text[96] = {};
  const uint32_t caller = uint32_t(ctx.lr);
  const bool readable = TryReadLabel(base, ctx.r5.u32, text, sizeof(text));
  const bool waiting = readable && (std::strstr(text, "warten") || std::strstr(text, "WARTEN") ||
                                   std::strstr(text, "wait") || std::strstr(text, "WAIT"));
  if (caller == 0x83497F10 || waiting) {
    static std::mutex lock;
    static std::set<std::string> seen;
    static std::atomic<uint32_t> count{0};
    const std::string key = fmt::format("{:08X}:{:08X}:{:08X}:{}", ctx.r3.u32, ctx.r4.u32, caller, text);
    std::lock_guard<std::mutex> guard(lock);
    if (seen.size() < 256 && seen.emplace(key).second && count.fetch_add(1) < 256)
      REXLOG_INFO("AO4_HUD_TEXT_DISPATCH target={:08X} message={:08X} args={:08X} caller={:08X} text={} words:{}",
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, caller, readable ? text : "unreadable",
                  Snapshot(base, ctx.r5.u32, 12));
  }
  __imp__sub_830A54C0(ctx, base);
}

REX_HOOK_RAW(sub_8349E4C0) {
  const uint32_t owner = ctx.r3.u32, output = ctx.r4.u32;
  uint32_t selected = 0, revision = 0;
  TryLoad32(base, owner + 120, selected);
  TryLoad32(base, owner + 132, revision);
  __imp__sub_8349E4C0(ctx, base);
  static std::mutex lock;
  static std::set<std::pair<uint32_t, uint32_t>> seen;
  std::lock_guard<std::mutex> guard(lock);
  if (seen.size() < 256 && seen.emplace(owner, revision).second)
    REXLOG_INFO("AO4_HUD_PROPERTY owner={:08X} selected={:08X} revision={} output:{}",
                owner, selected, revision, Snapshot(base, output, 8));
}

// ID_* localization requests can precede or bypass the observed HUD dispatch.
REX_HOOK_RAW(sub_83227B30) {
  const uint32_t output = ctx.r3.u32, key_pointer = ctx.r4.u32, caller = uint32_t(ctx.lr);
  char key[96] = {};
  const bool readable = TryReadLabel(base, key_pointer, key, sizeof(key));
  const bool relevant = readable && (std::strstr(key, "WAIT") || std::strstr(key, "LOADING"));
  __imp__sub_83227B30(ctx, base);
  if (relevant) {
    char translated[96] = {};
    const bool translated_readable = TryReadLabel(base, output, translated, sizeof(translated));
    static std::mutex lock;
    static std::set<std::string> seen;
    const std::string signature = fmt::format("{:08X}:{}:{}", caller, key, translated);
    std::lock_guard<std::mutex> guard(lock);
    if (seen.size() < 256 && seen.emplace(signature).second)
      REXLOG_INFO("AO4_WAIT_LOCALIZATION key={} caller={:08X} output={:08X} result={} text={} words:{}",
                  key, caller, output, ctx.r3.u32, translated_readable ? translated : "unreadable",
                  Snapshot(base, output, 8));
    if (std::strcmp(key, "ID_UI_PLEASE_WAIT") == 0 && caller == 0x83239420) {
      // Verified prologue of sub_83239398: savegprlr_28 stores its LR at
      // old SP-8, followed by stwu SP,-2176. The original caller is SP+2168.
      uint32_t parent_caller = 0;
      const bool saved_lr_readable = TryLoad32(base, ctx.r1.u32 + 2168, parent_caller);
      REXLOG_INFO("AO4_PLEASE_WAIT_SOURCE caller={:08X} saved_lr_readable={} translator={:08X} text_object={:08X} translator_words:{} text_words:{}",
                  parent_caller, saved_lr_readable, output - 20, ctx.r29.u32,
                  Snapshot(base, output - 20, 8), Snapshot(base, ctx.r29.u32, 8));
    }
  }
}

REX_HOOK_RAW(sub_832D87F8) {
  const uint32_t owner = ctx.r3.u32, event = ctx.r4.u32;
  uint32_t data = 0, key_pointer = 0;
  char key[96] = {};
  TryLoad32(base, owner + 16, data);
  if (data) TryLoad32(base, data + 12, key_pointer);
  const bool relevant = TryReadLabel(base, key_pointer, key, sizeof(key)) &&
                        std::strcmp(key, "ID_UI_PLEASE_WAIT") == 0;
  if (relevant) {
    uint32_t reference = 0, indirect = 0, player = 0, vtable = 0, getter = 0;
    TryLoad32(base, event + 16, reference);
    if (reference) TryLoad32(base, reference, indirect);
    if (indirect >= 8) player = indirect - 8;
    if (player) TryLoad32(base, player, vtable);
    if (vtable) TryLoad32(base, vtable + 32, getter);
    REXLOG_INFO("AO4_GATE_WAIT_RECIPIENT owner={:08X} reference={:08X} player={:08X} vtable={:08X} getter={:08X} player_words:{}",
                owner, reference, player, vtable, getter, Snapshot(base, player, 16));
    REXLOG_INFO("AO4_GATE_WAIT_EVENT owner={:08X} caller={:08X} event:{} state:{} data:{}",
                owner, uint32_t(ctx.lr), Snapshot(base, event, 6),
                Snapshot(base, owner + 16, 4), Snapshot(base, data + 12, 5));
  }
  __imp__sub_832D87F8(ctx, base);
  if (relevant)
    REXLOG_INFO("AO4_GATE_WAIT_EVENT_AFTER owner={:08X} state:{}", owner,
                Snapshot(base, owner + 16, 4));
}

REX_HOOK_RAW(sub_832D8550) {
  const uint32_t owner = ctx.r3.u32, update = ctx.r4.u32;
  uint32_t data = 0, key_pointer = 0, elapsed_bits = 0;
  char key[96] = {};
  TryLoad32(base, owner + 16, data);
  if (data) TryLoad32(base, data + 12, key_pointer);
  const bool relevant = TryReadLabel(base, key_pointer, key, sizeof(key)) &&
                        std::strcmp(key, "ID_UI_PLEASE_WAIT") == 0;
  bool record = false;
  if (relevant && TryLoad32(base, owner + 20, elapsed_bits)) {
    float elapsed = 0;
    std::memcpy(&elapsed, &elapsed_bits, sizeof(elapsed));
    if (elapsed >= 0 && elapsed < 60) {
      static std::mutex lock;
      static std::set<std::pair<uint32_t, uint32_t>> seen;
      std::lock_guard<std::mutex> guard(lock);
      record = seen.size() < 512 && seen.emplace(owner, uint32_t(elapsed)).second;
    }
  }
  if (record)
    REXLOG_INFO("AO4_GATE_WAIT_TICK owner={:08X} update:{} before:{} data:{}", owner,
                Snapshot(base, update, 2), Snapshot(base, owner + 16, 4), Snapshot(base, data + 12, 5));
  __imp__sub_832D8550(ctx, base);
  if (record)
    REXLOG_INFO("AO4_GATE_WAIT_TICK_AFTER owner={:08X} state:{}", owner, Snapshot(base, owner + 16, 4));
}
