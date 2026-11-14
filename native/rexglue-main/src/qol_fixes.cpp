// Quality-of-life fixes for original-game defects (native/quality-of-life.ini, env
// AO4_QOL_SETTINGS). Independent of the Alpha/Bravo swap: they apply in both modes.
#include "generated/ao4.main/ao4_main_init.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include <cstdio>
#include <string>
#include <vector>
#include "qol_settings.h"
#include "qol_appearance_policy.h"

const Ao4QolSettings& Ao4Qol() {
  static const Ao4QolSettings settings = [] {
    wchar_t path[32768] = {};
    std::string text;
    const bool configured = GetEnvironmentVariableW(L"AO4_QOL_SETTINGS", path, 32768) != 0;
    if (configured) {
      if (FILE* file = _wfopen(path, L"rb")) {
        char buffer[4096];
        size_t read = 0;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < 65536) text.append(buffer, read);
        fclose(file);
      }
    }
    std::vector<std::string> problems;
    const Ao4QolSettings s = Ao4ParseQolSettings(text, &problems);
    REXLOG_WARN("AO4_QOL settings file={} loaded={} dialogzweig_fix={} mg_ansage_fix={} ausruestung_ladefix={}",
                configured ? "set" : "none", !text.empty(), s.dialog_branch_fix, s.mg_callout_fix, s.gear_reload_fix);
    for (const auto& problem : problems) REXLOG_WARN("AO4_QOL settings problem: {}", problem);
    return s;
  }();
  return settings;
}

namespace {
uint32_t QolWord(uint8_t* base, uint32_t address) {
  uint8_t bytes[4]{};
  SIZE_T read = 0;
  if (address < 0x40000000 || address > 0xDFFFFFFC ||
      !ReadProcessMemory(GetCurrentProcess(), base + address, bytes, 4, &read) || read != 4) return 0;
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
}
uint32_t QolArrayCount(uint8_t* base, uint32_t array) { return array ? QolWord(base, array - 4) : 0; }

// VO logic node type objects returned by the nodes' virtual getType (see 82E62380).
constexpr uint32_t kVoContainerNodeType = 0x842822A8;     // True/FalseRelationship +20/+24
constexpr uint32_t kVoConversationNodeType = 0x84282420;  // Finished/Blocked +20/+24
// Return address of the FlowMode_One pick (bl 82DA6198) in the original node executor 82E62380:
// r31 = node, r30 = relationship array, r29 = count, r25 = execution object.
constexpr uint32_t kFlowModeOnePick = 0x82E627F4;

// Guest calls run on scratch stack below the executor frame; all registers are restored.
uint32_t NodeType(PPCContext& ctx, uint8_t* base, uint32_t node) {
  const uint32_t get_type = QolWord(base, QolWord(base, node));
  if (get_type < REX_CODE_BASE || get_type >= REX_CODE_BASE + REX_CODE_SIZE) return 0;
  const PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x300;
  ctx.r3.u64 = node;
  REX_CALL_INDIRECT_FUNC(get_type);
  const uint32_t type = ctx.r3.u32;
  ctx = saved;
  return type;
}
// Original condition evaluation 82E5C2F8 (exec, &conditions, mode, probability). A probability
// above every threshold skips its random roll, so the real pick still rolls exactly once.
bool ConditionHolds(PPCContext& ctx, uint8_t* base, uint32_t exec, uint32_t node) {
  const PPCContext saved = ctx;
  const uint32_t slot = saved.r1.u32 - 0x20;
  REX_STORE_U32(slot, node + 12);
  ctx.r1.u64 = saved.r1.u32 - 0x300;
  ctx.r3.u64 = exec;
  ctx.r4.u64 = slot;
  ctx.r5.u64 = QolWord(base, node + 28);
  ctx.f1.f64 = 1e30;
  __imp__sub_82E5C2F8(ctx, base);
  const bool holds = ctx.r3.u8 != 0;
  ctx = saved;
  return holds;
}
bool CanPlay(PPCContext& ctx, uint8_t* base, uint32_t exec, uint32_t node, uint32_t& type) {
  type = NodeType(ctx, base, node);
  if (type == kVoConversationNodeType) return ConditionHolds(ctx, base, exec, node);
  if (type == kVoContainerNodeType)
    return QolArrayCount(base, QolWord(base, node + (ConditionHolds(ctx, base, exec, node) ? 20 : 24))) > 0;
  return true;  // Triggers, events and other nodes always do something.
}
}  // namespace

// Name of the first wave of a conversation track (runtime Clips +24 -> clip Takes +12 ->
// take Wave +0 -> SoundWaveAsset Name +8), read safely; empty if anything is missing.
std::string FirstWaveName(uint8_t* base, uint32_t track) {
  return Ao4QolTrackFirstWave(track,[&](uint32_t a){return QolWord(base,a);},[&](uint32_t name) -> std::string {
  std::string text;
  for (uint32_t i = 0; name && i < 160; ++i) {
    const uint32_t word = QolWord(base, (name + i) & ~3u);
    const char c = char(word >> (8 * (3 - ((name + i) & 3))));
    if (!c) return text;
    if (uint8_t(c) < 0x20 || uint8_t(c) >= 0x7F) return {};
    text += c;
  }
  return std::string();
  });
}

// Conversation nodes reach 82E5C2F8 (their condition test) right before the executor
// 82E62380 creates the conversation from node+36 (Groups +36, Tracks +40, LastSequenceIndex
// +64, one byte). OS_08_02 (always evaluated first, it holds Bravo's spotting track) is the
// donor; OS_08_02_01 gets that track instead of Alpha's two self-instructions.
void MaybeSwapConversationTrack(uint8_t* base, uint32_t node) {
  static std::mutex lock;
  static uint32_t donor_track = 0;
  const uint32_t tracks = QolWord(base, node + 40);
  const uint32_t count = QolArrayCount(base, tracks);
  if (QolWord(base,node)!=0x82A1CB64 || !count || count > 8) return;
  std::lock_guard<std::mutex> guard(lock);
  // Classify live contents every time: reloading may reuse BOTH node and array addresses.
  const bool target=Ao4QolWaveIs(FirstWaveName(base,QolWord(base,tracks)),kAo4QolMgCallout.target_wave);
  if (!target) {
    for (uint32_t i = 0; i < count; ++i)
      if (Ao4QolWaveIs(FirstWaveName(base, QolWord(base, tracks + 4 * i)), kAo4QolMgCallout.donor_wave)) {
        donor_track = QolWord(base, tracks + 4 * i);
      }
    return;
  }
  if (count != 1 || !donor_track) return;
  if (QolWord(base, tracks) == donor_track) return;  // already swapped
  // The donor must still be the loaded Bravo track (level reloads move the data).
  if (!Ao4QolWaveIs(FirstWaveName(base, donor_track), kAo4QolMgCallout.donor_wave)) {
    donor_track = 0;
    return;
  }
  const uint32_t replaced = QolWord(base, tracks);
  REX_STORE_U32(tracks, donor_track);
  REX_STORE_U8(node + 64, 0);  // the donor track has a single clip, sequence 0
  REXLOG_WARN("AO4_QOL_MG_CALLOUT node={:08X} track {:08X} -> {:08X} ({})", node, replaced,
              donor_track, FirstWaveName(base, donor_track));
}

// Original condition test (exec r3, &node+12 r4, mode r5, probability f1).
REX_HOOK_RAW(sub_82E5C2F8) {
  if (Ao4Qol().mg_callout_fix) {
    const uint32_t node = QolWord(base, ctx.r4.u32) - 12;
    if (node >= 0x40000000) MaybeSwapConversationTrack(base, node);
  }
  __imp__sub_82E5C2F8(ctx, base);
}

// Original random range (rng r3, min r4, max r5). Only the VO FlowMode_One pick is adjusted.
REX_HOOK_RAW(sub_82DA6198) {
  if (uint32_t(ctx.lr) != kFlowModeOnePick || ctx.r4.u32 != 0 || ctx.r5.u32 == 0 ||
      ctx.r5.u32 >= 16 || !Ao4Qol().dialog_branch_fix) {
    __imp__sub_82DA6198(ctx, base);
    return;
  }
  const uint32_t node = ctx.r31.u32, relationships = ctx.r30.u32, exec = ctx.r25.u32;
  const uint32_t count = ctx.r5.u32 + 1;
  if (!exec || QolArrayCount(base, relationships) != count || ctx.r1.u32 < 0x1000) {
    __imp__sub_82DA6198(ctx, base);
    return;
  }
  std::vector<bool> playable;
  std::string detail;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t child = QolWord(base, relationships + 4 * i);
    uint32_t type = 0;
    const bool ok = child && CanPlay(ctx, base, exec, child, type);
    playable.push_back(ok);
    detail += fmt::format(" {:08X}:{}:{}", child,
                          type == kVoConversationNodeType ? "conversation" : type == kVoContainerNodeType ? "container" : "other",
                          ok ? "playable" : "silent");
  }
  uint32_t playable_count = 0;
  for (bool ok : playable) playable_count += ok;
  const bool adjust = playable_count && playable_count < count;
  if (adjust) ctx.r5.u64 = playable_count - 1;  // Same single draw of the original RNG.
  __imp__sub_82DA6198(ctx, base);
  const uint32_t draw = ctx.r3.u32;
  const int chosen = adjust ? Ao4QolPlayableIndex(playable, draw) : int(draw);
  if (adjust && chosen >= 0) ctx.r3.u64 = uint32_t(chosen);
  // Unconditional random barks are left alone and not logged; only silent candidates matter.
  static std::atomic<uint32_t> lines{0};
  if (playable_count < count && lines.fetch_add(1, std::memory_order_relaxed) < 256)
    REXLOG_WARN("AO4_QOL_DIALOG_BRANCH node={:08X} candidates={} playable={} draw={} chosen={} adjusted={}{}",
                node, count, playable_count, draw, ctx.r3.u32, adjust, detail);
}
