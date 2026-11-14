// Alpha/Bravo role resolution research. Read-only: the original functions always run and
// nothing in guest memory is changed.
//   AO4_ROLE_TRACE: log the players the level logic sees and which player/soldier every
//                   role query (Player1, Partner, Alpha/Bravo/BravoPAI/BravoHuman) returns.
// Experiment (changes guest state, only with AO4_CONTROL_SWAP set):
//   F8 toggles control: the human player takes the AI partner's soldier and vice versa,
//      through the game's own enter-controllable function.
//   Bravo mode (key "bravo_mode", F9 by default): the Player1 lookup returns the AI partner
//      instead of the human, so the level logic treats the AI partner as Alpha and the human
//      as (Bravo)Human. The key only sets the wish (saved); it takes effect in the menu, while
//      loading and when a level or checkpoint spawns the soldiers.
//   AO4_PLAYER1_PAI: Bravo mode default before anything was saved.
//   AO4_CONTROL_SWAP_AUTO=<seconds>: for unattended tests, toggle control once, that many
//      seconds after the human and the AI partner both have a soldier.
#include "generated/ao4.main/ao4_main_init.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <windows.h>
#include "ao4_readable.h"
#include <rex/system/xthread.h>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include "partner_appearance_state.h"
#include "ao4_overlay_state.h"
#include "aim_source_observer.h"
#include "coop_route_policy.h"
#include "coop_route_memory.h"
#include "ono_trace_policy.h"
#include "mission_vo_trace_policy.h"
#include "qol_settings.h"
#include "qol_appearance_policy.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <vector>

namespace {
bool VoIdentityTraceEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_VO_IDENTITY_TRACE", nullptr, 0) != 0;
  return enabled;
}
bool OnlineRoleIsolation() {
  static const bool enabled=GetEnvironmentVariableW(L"AO4_ONLINE_ROLE_ISOLATION",nullptr,0)!=0;
  return enabled;
}
bool OnlineVoiceTrace() {
  static const bool enabled=GetEnvironmentVariableW(L"AO4_ONLINE_VO_TRACE",nullptr,0)!=0;
  return enabled;
}
bool OnoTraceEnabled() {
  static const bool enabled=GetEnvironmentVariableW(L"AO4_ONO_TRACE",nullptr,0)!=0;
  return enabled;
}
thread_local uint32_t vo_select_soldier = 0;
thread_local uint32_t vo_select_player = 0;
struct VoSelectionScope {
  uint32_t soldier = vo_select_soldier, player = vo_select_player;
  VoSelectionScope(uint32_t s, uint32_t p) { vo_select_soldier=s; vo_select_player=p; }
  ~VoSelectionScope() { vo_select_soldier=soldier; vo_select_player=player; }
};
bool OnlineVoiceRebind() {
  static const bool enabled=GetEnvironmentVariableW(L"AO4_ONLINE_VOICE_REBIND",nullptr,0)!=0;
  return enabled;
}
struct OnlineVoiceState { uint32_t player=0, owner=0; bool alpha=false; };
std::mutex online_voice_mutex;
std::map<uint32_t,OnlineVoiceState> online_voice_states;
thread_local bool online_voice_rebinding=false;
// Game context pointer (lis r11,-31718 / lwz 6660); context+52 holds the player list object
// whose players live in [+120, +124).
constexpr uint32_t kGameContext = 0x841A1A04;
constexpr uint32_t kPlayerList = 52;
constexpr uint32_t kPlayersBegin = 120;
constexpr uint32_t kPlayersEnd = 124;
// Player fields used by the role code.
constexpr uint32_t kPlayerIndex = 972;     // vtable[8]; Player1 is the lowest index
constexpr uint32_t kPlayerIsBot = 804;      // excluded from Player1 unless kPlayerIsPai
constexpr uint32_t kPlayerIsPai = 840;      // BravoPAI requires it, BravoHuman requires it clear
constexpr uint32_t kPlayerSoldier = 984;    // soldier returned to the level logic
constexpr uint32_t kEntityData = 16;        // Entity::m_data
constexpr uint32_t kPmcOptions = 16;        // AO2PMCSplitterData / AO2PlayerEntitiesData

bool Enabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_ROLE_TRACE", nullptr, 0) != 0;
  return enabled;
}

bool IsReadable(uint8_t* base, uint32_t address, uint32_t length) {
  if (address < 0x40000000 || address >= 0xE0000000) {
    return false;
  }
  return Ao4CommittedRange(base + address, length);
}

std::string GuestString(uint8_t* base, uint32_t address) {
  std::string text;
  for (uint32_t i = 0; i < 64 && IsReadable(base, address + i, 1); ++i) {
    const uint8_t c = REX_LOAD_U8(address + i);
    if (!c || c < 0x20 || c >= 0x7F) {
      break;
    }
    text.push_back(char(c));
  }
  return text;
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  return IsReadable(base, address, 4) ? REX_LOAD_U32(address) : 0;
}

bool CoopLifecycleTraceEnabled() {
  static const bool enabled=GetEnvironmentVariableW(L"AO4_COOP_LIFECYCLE_TRACE",nullptr,0)!=0;
  return enabled;
}
std::string CoopLifecycleName(uint8_t* base,uint32_t event) {
  if (Load32(base,event)!=0x82A1CAB4) return {};
  const uint32_t pointer=Load32(base,event+8),prefix=Load32(base,pointer);
  if (prefix!=0x4F535F30 && prefix!=0x4F735F30) return {};
  const auto name=GuestString(base,pointer);
  return Ao4CoopLifecycleEvent(name)?name:std::string();
}

uint32_t Load8(uint8_t* base, uint32_t address) {
  return IsReadable(base, address, 1) ? REX_LOAD_U8(address) : 0xFF;
}

std::string DescribePlayer(uint8_t* base, uint32_t player) {
  if (!IsReadable(base, player, kPlayerSoldier + 4)) {
    return fmt::format("{:08X}", player);
  }
  const uint32_t vtable = REX_LOAD_U32(player);
  const uint32_t soldier = REX_LOAD_U32(player + kPlayerSoldier);
  const uint32_t soldier_data = Load32(base, soldier + kEntityData);
  return fmt::format("{:08X}[vt={:08X} slot2={:08X} index={} bot={} pai={} soldier={:08X} "
                     "soldier_vt={:08X} soldier_data={:08X} data_vt={:08X}]",
                     player, vtable, Load32(base, vtable + 8), REX_LOAD_U32(player + kPlayerIndex),
                     REX_LOAD_U8(player + kPlayerIsBot), REX_LOAD_U8(player + kPlayerIsPai),
                     soldier, Load32(base, soldier), soldier_data, Load32(base, soldier_data));
}

// Logs the player list whenever its membership or the per-player state changes.
void LogPlayers(uint8_t* base) {
  static std::string last;
  const uint32_t context = Load32(base, kGameContext);
  const uint32_t list = Load32(base, context + kPlayerList);
  const uint32_t begin = Load32(base, list + kPlayersBegin);
  const uint32_t end = Load32(base, list + kPlayersEnd);
  std::string text;
  for (uint32_t slot = begin; slot && slot < end && slot < begin + 16 * 4; slot += 4) {
    text += " " + DescribePlayer(base, Load32(base, slot));
  }
  if (text != last) {
    last = text;
    REXLOG_WARN("AO4_ROLE players context={:08X} list={:08X}:{}", context, list, text);
  }
}

// Logs a role query once per distinct (function, entity, argument, result).
void LogQuery(uint8_t* base, const char* what, uint32_t entity, uint32_t argument, uint32_t result,
              bool result_is_player) {
  static std::mutex lock;
  static std::map<std::tuple<const char*, uint32_t, uint32_t>, uint32_t> last;
  std::lock_guard<std::mutex> guard(lock);
  auto [it, inserted] = last.try_emplace({what, entity, argument}, result);
  if (!inserted && it->second == result) {
    return;
  }
  it->second = result;
  LogPlayers(base);
  if (last.size() > 4096) {
    return;
  }
  REXLOG_WARN("AO4_ROLE {} entity={:08X} arg={} -> {}", what, entity, argument,
              result_is_player ? DescribePlayer(base, result)
                               : fmt::format("{:08X} vt={:08X}", result, Load32(base, result)));
}

// Human (not a bot) and AI partner (bot with the PAI flag) from the player list.
bool FindHumanAndPartner(uint8_t* base, uint32_t& human, uint32_t& partner) {
  human = partner = 0;
  const uint32_t list = Load32(base, Load32(base, kGameContext) + kPlayerList);
  const uint32_t begin = Load32(base, list + kPlayersBegin);
  const uint32_t end = Load32(base, list + kPlayersEnd);
  for (uint32_t slot = begin; slot && slot < end && slot < begin + 16 * 4; slot += 4) {
    const uint32_t player = Load32(base, slot);
    if (!IsReadable(base, player, kPlayerSoldier + 4)) {
      continue;
    }
    if (!REX_LOAD_U8(player + kPlayerIsBot)) {
      human = human ? human : player;
    } else if (REX_LOAD_U8(player + kPlayerIsPai)) {
      partner = partner ? partner : player;
    }
  }
  return human && partner;
}

bool SwapEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_CONTROL_SWAP", nullptr, 0) != 0;
  return enabled;
}

DWORD g_server_thread = 0;      // thread that runs ServerPlayer::enterControllable
bool g_in_enter = false;        // inside enterControllable on the server thread
// Bravo mode in effect (roles, voice, appearance); follows ao4_bravo_wanted (the key) in the
// main menu, while loading and at spawn, see the hotkey thread and soldier_observer.cpp.
std::atomic<bool>& g_roles_swapped = ao4_bravo_mode;

// Edge-triggered key press, only while this process owns the foreground window.
bool KeyPressed(int key, bool& was_down) {
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  const bool down = process == GetCurrentProcessId() && (GetAsyncKeyState(key) & 0x8000);
  const bool pressed = down && !was_down;
  was_down = down;
  return pressed;
}
}  // namespace

// ServerPlayer::enterControllable wrapper: also binds input for a human player.
// Releasing (controllable 0) goes straight to enterControllable, like the game does: the
// wrapper would bind input to a null soldier.
extern "C" REX_FUNC(sub_8313A410);
extern "C" REX_FUNC(__imp__sub_8313A170);

namespace {
// Calls the original enter wrapper (player, controllable) without disturbing the caller.
void EnterControllable(PPCContext& ctx, uint8_t* base, uint32_t player, uint32_t controllable) {
  auto saved = std::make_unique<PPCContext>(ctx);
  ctx.r1.u64 = ctx.r1.u32 - 0x400;
  ctx.r3.u64 = player;
  ctx.r4.u64 = controllable;
  g_in_enter = true;
  if (controllable) {
    sub_8313A410(ctx, base);
  } else {
    __imp__sub_8313A170(ctx, base);
  }
  g_in_enter = false;
  ctx = *saved;
}

// Runs on the server thread at a point where no enterControllable is in progress.
void PollSwapKeys(PPCContext& ctx, uint8_t* base) {
  static bool f8 = false;
  bool toggle_control = KeyPressed(VK_F8, f8);
  static const int auto_seconds = [] {
    wchar_t text[16] = {};
    return GetEnvironmentVariableW(L"AO4_CONTROL_SWAP_AUTO", text, 16) ? _wtoi(text) : -1;
  }();
  static ULONGLONG ready_since = 0;
  static bool auto_done = false;
  if (auto_seconds >= 0 && !auto_done) {
    uint32_t human, partner;
    if (FindHumanAndPartner(base, human, partner) && REX_LOAD_U32(human + kPlayerSoldier) &&
        REX_LOAD_U32(partner + kPlayerSoldier)) {
      ready_since = ready_since ? ready_since : GetTickCount64();
      if (GetTickCount64() - ready_since >= ULONGLONG(auto_seconds) * 1000) {
        auto_done = toggle_control = true;
      }
    } else {
      ready_since = 0;
    }
  }
  if (!toggle_control) {
    return;
  }
  uint32_t human, partner;
  if (!FindHumanAndPartner(base, human, partner)) {
    REXLOG_WARN("AO4_CONTROL_SWAP ignored: human and AI partner not both present");
    return;
  }
  if (toggle_control) {
    const uint32_t human_soldier = REX_LOAD_U32(human + kPlayerSoldier);
    const uint32_t partner_soldier = REX_LOAD_U32(partner + kPlayerSoldier);
    if (!human_soldier || !partner_soldier) {
      REXLOG_WARN("AO4_CONTROL_SWAP control ignored: soldier missing ({:08X}/{:08X})",
                  human_soldier, partner_soldier);
      return;
    }
    REXLOG_WARN("AO4_CONTROL_SWAP control: before human={} partner={}",
                DescribePlayer(base, human), DescribePlayer(base, partner));
    EnterControllable(ctx, base, human, 0);
    EnterControllable(ctx, base, partner, 0);
    EnterControllable(ctx, base, human, partner_soldier);
    EnterControllable(ctx, base, partner, human_soldier);
    REXLOG_WARN("AO4_CONTROL_SWAP control: after human={} partner={}",
                DescribePlayer(base, human), DescribePlayer(base, partner));
  }
}

uint32_t PmcOption(uint8_t* base, uint32_t entity) {
  return Load32(base, Load32(base, entity + kEntityData) + kPmcOptions);
}
}  // namespace

// Player1 lookup: lowest (r4 != 0) or highest player index among humans and PAI.
uint32_t AiPartnerForBravoPai(PPCContext& ctx, uint8_t* base, bool server, uint32_t entity);

namespace {
// The role swap only applies to the Alpha/Bravo role entities (cutscene placement, speakers,
// BravoPAI orders). Every other Player1 user (area triggers, AI following, progression)
// keeps the human as Player1, otherwise all friendly AI waits for the AI partner.
bool IsRoleCaller(uint32_t caller) {
  switch (caller) {
    case 0x8342A77C:  // ServerAO2PlayerEntities role resolver (sub_8342A750)
    case 0x8342DE40:  // ServerAO2PlayerEntities soldier getter (sub_8342DE20)
    case 0x832C52A4:  // ClientAO2PlayerEntities role resolver (sub_832C5278)
      return true;
    default:
      return false;
  }
}
thread_local bool t_force_role_swap = false;  // set by AiPartnerForBravoPai
thread_local bool t_client_soldier_query = false;  // inside ClientAO2PlayerEntities soldier getter
thread_local bool t_client_original_identity = false;  // event reference resolution

// The client swap only applies to soldier queries (cutscene placement). Player queries keep
// the human as Alpha: a cutscene in the prologue (after Fiona_Young spawns) uses Alpha's
// player directly and crashed with the AI partner's player (invalid guest call).
// AO4_CLIENT_SWAP_PLAYERS restores the old behavior (player queries swapped too).
bool ClientSwapApplies() {
  static const bool players = GetEnvironmentVariableW(L"AO4_CLIENT_SWAP_PLAYERS", nullptr, 0) != 0;
  return !t_client_original_identity && (t_client_soldier_query || players);
}

// AO4_ROLE_SWAP_SERVER: also swap the server-side role entities. Off by default: the level
// scripts use the server's Alpha as the leader all friendly AI waits for and follows, so
// a server swap stalls them. The client side (cutscene placement) is always swapped.
bool ServerRolesSwapped() {
  static const bool server = GetEnvironmentVariableW(L"AO4_ROLE_SWAP_SERVER", nullptr, 0) != 0;
  return g_roles_swapped && server;
}

// Logs the first call of a role function from each guest call site.
void LogCaller(const char* what, uint32_t caller, uint32_t result) {
  if (!Enabled()) {
    return;
  }
  static std::mutex lock;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> guard(lock);
  if (seen.size() < 4096 && seen.insert((uint64_t(uintptr_t(what)) << 32) | caller).second) {
    REXLOG_WARN("AO4_ROLE caller {} lr={:08X} -> {:08X}", what, caller, result);
  }
}
}  // namespace

REX_HOOK_RAW(sub_83428168) {
  const uint32_t entity = ctx.r3.u32, lowest = ctx.r4.u32 & 0xFF, count = ctx.r5.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_83428168(ctx, base);
  // With exactly two candidates (human and AI partner) the other one is the opposite end.
  if (ServerRolesSwapped() && (IsRoleCaller(caller) || t_force_role_swap) &&
      Load32(base, count) == 2) {
    const uint32_t r1 = ctx.r1.u32;
    ctx.r3.u64 = entity;
    ctx.r4.u64 = lowest ? 0 : 1;
    ctx.r5.u64 = count;
    __imp__sub_83428168(ctx, base);
    ctx.r1.u64 = r1;
  }
  LogCaller("player1_lookup", caller, ctx.r3.u32);
  if (Enabled()) {
    LogQuery(base, "player1_lookup", entity, lowest, ctx.r3.u32, true);
  }
}

// Client side Player1 lookup (client player list), same rule as the server one.
REX_HOOK_RAW(sub_832C2A30) {
  const uint32_t lowest = ctx.r3.u32 & 0xFF, count = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_832C2A30(ctx, base);
  if (g_roles_swapped && !t_client_original_identity &&
      ((IsRoleCaller(caller) && ClientSwapApplies()) || t_force_role_swap) &&
      Load32(base, count) == 2) {
    const uint32_t r1 = ctx.r1.u32;
    ctx.r3.u64 = lowest ? 0 : 1;
    ctx.r4.u64 = count;
    __imp__sub_832C2A30(ctx, base);
    ctx.r1.u64 = r1;
  }
  LogCaller("client_player1_lookup", caller, ctx.r3.u32);
  if (Enabled()) {
    LogQuery(base, "client_player1_lookup", 0, lowest, ctx.r3.u32, true);
  }
}

namespace {
DWORD g_client_thread = 0;  // thread that runs the client role entities (client update)
}  // namespace

// ClientAO2PlayerEntities: soldier of the resolved player (vtable slot 20, calls slot 21).
REX_HOOK_RAW(sub_832C8058) {
  // Diagnostic (Alpha mode: AI gear only after ~80 s): which thread claims the client role.
  if (!Ao4Qol().gear_reload_fix) {
  if (const DWORD thread = GetCurrentThreadId(); thread != g_client_thread) {
    static std::atomic<uint32_t> changes{0};
    if (changes.fetch_add(1, std::memory_order_relaxed) < 64)
      REXLOG_WARN("AO4_CLIENT_THREAD {} -> {} lr={:08X} bravo={}", g_client_thread, thread,
                  uint32_t(ctx.lr), g_roles_swapped.load());
  }
  g_client_thread = GetCurrentThreadId();
  }
  const uint32_t entity = ctx.r3.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  const bool outer = t_client_soldier_query;
  const bool original_outer = t_client_original_identity;
  // sub_833CD378 resolves an entity reference through vtable slot 20. Church Hide
  // events use this path, while networked Show events retain the original player.
  // Preserve identity here rather than swapping the recipient of the Hide event.
  t_client_original_identity = original_outer || caller == 0x833CD414;
  t_client_soldier_query = true;
  __imp__sub_832C8058(ctx, base);
  t_client_soldier_query = outer;
  t_client_original_identity = original_outer;
  if (Enabled()) {
    static std::mutex lock;
    static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
    std::lock_guard<std::mutex> guard(lock);
    if (seen.size() < 512 && seen.emplace(entity, caller, ctx.r3.u32).second) {
      REXLOG_WARN("AO4_ROLE soldier_query_source entity={:08X} option={} caller={:08X} "
                  "result={:08X} role_swap={} nested={}",
                  entity, PmcOption(base, entity), caller, ctx.r3.u32, g_roles_swapped.load(), outer);
    }
  }
}

// ClientAO2PlayerEntities: player for Alpha(0)/Bravo(1)/BravoPAI(2)/BravoHuman(3).
REX_HOOK_RAW(sub_832C5340) {
  const uint32_t entity = ctx.r3.u32;
  LogCaller(t_client_soldier_query ? "client_player_entities(soldier)"
                                   : "client_player_entities(player)",
            uint32_t(ctx.lr), entity);
  __imp__sub_832C5340(ctx, base);
  if (g_roles_swapped && !t_client_original_identity && !ctx.r3.u32 &&
      PmcOption(base, entity) == 2) {
    ctx.r3.u64 = AiPartnerForBravoPai(ctx, base, false, entity);
  }
  if (Enabled()) {
    LogQuery(base, "client_player_entities_player", entity, PmcOption(base, entity), ctx.r3.u32,
             true);
  }
}

// With the roles swapped, the scripts' "BravoPAI" (AI-only Bravo orders, e.g. the automatic
// boost) would resolve to nothing, because Bravo is now the human. Point it at the AI
// partner instead, which is Player1 under the swap. `lookup` is the (hooked) Player1 lookup.
uint32_t AiPartnerForBravoPai(PPCContext& ctx, uint8_t* base, bool server, uint32_t entity) {
  auto saved = std::make_unique<PPCContext>(ctx);
  const uint32_t count = ctx.r1.u32 - 0x400;
  ctx.r1.u64 = ctx.r1.u32 - 0x800;
  t_force_role_swap = true;
  if (server) {
    ctx.r3.u64 = entity;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = count;
    sub_83428168(ctx, base);
  } else {
    ctx.r3.u64 = 1;
    ctx.r4.u64 = count;
    sub_832C2A30(ctx, base);
  }
  t_force_role_swap = false;
  const uint32_t player = ctx.r3.u32;
  ctx = *saved;
  return IsReadable(base, player, kPlayerIsPai + 1) && REX_LOAD_U8(player + kPlayerIsPai) ? player
                                                                                         : 0;
}

// ServerAO2PlayerEntities: player for Alpha(0)/Bravo(1)/BravoPAI(2)/BravoHuman(3).
REX_HOOK_RAW(sub_8342A750) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_8342A750(ctx, base);
  if (ServerRolesSwapped() && !ctx.r3.u32 && PmcOption(base, entity) == 2) {
    ctx.r3.u64 = AiPartnerForBravoPai(ctx, base, true, entity);
  }
  if (Enabled()) {
    LogQuery(base, "player_entities_player", entity, PmcOption(base, entity), ctx.r3.u32, true);
  }
}

// ServerAO2PlayerEntities: soldier of that player.
REX_HOOK_RAW(sub_8342DE20) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_8342DE20(ctx, base);
  if (Enabled()) {
    LogQuery(base, "player_entities_soldier", entity, PmcOption(base, entity), ctx.r3.u32, false);
  }
}

// ServerAO2PMCSplitterEntity: soldier for Player1(0)/Player1Partner(1)/FirstPlayerInList(2).
REX_HOOK_RAW(sub_83433CD8) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_83433CD8(ctx, base);
  if (Enabled()) {
    LogQuery(base, "pmc_splitter_soldier", entity, PmcOption(base, entity), ctx.r3.u32, false);
  }
}

// ServerGetPartnerEntity: partner soldier.
REX_HOOK_RAW(sub_834278F8) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_834278F8(ctx, base);
  if (Enabled()) {
    LogQuery(base, "get_partner_soldier", entity, 0, ctx.r3.u32, false);
  }
}

// ServerPlayer: take control of a controllable (soldier -> +984, controllable -> +988).
REX_HOOK_RAW(sub_8313A170) {
  const uint32_t player = ctx.r3.u32, controllable = ctx.r4.u32;
  g_server_thread = GetCurrentThreadId();
  if (Enabled()) {
    REXLOG_WARN("AO4_ROLE enter_controllable thread={} player={} new={:08X} vt={:08X} lr={:08X}",
                GetCurrentThreadId(), DescribePlayer(base, player), controllable,
                Load32(base, controllable), uint32_t(ctx.lr));
  }
  const bool outer = !g_in_enter;
  g_in_enter = true;
  __imp__sub_8313A170(ctx, base);
  g_in_enter = !outer;
  if (Enabled()) {
    REXLOG_WARN("AO4_ROLE enter_controllable done player={}", DescribePlayer(base, player));
  }
}

// ServerPlayer index getter: call rate per thread, to find a periodic hook point.
REX_HOOK_RAW(sub_830733E0) {
  const uint32_t aim_source_owner = ctx.r3.u32;
  __imp__sub_830733E0(ctx, base);
  Ao4ObserveAimSource(ctx, base, aim_source_owner, ctx.r3.u32, false);
  if (SwapEnabled() && !g_in_enter && GetCurrentThreadId() == g_server_thread) {
    PollSwapKeys(ctx, base);
  }
  if (Enabled()) {
    static std::mutex lock;
    static std::map<uint32_t, uint32_t> calls;
    static ULONGLONG last = GetTickCount64();
    std::lock_guard<std::mutex> guard(lock);
    ++calls[GetCurrentThreadId()];
    if (GetTickCount64() - last >= 5000) {
      last = GetTickCount64();
      std::string text;
      for (auto& [thread, count] : calls) {
        text += fmt::format(" {}:{}", thread, count);
      }
      calls.clear();
      REXLOG_WARN("AO4_ROLE index_getter_calls_5s{}", text);
    }
  }
}

// Appearance: customization objects carry the role at +4 (0 Alpha, 1 Bravo) and pick the
// matching AO4CustomizationAsset fields. With AO4_SKIN_SWAP the role is flipped for the
// duration of the call only, so Alpha's objects take Bravo's look and vice versa.
namespace {
constexpr uint32_t kCustomizationRole = 4;

void CallWithFlippedRole(PPCContext& ctx, uint8_t* base, const char* what,
                         void (*original)(PPCContext&, uint8_t*)) {
  static const bool swap = GetEnvironmentVariableW(L"AO4_SKIN_SWAP", nullptr, 0) != 0;
  const uint32_t object = ctx.r3.u32;
  const bool valid = IsReadable(base, object + kCustomizationRole, 4);
  const uint32_t role = valid ? REX_LOAD_U32(object + kCustomizationRole) : 0xFFFFFFFF;
  if (Enabled() || swap) {
    static std::mutex lock;
    static std::map<std::pair<const char*, uint32_t>, uint32_t> seen;
    std::lock_guard<std::mutex> guard(lock);
    if (seen.size() < 1024 && seen.try_emplace({what, object}, role).second) {
      REXLOG_WARN("AO4_SKIN {} object={:08X} role={} swap={} lr={:08X}", what, object, role, swap,
                  uint32_t(ctx.lr));
    }
  }
  // AO4_SKIN_BRAVO: sub_832EC0D8 picks AlphaSkin for role 0, BravoSkin otherwise, and puts it on
  // the soldier it is given (slot 2). It runs only while loading; in levels the skin is set by
  // the client-thread hook below. With the roles swapped, slot 0 takes BravoSkin here too.
  // Outfit, mask and tattoo keep the profile choice (+4 is also the profile slot).
  static const bool skin_bravo = GetEnvironmentVariableW(L"AO4_SKIN_BRAVO", nullptr, 0) != 0;
  const bool flip = (swap && role <= 1) ||
                    (skin_bravo && g_roles_swapped && role == 0 && strcmp(what, "skin") == 0);
  if (flip) {
    REX_STORE_U32(object + kCustomizationRole, role ^ 1);
  }
  original(ctx, base);
  if (flip) {
    REX_STORE_U32(object + kCustomizationRole, role);
  }
}
}  // namespace

REX_HOOK_RAW(sub_832EC0D8) { CallWithFlippedRole(ctx, base, "skin", __imp__sub_832EC0D8); }

REX_HOOK_RAW(sub_832F34A0) { CallWithFlippedRole(ctx, base, "mask", __imp__sub_832F34A0); }
REX_HOOK_RAW(sub_832F43C0) { CallWithFlippedRole(ctx, base, "outfit", __imp__sub_832F43C0); }
REX_HOOK_RAW(sub_832F35C8) { CallWithFlippedRole(ctx, base, "tattoo", __imp__sub_832F35C8); }

// Character voice selection: if the soldier's voice name is "Alpha" and bit 0x40 of
// player+1113 is clear, the game switches it to "Bravo" (second player in co-op). With
// the roles swapped, the human takes the Bravo branch and the AI partner keeps "Alpha".
REX_HOOK_RAW(sub_83323F50) {
  VoSelectionScope selection(ctx.r3.u32, ctx.r4.u32);
  const uint32_t soldier = ctx.r3.u32, player = ctx.r4.u32;
  constexpr uint32_t kVoiceFlags = 1113;
  // Server player and client player (r4 here is the client player of the soldier).
  const bool is_player = IsReadable(base, player, kVoiceFlags + 1) &&
                         (REX_LOAD_U32(player) == 0x82A576F0 || REX_LOAD_U32(player) == 0x82A3F850);
  // Voice name: soldier+540 -> +264 -> string object at +12 (first word: character pointer).
  const uint32_t voice_owner = Load32(base, Load32(base, soldier + 540) + 264);
  const uint32_t voice_word = Load32(base, voice_owner + 12);
  const uint8_t flags = is_player ? REX_LOAD_U8(player + kVoiceFlags) : 0;
  if (OnlineVoiceTrace()) {
    static std::atomic<unsigned> lines{0};
    if (lines.fetch_add(1)<96)
      REXLOG_WARN("AO4_ONLINE_VO_SELECT soldier={:08X} player={:08X} vt={:08X} flags={:02X} name={} owner={:08X}",
        soldier,player,Load32(base,player),flags,GuestString(base,voice_word),voice_owner);
  }
  uint8_t patched = flags;
  if (g_roles_swapped && is_player) {
    patched = REX_LOAD_U8(player + kPlayerIsPai) ? uint8_t(flags | 0x40) : uint8_t(flags & ~0x40);
    REX_STORE_U8(player + kVoiceFlags, patched);
  }
  if (Enabled()) {
    REXLOG_WARN("AO4_VOICE select soldier={:08X} r4={:08X} vt={:08X} player={} bot={} pai={} "
                "flags={:02X}->{:02X} voice_owner={:08X} word={:08X} text='{}' direct='{}'",
                soldier, player, Load32(base, player), is_player,
                is_player ? REX_LOAD_U8(player + kPlayerIsBot) : 0xFF,
                is_player ? REX_LOAD_U8(player + kPlayerIsPai) : 0xFF, flags, patched, voice_owner,
                voice_word, GuestString(base, voice_word), GuestString(base, voice_owner + 12));
  }
  __imp__sub_83323F50(ctx, base);
  if (OnlineVoiceRebind() && is_player && !online_voice_rebinding &&
      GuestString(base,voice_word)=="Alpha") {
    std::lock_guard<std::mutex> lock(online_voice_mutex);
    if(online_voice_states.size()>256) online_voice_states.clear();
    online_voice_states[soldier]={player,voice_owner,(flags&0x40)!=0};
  }
  if (patched != flags) {
    REX_STORE_U8(player + kVoiceFlags, flags);
  }
}

// ServerPlayer::getPartner (vtable slot 2): first other same-team player in the list.
REX_HOOK_RAW(sub_83132EF0) {
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_83132EF0(ctx, base);
  LogCaller("get_partner_player", caller, ctx.r3.u32);
}

// Speaker assignment: stores the djb2-xor hash of the speaker name ("Alpha", "Bravo", ...)
// at object+16 unless object+20 overrides it. Logged per (object, name).
REX_HOOK_RAW(sub_83350AB0) {
  const uint32_t object = ctx.r3.u32, name_object = ctx.r4.u32, caller = uint32_t(ctx.lr);
  const uint32_t override_value = Load32(base, object + 20);
  const std::string name = GuestString(base, Load32(base, name_object));
  __imp__sub_83350AB0(ctx, base);
  if ((VoIdentityTraceEnabled() || OnoTraceEnabled()) && (name == "Alpha" || name == "Bravo")) {
    static std::atomic<uint32_t> count{0};
    if (count.fetch_add(1, std::memory_order_relaxed) < 128)
      REXLOG_WARN("AO4_VO_IDENTITY soldier={:08X} player={:08X} package_object={:08X} "
                  "requested={} final_hash={:08X} override={:08X} bravo={} lr={:08X}",
                  vo_select_soldier, vo_select_player, object, name,
                  Load32(base, object+16), override_value, g_roles_swapped.load(), caller);
  }
  if (Enabled()) {
    static std::mutex lock;
    static std::map<uint32_t, std::string> last;
    std::lock_guard<std::mutex> guard(lock);
    auto [it, inserted] = last.try_emplace(object, name);
    if (inserted || it->second != name) {
      it->second = name;
      REXLOG_WARN("AO4_VOICE speaker object={:08X} name='{}' override={:08X} hash={:08X} lr={:08X}",
                  object, name, override_value, Load32(base, object + 16), caller);
    }
  }
}

// Voice-over property query of a client soldier (AO2ClientSoldierEntity vtable method):
// r5 = property (name hash at +12), r6 = soldier, r7 = value out (type at +40, data at +0).
// The property hashes are built from a name table at 0x841BE838 (Person, Health, ...,
// Partner, IsAI, Killer, WasAgonizing). The VO logic uses them in its conditions, e.g. to
// pick a radio-filtered variant for a speaker that is not the human.
//   AO4_ROLE_TRACE: log each distinct (property, soldier, result) once.
//   AO4_VO_ISAI_SWAP: with the roles swapped, the human reports IsAI = true and the AI
//                     partner IsAI = false, so the VO logic treats the AI partner (Alpha)
//                     as the player character.
extern "C" REX_FUNC(sub_82E41600);  // set bool value (value, bool, 0)

namespace {
constexpr uint32_t kVoPropertyHashes = 0x841BE838;
constexpr const char* kVoPropertyNames[] = {
    "Person",          "Health",         "NormalizedHealth", "Position",       "TeamId",
    "CurrentWeapon",   "CurrentVehicle", "ManDownTime",      "LastHealthChange", "IsDead",
    "IsManDown",       "GetClosestTeammate", "GetClosestEnemy", "ClosestAttacker",
    "ClosestShooter",  "Attackers",      "Shooters",         "TotalEnemies",   "IsInCover",
    "IsHeadShot",      "Partner",        "IsAI",             "Killer",         "WasAgonizing"};
constexpr uint32_t kVoIsAiIndex = 21;
constexpr uint32_t kClientSoldierPlayer = 204;

std::string VoPropertyName(uint8_t* base, uint32_t hash) {
  for (uint32_t i = 0; i < std::size(kVoPropertyNames); ++i) {
    if (Load32(base, kVoPropertyHashes + 4 * i) == hash) {
      return kVoPropertyNames[i];
    }
  }
  return fmt::format("hash_{:08X}", hash);
}

bool VoIsAiSwapEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_VO_ISAI_SWAP", nullptr, 0) != 0;
  return enabled;
}
}  // namespace

REX_HOOK_RAW(sub_83356FA0) {
  const uint32_t property = ctx.r5.u32, soldier = ctx.r6.u32, value = ctx.r7.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t hash = Load32(base, property + 12);
  __imp__sub_83356FA0(ctx, base);
  const uint32_t handled = ctx.r3.u32;
  const uint32_t player = Load32(base, soldier + kClientSoldierPlayer);
  const bool is_player = IsReadable(base, player, kPlayerIsPai + 1) &&
                         (Load32(base, player) == 0x82A3F850 || Load32(base, player) == 0x82A576F0);
  const uint32_t bot = is_player ? REX_LOAD_U8(player + kPlayerIsBot) : 0xFF;
  const uint32_t pai = is_player ? REX_LOAD_U8(player + kPlayerIsPai) : 0xFF;
  const bool is_ai_query = hash && hash == Load32(base, kVoPropertyHashes + 4 * kVoIsAiIndex);
  bool flipped = false;
  if (handled && is_ai_query && is_player && g_roles_swapped && VoIsAiSwapEnabled() &&
      (!bot || pai)) {
    // Human -> "AI", AI partner -> "not AI".
    const uint32_t r1 = ctx.r1.u32;
    ctx.r3.u64 = value;
    ctx.r4.u64 = bot ? 0 : 1;
    ctx.r5.u64 = 0;
    sub_82E41600(ctx, base);
    ctx.r1.u64 = r1;
    ctx.r3.u64 = handled;
    flipped = true;
  }
  if (Enabled()) {
    const uint32_t type = Load32(base, value + 40);
    const uint32_t data = Load32(base, value);
    static std::mutex lock;
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> seen;
    std::lock_guard<std::mutex> guard(lock);
    auto [it, inserted] = seen.try_emplace({hash, soldier, caller}, data);
    // Changes are only logged for bool/int results; positions and floats change constantly.
    if ((inserted || (type <= 1 && it->second != data)) && seen.size() < 4096) {
      it->second = data;
      REXLOG_WARN("AO4_VOPROP {} soldier={:08X} player={:08X} bot={} pai={} handled={} "
                  "type={} data={:08X}{} lr={:08X}",
                  VoPropertyName(base, hash), soldier, player, bot, pai, handled, type, data,
                  flipped ? " (IsAI swapped)" : "", caller);
    }
  }
}

// Conversation track sources: VoiceOverConversationEntity (sub_82F6EB80) resolves each track's
// source straight from the level's character spawners, not from the Alpha/Bravo role entities,
// so Alpha's track always plays on the human's soldier and Bravo's on the AI partner's.
// The VO system then filters the partner's lines as radio. sub_82E56538(vo, conversation,
// sources, 0, count) receives the resolved {client soldier, VO object} pairs (8 bytes each).
//   AO4_VO_SOURCE_SWAP: with the roles swapped, exchange the human's and the AI partner's
//                       soldier in these pairs, so Bravo's lines come from the human.
namespace {
constexpr uint32_t kClientContext = 0x841350DC;     // context+52 -> client player list
constexpr uint32_t kClientPlayersBegin = 152;
constexpr uint32_t kClientPlayersEnd = 156;
constexpr uint32_t kClientPlayerSoldierLink = 1004;  // *(*(player+1004)) - 4 = soldier
constexpr uint32_t kClientPlayerVtable = 0x82A3F850;

bool VoSourceSwapEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_VO_SOURCE_SWAP", nullptr, 0) != 0;
  return enabled;
}

uint32_t ClientPlayerSoldier(uint8_t* base, uint32_t player) {
  const uint32_t link = Load32(base, Load32(base, player + kClientPlayerSoldierLink));
  return link ? link - 4 : 0;
}

// Client soldiers of the human and the AI partner.
bool FindClientSoldiers(uint8_t* base, uint32_t& human, uint32_t& partner) {
  human = partner = 0;
  const uint32_t list = Load32(base, Load32(base, kClientContext) + kPlayerList);
  const uint32_t begin = Load32(base, list + kClientPlayersBegin);
  const uint32_t end = Load32(base, list + kClientPlayersEnd);
  for (uint32_t slot = begin; slot && slot < end && slot < begin + 16 * 4; slot += 4) {
    const uint32_t player = Load32(base, slot);
    if (!IsReadable(base, player, kClientPlayerSoldierLink + 4) ||
        REX_LOAD_U32(player) != kClientPlayerVtable) {
      continue;
    }
    if (!REX_LOAD_U8(player + kPlayerIsBot)) {
      human = human ? human : ClientPlayerSoldier(base, player);
    } else if (REX_LOAD_U8(player + kPlayerIsPai)) {
      partner = partner ? partner : ClientPlayerSoldier(base, player);
    }
  }
  return human && partner;
}

// Local split screen (or any second human client player): the equipment QoL acts on the
// AI partner through customization slot 1, which then belongs to the human player 2.
// AO4_LOCAL_COOP (split screen starter) or two human client players seen in a level; a PAI
// partner clears it again.
std::atomic<bool> g_human_coop{false};
bool LocalCoopActive() {
  static const bool forced = GetEnvironmentVariableW(L"AO4_LOCAL_COOP", nullptr, 0) != 0;
  return forced || g_human_coop.load();
}
void ObserveHumanCoop(uint8_t* base) {
  const uint32_t list = Load32(base, Load32(base, kClientContext) + kPlayerList);
  const uint32_t begin = Load32(base, list + kClientPlayersBegin);
  const uint32_t end = Load32(base, list + kClientPlayersEnd);
  uint32_t humans = 0;
  bool pai = false;
  for (uint32_t slot = begin; slot && slot < end && slot < begin + 16 * 4; slot += 4) {
    const uint32_t player = Load32(base, slot);
    if (!IsReadable(base, player, kPlayerIsPai + 1) || REX_LOAD_U32(player) != kClientPlayerVtable) {
      continue;
    }
    if (!REX_LOAD_U8(player + kPlayerIsBot)) ++humans;
    else if (REX_LOAD_U8(player + kPlayerIsPai)) pai = true;
  }
  const bool coop = humans >= 2 ? true : pai ? false : g_human_coop.load();
  if (coop != g_human_coop.exchange(coop)) {
    REXLOG_WARN("AO4_LOCAL_COOP {} (human players={} pai={})", coop ? "on" : "off", humans, pai);
  }
}

// VO object of a client soldier, as sub_82F6EB80 builds it: soldier+540 -> +264 -> +8.
uint32_t SoldierVoObject(uint8_t* base, uint32_t soldier) {
  const uint32_t owner = Load32(base, Load32(base, soldier + 540) + 264);
  return owner ? Load32(base, owner + 8) : 0;
}

// ONO diagnostics use safe bulk reads rather than per-character VirtualQuery.
uint32_t OnoWord(uint8_t* base,uint32_t address) {
  uint8_t bytes[4]{};SIZE_T read=0;
  if (address<0x40000000 || address>0xDFFFFFFC ||
      !ReadProcessMemory(GetCurrentProcess(),base+address,bytes,4,&read) || read!=4) return 0;
  return (uint32_t(bytes[0])<<24)|(uint32_t(bytes[1])<<16)|(uint32_t(bytes[2])<<8)|bytes[3];
}
std::string OnoPath(uint8_t* base,uint32_t address) {
  std::string text;
  for (uint32_t offset=0;offset<512;offset+=64) {
    const uint64_t at=uint64_t(address)+offset;
    if (at<0x40000000 || at+64>0xE0000000ull) return {};
    char bytes[64]{};SIZE_T read=0;
    ReadProcessMemory(GetCurrentProcess(),base+at,bytes,sizeof(bytes),&read);
    for (size_t i=0;i<read;++i) {
      if (!bytes[i]) return text;
      if (uint8_t(bytes[i])<0x20 || uint8_t(bytes[i])>=0x7F) return {};
      text+=bytes[i];
    }
    if (read!=sizeof(bytes)) return {};
  }
  return {};
}
int OnoRole(uint8_t* base,uint32_t actor,uint32_t& human,uint32_t& partner) {
  if (!actor) return -1;
  struct Roles { uint32_t human=0,partner=0; ULONGLONG at=0; };
  thread_local Roles cache;
  const auto now=GetTickCount64();
  const bool refreshed=!cache.at || now-cache.at>=200;
  if (refreshed) {FindClientSoldiers(base,cache.human,cache.partner);cache.at=now;}
  if (actor!=cache.human && actor!=cache.partner) return -1;
  // Enemy traffic only sees the short-lived candidate cache. Before labelling
  // either primary actor, validate membership freshly (address reuse/load).
  if (refreshed) {human=cache.human;partner=cache.partner;}
  else if (!FindClientSoldiers(base,human,partner)) return -1;
  return actor==human?0:actor==partner?1:-1;
}
bool OnoBudget(unsigned stream,unsigned role,unsigned kind) {
  static std::atomic<uint32_t> counts[3][2][4]{};
  return counts[stream][role][kind].fetch_add(1,std::memory_order_relaxed)<32;
}
void TraceOnoParameter(PPCContext& ctx,uint8_t* base) {
  const uint32_t event=OnoWord(base,ctx.r3.u32+220);
  if (OnoWord(base,event)!=0x82A1CAB4) return;
  const uint32_t name_pointer=OnoWord(base,event+8),prefix=OnoWord(base,name_pointer);
  if (prefix!=0x47656E65 && prefix!=0x46726965) return; // General / Friendly
  const std::string name=OnoPath(base,name_pointer);
  const int kind=Ao4OnoEventKind(name);
  if (kind<0) return;
  uint32_t human=0,partner=0;
  const int role=OnoRole(base,ctx.r6.u32,human,partner);
  if (role<0 || !OnoBudget(0,unsigned(role),unsigned(kind))) return;
  const uint32_t hash=OnoWord(base,ctx.r4.u32+12);
  REXLOG_WARN("AO4_ONO_PARAMETER event={} kind={} context={:08X} key={:08X} key_hash={:08X} sender={} actor={:08X} role={} vo_class={:08X} human={:08X} partner={:08X} bravo={} lr={:08X}",
      name,kind,ctx.r3.u32,ctx.r4.u32,hash,hash==3337816366u,ctx.r6.u32,
      role==0?"human":"partner",ctx.r5.u32,human,partner,g_roles_swapped.load(),uint32_t(ctx.lr));
}
struct OnoPackageProbe { uint32_t actor=0,component=0,hash=0,cached=0; };
thread_local OnoPackageProbe* ono_package_probe=nullptr;
struct OnoPlay {
  uint32_t actor=0,wave=0,track=0,clip=0,conversation=0,vo_class=0,caller=0,human=0,partner=0;
  int role=-1,kind=-1;
  bool bravo=false;
  std::string path;
};
OnoPlay ObserveOnoPlay(PPCContext& ctx,uint8_t* base,uint32_t wave) {
  OnoPlay sample;
  if (!OnoTraceEnabled()) return sample;
  // Original 82E5E378, immediately after package override selection:
  // r25 = 40-byte track state, +8 = actor weakref, +12 = VO class;
  // r24 = clip, r21 = conversation, r30 = selected SoundWaveAsset.
  sample.track=ctx.r25.u32;
  const uint32_t raw=OnoWord(base,OnoWord(base,sample.track+8));
  sample.actor=raw>=4?raw-4:0;
  sample.role=OnoRole(base,sample.actor,sample.human,sample.partner);
  if (sample.role<0) return sample;
  sample.path=OnoPath(base,OnoWord(base,wave+8));
  sample.kind=Ao4OnoWaveKind(sample.path);
  if (sample.kind<0) {
    if (sample.path.empty()) {
      static std::atomic<uint32_t> unresolved{0};
      if (unresolved.fetch_add(1,std::memory_order_relaxed)<8)
        REXLOG_WARN("AO4_ONO_UNRESOLVED actor={:08X} wave={:08X} name_pointer={:08X}; no valid asset path",sample.actor,wave,OnoWord(base,wave+8));
    }
    return sample;
  }
  sample.wave=wave;sample.clip=ctx.r24.u32;sample.conversation=ctx.r21.u32;
  sample.vo_class=OnoWord(base,sample.track+12);sample.caller=uint32_t(ctx.lr);
  sample.bravo=g_roles_swapped.load();
  return sample;
}
void LogOnoPlay(const OnoPlay& s,bool accepted) {
  if (s.role<0 || s.kind<0) return;
  // Failed loads can retry every frame. Keep the first attempt and transitions.
  thread_local std::map<std::tuple<uint32_t,uint32_t,uint32_t>,std::pair<bool,ULONGLONG>> recent;
  if (recent.size()>128) recent.clear();
  auto [it,inserted]=recent.try_emplace({s.actor,s.wave,s.clip},std::make_pair(accepted,GetTickCount64()));
  const auto now=GetTickCount64();
  if (!inserted && it->second.first==accepted && now-it->second.second<1000) return;
  it->second={accepted,now};
  if (!OnoBudget(1,unsigned(s.role),unsigned(s.kind))) return;
  const std::string_view actual=Ao4OnoWaveVoice(s.path);
  const std::string_view expected=(s.role==0)==s.bravo?"Bravo":"Alpha";
  REXLOG_WARN("AO4_ONO_PLAY kind={} actor={:08X} role={} human={:08X} partner={:08X} vo_class={:08X} conversation={:08X} track={:08X} clip={:08X} wave={:08X} wave_name={} wave_voice={} expected={} candidate_mismatch={} accepted={} bravo={} lr={:08X}",
      s.kind,s.actor,s.role==0?"human":"partner",s.human,s.partner,s.vo_class,s.conversation,s.track,s.clip,s.wave,s.path,actual,expected,
      actual!="unknown" && actual!=expected,accepted,s.bravo,s.caller);
}

// AO4_MISSION_VO_TRACE: scripted mission dialogue only. Records which figure each event
// parameter (Sender, FIONA, ...) names, the GameState Alpha/Bravo answers the VO logic
// compares against, and the wave the original selection chose with its speaker. Read-only.
bool MissionVoTraceEnabled() {
  static const bool enabled=GetEnvironmentVariableW(L"AO4_MISSION_VO_TRACE",nullptr,0)!=0;
  return enabled;
}
bool MissionVoBudget(unsigned stream) {
  static std::atomic<uint32_t> counts[3]{};
  return counts[stream].fetch_add(1,std::memory_order_relaxed)<2048;
}
const char* MissionVoRole(uint8_t* base,uint32_t actor,uint32_t& human,uint32_t& partner) {
  human=partner=0;
  if (!FindClientSoldiers(base,human,partner) || !actor) return "other";
  return actor==human?"human":actor==partner?"partner":"other";
}
void TraceMissionParameter(PPCContext& ctx,uint8_t* base) {
  const uint32_t event=OnoWord(base,ctx.r3.u32+220);
  if (OnoWord(base,event)!=0x82A1CAB4) return;
  // Hot path (every enemy shot): reject systemic General/Friendly/Enemy names by prefix first.
  const uint32_t name_pointer=OnoWord(base,event+8),prefix=OnoWord(base,name_pointer);
  if (prefix==0x47656E65 || prefix==0x46726965 || prefix==0x456E656D) return;
  const std::string name=OnoPath(base,name_pointer);
  if (!Ao4MissionVoEvent(name)) return;
  const uint32_t hash=OnoWord(base,ctx.r4.u32+12),actor=ctx.r6.u32;
  // Constructors may insert the same parameter twice; keep one line per second.
  static std::mutex lock;
  static std::map<std::tuple<uint32_t,uint32_t,uint32_t>,ULONGLONG> recent;
  {
    std::lock_guard<std::mutex> guard(lock);
    if (recent.size()>256) recent.clear();
    const auto now=GetTickCount64();
    auto [it,inserted]=recent.try_emplace({event,hash,actor},now);
    if (!inserted && now-it->second<1000) return;
    it->second=now;
  }
  if (!MissionVoBudget(0)) return;
  uint32_t human=0,partner=0;
  const char* role=MissionVoRole(base,actor,human,partner);
  const std::string_view key=Ao4VoKeyName(hash);
  REXLOG_WARN("AO4_MISSION_VO_PARAMETER event={} key={} key_hash={:08X} actor={:08X} role={} vo_class={:08X} actor_vo={:08X} human={:08X} partner={:08X} human_vo={:08X} partner_vo={:08X} bravo={} lr={:08X}",
      name,key.empty()?"unknown":key,hash,actor,role,ctx.r5.u32,actor?SoldierVoObject(base,actor):0,
      human,partner,human?SoldierVoObject(base,human):0,partner?SoldierVoObject(base,partner):0,
      g_roles_swapped.load(),uint32_t(ctx.lr));
}
struct MissionPlay { uint32_t actor=0,wave=0,track=0,clip=0,conversation=0,caller=0; std::string path; };
MissionPlay ObserveMissionPlay(PPCContext& ctx,uint8_t* base,uint32_t wave) {
  MissionPlay sample;
  if (!MissionVoTraceEnabled() && !OnlineVoiceTrace()) return sample;
  // Same original 82E5E378 registers as the ONO observer (track r25, clip r24, conversation r21).
  const std::string path=OnoPath(base,OnoWord(base,wave+8));
  if (!Ao4MissionVoWave(path) && !(OnlineVoiceTrace() && Ao4OnoWaveVoice(path)!="unknown")) return sample;
  sample.track=ctx.r25.u32;
  const uint32_t raw=OnoWord(base,OnoWord(base,sample.track+8));
  sample.actor=raw>=4?raw-4:0;
  sample.path=path;sample.wave=wave;sample.clip=ctx.r24.u32;sample.conversation=ctx.r21.u32;
  sample.caller=uint32_t(ctx.lr);
  return sample;
}
void LogMissionPlay(uint8_t* base,const MissionPlay& s,bool accepted) {
  if (s.path.empty()) return;
  thread_local std::map<std::tuple<uint32_t,uint32_t,uint32_t>,std::pair<bool,ULONGLONG>> recent;
  if (recent.size()>128) recent.clear();
  const auto now=GetTickCount64();
  auto [it,inserted]=recent.try_emplace({s.actor,s.wave,s.clip},std::make_pair(accepted,now));
  if (!inserted && it->second.first==accepted && now-it->second.second<1000) return;
  it->second={accepted,now};
  if (OnlineVoiceTrace()) {
    static std::atomic<unsigned> lines{0};
    if (lines.load()>=192) return;
    const uint32_t manager=Load32(base,Load32(base,kClientContext)+kPlayerList);
    const uint32_t local=Load32(base,manager+184);
    const uint32_t begin=Load32(base,manager+kClientPlayersBegin),end=Load32(base,manager+kClientPlayersEnd);
    uint32_t player=0;
    for(uint32_t slot=begin;slot && slot<end && slot<begin+64;slot+=4){
      const uint32_t p=Load32(base,slot);
      if(Load32(base,p)==kClientPlayerVtable && ClientPlayerSoldier(base,p)==s.actor){player=p;break;}
    }
    if (!player || lines.fetch_add(1)>=192) return;
    REXLOG_WARN("AO4_ONLINE_VO actor={:08X} player={:08X} local={} flags={:02X} wave_voice={} accepted={} wave={} swapped={}",
      s.actor,player,player==local,
      IsReadable(base,player+1113,1)?REX_LOAD_U8(player+1113):0,
      Ao4OnoWaveVoice(s.path),accepted,s.path,g_roles_swapped.load());
    return;
  }
  if (!MissionVoBudget(1)) return;
  uint32_t human=0,partner=0;
  const char* role=MissionVoRole(base,s.actor,human,partner);
  const std::string speaker=Ao4MissionVoSpeaker(s.path);
  const bool bravo=g_roles_swapped.load();
  // In Bravo mode the human is Bravo and the partner Alpha; otherwise the reverse.
  const std::string_view expected=std::string_view(role)=="human"?(bravo?"bravo":"alpha")
      :std::string_view(role)=="partner"?(bravo?"alpha":"bravo"):"";
  const bool mismatch=!expected.empty() && (speaker=="alpha" || speaker=="bravo") && speaker!=expected;
  REXLOG_WARN("AO4_MISSION_VO_PLAY actor={:08X} role={} speaker={} expected={} speaker_mismatch={} accepted={} wave={:08X} wave_name={} conversation={:08X} track={:08X} clip={:08X} human={:08X} partner={:08X} bravo={} lr={:08X}",
      s.actor,role,speaker,expected.empty()?"-":expected,mismatch,accepted,s.wave,s.path,s.conversation,s.track,s.clip,
      human,partner,bravo,s.caller);
}
// GameState SP_AlphaPlayer/SP_BravoPlayer answers after the optional swap: one line per change.
void TraceMissionGameState(uint8_t* base,uint32_t hash,uint32_t data,uint32_t data4,bool swapped,uint32_t caller) {
  const std::string_view key=Ao4VoKeyName(hash);
  if (key!="SP_AlphaPlayer" && key!="SP_BravoPlayer") return;
  static std::atomic<uint64_t> last[2]{};
  const uint64_t value=(uint64_t(data)<<32)|data4;
  if (last[key=="SP_BravoPlayer"].exchange(value)==value || !MissionVoBudget(2)) return;
  uint32_t human=0,partner=0;
  FindClientSoldiers(base,human,partner);
  const uint32_t human_vo=human?SoldierVoObject(base,human):0,partner_vo=partner?SoldierVoObject(base,partner):0;
  auto who=[&](uint32_t v){return !v?"none":v==human||v==human_vo?"human":v==partner||v==partner_vo?"partner":"other";};
  REXLOG_WARN("AO4_MISSION_VO_GAMESTATE property={} data={:08X} data_role={} data4={:08X} data4_role={} swapped={} human={:08X} partner={:08X} human_vo={:08X} partner_vo={:08X} bravo={} lr={:08X}",
      key,data,who(data),data4,who(data4),swapped,human,partner,human_vo,partner_vo,g_roles_swapped.load(),caller);
}

}  // namespace

// The original package resolver calls 83321E20 to find the actor's voice
// component, then returns the cached/resolved package from component+20.
REX_HOOK_RAW(sub_83321E20) {
  const uint32_t actor=ctx.r3.u32;
  const uint32_t online_caller=uint32_t(ctx.lr);
  OnoPackageProbe* probe=ono_package_probe;
  const bool capture=probe && probe->actor==ctx.r3.u32;
  __imp__sub_83321E20(ctx,base);
  // The network role can arrive after initial voice selection. Reinitialize only
  // a known original Alpha-template player whose role bit changed since selection.
  // +20 caches the package chosen by 83357CC8; 83350AB0 won't update +16 while it is set.
  if (OnlineVoiceRebind() && online_caller==0x83357D48 && !online_voice_rebinding && !vo_select_soldier && ctx.r3.u32 &&
      ctx.r1.u32>=0x400 && IsReadable(base,ctx.r1.u32-0x400,0x400)) {
    std::lock_guard<std::mutex> lock(online_voice_mutex);
    auto it=online_voice_states.find(actor);
    if(it!=online_voice_states.end()) {
      auto& state=it->second;
      const uint32_t player=state.player,component=ctx.r3.u32;
      const uint32_t owner=Load32(base,Load32(base,actor+540)+264);
      if(Load32(base,actor)!=0x82AAB0D0 || Load32(base,player)!=kClientPlayerVtable ||
         ClientPlayerSoldier(base,player)!=actor || owner!=state.owner ||
         !IsReadable(base,player+1113,1) || !IsReadable(base,component+16,8)) {
        online_voice_states.erase(it);
      } else {
        const bool alpha=(REX_LOAD_U8(player+1113)&0x40)!=0;
        if(alpha!=state.alpha) {
          const uint32_t before=Load32(base,component+16),cached=Load32(base,component+20);
          if(before==0x0C426471 || before==0x0C9EE86D) {
            const PPCContext saved=ctx;
            REX_STORE_U32(component+20,0);
            online_voice_rebinding=true;
            ctx.r1.u64=saved.r1.u32-0x300;ctx.r3.u64=actor;ctx.r4.u64=player;
            __imp__sub_83323F50(ctx,base);
            ctx=saved;
            online_voice_rebinding=false;
            const uint32_t after=Load32(base,component+16);
            const bool valid=after==(alpha?0x0C426471u:0x0C9EE86Du);
            if(!valid){REX_STORE_U32(component+16,before);REX_STORE_U32(component+20,cached);}
            else state.alpha=alpha;
            REXLOG_WARN("AO4_ONLINE_VO_REBIND actor={:08X} player={:08X} alpha={} hash={:08X}->{:08X} cache={:08X} validated={}",
              actor,player,alpha,before,after,cached,valid);
          }
        }
      }
    }
  }
  if (capture) {
    probe->component=ctx.r3.u32;
    probe->hash=OnoWord(base,ctx.r3.u32+16);
    probe->cached=OnoWord(base,ctx.r3.u32+20);
  }
}
REX_HOOK_RAW(sub_83357CC8) {
  if (!OnoTraceEnabled()) return __imp__sub_83357CC8(ctx,base);
  OnoPackageProbe probe;probe.actor=ctx.r4.u32;
  uint32_t human=0,partner=0;
  const int role=OnoRole(base,probe.actor,human,partner);
  if (role<0) return __imp__sub_83357CC8(ctx,base);
  struct Scope {
    OnoPackageProbe* previous=ono_package_probe;
    Scope(OnoPackageProbe& p){ono_package_probe=&p;}
    ~Scope(){ono_package_probe=previous;}
  } scope(probe);
  const uint32_t caller=uint32_t(ctx.lr);
  __imp__sub_83357CC8(ctx,base);
  thread_local std::map<uint32_t,std::tuple<uint32_t,uint32_t,uint32_t>> seen;
  const auto value=std::make_tuple(probe.component,probe.hash,ctx.r3.u32);
  auto [it,inserted]=seen.try_emplace(probe.actor,value);
  if (!inserted && it->second==value) return;
  it->second=value;
  if (seen.size()>64) seen.clear();
  static std::atomic<uint32_t> lines{0};
  if (lines.fetch_add(1,std::memory_order_relaxed)<128)
    REXLOG_WARN("AO4_ONO_PACKAGE actor={:08X} role={} component={:08X} name_hash={:08X} cached_before={:08X} package={:08X} bravo={} lr={:08X}",
        probe.actor,role==0?"human":"partner",probe.component,probe.hash,probe.cached,ctx.r3.u32,g_roles_swapped.load(),caller);
}
REX_HOOK_RAW(sub_82E247F8) {
  const auto sample=uint32_t(ctx.lr)==0x82E5EC30?ObserveOnoPlay(ctx,base,ctx.r4.u32):OnoPlay{};
  const auto mission=uint32_t(ctx.lr)==0x82E5EC30?ObserveMissionPlay(ctx,base,ctx.r4.u32):MissionPlay{};
  __imp__sub_82E247F8(ctx,base);
  LogOnoPlay(sample,ctx.r3.u8!=0);
  LogMissionPlay(base,mission,ctx.r3.u8!=0);
}
REX_HOOK_RAW(sub_832AC558) {
  const auto sample=uint32_t(ctx.lr)==0x82E5EC40?ObserveOnoPlay(ctx,base,ctx.r5.u32):OnoPlay{};
  const auto mission=uint32_t(ctx.lr)==0x82E5EC40?ObserveMissionPlay(ctx,base,ctx.r5.u32):MissionPlay{};
  __imp__sub_832AC558(ctx,base);
  LogOnoPlay(sample,ctx.r3.u8!=0);
  LogMissionPlay(base,mission,ctx.r3.u8!=0);
}

// Original parameter insertion: (context, named value, VO class, actor).
// Capture the actor before original weak-reference construction, without replacing it.
REX_HOOK_RAW(sub_82E5D630) {
  if (OnlineVoiceTrace()) {
    static std::atomic<unsigned> lines{0};
    if(lines.load()<64){
      const uint32_t event=Load32(base,ctx.r3.u32+220);
      const std::string name=Load32(base,event)==0x82A1CAB4?GuestString(base,Load32(base,event+8)):std::string();
      const auto lower=Ao4OnoLower(name);
      if(lower.find("grenade")!=std::string::npos && lines.fetch_add(1)<64)
        REXLOG_WARN("AO4_ONLINE_VO_EVENT event={} actor={:08X} key_hash={:08X} context={:08X}",name,ctx.r6.u32,Load32(base,ctx.r4.u32+12),ctx.r3.u32);
    }
  }
  if (OnoTraceEnabled()) TraceOnoParameter(ctx,base);
  if (MissionVoTraceEnabled()) TraceMissionParameter(ctx,base);
  // Context maps are keyed by NamedValue POINTER (82E5B738/82E36CE8), not its
  // name hash. The event constructor is remapped before the original handler
  // fills parameters from the source event. Alias consumed object keys to the
  // target event's unique same-name/type key; retain the actual actor/value.
  static const bool route_fix=GetEnvironmentVariableW(L"AO4_COOP_ROUTE_FIX",nullptr,0)!=0;
  const uint32_t caller=uint32_t(ctx.lr);
  if (route_fix && g_roles_swapped && caller>=0x82F6F160 && caller<0x82F6FA84 &&
      Load32(base,ctx.r4.u32+12)==205558026u) {
    const uint32_t event=Load32(base,ctx.r3.u32+220);
    const std::string name=Load32(base,event)==0x82A1CAB4
        ? GuestString(base,Load32(base,event+8)) : std::string();
    if (name=="Os_04" || name=="OS_04_01" || name=="Os_05" || name=="OS_05_00" ||
        name=="OS_04_02" || name=="OS_04_03") {
      const uint32_t array=Load32(base,event+16),count=Load32(base,array-4);
      if (count<=16 && IsReadable(base,array,count*4) && IsReadable(base,ctx.r4.u32,16)) {
        std::vector<Ao4VoParameter> parameters;
        for (uint32_t i=0;i<count;++i) {
          const uint32_t key=Load32(base,array+4*i);
          if (IsReadable(base,key,16)) parameters.push_back({key,Load32(base,key+12),Load32(base,key+8)});
        }
        const uint32_t before=ctx.r4.u32;
        const uint32_t after=Ao4VoParameterAlias(Load32(base,before+12),Load32(base,before+8),parameters);
        if (after && after!=before) {
          ctx.r4.u64=after;
          static std::atomic<uint32_t> lines{0};
          if (lines.fetch_add(1,std::memory_order_relaxed)<64)
            REXLOG_WARN("AO4_COOP_PARAMETER event={} key={:08X}->{:08X} hash={:08X} actor={:08X}; original value",
                        name,before,after,Load32(base,before+12),ctx.r6.u32);
        }
      }
    }
  }
  if (VoIdentityTraceEnabled() && Load32(base, ctx.r4.u32+12) == 3337816366u) {
    static std::atomic<uint32_t> count{0};
    if (count.load(std::memory_order_relaxed) < 128) {
      const uint32_t event = Load32(base, ctx.r3.u32+220);
      const std::string name = Load32(base, event) == 0x82A1CAB4
          ? GuestString(base, Load32(base, event+8)) : std::string();
      if ((name == "General_Hit" || name == "Friendly_Hit" ||
           name == "Friendly_ONO_Wounded_Crawl") &&
          count.fetch_add(1, std::memory_order_relaxed) < 128) {
        uint32_t human=0, partner=0;
        const bool resolved=FindClientSoldiers(base,human,partner);
        REXLOG_WARN("AO4_VO_SENDER event={} context={:08X} actor={:08X} vo_class={:08X} "
                    "human={:08X} partner={:08X} resolved={} bravo={} lr={:08X}",
                    name, ctx.r3.u32, ctx.r6.u32, ctx.r5.u32,
                    human, partner, resolved, g_roles_swapped.load(), uint32_t(ctx.lr));
      }
    }
  }
  __imp__sub_82E5D630(ctx,base);
}

// Enqueue sees the completed context, after the constructor's event redirection.
REX_HOOK_RAW(sub_82E61968) {
  const uint32_t caller=uint32_t(ctx.lr);
  const bool lifecycle=CoopLifecycleTraceEnabled() && caller>=0x82F6F160 && caller<0x82F6FA84;
  const std::string lifecycle_name=lifecycle?CoopLifecycleName(base,Load32(base,ctx.r4.u32+220)):std::string();
  // Original 82F6F160 retains its entity in r25 and passes &entity->handle as r5
  // when it needs completion/output polling. Save these BEFORE the original call.
  const uint32_t lifecycle_entity=lifecycle_name.empty()?0:ctx.r25.u32;
  const uint32_t lifecycle_output=ctx.r5.u32;
  if (VoIdentityTraceEnabled() && caller>=0x82F6F160 && caller<0x82F6FA84) {
    static std::atomic<uint32_t> count{0};
    if (count.load(std::memory_order_relaxed) < 128) {
      const uint32_t event=Load32(base,ctx.r4.u32+220);
      const uint32_t name_pointer=Load32(base,event+8);
      const uint32_t prefix=Load32(base,name_pointer);
      const std::string name=Load32(base,event)==0x82A1CAB4 &&
          (prefix==0x4F535F30 || prefix==0x4F735F30 || prefix==0x47545F31)
          ? GuestString(base,name_pointer) : std::string();
      if (!Ao4CoopRouteEvent(name).empty() &&
          count.fetch_add(1,std::memory_order_relaxed)<128)
        REXLOG_WARN("AO4_VO_ENQUEUE event={} asset={:08X} context={:08X} bravo={} lr={:08X}",
                    name,event,ctx.r4.u32,g_roles_swapped.load(),uint32_t(ctx.lr));
    }
  }
  __imp__sub_82E61968(ctx,base);
  if (!lifecycle_name.empty()) {
    static std::atomic<uint32_t> lines{0};
    if (lines.fetch_add(1,std::memory_order_relaxed)<128)
      REXLOG_WARN("AO4_COOP_LIFECYCLE enqueue target={} entity={:08X} handle_out={:08X} handle={:08X} lr={:08X}",
          lifecycle_name,lifecycle_entity,lifecycle_output,Load32(base,lifecycle_output),caller);
  }
}

// Observe original VO entity inputs (Start/Cancel) and completion/clip messages.
// No callback or gate is manufactured, suppressed or replayed.
REX_HOOK_RAW(sub_82F70C00) {
  if (CoopLifecycleTraceEnabled()) {
    static std::atomic<uint32_t> lines{0};
    const uint32_t entity=ctx.r3.u32;
    const std::string name=CoopLifecycleName(base,Load32(base,Load32(base,entity+16)+16));
    if (!name.empty() && lines.fetch_add(1,std::memory_order_relaxed)<128)
      REXLOG_WARN("AO4_COOP_LIFECYCLE input source={} entity={:08X} input={:08X} handle={:08X} flags={:08X} lr={:08X}",
          name,entity,Load32(base,ctx.r4.u32+4),Load32(base,entity+28),Load32(base,entity+40),uint32_t(ctx.lr));
  }
  __imp__sub_82F70C00(ctx,base);
}
REX_HOOK_RAW(sub_82E55158) {
  const uint32_t caller=uint32_t(ctx.lr),entity=ctx.r31.u32,output=ctx.r5.u32,handle=ctx.r4.u32;
  __imp__sub_82E55158(ctx,base);
  // Verified caller in original 82F71030: entity r31, output {kind, hash} r5.
  // Kind 4 is explicitly ignored by that caller; do not log its polling traffic.
  if (CoopLifecycleTraceEnabled() && caller==0x82F710E8 && ctx.r3.u8 &&
      IsReadable(base,output,8) && Load32(base,output)!=4) {
    // Kind 1 / hash 0 is a repeated status, not a new completion every frame.
    // Keep transitions so it cannot consume the cap before the clip finishes.
    thread_local std::map<uint32_t,std::tuple<uint32_t,uint32_t,uint32_t>> states;
    if (states.size()>256) states.clear();
    const auto value=std::make_tuple(handle,Load32(base,output),Load32(base,output+4));
    auto [it,inserted]=states.try_emplace(entity,value);
    if (!inserted && it->second==value) return;
    it->second=value;
    static std::atomic<uint32_t> lines{0};
    const std::string name=CoopLifecycleName(base,Load32(base,Load32(base,entity+16)+16));
    if (!name.empty() && lines.fetch_add(1,std::memory_order_relaxed)<128)
      REXLOG_WARN("AO4_COOP_LIFECYCLE message source={} entity={:08X} handle={:08X} kind={} hash={:08X} lr={:08X}",
          name,entity,handle,Load32(base,output),Load32(base,output+4),caller);
  }
}

REX_HOOK_RAW(sub_82E56538) {
  const uint32_t sources = ctx.r5.u32, count = ctx.r7.u32;
  uint32_t human = 0, partner = 0;
  const bool swap = g_roles_swapped && VoSourceSwapEnabled() && count <= 64 &&
                    IsReadable(base, sources, count * 8) &&
                    FindClientSoldiers(base, human, partner);
  for (uint32_t i = 0; i < count && IsReadable(base, sources, count * 8); ++i) {
    const uint32_t entry = sources + 8 * i;
    const uint32_t soldier = REX_LOAD_U32(entry);
    uint32_t replacement = 0;
    if (swap && soldier == human) {
      replacement = partner;
    } else if (swap && soldier == partner) {
      replacement = human;
    }
    if (replacement) {
      REX_STORE_U32(entry, replacement);
      REX_STORE_U32(entry + 4, SoldierVoObject(base, replacement));
    }
    if (Enabled()) {
      REXLOG_WARN("AO4_VOSRC conversation={:08X} track={} soldier={:08X} vo={:08X}{} "
                  "human={:08X} partner={:08X}",
                  ctx.r4.u32, i, soldier, Load32(base, entry + 4),
                  replacement ? fmt::format(" -> {:08X}", replacement) : std::string(),
                  human, partner);
    }
  }
  __imp__sub_82E56538(ctx, base);
}

// VO global "GameState" (sub_8335AE00): properties GameState, SP_AlphaPlayer, SP_BravoPlayer,
// RestartFromCheckPoint, Scavenging (hashes at 0x841BE800, built by sub_83351300).
// SP_AlphaPlayer is the local player's soldier unless the local client player says it is
// Bravo (vtable[8]), otherwise its partner; SP_BravoPlayer the other way round. The VO logic
// uses them to decide who says which mission line and whose lines go over the radio.
//   AO4_VO_GAMESTATE_SWAP: with the roles swapped, answer SP_AlphaPlayer with the
//                          SP_BravoPlayer result and vice versa (human = Bravo, AI = Alpha).
namespace {
constexpr uint32_t kVoGameStateHashes = 0x841BE800;
constexpr const char* kVoGameStateNames[] = {"GameState", "SP_AlphaPlayer", "SP_BravoPlayer",
                                             "RestartFromCheckPoint", "Scavenging"};

bool VoGameStateSwapEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_VO_GAMESTATE_SWAP", nullptr, 0) != 0;
  return enabled;
}

uint32_t FindRouteEvent(uint8_t* base, uint32_t source, std::string_view target) {
  // Read event objects near the source, then follow each candidate's actual name
  // pointer. Short mixed-case names may be interned outside both source regions.
  const auto snapshots=Ao4VoMemorySnapshots(reinterpret_cast<uintptr_t>(base),source);
  std::set<uint32_t> candidates;
  if (target.empty() || target.size()>=64) return 0;
  auto read_guest = [&](uint32_t address,void* data,size_t length) {
    if (address<0x40000000 || uint64_t(address)+length>0xE0000000ull) return false;
    SIZE_T read=0;
    return ReadProcessMemory(GetCurrentProcess(),base+address,data,length,&read) && read==length;
  };
  auto matches = [&](uint32_t event,uint32_t name) {
    return Ao4VoEventHasName(event,name,target,read_guest);
  };
  size_t scanned=0;
  for (const auto& objects:snapshots) {
    scanned+=objects.bytes.size();
    const auto found=Ao4VoNamedEventsInSnapshot(objects.bytes.data(),objects.bytes.size(),objects.start,matches);
    candidates.insert(found.begin(),found.end());
  }
  if (candidates.size() != 1) {
    REXLOG_WARN("AO4_COOP_ROUTE unresolved target={} source={:08X} matches={} regions={} bytes={}; original retained",
                target, source, candidates.size(),snapshots.size(),scanned);
    return 0;
  }
  return *candidates.begin();
}
}  // namespace

// All mission VO events build their parameter context here (the event asset is r4).
// The existing GameState/source hooks below/above are intentionally mission-independent.
// Record actual event selection during the combined checkpoint/terrain test without
// changing server role queries or guessing that Alpha/Bravo-named events are inverses.
REX_HOOK_RAW(sub_82E581C8) {
  static const bool trace =
      GetEnvironmentVariableW(L"AO4_COOP_DIALOGUE_TRACE", nullptr, 0) != 0;
  static const bool route_fix =
      GetEnvironmentVariableW(L"AO4_COOP_ROUTE_FIX", nullptr, 0) != 0;
  const bool mission = uint32_t(ctx.lr) == 0x82F6F1C4;
  if (mission && route_fix && g_roles_swapped && VoGameStateSwapEnabled()) {
    const uint32_t source = ctx.r4.u32;
    if (Load32(base, source) == 0x82A1CAB4) {
      const std::string name = GuestString(base, Load32(base, source+8));
      const std::string_view target = Ao4CoopRouteEvent(name);
      if (!target.empty()) {
        static std::mutex lock;
        static std::map<std::pair<uint32_t, uint32_t>, uint32_t> cache;
        std::lock_guard<std::mutex> guard(lock);
        // A checkpoint can recycle an event object's address. Its loaded name string
        // is part of the cache identity, so it cannot inherit an old generation's target.
        uint32_t& replacement = cache[{source, Load32(base, source+8)}];
        if (Load32(base, replacement) != 0x82A1CAB4 ||
            GuestString(base, Load32(base, replacement+8)) != target)
          replacement = FindRouteEvent(base, source, target);
        if (replacement) {
          ctx.r4.u64 = replacement;
          REXLOG_WARN("AO4_COOP_ROUTE {} -> {} source={:08X} target={:08X}; original conversation",
                      name, target, source, replacement);
        }
      }
    }
  }
  // Contexts are also constructed for every enemy shot, twice per event. Do not perform
  // per-character VirtualQuery/string reads on those hot paths or after the trace limit.
  static std::atomic<uint32_t> lines{0};
  if (trace && mission &&
      lines.load(std::memory_order_relaxed) < 4096) {
    const uint32_t event = ctx.r4.u32;
    if (IsReadable(base, event, 24) && Load32(base, event) == 0x82A1CAB4) {
      const std::string name = GuestString(base, Load32(base, event + 8));
      const uint32_t owner = Load32(base, event + 20);
      if (!name.empty() && lines.fetch_add(1, std::memory_order_relaxed) < 4096) {
        REXLOG_WARN("AO4_COOP_DIALOGUE event={} asset={:08X} owner={:08X} bravo={} "
                    "gamestate_swap={} source_swap={} lr={:08X}",
                    name, event, owner, g_roles_swapped.load(), VoGameStateSwapEnabled(),
                    VoSourceSwapEnabled(), uint32_t(ctx.lr));
      }
    }
  }
  __imp__sub_82E581C8(ctx, base);
}

REX_HOOK_RAW(sub_8335AE00) {
  const uint32_t property = ctx.r5.u32, value = ctx.r7.u32, caller = uint32_t(ctx.lr);
  const uint32_t hash = Load32(base, property + 12);
  const uint32_t alpha = Load32(base, kVoGameStateHashes + 4);
  const uint32_t bravo = Load32(base, kVoGameStateHashes + 8);
  const bool swap = g_roles_swapped && VoGameStateSwapEnabled() && hash &&
                    (hash == alpha || hash == bravo);
  const uint32_t r1 = ctx.r1.u32;
  if (swap) {
    // Ask with a copy of the property that carries the other name hash. The copy lives in a
    // frame reserved below the caller's stack pointer, above everything the original uses.
    ctx.r1.u64 = r1 - 0x100;
    const uint32_t copy = (r1 - 0x40) & ~0xFu;
    for (uint32_t i = 0; i < 16; i += 4) {
      REX_STORE_U32(copy + i, Load32(base, property + i));
    }
    REX_STORE_U32(copy + 12, hash == alpha ? bravo : alpha);
    ctx.r5.u64 = copy;
  }
  __imp__sub_8335AE00(ctx, base);
  ctx.r1.u64 = r1;
  if (MissionVoTraceEnabled() && hash && (hash == alpha || hash == bravo))
    TraceMissionGameState(base, hash, Load32(base, value), Load32(base, value + 4), swap, caller);
  if (Enabled()) {
    std::string name = fmt::format("hash_{:08X}", hash);
    for (uint32_t i = 0; i < std::size(kVoGameStateNames); ++i) {
      if (hash && Load32(base, kVoGameStateHashes + 4 * i) == hash) {
        name = kVoGameStateNames[i];
      }
    }
    // Object results: data at +0 is the VO object; log the soldier through the value too.
    const uint32_t type = Load32(base, value + 40);
    const uint32_t data = Load32(base, value);
    const uint32_t data4 = Load32(base, value + 4);
    static std::mutex lock;
    static std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard<std::mutex> guard(lock);
    auto [it, inserted] = seen.try_emplace({hash, caller}, std::make_pair(data, data4));
    if ((inserted || it->second != std::make_pair(data, data4)) && seen.size() < 1024) {
      it->second = {data, data4};
      REXLOG_WARN("AO4_VOGAME {} handled={} type={} data={:08X} data4={:08X}{} lr={:08X}", name,
                  ctx.r3.u32, type, data, data4, swap ? " (Alpha/Bravo swapped)" : "", caller);
    }
  }
}

// Original routine that creates the ServerLoad thread (ServerLoad string 82A4F668).
// Record the caller chain at actual load boundaries; do not hide UI or skip the load.
REX_HOOK_RAW(sub_830E4F38) {
  static const bool trace =
      GetEnvironmentVariableW(L"AO4_LEVEL_LOAD_TRACE", nullptr, 0) != 0;
  static std::atomic<uint32_t> count{0};
  if (trace && count.fetch_add(1, std::memory_order_relaxed) < 32) {
    std::string frames, fields;
    uint32_t stack = ctx.r1.u32;
    for (unsigned i = 0; i < 10; ++i) {
      const uint32_t previous = Load32(base, stack);
      if (previous <= stack || previous-stack > 0x100000 || !IsReadable(base, previous-8, 8)) break;
      frames += fmt::format(" {:08X}", Load32(base, previous-8));
      stack = previous;
    }
    for (unsigned i = 0; i < 64; i += 4)
      fields += fmt::format(" +{:X}={:08X}", i, Load32(base, ctx.r3.u32+i));
    REXLOG_WARN("AO4_LEVEL_LOAD object={:08X} lr={:08X} bravo={} caller_frames:{} fields:{}",
                ctx.r3.u32, uint32_t(ctx.lr), g_roles_swapped.load(), frames, fields);
  }
  __imp__sub_830E4F38(ctx, base);
}

// Fatal errors (e.g. "Call to invalid or unregistered function") end in std::abort(). Log
// the host call stack first: recompiled guest functions appear as sub_XXXXXXXX frames
// (symbols from ao4_main_native.pdb), which shows which guest code made the bad call.
#include <dbghelp.h>
#include <csignal>
#pragma comment(lib, "dbghelp.lib")

namespace {
void LogAbortStack(int) {
  void* frames[64] = {};
  const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  for (USHORT i = 0; i < count; ++i) {
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
    char module[MAX_PATH] = "?";
    HMODULE handle = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(frames[i]), &handle)) {
      GetModuleFileNameA(handle, module, MAX_PATH);
    }
    const char* module_name = strrchr(module, '\\') ? strrchr(module, '\\') + 1 : module;
    if (SymFromAddr(process, address, &displacement, symbol)) {
      REXLOG_CRITICAL("AO4_ABORT_STACK #{} {}!{}+0x{:X}", i, module_name, symbol->Name,
                      displacement);
    } else {
      REXLOG_CRITICAL("AO4_ABORT_STACK #{} {}!0x{:X}", i, module_name, address);
    }
  }
  if (auto logger = ::rex::GetLogger()) {
    logger->flush();
  }
}

const bool g_abort_handler = [] {
  std::signal(SIGABRT, LogAbortStack);
  return true;
}();
}  // namespace

// AO4_SKIN_AI_ALPHA (experiment, do not use): AlphaSkin is only the arm template texture, and
// sub_832F13C8 does not run in levels, so this never fires there. The AI partner has no
// customization objects, so its body keeps the skin of its Bravo outfit. With the roles swapped, set AlphaSkin on its body mesh the way
// sub_832EC0D8 does for players (texture handle, mesh component slot 2), once per partner
// soldier, as soon as the mesh is loaded. Driven from the skin objects' update
// (sub_832F13C8), which runs on the client thread.
extern "C" REX_FUNC(sub_83915F70);  // texture handle init
extern "C" REX_FUNC(sub_82DC5F78);  // texture handle from asset (handle, manager, asset, type, 0)
extern "C" REX_FUNC(sub_82DBF3A8);  // texture handle release
extern "C" REX_FUNC(sub_82FF0F40);  // soldier -> body mesh component
extern "C" REX_FUNC(sub_82F772C8);  // mesh component set texture (component, slot, texture)
extern "C" REX_FUNC(sub_832EB590);  // AO4CustomizationAsset

namespace {
constexpr uint32_t kAlphaSkin = 0x54;  // AO4CustomizationAsset::AlphaSkin
constexpr uint32_t kBravoSkin = 0x58;  // AO4CustomizationAsset::BravoSkin
constexpr uint32_t kTextureType = 0x84286994;

bool SkinAiAlphaEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_SKIN_AI_ALPHA", nullptr, 0) != 0;
  return enabled;
}

namespace {
// Like sub_82FF0F40 (first component of the body mesh type in the soldier's component
// table: *(soldier+28), entries of 32 bytes from +16, count at +5, type id at +12, component
// at +4), but returns the first one whose mesh (+16) has at least `min_slots` texture slots
// (+5). The AI partner's first match is its spawn mesh without slots; the outfit mesh comes
// later in the table.
uint32_t FindMeshComponent(uint8_t* base, uint32_t soldier, uint32_t min_slots, std::string* log) {
  constexpr uint32_t kMeshComponentType = 0x8428B800;
  const uint32_t table = Load32(base, soldier + 28);
  const uint32_t count = Load8(base, table + 5);
  const uint32_t low = IsReadable(base, kMeshComponentType + 20, 4)
                           ? REX_LOAD_U16(kMeshComponentType + 20) : 0;
  const uint32_t high = IsReadable(base, kMeshComponentType + 20, 4)
                            ? REX_LOAD_U16(kMeshComponentType + 22) : 0;
  if (!(Load32(base, soldier + 8) & 0x00400000) || count == 0xFF || !high) {
    return 0;
  }
  uint32_t found = 0;
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    const uint32_t entry = table + 16 + i * 32;
    if (!IsReadable(base, entry, 16)) {
      break;
    }
    const uint32_t type = REX_LOAD_U16(entry + 12);
    const uint32_t component = REX_LOAD_U32(entry + 4);
    if (!component || uint16_t(type - low) > uint16_t(high - low)) {
      continue;
    }
    const uint32_t mesh = Load32(base, component + 16);
    const uint32_t slots = mesh ? Load8(base, mesh + 5) : 0;
    if (log) {
      *log += fmt::format(" [{}] component={:08X} mesh={:08X} slots={}", i, component, mesh, slots);
    }
    if (!found && mesh && slots != 0xFF && slots >= min_slots) {
      found = component;
    }
  }
  return found;
}
}  // namespace

// Returns true once the skin is set.
bool SetSoldierSkin(PPCContext& ctx, uint8_t* base, uint32_t soldier, uint32_t skin_field) {
  auto saved = std::make_unique<PPCContext>(ctx);
  const uint32_t r1 = ctx.r1.u32 - 0x200;
  const uint32_t handle = r1 + 0x180;  // inside the reserved frame
  bool done = false;
  std::string components;
  const uint32_t component = FindMeshComponent(base, soldier, 3, &components);
  const uint32_t mesh = Load32(base, component + 16);
  ctx.r1.u64 = r1;
  sub_832EB590(ctx, base);
  const uint32_t asset = ctx.r3.u32;
  const uint32_t skin = Load32(base, asset + skin_field);
  const uint32_t manager = Load32(base, Load32(base, Load32(base, kClientContext) + 16) + 56);
  const bool ready = component && mesh && Load8(base, mesh + 5) >= 3 &&
                     Load8(base, mesh + 5) != 0xFF && skin && manager;
  if (!ready) {
    static ULONGLONG last = 0;
    if (GetTickCount64() - last > 5000) {
      last = GetTickCount64();
      REXLOG_WARN("AO4_SKIN not ready: soldier={:08X} flags={:08X} component={:08X} mesh={:08X} "
                  "state={} asset={:08X} skin={:08X} manager={:08X} meshes:{}",
                  soldier, Load32(base, soldier + 8), component, mesh,
                  mesh ? Load8(base, mesh + 5) : 0xFF, asset, skin, manager, components);
    }
  }
  if (ready) {
    ctx.r1.u64 = r1;
    ctx.r3.u64 = handle;
    sub_83915F70(ctx, base);
    ctx.r1.u64 = r1;
    ctx.r3.u64 = handle;
    ctx.r4.u64 = manager;
    ctx.r5.u64 = skin;
    ctx.r6.u64 = kTextureType;
    ctx.r7.u64 = 0;
    sub_82DC5F78(ctx, base);
    const uint32_t texture = Load32(base, handle);
    if (texture) {
      ctx.r1.u64 = r1;
      ctx.r3.u64 = component;
      ctx.r4.u64 = 2;
      ctx.r5.u64 = texture;
      sub_82F772C8(ctx, base);
      done = true;
    }
    ctx.r1.u64 = r1;
    ctx.r3.u64 = handle;
    ctx.r4.u64 = 0;
    sub_82DBF3A8(ctx, base);
    if (Enabled()) {
      REXLOG_WARN("AO4_SKIN soldier={:08X} component={:08X} skin={:08X}({}) texture={:08X} {}",
                  soldier, component, skin, GuestString(base, Load32(base, skin + 8)), texture,
                  done ? "set" : "failed");
    }
  }
  ctx = *saved;
  return done;
}
}  // namespace

// The outfit-object update is a preview path and is NOT a mission heartbeat.
// T20 incorrectly used it for the equipment check; keep only the old experiment.
void Ao4UpdateEquipment(PPCContext& ctx,uint8_t* base,bool client_update);
REX_HOOK_RAW(sub_832F13C8) {
  __imp__sub_832F13C8(ctx, base);
  static uint32_t calls = 0;
  if (Enabled() && (++calls == 1 || calls % 2000 == 0)) {
    REXLOG_WARN("AO4_SKIN update calls={}", calls);
  }
  if (!SkinAiAlphaEnabled() || !g_roles_swapped) {
    return;
  }
  static uint32_t done_for = 0;
  uint32_t human = 0, partner = 0;
  if (FindClientSoldiers(base, human, partner) && partner != done_for &&
      SetSoldierSkin(ctx, base, partner, kAlphaSkin)) {
    done_for = partner;
  }
}

// AO4_AI_ALPHA_OUTFIT: give the AI partner (Alpha under the role swap) Alpha's outfit body.
// In single player only player slot 0's customization bundles are loaded, so Alpha's outfit
// mesh does not exist. Step 1: whenever the game applies slot 0's customization set
// (sub_832E8958(manager, 0)), also apply slot 1's set (the split-screen player 2 objects),
// with Bravo's default outfit and tattoo bundle ids temporarily replaced by Alpha's, so the
// game queues Alpha's outfit bundle itself. Step 2: on the client thread, once that bundle is
// loaded and the AI partner's soldier exists, apply the outfit to it with the function the
// menu preview uses (sub_83006B38(customization component, outfit info)), which returns
// without changes while the bundle is not loaded.
extern "C" REX_FUNC(sub_832F1FE0);  // outfit info for a bundle id
extern "C" REX_FUNC(sub_83061C00);  // soldier -> customization component
extern "C" REX_FUNC(sub_83006B38);  // apply outfit (component, info)
extern "C" REX_FUNC(sub_82EEB0A8);  // loaded bundle by name (bundle manager, name) or 0

namespace {
constexpr uint32_t kDefaultAlphaOutfit = 0x4C, kDefaultBravoOutfit = 0x50;
constexpr uint32_t kDefaultAlphaTattoo = 0x60, kDefaultBravoTattoo = 0x64;
constexpr uint32_t kCustomizationManager = 0x841BC770;  // *(...) -> manager, objects at +4..+32
constexpr uint32_t kOutfitObjectVtable = 0x82AA9A90;
#include "partner_catalog_ids.h"
struct PartnerEquipment {
  bool active = false;
  uint32_t mask = 0, outfit = 0, tattoo = 0;
};
PartnerEquipment g_partner_equipment;
template <size_t N>
bool ReadEquipmentId(const wchar_t* path, const wchar_t* key, const uint32_t (&ids)[N],
                     uint32_t& value) {
  wchar_t text[32] = {};
  GetPrivateProfileStringW(L"equipment", key, L"0", text, 32, path);
  wchar_t* end = nullptr;
  const unsigned long parsed = wcstoul(text, &end, 10);
  if (text[0] < L'0' || text[0] > L'9' || !end || *end) return false;
  value = uint32_t(parsed);
  if (!value) return true;
  for (auto id : ids) if (id == value) return true;
  return false;
}
bool ReloadPartnerEquipment() {
  wchar_t path[32768] = {};
  if (!GetEnvironmentVariableW(L"AO4_PARTNER_CONFIG", path, 32768) ||
      GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
    REXLOG_WARN("AO4_PARTNER_EQUIPMENT config missing; save in Partner-Customizer first");
    return false;
  }
  PartnerEquipment next;
  if (!ReadEquipmentId(path, L"mask", kPartnerMaskIds, next.mask) ||
      !ReadEquipmentId(path, L"outfit", kPartnerOutfitIds, next.outfit) ||
      !ReadEquipmentId(path, L"tattoo", kPartnerTattooIds, next.tattoo)) {
    REXLOG_WARN("AO4_PARTNER_EQUIPMENT rejected unknown asset id");
    return false;
  }
  next.active = true;
  g_partner_equipment = next;
  return true;
}

// Hotkeys (host thread, also in the main menu):
// - Partner gear (F6 by default): flips g_f6_wanted; the client-thread hook below loads or
//   drops the equipment when it differs from the state.
// - Bravo mode (F9 by default): in a level the request goes to PollSwapKeys (server thread,
//   needs the human and the AI partner), otherwise the mode is switched directly and takes
//   effect with the next spawn (look swap in soldier_observer.cpp, roles, voice).
// Both states are kept in partner-active.ini next to AO4_PARTNER_CONFIG ([state] active,
// bravo) until changed again; a short beep confirms (high = on, low = off). The keys come from
// "hotkeys" in input-bindings.json (AO4_INPUT_BINDINGS), re-read when the file changes.
std::wstring PartnerStatePath() {
  wchar_t path[32768] = {};
  if (!GetEnvironmentVariableW(L"AO4_PARTNER_CONFIG", path, 32768)) return {};
  std::wstring text = path;
  const size_t slash = text.find_last_of(L"\\/");
  return (slash == std::wstring::npos ? std::wstring() : text.substr(0, slash + 1)) +
         L"partner-active.ini";
}
std::atomic<bool> g_f6_wanted{false};
// Set by the cutscene mask handler (sub_832F3F28 hook): masks are put on now.
std::atomic<bool> g_mask_cutscene{false};
// Last time (GetTickCount64) the human and the AI partner both had a client soldier.
std::atomic<uint64_t> g_partner_seen{0};
std::atomic<uint32_t> g_soldier_epoch{0};
// Guest memory base for host threads (set by the hooks below).
std::atomic<uint8_t*> g_guest_base{nullptr};
bool InLevel() { return GetTickCount64() - g_partner_seen.load() < 3000; }

void SaveHotkeyState() {
  const std::wstring path = PartnerStatePath();
  if (path.empty()) return;
  WritePrivateProfileStringW(L"state", L"active", g_f6_wanted ? L"1" : L"0", path.c_str());
  WritePrivateProfileStringW(L"state", L"bravo", ao4_bravo_wanted ? L"1" : L"0", path.c_str());
}

struct Hotkey {
  int vk;
  std::string name;
};
std::mutex g_hotkey_lock;
Hotkey g_key_bravo{VK_F9, "F9"}, g_key_gear{VK_F6, "F6"};

// Key name as in input-bindings.json ("F9", "G", "7", "Insert", "Numpad5", ...) -> virtual key.
int VirtualKeyFromName(std::string name) {
  for (auto& c : name) c = char(toupper(uint8_t(c)));
  if (name.size() == 1 &&
      ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) {
    return name[0];
  }
  if (name.size() >= 2 && name[0] == 'F' && isdigit(uint8_t(name[1]))) {
    const int n = atoi(name.c_str() + 1);
    if (n >= 1 && n <= 24) return VK_F1 + n - 1;
  }
  if (name.rfind("NUMPAD", 0) == 0 && name.size() == 7 && isdigit(uint8_t(name[6]))) {
    return VK_NUMPAD0 + (name[6] - '0');
  }
  static const std::pair<const char*, int> names[] = {
      {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},       {"HOME", VK_HOME},
      {"END", VK_END},       {"PAGEUP", VK_PRIOR},        {"PAGEDOWN", VK_NEXT},
      {"PAUSE", VK_PAUSE},   {"SCROLLLOCK", VK_SCROLL},   {"BACKSPACE", VK_BACK},
      {"TAB", VK_TAB},       {"SPACE", VK_SPACE},         {"RETURN", VK_RETURN},
      {"ENTER", VK_RETURN}};
  for (auto [text, vk] : names) {
    if (name == text) return vk;
  }
  return 0;
}

std::wstring InputBindingsPath() {
  wchar_t path[32768] = {};
  return GetEnvironmentVariableW(L"AO4_INPUT_BINDINGS", path, 32768) ? std::wstring(path)
                                                                       : std::wstring();
}

uint64_t FileWriteTime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// "key": "value" inside the "hotkeys" object; empty if missing.
std::string HotkeyValue(const std::string& json, const std::string& key) {
  const size_t section = json.find("\"hotkeys\"");
  if (section == std::string::npos) return {};
  const size_t open = json.find('{', section), close = json.find('}', section);
  if (open == std::string::npos || close == std::string::npos || close < open) return {};
  const std::string body = json.substr(open, close - open);
  size_t at = body.find("\"" + key + "\"");
  if (at == std::string::npos) return {};
  at = body.find(':', at);
  const size_t first = at == std::string::npos ? at : body.find('"', at);
  const size_t last = first == std::string::npos ? first : body.find('"', first + 1);
  return last == std::string::npos ? std::string() : body.substr(first + 1, last - first - 1);
}

void ReloadHotkeys(const std::wstring& path) {
  std::string json;
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof(buffer), file)) > 0;) json.append(buffer, n);
    fclose(file);
  }
  std::lock_guard<std::mutex> guard(g_hotkey_lock);
  for (auto [key, hotkey, fallback] : {std::tuple{"bravo_mode", &g_key_bravo, "F9"},
                                       std::tuple{"partner_gear", &g_key_gear, "F6"}}) {
    std::string name = HotkeyValue(json, key);
    if (name.empty()) name = fallback;
    const int vk = VirtualKeyFromName(name);
    if (!vk) {
      REXLOG_WARN("AO4_HOTKEYS unknown key '{}' for {}; keeping {}", name, key, hotkey->name);
      continue;
    }
    hotkey->vk = vk;
    hotkey->name = name;
  }
  REXLOG_WARN("AO4_HOTKEYS bravo_mode={} partner_gear={}", g_key_bravo.name, g_key_gear.name);
}

struct HotkeyPoller {
  HotkeyPoller() {
    if (OnlineRoleIsolation() || GetEnvironmentVariableW(L"AO4_LOCAL_COOP", nullptr, 0)) {
      // Online and split screen: two human players keep their original roles (Alpha = player 1).
      // Bravo mode and partner gear stay off; the saved single-player state is not changed.
      ao4_bravo_wanted=false; g_roles_swapped=false; g_f6_wanted=false;
      REXLOG_WARN("AO4_COOP role/partner hotkeys disabled; original roles retained ({})",
                  OnlineRoleIsolation() ? "online" : "split screen");
      return;
    }
    const std::wstring path = PartnerStatePath();
    bool bravo = GetEnvironmentVariableW(L"AO4_PLAYER1_PAI", nullptr, 0) != 0;
    if (!path.empty()) {
      g_f6_wanted = GetPrivateProfileIntW(L"state", L"active", 0, path.c_str()) != 0;
      bravo = GetPrivateProfileIntW(L"state", L"bravo", bravo ? 1 : 0, path.c_str()) != 0;
    }
    ao4_bravo_wanted = bravo;
    g_roles_swapped = bravo;
    std::thread([] {
      const std::wstring bindings = InputBindingsPath();
      uint64_t bindings_time = UINT64_MAX;
      bool bravo_down = false, gear_down = false;
      int bravo_vk = 0, gear_vk = 0;
      for (uint32_t tick = 0;; ++tick) {
        Sleep(30);
        if (tick % 33 == 0 && FileWriteTime(bindings) != bindings_time) {
          bindings_time = FileWriteTime(bindings);
          ReloadHotkeys(bindings);
        }
        {
          // A changed key counts as held until released, so it does not fire at once.
          std::lock_guard<std::mutex> guard(g_hotkey_lock);
          if (g_key_bravo.vk != bravo_vk) bravo_vk = g_key_bravo.vk, bravo_down = true;
          if (g_key_gear.vk != gear_vk) gear_vk = g_key_gear.vk, gear_down = true;
        }
        if (KeyPressed(gear_vk, gear_down)) {
          g_f6_wanted = !g_f6_wanted;
          SaveHotkeyState();
          REXLOG_WARN("AO4_PARTNER_EQUIPMENT key pressed -> {} (saved)",
                      g_f6_wanted ? "on" : "off");
          Beep(g_f6_wanted ? 880 : 440, 120);
        }
        // Bravo mode: only the wish changes here. In a running level it takes effect with the
        // next load (checkpoint, restart, next level), when the soldiers spawn again.
        if (KeyPressed(bravo_vk, bravo_down)) {
          ao4_bravo_wanted = !ao4_bravo_wanted;
          SaveHotkeyState();
          REXLOG_WARN("AO4_CONTROL_SWAP bravo key pressed -> {} (saved, {})",
                      ao4_bravo_wanted ? "on" : "off",
                      InLevel() ? "from the next checkpoint or level load" : "now");
          Beep(ao4_bravo_wanted ? 880 : 440, 120);
        }
        // Two human players seen (co-op without the split-screen switch): Bravo mode off, so the
        // role lookup never trades the two humans. Only the running state, not the saved wish.
        if (g_human_coop.load() && g_roles_swapped.load()) {
          g_roles_swapped = false;
          REXLOG_WARN("AO4_CONTROL_SWAP Bravo mode off: two human players");
        }
        if (g_human_coop.load()) continue;
        // The wish takes effect only while the human and the AI partner have no soldier (main
        // menu, loading, checkpoint reload), before the new soldiers spawn.
        if (g_roles_swapped != ao4_bravo_wanted) {
          uint32_t human = 0, partner = 0;
          uint8_t* base = g_guest_base.load();
          if (!base || !FindClientSoldiers(base, human, partner)) {
            g_roles_swapped = ao4_bravo_wanted.load();
            REXLOG_WARN("AO4_CONTROL_SWAP Bravo mode {} (no soldiers)", g_roles_swapped ? "on" : "off");
          }
        }
      }
    }).detach();
  }
} g_hotkey_poller;

// Menu screens (sub_830ABB10 pushes screen r4 onto manager+12..+16; the menu mostly replaces
// the top screen: FE02MainMenu -> FE09CampaignMenu (Solo/Koop choice) -> FE_StartGame ...).
// The overlay shows from FE_StartGame (Solo pressed) on, also on the screens opened from there,
// until the main or campaign menu, the title or the co-op menu comes back.
std::atomic<bool> g_solo_menu{false};
std::atomic<uint32_t> g_ui_manager{0};
// Screen data name, as sub_830ABB10 puts it into its metrics message: *(*(screen + 4) + 8).
std::string ScreenName(uint8_t* base, uint32_t screen) {
  return GuestString(base, Load32(base, Load32(base, screen + 4) + 8));
}
// In-level screen manager: the one that pushed a HUD screen (UI/Flow/Screen/HD...). Pause,
// settings, popups and result screens are pushed on top of the HUD in the same manager.
std::atomic<uint32_t> g_hud_manager{0};
bool IsHudScreen(const std::string& name) { return name.rfind("UI/Flow/Screen/HD", 0) == 0; }
bool SoloMenuOpen(uint8_t*) { return g_solo_menu; }
void TrackSoloMenu(const std::string& name) {
  if (name.find("FE_StartGame") != std::string::npos) g_solo_menu = true;
  for (const char* end : {"FE02MainMenu", "FE09CampaignMenu", "FE01TitleScreen", "FE_Coop"}) {
    if (name.find(end) != std::string::npos) g_solo_menu = false;
  }
}
}  // namespace

REX_HOOK_RAW(sub_830ABB10) {
  const uint32_t manager = ctx.r3.u32, screen = ctx.r4.u32;
  g_guest_base = base;
  g_ui_manager = manager;
  const std::string name = ScreenName(base, screen);
  __imp__sub_830ABB10(ctx, base);
  TrackSoloMenu(name);
  // A front-end screen ends the level only when it comes in another manager (main menu). The
  // pause menu's settings reuse FE screens inside the level's HUD manager (FE05Settings_Brightness):
  // forgetting the manager there switched the direct mouse off until the next level load.
  if (IsHudScreen(name)) g_hud_manager = manager;
  else if ((name.rfind("UI/Flow/Screen/FE", 0) == 0 || name.find("/Frontend/") != std::string::npos) &&
           manager != g_hud_manager.load()) g_hud_manager = 0;
  const uint32_t begin = Load32(base, manager + 12), end = Load32(base, manager + 16);
  REXLOG_WARN("AO4_UI_SCREEN push '{}' manager={:08X} depth={} solo={}", name, manager,
              end >= begin ? (end - begin) / 4 : 0, SoloMenuOpen(base));
}

// Direct mouse look gate: only while a level HUD screen is the top screen. The top screen
// pointer is cached so the name is read only when the stack changes (or every 500 ms).
bool Ao4LevelHudOnTop(uint8_t* base) {
  const uint32_t manager = g_hud_manager.load();
  uint32_t top = 0;
  if (manager) {
    const uint32_t begin = Load32(base, manager + 12), end = Load32(base, manager + 16);
    if (begin && end > begin && end - begin <= 64 * 4) top = Load32(base, end - 4);
  }
  thread_local uint32_t cached_top = 0;
  thread_local bool cached_on = false;
  static std::atomic<bool> logged_on{false};
  thread_local ULONGLONG cached_at = 0;
  const ULONGLONG now = GetTickCount64();
  if (top == cached_top && now - cached_at < 500) return cached_on;
  const std::string name = top ? ScreenName(base, top) : std::string();
  cached_top = top;
  cached_at = now;
  cached_on = IsHudScreen(name);
  if (logged_on.exchange(cached_on) != cached_on) {
    REXLOG_WARN("AO4_NATIVE_MOUSE gate={} top='{}'", cached_on ? "on" : "off (menu/pause)", name);
  }
  return cached_on;
}

// Status for the launcher's overlay (top left in the campaign/solo menus).
extern "C" __declspec(dllexport) void AO4_GetOverlayState(Ao4OverlayState* out) {
  if (!out) return;
  uint8_t* base = g_guest_base.load();
  out->show = ao4_frontend && !InLevel() && base && SoloMenuOpen(base);
  out->bravo_mode = ao4_bravo_wanted;
  out->partner_gear = g_f6_wanted;
  std::lock_guard<std::mutex> guard(g_hotkey_lock);
  snprintf(out->bravo_key, sizeof(out->bravo_key), "%s", g_key_bravo.name.c_str());
  snprintf(out->gear_key, sizeof(out->gear_key), "%s", g_key_gear.name.c_str());
}

// Port menu (F4) switches: same effect as the Bravo-mode / partner-gear keys (wish saved in
// partner-active.ini; Bravo mode applies with the next spawn). 1 = Bravo mode, 2 = partner gear.
// Not in online co-op (original network roles). Returns the new state, -1 if not available.
extern "C" __declspec(dllexport) int AO4_ToggleFeature(int which) {
  if (OnlineRoleIsolation() || LocalCoopActive()) return -1;
  if (which == 1) {
    ao4_bravo_wanted = !ao4_bravo_wanted;
    SaveHotkeyState();
    REXLOG_WARN("AO4_CONTROL_SWAP bravo menu switch -> {} (saved)", ao4_bravo_wanted ? "on" : "off");
    return ao4_bravo_wanted ? 1 : 0;
  }
  if (which == 2) {
    g_f6_wanted = !g_f6_wanted;
    SaveHotkeyState();
    REXLOG_WARN("AO4_PARTNER_EQUIPMENT menu switch -> {} (saved)", g_f6_wanted ? "on" : "off");
    return g_f6_wanted ? 1 : 0;
  }
  return -1;
}

namespace {

// Last write time of the equipment file, to pick up a new choice while F6 is on.
uint64_t EquipmentFileTime() {
  wchar_t path[32768] = {};
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetEnvironmentVariableW(L"AO4_PARTNER_CONFIG", path, 32768) ||
      !GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

bool AiAlphaOutfitEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_AI_ALPHA_OUTFIT", nullptr, 0) != 0;
  return enabled || Ao4Qol().gear_reload_fix;
}

uint32_t CustomizationAssetGuest(PPCContext& ctx, uint8_t* base) {
  auto saved = std::make_unique<PPCContext>(ctx);
  ctx.r1.u64 = ctx.r1.u32 - 0x200;
  sub_832EB590(ctx, base);
  const uint32_t asset = ctx.r3.u32;
  ctx = *saved;
  return asset;
}

thread_local bool t_in_ai_outfit = false;
}  // namespace

REX_HOOK_RAW(sub_832E8958) {
  const uint32_t manager = ctx.r3.u32, slot = ctx.r4.u32;
  __imp__sub_832E8958(ctx, base);
  if (!AiAlphaOutfitEnabled() || slot != 0 || t_in_ai_outfit) {
    return;
  }
  const uint32_t asset = CustomizationAssetGuest(ctx, base);
  if (!IsReadable(base, asset + kDefaultBravoTattoo, 4)) {
    return;
  }
  const uint32_t bravo_outfit = REX_LOAD_U32(asset + kDefaultBravoOutfit);
  const uint32_t bravo_tattoo = REX_LOAD_U32(asset + kDefaultBravoTattoo);
  if (g_roles_swapped) {
    REX_STORE_U32(asset + kDefaultBravoOutfit, REX_LOAD_U32(asset + kDefaultAlphaOutfit));
    REX_STORE_U32(asset + kDefaultBravoTattoo, REX_LOAD_U32(asset + kDefaultAlphaTattoo));
  }
  auto saved = std::make_unique<PPCContext>(ctx);
  ctx.r1.u64 = ctx.r1.u32 - 0x200;
  ctx.r3.u64 = manager;
  ctx.r4.u64 = 1;
  t_in_ai_outfit = true;
  __imp__sub_832E8958(ctx, base);
  t_in_ai_outfit = false;
  ctx = *saved;
  REX_STORE_U32(asset + kDefaultBravoOutfit, bravo_outfit);
  REX_STORE_U32(asset + kDefaultBravoTattoo, bravo_tattoo);
  std::string objects;
  for (uint32_t i = 1; i <= 8; ++i) {
    const uint32_t object = Load32(base, manager + i * 4);
    objects += fmt::format(" {:08X}[vt={:08X} slot={}]", object, Load32(base, object),
                           Load32(base, object + 4));
  }
  REXLOG_WARN("AO4_AI_OUTFIT slot 1 applied with Alpha defaults, manager={:08X}:{}", manager,
              objects);
}

extern "C" REX_FUNC(sub_832ED0F8);  // apply tattoo (object, info, soldier); caches info at +40
extern "C" REX_FUNC(sub_832ECDC0);  // original mask application (object, info, soldier)

namespace {
constexpr uint32_t kTattooObjectVtable = 0x82AA9B30;

// Outfit/tattoo info for a bundle id and whether that bundle is loaded.
uint32_t BundleInfo(PPCContext& ctx, uint8_t* base, uint32_t r1, uint32_t bundle_id,
                    uint32_t& loaded) {
  ctx.r1.u64 = r1;
  ctx.r3.u64 = bundle_id;
  sub_832F1FE0(ctx, base);
  const uint32_t info = ctx.r3.u32;
  const uint32_t bundles = Load32(base, Load32(base, kClientContext) + 32);
  loaded = 0;
  if (info && bundles) {
    ctx.r1.u64 = r1;
    ctx.r3.u64 = bundles;
    ctx.r4.u64 = Load32(base, info + 8);
    sub_82EEB0A8(ctx, base);
    loaded = ctx.r3.u32;
  }
  return info;
}

// Lets slot 1's outfit or tattoo object queue Alpha's bundle: the object falls back to the
// Bravo default, which is replaced by Alpha's id for the duration of the call.
void RequestThroughSlot1(PPCContext& ctx, uint8_t* base, uint32_t r1, uint32_t asset,
                         uint32_t object_offset, uint32_t vtable, uint32_t bravo_field,
                         uint32_t alpha_id, void (*select)(PPCContext&, uint8_t*),
                         const char* what) {
  const uint32_t object = Load32(base, Load32(base, kCustomizationManager) + object_offset);
  if (Load32(base, object) != vtable || Load32(base, object + 4) != 1 ||
      !IsReadable(base, asset + bravo_field, 4)) {
    REXLOG_WARN("AO4_AI_OUTFIT slot 1 {} object missing ({:08X})", what, object);
    return;
  }
  const uint32_t bravo = REX_LOAD_U32(asset + bravo_field);
  REX_STORE_U32(asset + bravo_field, alpha_id);
  ctx.r1.u64 = r1;
  ctx.r3.u64 = object;
  select(ctx, base);
  REX_STORE_U32(asset + bravo_field, bravo);
  REXLOG_WARN("AO4_AI_OUTFIT requested Alpha {} {:08X} through slot 1 object {:08X}", what,
              alpha_id, object);
}

struct AiLook {
  uint32_t soldier = 0;
  uint32_t component = 0, body = 0;
  bool repairing = false;
  // Bundle requests are repeated while the bundle does not arrive: after a checkpoint reload
  // the first request of a custom tattoo or mask stayed without effect.
  struct Request {
    ULONGLONG last = 0;
    uint32_t count = 0;
    bool Due() {
      if ((!Ao4Qol().gear_reload_fix && count >= 8) ||
          (last && GetTickCount64() - last < (count<8?4000:15000))) return false;
      last = GetTickCount64();
      ++count;
      return true;
    }
  } outfit_request, tattoo_request, mask_request;
  bool outfit = false, skin = false, tattoo = false;
  bool mask = false, mask_ready = false;
};

struct LiveGear {
  uint32_t component=0, outfit=0, body=0;
  bool enabled=false, attached=false;
  bool Missing(bool mission) const {
    return Ao4QolGearMissing(component!=0,enabled,mission,attached,body!=0);
  }
};
LiveGear InspectGear(uint8_t* base,uint32_t soldier) {
  LiveGear state;
  constexpr uint32_t type_info=0x8428D370; // original 83061C00's component type
  if (!IsReadable(base,type_info+20,4) || !(Load32(base,soldier+8)&0x00400000)) return state;
  const uint16_t low=REX_LOAD_U16(type_info+20),high=REX_LOAD_U16(type_info+22);
  const uint32_t table=Load32(base,soldier+28),count=Load8(base,table+5);
  for (uint32_t i=0;high && i<count && i<64;++i) {
    const uint32_t entry=table+16+i*32;
    if (!IsReadable(base,entry,16)) break;
    const uint16_t type=REX_LOAD_U16(entry+12);
    if (uint16_t(type-low)>uint16_t(high-low)) continue;
    const uint32_t component=Load32(base,entry+4);
    if (Load32(base,component)!=0x82A45948 || !IsReadable(base,component,3380) ||
        Load32(base,Load32(base,component+8))!=soldier) continue;
    state.component=component;
    state.enabled=Load8(base,component+3369)!=0;
    state.outfit=Load32(base,component+3372);
    const uint32_t body_component=FindMeshComponent(base,soldier,3,nullptr);
    state.body=Load32(base,body_component+16);
    state.attached=state.outfit && Load8(base,component+3370) && state.body;
    break;
  }
  return state;
}

// Human equipment is the actual selected slot-0 profile, never the partner's defaults.
// Reuse the original bundle and application functions; do not change the saved profile.
void RepairHumanGear(PPCContext& ctx,uint8_t* base,uint32_t human,bool mission) {
  const LiveGear state=InspectGear(base,human);
  if (!state.Missing(mission)) return;
  auto saved=std::make_unique<PPCContext>(ctx);
  const uint32_t r1=ctx.r1.u32-0x300;
  const uint32_t manager=Load32(base,kCustomizationManager),object=Load32(base,manager+16);
  if (Load32(base,object)!=kOutfitObjectVtable || Load32(base,object+4)!=0) return;
  const uint32_t info=Load32(base,object+32); // selected info, cached/applied info is +36
  const uint32_t bundles=Load32(base,Load32(base,kClientContext)+32);
  uint32_t loaded=0;
  if (info && bundles) {
    ctx.r1.u64=r1;ctx.r3.u64=bundles;ctx.r4.u64=Load32(base,info+8);
    sub_82EEB0A8(ctx,base);loaded=ctx.r3.u32;
  }
  if (loaded) {
    ctx.r1.u64=r1;ctx.r3.u64=state.component;ctx.r4.u64=info;
    sub_83006B38(ctx,base);
    const LiveGear after=InspectGear(base,human);
    REXLOG_WARN("AO4_QOL_GEAR human={:08X} component={:08X} profile_info={:08X} body_after={:08X} attached={}",
                human,state.component,info,after.body,after.attached);
    if (after.body) {
      SetSoldierSkin(ctx,base,human,g_roles_swapped?kBravoSkin:kAlphaSkin);
      const uint32_t tattoo_info=Load32(base,Load32(base,manager+4)+36);
      if (tattoo_info) {
        const uint32_t scratch=r1+0x100;
        for (uint32_t i=0;i<0x40;i+=4) REX_STORE_U32(scratch+i,0);
        ctx.r1.u64=r1;ctx.r3.u64=scratch;ctx.r4.u64=tattoo_info;ctx.r5.u64=human;
        sub_832ED0F8(ctx,base);
      }
      const uint32_t mask_object=Load32(base,manager+8),mask_info=Load32(base,mask_object+292);
      if (Load32(base,mask_object)==0x82AA9AD8 && mask_info) {
        ctx.r1.u64=r1;ctx.r3.u64=mask_object;ctx.r4.u64=mask_info;ctx.r5.u64=human;
        sub_832ECDC0(ctx,base);
      }
    }
  } else {
    static uint64_t next_request=0;
    if (GetTickCount64()>=next_request) {
      next_request=GetTickCount64()+4000;
      ctx.r1.u64=r1;ctx.r3.u64=object;
      __imp__sub_832F43C0(ctx,base);
      REXLOG_WARN("AO4_QOL_GEAR human={:08X} profile outfit pending; requested slot 0",human);
    }
  }
  ctx=*saved;
}

// Bravo mode / F6: the AI partner's bundles are not part of the level's own preload
// (Alpha_Escort in Bravo mode streamed ~2 s after the T21 spawn). Request them on the
// main thread as soon as the target is known, also while the next load runs.
void PreloadPartnerBundles(PPCContext& ctx,uint8_t* base,uint64_t now) {
  static Ao4QolPreload preload;
  const bool swapped=g_roles_swapped.load();
  const bool mission=ao4_scavenge_appearance.load() || ao4_scavenge_level.load() || ao4_tutorial_level.load();
  const PartnerEquipment& eq=g_partner_equipment;
  const uint32_t context=g_soldier_epoch.load()*0x9E3779B1u^ao4_mission_appearance_revision.load();
  const uint64_t target=(uint64_t(eq.active?eq.outfit^(eq.tattoo*31u)^(eq.mask*131u):0)<<32)|
      (uint64_t(context&0xFFFFFF)<<8)|(mission?4u:0u)|(eq.active?2u:0u)|(swapped?1u:0u);
  if (!preload.Due(now,target) || !Load32(base,kCustomizationManager)) return;
  auto saved=std::make_unique<PPCContext>(ctx);
  const uint32_t r1=ctx.r1.u32-0x200;
  const uint32_t asset=CustomizationAssetGuest(ctx,base);
  if (!asset) {ctx=*saved;return;}
  const uint32_t outfit_id=eq.active && eq.outfit ? eq.outfit
      : Load32(base,asset+(swapped?kDefaultAlphaOutfit:kDefaultBravoOutfit));
  const uint32_t tattoo_id=eq.active && eq.tattoo ? eq.tattoo
      : Load32(base,asset+(swapped?kDefaultAlphaTattoo:kDefaultBravoTattoo));
  const uint32_t mask_id=eq.active && eq.mask ? eq.mask : Load32(base,asset+(swapped?0x44:0x48));
  uint32_t outfit_loaded=0,tattoo_loaded=0,mask_loaded=0;
  const uint32_t outfit_info=BundleInfo(ctx,base,r1,outfit_id,outfit_loaded);
  const uint32_t tattoo_info=BundleInfo(ctx,base,r1,tattoo_id,tattoo_loaded);
  const uint32_t mask_info=BundleInfo(ctx,base,r1,mask_id,mask_loaded);
  bool pending=false;
  // The mission's own body (Drogenlabor, tutorial) is never replaced, so its outfit is not needed.
  if (!mission && outfit_info && !outfit_loaded) {
    RequestThroughSlot1(ctx,base,r1,asset,32,kOutfitObjectVtable,kDefaultBravoOutfit,
                        outfit_id,__imp__sub_832F43C0,"outfit");
    pending=true;
  }
  if (tattoo_info && !tattoo_loaded) {
    RequestThroughSlot1(ctx,base,r1,asset,20,kTattooObjectVtable,kDefaultBravoTattoo,
                        tattoo_id,__imp__sub_832F35C8,"tattoo");
    pending=true;
  }
  if (mask_info && !mask_loaded) {
    RequestThroughSlot1(ctx,base,r1,asset,24,0x82AA9AD8,0x48,mask_id,__imp__sub_832F34A0,"mask");
    pending=true;
  }
  if (!pending) preload.Finish();
  REXLOG_WARN("AO4_QOL_GEAR_PRELOAD swapped={} f6={} mission={} outfit={:08X}:{} tattoo={:08X}:{} mask={:08X}:{} {}",
              swapped,eq.active,mission,outfit_id,outfit_loaded!=0,tattoo_id,tattoo_loaded!=0,
              mask_id,mask_loaded!=0,pending?"requested":"all loaded");
  ctx=*saved;
}
std::recursive_mutex g_equipment_lock;
}  // namespace

// Input poll on the guest main thread, which keeps running while a level loads (the
// client-player getter does not). Once the new level's data is registered (mission
// appearance revision changed after the soldiers vanished), request the partner's
// bundles so they are loaded when the soldiers spawn.
void Ao4QolLoadingTick(PPCContext& ctx,uint8_t* base) {
  if (!Ao4Qol().gear_reload_fix || !AiAlphaOutfitEnabled() || t_in_ai_outfit || LocalCoopActive()) return;
  static uint64_t next=0;
  const uint64_t now=GetTickCount64();
  if (now<next) return;
  next=now+250;
  auto* thread=rex::system::XThread::GetCurrentThread();
  if (!thread || !thread->is_guest_thread() || !thread->main_thread()) return;
  static bool present=true;
  static uint32_t vanished_revision=0;
  uint32_t human=0,partner=0;
  const bool now_present=FindClientSoldiers(base,human,partner);
  if (present && !now_present) vanished_revision=ao4_mission_appearance_revision.load();
  present=now_present;
  // In a level the getter path handles it; before the new data the old level is torn down.
  if (present || ao4_mission_appearance_revision.load()==vanished_revision) return;
  std::unique_lock<std::recursive_mutex> lock(g_equipment_lock,std::try_to_lock);
  if (!lock.owns_lock()) return;
  t_in_ai_outfit=true;
  PreloadPartnerBundles(ctx,base,now);
  t_in_ai_outfit=false;
}

// Player -> soldier; called very often, also on the client thread. Used as the periodic
// point for step 2: once per second, until outfit, skin and tattoo are on the AI partner.
namespace {
std::string GuestWords(uint8_t* base, uint32_t address, uint32_t words) {
  std::string text;
  for (uint32_t i = 0; i < words; ++i) {
    text += fmt::format(" +{:X}={:08X}", i * 4, Load32(base, address + i * 4));
  }
  return text;
}

// Once per soldier: the shader parameter components (texture slots) of the human and the
// AI partner, their slot objects (+16) and data candidates, to compare player and AI setup.
void DumpShaderComponents(uint8_t* base, uint32_t human, uint32_t partner) {
  static std::unordered_set<uint32_t> done;
  for (auto [label, soldier] : {std::pair{"human", human}, std::pair{"partner", partner}}) {
    if (!soldier || !done.insert(soldier).second) {
      continue;
    }
    std::string components;
    FindMeshComponent(base, soldier, 0, &components);
    REXLOG_WARN("AO4_SHADERCOMP {} soldier={:08X}:{}", label, soldier, components);
    const uint32_t component = FindMeshComponent(base, soldier, 0, nullptr);
    if (!component) {
      continue;
    }
    REXLOG_WARN("AO4_SHADERCOMP {} component {:08X}:{}", label, component,
                GuestWords(base, component, 24));
    const uint32_t slots = Load32(base, component + 16);
    REXLOG_WARN("AO4_SHADERCOMP {} slots {:08X}:{}", label, slots, GuestWords(base, slots, 16));
    for (uint32_t w = 1; w < 24; ++w) {
      const uint32_t candidate = Load32(base, component + w * 4);
      const uint32_t vtable = Load32(base, candidate);
      if (vtable == 0x82A6D300) {  // ShaderParameterComponentData
        const uint32_t names = Load32(base, candidate + 0x64);
        std::string list;
        for (uint32_t k = 0; k < Load32(base, names - 4) && k < 16; ++k) {
          list += " '" + GuestString(base, Load32(base, names + k * 4)) + "'";
        }
        REXLOG_WARN("AO4_SHADERCOMP {} data at component+{:X} = {:08X} DynamicShaderTextures "
                    "{:08X} count={}:{}",
                    label, w * 4, candidate, names, Load32(base, names - 4), list);
      }
    }
  }
}
}  // namespace

REX_HOOK_RAW(sub_82FAAC18) {
  const uint32_t receiver=ctx.r3.u32,caller=uint32_t(ctx.lr);
  __imp__sub_82FAAC18(ctx, base);
  g_guest_base = base;
  // Level heartbeat for the hotkeys and the overlay: both client soldiers present.
  static std::atomic<uint64_t> next_level_check{0};
  if (const uint64_t now = GetTickCount64(); now >= next_level_check.load()) {
    next_level_check = now + 500;
    uint32_t human = 0, partner = 0;
    const bool present = FindClientSoldiers(base, human, partner);
    if (present) g_partner_seen = now;
    ObserveHumanCoop(base);
    // Soldiers vanishing (load) or new/reused ones appearing: a load context for the QoL check.
    static std::atomic<uint64_t> last_pair{0};
    const uint64_t pair = present ? (uint64_t(human) << 32) | partner : 0;
    if (last_pair.exchange(pair) != pair) g_soldier_epoch.fetch_add(1);
  }
  if (!Ao4Qol().gear_reload_fix) {
    Ao4UpdateEquipment(ctx,base,false);
  } else {
    auto* thread=rex::system::XThread::GetCurrentThread();
    const uint32_t vtable=Load32(base,receiver);
    const bool guest=thread && thread->is_guest_thread(),main=thread && thread->main_thread();
    const bool allowed=Ao4QolGearMainScope(guest,main,vtable);
    // One line for each observed scope category, not one line per getter/frame.
    static std::atomic<uint32_t> scopes{0};
    const uint32_t bit=1u<<((main?2u:0u)+(vtable==kClientPlayerVtable?1u:0u));
    if (!(scopes.fetch_or(bit,std::memory_order_relaxed)&bit))
      REXLOG_WARN("AO4_QOL_GEAR_SCOPE guest={} main={} guest_id={} receiver={:08X} vt={:08X} caller={:08X} allowed={}",
                  guest,main,thread?thread->thread_id():0,receiver,vtable,caller,allowed);
    if (allowed) Ao4UpdateEquipment(ctx,base,true);
  }
}

// The client-player getter is frequent in missions. For the QoL path ONLY the
// module-entry guest main thread may enter here, never a VO/worker/host thread.
void Ao4UpdateEquipment(PPCContext& ctx,uint8_t* base,bool client_update) {
  // No AI partner to equip; slot 1 is the human player 2's customization.
  if (LocalCoopActive()) return;
  std::unique_lock<std::recursive_mutex> lock(g_equipment_lock,std::try_to_lock);
  if (!lock.owns_lock()) return;
  // AO4_SKIN_BRAVO: in levels nothing sets the arm skin of the human's soldier (sub_832EC0D8
  // runs only while loading, for the menu soldier), so it keeps the default Alpha arm template.
  // With the roles swapped, put BravoSkin on it, once per soldier.
  static const bool skin_bravo = GetEnvironmentVariableW(L"AO4_SKIN_BRAVO", nullptr, 0) != 0;
  if ((!AiAlphaOutfitEnabled() && !(skin_bravo && g_roles_swapped)) || t_in_ai_outfit) {
    return;
  }
  // Other threads always fail this check; only a level where the client thread has not
  // passed for 3 s is worth a diagnostic line.
  static std::atomic<uint64_t> last_client_pass{0};
  if (!client_update && (!g_client_thread || GetCurrentThreadId() != g_client_thread)) {
    static std::atomic<uint64_t> next_gate_log{0};
    const uint64_t now = GetTickCount64();
    if (now >= next_gate_log.load() && now - g_partner_seen.load() < 1000 &&
        now - last_client_pass.load() > 3000) {
      next_gate_log = now + 10000;
      REXLOG_WARN("AO4_CLIENT_THREAD gear loop gated: current={} client={} bravo={}",
                  GetCurrentThreadId(), g_client_thread, g_roles_swapped.load());
    }
    return;
  }
  last_client_pass = GetTickCount64();
  static AiLook look;
  static uint32_t mission_revision = 0;
  static ULONGLONG next_try = 0;
  // F6 state from the poller thread (also set in the menu and kept across starts).
  static int applied_wanted = -1;
  static uint64_t equipment_time = 0;
  const bool wanted = g_f6_wanted.load();
  static Ao4QolGearWindow window;
  if (client_update) {
    const uint64_t now=GetTickCount64();
    // A new Partner-Customizer choice while F6 is on is also a trigger (file stat every 2 s).
    static uint64_t next_file=0,file_time=0;
    static uint32_t file_epoch=0;
    if (wanted && now>=next_file) {
      next_file=now+2000;
      if (const uint64_t t=EquipmentFileTime(); t!=file_time) {file_time=t;++file_epoch;}
    }
    t_in_ai_outfit=true;
    PreloadPartnerBundles(ctx,base,now);
    t_in_ai_outfit=false;
    const uint64_t key=Ao4QolGearKey(ao4_player_spawn_epoch.load(),ao4_mission_appearance_revision.load(),
        g_soldier_epoch.load(),wanted,g_mask_cutscene.load(),file_epoch);
    if (window.seen && key!=window.key)
      REXLOG_WARN("AO4_QOL_GEAR_WINDOW open spawn={} revision={} soldiers={} f6={} mask_phase={} file={} tutorial={}",
                  ao4_player_spawn_epoch.load(),ao4_mission_appearance_revision.load(),g_soldier_epoch.load(),
                  wanted,g_mask_cutscene.load(),file_epoch,ao4_tutorial_level.load());
    const bool was_open=window.open;
    if (!window.Due(now,key)) {
      if (was_open && !window.open)
        REXLOG_WARN("AO4_QOL_GEAR_WINDOW closed after {} ms complete={}",now-window.opened,window.done);
      return;
    }
  } else {
    if (int(wanted) != applied_wanted || (g_mask_cutscene && !look.mask)) next_try = 0;
    if (GetTickCount64() < next_try) return;
    next_try = GetTickCount64() + 1000;
  }
  struct CheckTimer {
    using Clock=std::chrono::steady_clock;
    Clock::time_point start=Clock::now();
    bool active=false;
    ~CheckTimer() {
      if (!active) return;
      // Called under equipment_lock: no cross-thread aggregation or extra timer thread.
      static uint64_t checks=0,total_us=0,max_us=0;
      const uint64_t us=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());
      ++checks;total_us+=us;max_us=(std::max)(max_us,us);
      if (checks%30==0)
        REXLOG_WARN("AO4_QOL_GEAR_TIMING checks={} mean_us={} max_us={} total_us={}; active check cost, not FPS percent",
                    checks,total_us/checks,max_us,total_us);
    }
  } timer;
  // Tells the window whether both soldiers are fully equipped (it then closes early).
  struct WindowReport {
    Ao4QolGearWindow& window;
    bool active,complete=false;
    ~WindowReport() {if (active) window.Report(complete);}
  } report{window,client_update};
  uint32_t human = 0, partner = 0;
  if (!FindClientSoldiers(base, human, partner)) {
    // A real unload also invalidates successful application state if the allocator
    // later reuses the same soldier address.
    look=AiLook{};
    return;
  }
  timer.active=client_update;
  const uint32_t revision = ao4_mission_appearance_revision.load();
  const bool mission_changed = revision != mission_revision;
  if (mission_changed) {
    mission_revision = revision;
    g_mask_cutscene = false;
    look = AiLook{};
    REXLOG_WARN("AO4_MISSION_LOOK revision={} scavenge={} mask_phase={}", revision,
                ao4_scavenge_appearance.load(), ao4_scavenge_has_mask.load());
  }
  bool changed = false;
  if (int(wanted) != applied_wanted) {
    applied_wanted = wanted;
    if (wanted) {
      equipment_time = EquipmentFileTime();
      changed = ReloadPartnerEquipment();
    } else {
      changed = g_partner_equipment.active;
      g_partner_equipment.active = false;
    }
  } else if (wanted && EquipmentFileTime() != equipment_time) {
    // New choice saved in the Partner-Customizer while F6 is on: take it over directly.
    equipment_time = EquipmentFileTime();
    changed = ReloadPartnerEquipment();
  }
  if (changed) {
    look = AiLook{};
    REXLOG_WARN("AO4_PARTNER_EQUIPMENT F6 active={} mask={} outfit={} tattoo={}",
                g_partner_equipment.active, g_partner_equipment.mask,
                g_partner_equipment.outfit, g_partner_equipment.tattoo);
  }
  // F10 switches the human's arm skin between Bravo and Alpha, to compare them in game.
  static uint32_t bravo_skin_for = 0;
  if (mission_changed) bravo_skin_for = 0;
  static bool human_alpha_skin = false;
  static bool f10 = false;
  if (skin_bravo && KeyPressed(VK_F10, f10)) {
    human_alpha_skin = !human_alpha_skin;
    bravo_skin_for = 0;
    REXLOG_WARN("AO4_SKIN F10: human arm skin -> {}", human_alpha_skin ? "Alpha" : "Bravo");
  }
  if (skin_bravo && g_roles_swapped && human && human != bravo_skin_for &&
      SetSoldierSkin(ctx, base, human, human_alpha_skin ? kAlphaSkin : kBravoSkin)) {
    bravo_skin_for = human;
  }
  // Proof of what is bound: the slot block (mesh component +16) of both soldiers every 5 s.
  static ULONGLONG next_slot_log = 0;
  if (Enabled() && GetTickCount64() >= next_slot_log) {
    next_slot_log = GetTickCount64() + 5000;
    for (auto [label, soldier] : {std::pair{"human", human}, std::pair{"partner", partner}}) {
      const uint32_t component = FindMeshComponent(base, soldier, 3, nullptr);
      const uint32_t slots = Load32(base, component + 16);
      REXLOG_WARN("AO4_SKIN slots {} soldier={:08X} block={:08X}:{}", label, soldier, slots,
                  GuestWords(base, slots, 20));
    }
  }
  if (!AiAlphaOutfitEnabled()) {
    return;
  }
  static ULONGLONG partner_since = 0;
  if (look.soldier != partner) {
    look = AiLook{};
    look.soldier = partner;
    partner_since = GetTickCount64();
    // Candidate for an immediate decision: before the gear return the human should carry no
    // weapons. Logged per spawn until confirmed for all Drogenlabor load points.
    const uint32_t weapons = Load32(base, human + 1380);
    const uint32_t begin = Load32(base, weapons + 460), end = Load32(base, weapons + 464);
    REXLOG_WARN("AO4_MISSION_LOOK partner spawn scavenge_level={} human_weapons={}",
                ao4_scavenge_level.load(), end >= begin && end - begin < 256 ? (end - begin) / 4 : 0);
  }
  // A Drogenlabor checkpoint after the gear return never replays a mission stage (the human
  // already wears the full gear). The level flag alone must not keep the AI body off: without
  // a stage within 3 s of the partner's spawn (seen after 0.2-2 s at the earlier load
  // points), the normal gear applies.
  if (ao4_scavenge_level.load() && !ao4_scavenge_appearance.load() &&
      GetTickCount64() - partner_since > 3000) {
    ao4_scavenge_level = false;
    ao4_mission_appearance_revision.fetch_add(1);
    REXLOG_WARN("AO4_MISSION_LOOK no Drogenlabor stage within 3 s of partner spawn -> normal gear");
  }
  if (Enabled()) {
    DumpShaderComponents(base, human, partner);
  }
  // Drogenlabor: from the level load until the gear comes back only the mission's body counts;
  // tattoo and (in the mask stage) mask may still change.
  // QoL: the tutorial's own bodies and masks belong to the level as well.
  const bool mission_outfit = ao4_scavenge_appearance.load() || ao4_scavenge_level.load() ||
                              (client_update && ao4_tutorial_level.load());
  const bool mask_allowed = !mission_outfit || ao4_scavenge_has_mask.load() || g_mask_cutscene;
  // Scavenge body/harness progression belongs to the mission, even with F6 enabled.
  // Reapply profile equipment only when the original mission restores it.
  if (mission_outfit) look.outfit = true;
  // A Drogenlabor load without its stage yet still needs the 3 s decision above.
  const bool stage_pending = ao4_scavenge_level.load() && !ao4_scavenge_appearance.load();
  bool human_ok = true;
  if (client_update) {
    // Always inspect both physical soldiers, even after a previous check succeeded.
    // The actual mesh/component identity catches same-address checkpoint reuse.
    t_in_ai_outfit=true;
    RepairHumanGear(ctx,base,human,mission_outfit);
    t_in_ai_outfit=false;
    const LiveGear live=InspectGear(base,partner);
    const LiveGear human_live=InspectGear(base,human);
    using Signature=std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,bool,bool,bool>;
    static Signature last{};
    const Signature signature{human,partner,human_live.component,live.component,human_live.body,live.body,
                              human_live.enabled,live.enabled,mission_outfit};
    human_ok=!human_live.Missing(mission_outfit);
    if (signature!=last) {
      last=signature;
      REXLOG_WARN("AO4_QOL_GEAR_CHECK human={:08X} component={:08X} body={:08X} enabled={} partner={:08X} component={:08X} body={:08X} enabled={} mission={}",
          human,human_live.component,human_live.body,human_live.enabled,partner,live.component,live.body,live.enabled,mission_outfit);
    }
    if (!live.component) return;
    if (!mission_outfit && !live.enabled) {
      report.complete=human_ok && !stage_pending;  // outfit intentionally hidden
      return;
    }
    const bool new_component=look.component && look.component!=live.component;
    const bool new_body=look.body && look.body!=live.body;
    const bool missing=live.Missing(mission_outfit);
    if (new_component || new_body || (look.outfit && missing)) {
      REXLOG_WARN("AO4_QOL_GEAR partner={:08X} component={:08X} body={:08X} missing={} component_changed={} body_changed={}",
                  partner,live.component,live.body,missing,new_component,new_body);
      if (!mission_outfit && (new_component || missing)) look.outfit=false;
      look.skin=look.tattoo=look.mask=false;
    }
    look.component=live.component;look.body=live.body;
    static const bool explicit_partner=GetEnvironmentVariableW(L"AO4_AI_ALPHA_OUTFIT",nullptr,0)!=0;
    // With all appearance mods off, a healthy original spawn body is already correct.
    // Only start a default-equipment repair when its normal body is actually missing.
    if (missing) look.repairing=true;
    if (!explicit_partner && !g_roles_swapped && !g_partner_equipment.active &&
        !mission_outfit && !look.repairing) {
      report.complete=human_ok && !stage_pending;
      return;
    }
  }
  // Before the mission puts the masks on, the mask bundle is only loaded (not applied), so
  // cutscene mask props (sub_832EE030 hook) can already show the partner's mask.
  if (look.outfit && look.skin && look.tattoo && (mask_allowed ? look.mask : look.mask_ready)) {
    report.complete=human_ok && !stage_pending;
    return;
  }
  t_in_ai_outfit = true;
  auto saved = std::make_unique<PPCContext>(ctx);
  const uint32_t r1 = ctx.r1.u32 - 0x200;
  if (mission_outfit && mission_changed) {
    for (uint32_t soldier : {human, partner}) {
      ctx.r1.u64 = r1;
      ctx.r3.u64 = soldier;
      ctx.r4.u64 = 0;
      sub_83061C00(ctx, base);
      const uint32_t component = ctx.r3.u32;
      if (!component) continue;
      // Read only: an additional clear here would remove the current last mesh,
      // which can already be the new mission body, rather than the old outfit.
      REXLOG_WARN("AO4_SCAVENGE_COMPONENT soldier={:08X} component={:08X} revision={} "
                  "meshes:{} outfit:{}", soldier, component, mission_revision,
                  GuestWords(base, component + 352, 3), GuestWords(base, component + 3368, 3));
    }
  }
  // The AI partner is Alpha in Bravo mode, otherwise Bravo: that one's defaults without F6.
  const uint32_t default_outfit = g_roles_swapped ? kDefaultAlphaOutfit : kDefaultBravoOutfit;
  const uint32_t default_tattoo = g_roles_swapped ? kDefaultAlphaTattoo : kDefaultBravoTattoo;
  const uint32_t default_mask = g_roles_swapped ? 0x44 : 0x48;
  const uint32_t asset = CustomizationAssetGuest(ctx, base);
  const uint32_t outfit_id = g_partner_equipment.active && g_partner_equipment.outfit
      ? g_partner_equipment.outfit : Load32(base, asset + default_outfit);
  const uint32_t tattoo_id = g_partner_equipment.active && g_partner_equipment.tattoo
      ? g_partner_equipment.tattoo : Load32(base, asset + default_tattoo);
  const uint32_t mask_id = g_partner_equipment.active && g_partner_equipment.mask
      ? g_partner_equipment.mask : Load32(base, asset + default_mask);
  uint32_t outfit_loaded = 0, tattoo_loaded = 0;
  const uint32_t outfit_info = BundleInfo(ctx, base, r1, outfit_id, outfit_loaded);
  const uint32_t tattoo_info = BundleInfo(ctx, base, r1, tattoo_id, tattoo_loaded);
  uint32_t mask_loaded = 0;
  const uint32_t mask_info = BundleInfo(ctx, base, r1, mask_id, mask_loaded);
  if (mask_loaded) look.mask_ready = true;
  if (mask_info && !mask_loaded && look.mask_request.Due()) {
    RequestThroughSlot1(ctx, base, r1, asset, 24, 0x82AA9AD8, 0x48,
                        mask_id, __imp__sub_832F34A0, "mask");
  }
  if (!mission_outfit && !outfit_loaded && look.outfit_request.Due()) {
    RequestThroughSlot1(ctx, base, r1, asset, 32, kOutfitObjectVtable, kDefaultBravoOutfit,
                        outfit_id, __imp__sub_832F43C0, "outfit");
  }
  if (!tattoo_loaded && look.tattoo_request.Due()) {
    RequestThroughSlot1(ctx, base, r1, asset, 20, kTattooObjectVtable, kDefaultBravoTattoo,
                        tattoo_id, __imp__sub_832F35C8, "tattoo");
  }
  // Step 2a: outfit body (customization component).
  if (outfit_loaded && !look.outfit) {
    ctx.r1.u64 = r1;
    ctx.r3.u64 = partner;
    ctx.r4.u64 = 0;
    sub_83061C00(ctx, base);
    const uint32_t component = ctx.r3.u32;
    if (component) {
      ctx.r1.u64 = r1;
      ctx.r3.u64 = component;
      ctx.r4.u64 = outfit_info;
      sub_83006B38(ctx, base);
      const LiveGear after=InspectGear(base,partner);
      look.outfit = !client_update || after.attached;
      REXLOG_WARN("AO4_AI_OUTFIT outfit '{}' apply invoked on partner {:08X}; body={:08X} attached={}",
                  GuestString(base, Load32(base, outfit_info + 8)), partner,after.body,after.attached);
    }
  }
  // Step 2b: once the outfit mesh is in place, skin texture (slot 2) and tattoo (slot 1).
  if (look.outfit && !look.skin) {
    ctx.r1.u64 = r1;
    look.skin = SetSoldierSkin(ctx, base, partner, g_roles_swapped ? kAlphaSkin : kBravoSkin);
    if (look.skin) {
      REXLOG_WARN("AO4_AI_OUTFIT skin set on partner {:08X}", partner);
    }
  }
  if (look.outfit && !look.tattoo && tattoo_loaded) {
    if (FindMeshComponent(base, partner, 2, nullptr)) {
      // Scratch object for the info cache at +40, inside the reserved frame.
      const uint32_t scratch = r1 + 0x100;
      for (uint32_t i = 0; i < 0x40; i += 4) {
        REX_STORE_U32(scratch + i, 0);
      }
      ctx.r1.u64 = r1;
      ctx.r3.u64 = scratch;
      ctx.r4.u64 = tattoo_info;
      ctx.r5.u64 = partner;
      sub_832ED0F8(ctx, base);
      look.tattoo = true;
      REXLOG_WARN("AO4_AI_OUTFIT tattoo '{}' applied to partner {:08X}",
                  GuestString(base, Load32(base, tattoo_info + 8)), partner);
    }
  }
  if (look.outfit && mask_allowed && !look.mask && mask_loaded) {
    const uint32_t object = Load32(base, Load32(base, kCustomizationManager) + 24);
    if (Load32(base, object) == 0x82AA9AD8 && mask_info) {
      ctx.r1.u64 = r1;
      ctx.r3.u64 = object;
      ctx.r4.u64 = mask_info;
      ctx.r5.u64 = partner;
      sub_832ECDC0(ctx, base);
      look.mask = true;
      REXLOG_WARN("AO4_PARTNER_EQUIPMENT mask apply invoked id={} partner={:08X}",
                  mask_id, partner);
    }
  }
  ctx = *saved;
  t_in_ai_outfit = false;
  report.complete=human_ok && !stage_pending && look.outfit && look.skin && look.tattoo &&
                  (mask_allowed ? look.mask : look.mask_ready);
}

// Cutscene mask props (AO4AssignMaskMeshEntity, e.g. TWO_Mask_Alpha/Bravo in DL_02_Masks): the
// event handler sub_832F3F28 passes the client player with the Alpha bit 0x40 of Player+1113 for
// the Alpha props and, only if that player's partner is no PAI (+840 clear, co-op), the partner
// for the Bravo props; in single player the Bravo props keep their default mesh (Bravo's mask).
// Per prop, sub_832EE030(entity, player, mesh entity) takes the mask from customization slot
// object *(manager + 8 + 16 * slot), slot 0 for the Alpha bit, through its loaded mask info at
// +292 (dereferenced without a null check). With the roles swapped (Bravo mode):
// - The Alpha props (passed with the human) take the AI partner's mask (F6 choice or Alpha
//   default, as in the gameplay path) through slot 1, the object the AI's own mask is applied
//   with, while that bundle is loaded. Otherwise they keep their default mesh (Alpha's mask).
// - The Bravo props (passed with the AI partner) read slot 0, the human's profile mask.
// Without Bravo mode the human keeps Alpha's props (original) and the Bravo props take the AI
// partner's mask (F6 choice or Bravo default) through slot 1.
// A prop whose mask info would be missing is skipped instead of crashing.
// - The Bravo props need the partner from client getPartner (sub_82FAB258), which accepts only a
//   PAI partner (sub_82ED5410), while the handler then requires +840 clear. So +840 of the
//   partner it returns is cleared only from that moment until the handler ends.
thread_local bool t_in_mask_handler = false;
thread_local std::vector<std::pair<uint32_t, uint8_t>> t_mask_pai_cleared;

REX_HOOK_RAW(sub_82FAB258) {
  __imp__sub_82FAB258(ctx, base);
  const uint32_t partner = ctx.r3.u32;
  if (!t_in_mask_handler || !IsReadable(base, partner, kPlayerIsPai + 1) ||
      REX_LOAD_U32(partner) != kClientPlayerVtable || !REX_LOAD_U8(partner + kPlayerIsPai)) {
    return;
  }
  t_mask_pai_cleared.emplace_back(partner, REX_LOAD_U8(partner + kPlayerIsPai));
  REX_STORE_U8(partner + kPlayerIsPai, 0);
}

REX_HOOK_RAW(sub_832F3F28) {
  const uint32_t event = Load32(base, ctx.r4.u32 + 4);
  const bool mask_event = event == 0xB06AD19C || event == 0xDE001442;
  if (!AiAlphaOutfitEnabled() || !mask_event || t_in_mask_handler) {
    __imp__sub_832F3F28(ctx, base);
    return;
  }
  REXLOG_WARN("AO4_CUTSCENE_MASK handler entity={:08X} event={:08X}", ctx.r3.u32, event);
  // The masks go on in this cutscene: the AI partner's own mask shows in the close-ups
  // after it, before the mission switches to the mask phase. Let the gameplay path apply it now.
  if (event == 0xB06AD19C) g_mask_cutscene = true;
  t_in_mask_handler = true;
  __imp__sub_832F3F28(ctx, base);
  t_in_mask_handler = false;
  for (auto [player, value] : t_mask_pai_cleared) REX_STORE_U8(player + kPlayerIsPai, value);
  t_mask_pai_cleared.clear();
}

REX_HOOK_RAW(sub_832EE030) {
  const uint32_t player = ctx.r4.u32;
  constexpr uint32_t kRoleFlags = 1113, kLoadedMaskInfo = 292;
  const bool is_player = IsReadable(base, player, kPlayerIsPai + 1) &&
                         REX_LOAD_U32(player) == kClientPlayerVtable;
  const bool human = is_player && !REX_LOAD_U8(player + kPlayerIsBot);
  // The handler hook above cleared +840 of the AI partner for the call; a bot is the partner.
  const bool partner = is_player && !human;
  if (!AiAlphaOutfitEnabled() || (!human && !partner)) {
    __imp__sub_832EE030(ctx, base);
    return;
  }
  // The prop of the AI partner's character (Alpha's props in Bravo mode, Bravo's otherwise)
  // reads slot 1 with the AI partner's mask; the human's prop reads slot 0, the profile mask.
  const bool ai_prop = g_roles_swapped ? human : partner;
  const uint8_t flags = REX_LOAD_U8(player + kRoleFlags);
  const uint8_t patched = ai_prop ? uint8_t(flags & ~0x40) : uint8_t(flags | 0x40);
  const uint32_t manager = Load32(base, kCustomizationManager);
  const uint32_t object = Load32(base, manager + (ai_prop ? 24 : 8));
  const bool object_ok = IsReadable(base, object + kLoadedMaskInfo, 4) &&
                         REX_LOAD_U32(object) == 0x82AA9AD8;  // mask object
  const uint32_t object_info = object_ok ? REX_LOAD_U32(object + kLoadedMaskInfo) : 0;
  uint32_t info = ai_prop ? 0 : object_info, ai_mask = 0;
  if (ai_prop && object_ok) {
    auto saved = std::make_unique<PPCContext>(ctx);
    const uint32_t r1 = ctx.r1.u32 - 0x200;
    const uint32_t asset = CustomizationAssetGuest(ctx, base);
    ai_mask = g_partner_equipment.active && g_partner_equipment.mask
        ? g_partner_equipment.mask : Load32(base, asset + (g_roles_swapped ? 0x44 : 0x48));
    uint32_t loaded = 0;
    const uint32_t ai_info = ai_mask ? BundleInfo(ctx, base, r1, ai_mask, loaded) : 0;
    ctx = *saved;
    if (loaded) info = ai_info;
  }
  REXLOG_WARN("AO4_CUTSCENE_MASK entity={:08X} mesh={:08X} player={:08X} soldier={:08X} {} flags={:02X}->{:02X} "
              "object={:08X} info={:08X}->{:08X} ai_mask={:08X}{}",
              ctx.r3.u32, ctx.r5.u32, player, ClientPlayerSoldier(base, player), human ? "human" : "partner", flags, patched, object,
              object_info, info, ai_mask, info ? "" : " skipped (default mesh kept)");
  if (!info) {
    return;
  }
  REX_STORE_U8(player + kRoleFlags, patched);
  REX_STORE_U32(object + kLoadedMaskInfo, info);
  __imp__sub_832EE030(ctx, base);
  REX_STORE_U32(object + kLoadedMaskInfo, object_info);
  REX_STORE_U8(player + kRoleFlags, flags);
}

// Nametag letter over the partner: sub_834AAF18 picks the tag status STATUS_AlphaAlive (0) or
// STATUS_BravoAlive (1) from sub_834ABB58 (its only caller), which reads the Alpha bit 0x40 of
// Player+1113. The other statuses (incapacitated, dead, overkill, ...) do not depend on the
// role. With the roles swapped, the human and the AI partner answer the other way round, so
// the partner shows "A". Rios, Fiona and other named bots are unaffected.
REX_HOOK_RAW(sub_834ABB58) {
  const uint32_t player = ctx.r3.u32;
  __imp__sub_834ABB58(ctx, base);
  if (!g_roles_swapped || !IsReadable(base, player + kPlayerIsPai, 1)) {
    return;
  }
  const bool human = !REX_LOAD_U8(player + kPlayerIsBot);
  const bool partner = REX_LOAD_U8(player + kPlayerIsPai) != 0;
  if (human || partner) {
    const uint32_t original = ctx.r3.u32;
    ctx.r3.u64 = partner ? 1 : 0;
    static std::mutex lock;
    static std::unordered_set<uint32_t> logged;
    std::lock_guard<std::mutex> guard(lock);
    if (logged.insert(player).second) {
      REXLOG_WARN("AO4_NAMETAG player={:08X} {} alpha {} -> {}", player,
                  partner ? "partner" : "human", original, ctx.r3.u32);
    }
  }
}

// Menu lighting diagnostics (AO4_MENU_LIGHT_TRACE=1), pass-through otherwise. PlayerCustomization
// fades its key spot out on menu commands (FloatSwitch Select2 + sequence 35->0) and back in only
// when EventAndGate c10241ee (EventCount 2) fires: In1 with Reset from the fade command, In2 from
// another dispatcher command (FloatSwitch Select3 + sequence 0->35). Logs the gate inputs and
// the switch selections with their order. onEvent is vtable slot 4 (r3 entity, event id at r4+4).
namespace {
bool MenuLightTrace() {
  static const bool on = GetEnvironmentVariableW(L"AO4_MENU_LIGHT_TRACE", nullptr, 0) != 0;
  return on;
}
const char* MenuLightEventName(uint32_t id) {
  switch (id) {
    case 0x0B87D373: return "In1";
    case 0x0B87D370: return "In2";
    case 0x0DB4B330: return "Reset";
    case 0xA5BF143C: return "Select1";
    case 0xA5BF143F: return "Select2";
    case 0xA5BF143E: return "Select3";
    case 0x0DC0EFA5: return "Start";
    default: return nullptr;
  }
}
std::atomic<uint32_t> g_menu_light_logs{0};
void TraceMenuLight(PPCContext& ctx, uint8_t* base, const char* kind,
                    void (*original)(PPCContext&, uint8_t*)) {
  const uint32_t entity = ctx.r3.u32;
  const uint32_t id = IsReadable(base, ctx.r4.u32 + 4, 4) ? REX_LOAD_U32(ctx.r4.u32 + 4) : 0;
  const char* name = MenuLightEventName(id);
  uint32_t before[8] = {};
  const bool log = name && g_menu_light_logs.load() < 2000 && IsReadable(base, entity, 0x40);
  if (log) for (int i = 0; i < 8; ++i) before[i] = REX_LOAD_U32(entity + 0x20 + i * 4);
  original(ctx, base);
  if (!log) return;
  g_menu_light_logs.fetch_add(1);
  std::string words;
  for (int i = 0; i < 8; ++i) {
    const uint32_t after = REX_LOAD_U32(entity + 0x20 + i * 4);
    words += before[i] == after ? fmt::format(" {:08X}", after)
                                : fmt::format(" {:08X}>{:08X}", before[i], after);
  }
  REXLOG_WARN("AO4_MENU_LIGHT {} {:08X} {} tick={} +20:{}", kind, entity, name, GetTickCount64(),
              words);
}
}  // namespace

REX_HOOK_RAW(sub_82D57A70) {
  if (!MenuLightTrace()) return __imp__sub_82D57A70(ctx, base);
  TraceMenuLight(ctx, base, "gate", __imp__sub_82D57A70);
}

REX_HOOK_RAW(sub_82D5FB88) {
  if (!MenuLightTrace()) return __imp__sub_82D5FB88(ctx, base);
  TraceMenuLight(ctx, base, "fswitch", __imp__sub_82D5FB88);
}

REX_HOOK_RAW(sub_82D67760) {
  if (!MenuLightTrace()) return __imp__sub_82D67760(ctx, base);
  TraceMenuLight(ctx, base, "sequence", __imp__sub_82D67760);
}

// Customization command dispatcher (ClientAO4CustCommandDispatcherEntity): its message listener
// (this+100) handles category "Customization" (771B8298), type = hash of the message name at +8,
// index at +40/+44, and fires entity events via sub_82D409B0(entity, a, b, event id).
// SetWeaponSlotCamera (8BEBC932) fades the key light out, SetHighlightedLoadoutItemCamera
// (A4CD25F8) picks light set A/B/C, SetKitItemCamera (BAF20FF7) feeds the fade-in gate (In2).
REX_HOOK_RAW(sub_833C87B8) {
  if (MenuLightTrace() && IsReadable(base, ctx.r4.u32, 48) &&
      REX_LOAD_U32(ctx.r4.u32 + 4) == 0x771B8298 && g_menu_light_logs.load() < 2000) {
    g_menu_light_logs.fetch_add(1);
    const uint32_t m = ctx.r4.u32;
    REXLOG_WARN("AO4_MENU_LIGHT dispatcher {:08X} msg type={:08X} +40={} +44={} tick={}",
                ctx.r3.u32 - 100, REX_LOAD_U32(m + 8), int32_t(REX_LOAD_U32(m + 40)),
                int32_t(REX_LOAD_U32(m + 44)), GetTickCount64());
  }
  __imp__sub_833C87B8(ctx, base);
}

REX_HOOK_RAW(sub_82D409B0) {
  if (MenuLightTrace() && g_menu_light_logs.load() < 2000) {
    const uint32_t id = ctx.r6.u32;
    switch (id) {
      case 0x3A5A4367: case 0x4F4FAF51: case 0x7C40F23A: case 0xD295DAAA: case 0x36F98B3E:
      case 0x5F9B7E6D: case 0x687F2E76: case 0xE7FA174D: case 0xE4C366D2: case 0xB9503C24:
      case 0x067D2F67: case 0x3ACF0D78: case 0x8900FACE: case 0x310AAB0F:
        g_menu_light_logs.fetch_add(1);
        REXLOG_WARN("AO4_MENU_LIGHT fire {:08X} event={:08X} tick={}", ctx.r3.u32, id,
                    GetTickCount64());
        break;
      default: break;
    }
  }
  __imp__sub_82D409B0(ctx, base);
}

// Aim state discovery (AO4_AIM_SCAN=<directory>): F5 in a level writes a snapshot of the human
// client player, its soldier and the objects they point to (two levels) into
// <directory>/aim-scan-<n>.bin, raw guest bytes. Snapshots taken while looking in known
// directions show which floats hold the view yaw/pitch. Format: "AO4S", u32 count, then per
// block u32 address, u32 size, bytes.
namespace {
// Aim object found by the scans: client soldier +0x7C8 -> +0x15C, vtable 82AAF81C (the one
// right before AO2ServerSoldierEntity's). Yaw in radians, unwrapped, at +0x2A8, its negation at
// +0x300, pitch at +0x304 (+-1.484), zoom sensitivity factor at +0x2FC (1 / 0.4 aim / 0.2 scope).
constexpr uint32_t kAimObjectVtable = 0x82AAF81C;
uint32_t HumanClientSoldier(uint8_t* base) {
  const uint32_t list = Load32(base, Load32(base, kClientContext) + kPlayerList);
  const uint32_t begin = Load32(base, list + kClientPlayersBegin);
  const uint32_t end = Load32(base, list + kClientPlayersEnd);
  for (uint32_t slot = begin; slot && slot < end && slot < begin + 64; slot += 4) {
    const uint32_t player = Load32(base, slot);
    if (IsReadable(base, player, kClientPlayerSoldierLink + 4) &&
        REX_LOAD_U32(player) == kClientPlayerVtable && !REX_LOAD_U8(player + kPlayerIsBot)) {
      return ClientPlayerSoldier(base, player);
    }
  }
  return 0;
}
bool IsAimObject(uint8_t* base, uint32_t object) {
  return object >= 0x40000000 && object < 0x90000000 && !(object & 3) &&
         IsReadable(base, object, 0x308) && REX_LOAD_U32(object) == kAimObjectVtable;
}
uint32_t AimObject(uint8_t* base, uint32_t soldier) {
  if (!IsReadable(base, soldier, 0x1400)) {
    REXLOG_WARN("AO4_AIM_POKE soldier {:08X} not readable", soldier);
    return 0;
  }
  const uint32_t link = Load32(base, soldier + 0x7C8);
  const uint32_t aim = IsReadable(base, link, 0x160) ? Load32(base, link + 0x15C) : 0;
  if (IsAimObject(base, aim)) return aim;
  REXLOG_WARN("AO4_AIM_POKE soldier={:08X} +7C8={:08X} +15C={:08X} vtable={:08X}, searching",
              soldier, link, aim, IsReadable(base, aim, 4) ? REX_LOAD_U32(aim) : 0);
  // The scans showed several stable two-step paths; find any of them.
  for (uint32_t offset = 0; offset < 0x1400; offset += 4) {
    const uint32_t first = REX_LOAD_U32(soldier + offset);
    if (IsAimObject(base, first)) {
      REXLOG_WARN("AO4_AIM_POKE found soldier+{:X} -> {:08X}", offset, first);
      return first;
    }
    if (first < 0x40000000 || first >= 0x90000000 || (first & 3) ||
        !IsReadable(base, first, 0x600)) {
      continue;
    }
    for (uint32_t inner = 0; inner < 0x600; inner += 4) {
      const uint32_t second = REX_LOAD_U32(first + inner);
      if (IsAimObject(base, second)) {
        REXLOG_WARN("AO4_AIM_POKE found soldier+{:X} -> {:08X} +{:X} -> {:08X}", offset, first,
                    inner, second);
        return second;
      }
    }
  }
  return 0;
}
float LoadF(uint8_t* base, uint32_t address) {
  const uint32_t bits = REX_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
void StoreF(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  REX_STORE_U32(address, bits);
}

// Which function overwrites the aim object's yaw (the object is a per-frame copy): every
// function with object stores at +680/+768 compares the watched yaw before and after the call.
std::atomic<uint32_t> g_aim_watch{0};
constexpr const char* kAimWriters[] = {"sub_82F942B8", "sub_82F9C398", "sub_82FA0FE0", "sub_82FE7420", "sub_82FF1108", "sub_8303EC08", "sub_83165DB0", "sub_8316A8F0", "sub_8316B288", "sub_8329FFE0", "sub_83340E68", "sub_833461A0", "sub_833471B8", "sub_833AF700", "sub_833B10E0", "sub_833B8620", "sub_833D8EB8", "sub_8340E550", "sub_834A3910", "sub_834A77D0", "sub_834A80C8", "sub_8350D160", "sub_83619090", "sub_83619B30", "sub_83886510", "sub_838BA978", "sub_838D47B8", "sub_83B47040", "sub_83B55B18", "sub_83B58C68", "sub_83E27060", "sub_83E54878"};
std::atomic<uint32_t> g_aim_writer_hits[32];
std::atomic<uint32_t> g_aim_writer_last[32];
void WatchAim(int index, PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  const uint32_t aim = g_aim_watch.load();
  if (!aim) return original(ctx, base);
  const uint32_t before = REX_LOAD_U32(aim + 0x2A8);
  const uint32_t r3 = ctx.r3.u32;
  original(ctx, base);
  const uint32_t after = REX_LOAD_U32(aim + 0x2A8);
  if (after != before) {
    g_aim_writer_hits[index].fetch_add(1);
    g_aim_writer_last[index] = r3;
  }
}
void LogAimWriters() {
  for (size_t i = 0; i < std::size(kAimWriters); ++i) {
    const uint32_t hits = g_aim_writer_hits[i].exchange(0);
    if (hits) REXLOG_WARN("AO4_AIM_WRITER {} changed yaw {} times, last r3={:08X}", kAimWriters[i],
                          hits, g_aim_writer_last[i].load());
  }
}
// F2: tests the candidate fields listed in <scan dir>/candidates.txt (one hex guest address per
// line, written by aim-scan-analyze.py while the game keeps running). Each float gets +0.3 for
// 150 ms; a field the game recomputes every frame drops the change, the source of the view keeps
// it. The change is taken back right after the check.
void TestAimCandidates(uint8_t* base, const std::wstring& dir) {
  FILE* file = _wfopen((dir + L"\\candidates.txt").c_str(), L"r");
  if (!file) {
    REXLOG_WARN("AO4_AIM_TEST no candidates.txt");
    Beep(300, 600);
    return;
  }
  char line[64];
  uint32_t tested = 0, kept = 0;
  while (fgets(line, sizeof(line), file) && tested < 400) {
    const uint32_t address = uint32_t(strtoul(line, nullptr, 16));
    if (address < 0x40000000 || address >= 0x90000000 || (address & 3) ||
        !IsReadable(base, address, 4)) {
      continue;
    }
    const float before = LoadF(base, address);
    if (!std::isfinite(before) || std::fabs(before) > 1e4f) continue;
    ++tested;
    StoreF(base, address, before + 0.3f);
    Sleep(150);
    const float after = LoadF(base, address);
    const bool keeps = std::isfinite(after) && after - before > 0.2f && after - before < 0.4f;
    if (keeps) {
      ++kept;
      StoreF(base, address, after - 0.3f);
    }
    REXLOG_WARN("AO4_AIM_TEST {:08X} {:.4f} -> +0.3 -> {:.4f} {}", address, before, after,
                keeps ? "KEPT" : "dropped");
  }
  fclose(file);
  REXLOG_WARN("AO4_AIM_TEST done: {} tested, {} kept", tested, kept);
  Beep(1200, 70);
  Beep(1200, 70);
}
struct AimScanner {
  AimScanner() {
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"AO4_AIM_SCAN", dir, MAX_PATH)) return;
    std::wstring out = dir;
    std::thread([out] {
      REXLOG_WARN("AO4_AIM_SCAN active (F5 snapshot, F2 candidate test), {} chars of output path",
                  out.size());
      bool down = false, down2 = false;
      for (uint32_t n = 1;; Sleep(30)) {
        const bool f5 = KeyPressed(VK_F5, down), f2 = KeyPressed(VK_F2, down2);
        if (!f5 && !f2) continue;
        uint8_t* base = g_guest_base.load();
        REXLOG_WARN("AO4_AIM_SCAN key {} base={}", f5 ? "F5" : "F2", base ? "set" : "null");
        if (!base) continue;
        if (f2) {
          TestAimCandidates(base, out);
          continue;
        }
        uint32_t human_player = 0, human_soldier = 0;
        const uint32_t list = Load32(base, Load32(base, kClientContext) + kPlayerList);
        const uint32_t begin = Load32(base, list + kClientPlayersBegin);
        const uint32_t end = Load32(base, list + kClientPlayersEnd);
        for (uint32_t slot = begin; slot && slot < end && slot < begin + 64; slot += 4) {
          const uint32_t player = Load32(base, slot);
          if (IsReadable(base, player, kClientPlayerSoldierLink + 4) &&
              REX_LOAD_U32(player) == kClientPlayerVtable && !REX_LOAD_U8(player + kPlayerIsBot)) {
            human_player = player;
            human_soldier = ClientPlayerSoldier(base, player);
            break;
          }
        }
        if (!human_player) {
          REXLOG_WARN("AO4_AIM_SCAN no human player");
          Beep(300, 600);
          continue;
        }
        std::vector<std::pair<uint32_t, uint32_t>> blocks;
        std::unordered_set<uint32_t> seen;
        auto add = [&](uint32_t address, uint32_t size) {
          if (address < 0x40000000 || address >= 0x90000000 || (address & 3) ||
              !seen.insert(address).second || blocks.size() >= 40000) {
            return false;
          }
          while (size && !IsReadable(base, address, size)) size >>= 1;
          if (size < 0x40) return false;
          blocks.emplace_back(address, size);
          return true;
        };
        add(human_player, 0x1400);
        if (human_soldier) add(human_soldier, 0x1400);
        for (size_t level = 0, first = 0; level < 3; ++level) {
          const size_t last = blocks.size();
          for (size_t i = first; i < last; ++i) {
            const uint32_t scan = level == 0 ? blocks[i].second : std::min(blocks[i].second, level == 1 ? 0x400u : 0x100u);
            for (uint32_t offset = 0; offset + 4 <= scan; offset += 4) {
              add(REX_LOAD_U32(blocks[i].first + offset), level == 0 ? 0x800 : 0x400);
            }
          }
          first = last;
        }
        const std::wstring path = out + L"\\aim-scan-" + std::to_wstring(n) + L".bin";
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file) {
          REXLOG_WARN("AO4_AIM_SCAN cannot write the snapshot file (errno {})", errno);
          Beep(300, 600);
          continue;
        }
        const uint32_t count = uint32_t(blocks.size());
        fwrite("AO4S", 1, 4, file);
        fwrite(&count, 4, 1, file);
        for (auto [address, size] : blocks) {
          fwrite(&address, 4, 1, file);
          fwrite(&size, 4, 1, file);
          fwrite(base + address, 1, size, file);
        }
        fclose(file);
        REXLOG_WARN("AO4_AIM_SCAN {} player={:08X} soldier={:08X} blocks={}", n, human_player,
                    human_soldier, count);
        Beep(1200, 70);
        Beep(1200, 70);
        ++n;
      }
    }).detach();
  }
} g_aim_scanner;
}  // namespace

REX_HOOK_RAW(sub_82F942B8) { WatchAim(0, ctx, base, __imp__sub_82F942B8); }
REX_HOOK_RAW(sub_82F9C398) { WatchAim(1, ctx, base, __imp__sub_82F9C398); }
REX_HOOK_RAW(sub_82FA0FE0) { WatchAim(2, ctx, base, __imp__sub_82FA0FE0); }
REX_HOOK_RAW(sub_82FE7420) { WatchAim(3, ctx, base, __imp__sub_82FE7420); }
REX_HOOK_RAW(sub_82FF1108) { WatchAim(4, ctx, base, __imp__sub_82FF1108); }
REX_HOOK_RAW(sub_8303EC08) { WatchAim(5, ctx, base, __imp__sub_8303EC08); }
REX_HOOK_RAW(sub_83165DB0) { WatchAim(6, ctx, base, __imp__sub_83165DB0); }
REX_HOOK_RAW(sub_8316A8F0) { WatchAim(7, ctx, base, __imp__sub_8316A8F0); }
REX_HOOK_RAW(sub_8316B288) { WatchAim(8, ctx, base, __imp__sub_8316B288); }
REX_HOOK_RAW(sub_8329FFE0) { WatchAim(9, ctx, base, __imp__sub_8329FFE0); }
REX_HOOK_RAW(sub_83340E68) { WatchAim(10, ctx, base, __imp__sub_83340E68); }
REX_HOOK_RAW(sub_833461A0) { WatchAim(11, ctx, base, __imp__sub_833461A0); }
REX_HOOK_RAW(sub_833471B8) { WatchAim(12, ctx, base, __imp__sub_833471B8); }
REX_HOOK_RAW(sub_833AF700) { WatchAim(13, ctx, base, __imp__sub_833AF700); }
REX_HOOK_RAW(sub_833B10E0) { WatchAim(14, ctx, base, __imp__sub_833B10E0); }
REX_HOOK_RAW(sub_833B8620) { WatchAim(15, ctx, base, __imp__sub_833B8620); }
REX_HOOK_RAW(sub_833D8EB8) { WatchAim(16, ctx, base, __imp__sub_833D8EB8); }
REX_HOOK_RAW(sub_8340E550) { WatchAim(17, ctx, base, __imp__sub_8340E550); }
REX_HOOK_RAW(sub_834A3910) { WatchAim(18, ctx, base, __imp__sub_834A3910); }
REX_HOOK_RAW(sub_834A77D0) { WatchAim(19, ctx, base, __imp__sub_834A77D0); }
REX_HOOK_RAW(sub_834A80C8) { WatchAim(20, ctx, base, __imp__sub_834A80C8); }
REX_HOOK_RAW(sub_8350D160) { WatchAim(21, ctx, base, __imp__sub_8350D160); }
REX_HOOK_RAW(sub_83619090) { WatchAim(22, ctx, base, __imp__sub_83619090); }
REX_HOOK_RAW(sub_83619B30) { WatchAim(23, ctx, base, __imp__sub_83619B30); }
REX_HOOK_RAW(sub_83886510) { WatchAim(24, ctx, base, __imp__sub_83886510); }
REX_HOOK_RAW(sub_838BA978) { WatchAim(25, ctx, base, __imp__sub_838BA978); }
REX_HOOK_RAW(sub_838D47B8) { WatchAim(26, ctx, base, __imp__sub_838D47B8); }
REX_HOOK_RAW(sub_83B47040) { WatchAim(27, ctx, base, __imp__sub_83B47040); }
REX_HOOK_RAW(sub_83B55B18) { WatchAim(28, ctx, base, __imp__sub_83B55B18); }
REX_HOOK_RAW(sub_83B58C68) { WatchAim(29, ctx, base, __imp__sub_83B58C68); }
REX_HOOK_RAW(sub_83E27060) { WatchAim(30, ctx, base, __imp__sub_83E27060); }
REX_HOOK_RAW(sub_83E54878) { WatchAim(31, ctx, base, __imp__sub_83E54878); }
