// Online coop S2 (AO4_NET_TRACE=1): observes the game's online gate checks. Observation only:
// the original functions run unchanged and their results are kept.
#include "generated/ao4.main/ao4_main_init.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <windows.h>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

namespace {
bool NetTraceOn() {
  static const bool on = GetEnvironmentVariableW(L"AO4_NET_TRACE", nullptr, 0) != 0;
  return on;
}
uint32_t NetWord(uint8_t* base, uint32_t address) {
  uint8_t bytes[4]{};
  SIZE_T read = 0;
  if (address < 0x40000000 || address > 0xDFFFFFFC ||
      !ReadProcessMemory(GetCurrentProcess(), base + address, bytes, 4, &read) || read != 4) return 0;
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
}
// Logs each distinct (caller, detail) combination once.
bool FirstSeen(uint32_t caller, const std::string& detail) {
  static std::mutex mutex;
  static std::set<std::tuple<uint32_t, std::string>> seen;
  std::lock_guard<std::mutex> lock(mutex);
  return seen.size() < 256 && seen.insert({caller, detail}).second;
}
}  // namespace

// User online test (user object r3): index at +20, -1 = no profile; true when the
// sign-in state of that profile is 2 (Xbox LIVE).
REX_HOOK_RAW(sub_832279A0) {
  const uint32_t caller = uint32_t(ctx.lr), user = ctx.r3.u32;
  const uint32_t index = user ? NetWord(base, user + 20) : 0xFFFFFFFF;
  __imp__sub_832279A0(ctx, base);
  if (NetTraceOn()) {
    const std::string detail = fmt::format("user={:08X} index={:08X} online={}", user, index, ctx.r3.u32 & 0xFF);
    if (FirstSeen(caller, detail)) REXLOG_WARN("AO4_NET_GATE online-user caller={:08X} {}", caller, detail);
  }
}

// Online menu entry gate (r3 = menu object): picks a user slot from +60 (+28) or slot 3, then
// shows ID_UI_POPUP_NO_SERVER_CONNECTION_360 when that user is not online.
REX_HOOK_RAW(sub_83396DF0) {
  const uint32_t menu = ctx.r3.u32, caller = uint32_t(ctx.lr);
  const uint32_t owner = NetWord(base, menu + 60);
  const uint32_t slot = owner ? NetWord(base, owner + 28) : 3;
  if (NetTraceOn()) {
    REXLOG_WARN("AO4_NET_GATE online-menu caller={:08X} menu={:08X} owner={:08X} slot={} busy={:08X}", caller, menu,
                owner, slot, NetWord(base, menu + 132));
  }
  __imp__sub_83396DF0(ctx, base);
}
