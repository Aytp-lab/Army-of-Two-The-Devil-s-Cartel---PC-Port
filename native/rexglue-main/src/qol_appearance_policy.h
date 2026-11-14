#pragma once
#include <cstdint>

// A hidden outfit or a mission-controlled partial body is intentional. Absence of
// an outfit overlay alone does not mean a body is missing: the spawn mesh may be complete.
inline bool Ao4QolGearMissing(bool component_ready, bool outfit_enabled,
    bool mission_body, bool outfit_attached, bool body_with_materials) {
  return component_ready && outfit_enabled && !mission_body &&
      !outfit_attached && !body_with_materials;
}
// Stable runtime metadata of the module-entry XThread, not the last thread to
// execute a role query. A ClientPlayer getter also runs regularly in missions.
inline bool Ao4QolGearMainScope(bool guest_thread, bool main_thread, uint32_t receiver_vtable) {
  return guest_thread && main_thread && receiver_vtable==0x82A3F850;
}
// The equipment check runs only in a short window after a load context: player
// spawn (mission/checkpoint/game-over restart), soldiers (re)appearing, a mission
// appearance stage, F6/equipment file or mask-phase change. Any change of the
// trigger key opens a window; outside it nothing is inspected.
struct Ao4QolGearWindow {
  static constexpr uint64_t kFast=250,kSlow=1000,kFastPhase=5000,kMinOpen=4000,kMaxOpen=30000;
  uint64_t key=0,opened=0,next=0;
  bool seen=false,open=false,done=false;
  bool Due(uint64_t now,uint64_t current_key) {
    if (!seen || current_key!=key) {
      seen=true;key=current_key;opened=now;next=0;open=true;done=false;
    }
    if (!open) return false;
    // Done: kept until kMinOpen for late original body/stage changes after spawn.
    if (now-opened>=kMaxOpen || (done && now-opened>=kMinOpen)) {open=false;return false;}
    if (now<next) return false;
    next=now+(now-opened<kFastPhase && !done ? kFast : kSlow);
    return true;
  }
  void Report(bool complete) {done=complete;}
};
inline uint64_t Ao4QolGearKey(uint32_t spawn_epoch,uint32_t mission_revision,uint32_t soldier_epoch,
                              bool wanted,bool mask_phase,uint32_t equipment_epoch) {
  return (uint64_t(spawn_epoch&0xFFFF)<<48)|(uint64_t(mission_revision&0xFFFF)<<32)|
         (uint64_t(soldier_epoch&0xFFFF)<<16)|(uint64_t(equipment_epoch&0x3FFF)<<2)|
         (wanted?2u:0u)|(mask_phase?1u:0u);
}
// Partner bundle preload: what the AI partner will wear after the next spawn. Rearmed
// whenever that target changes or a load context starts; finished once all are loaded.
struct Ao4QolPreload {
  uint64_t target=0,next=0;
  bool seen=false,finished=false;
  bool Due(uint64_t now,uint64_t current_target) {
    if (!seen || current_target!=target) {seen=true;target=current_target;finished=false;next=0;}
    if (finished || now<next) return false;
    next=now+2000; // a pending bundle request is repeated, not issued every frame
    return true;
  }
  void Finish() {finished=true;}
};

// Runtime VoiceOverDialogTrack::Clips is +24, unlike the serialized EBX +40.
// Original 82E61DA0 reads +24; actual T19 memory confirmed this layout.
template<class Word, class Name>
inline auto Ao4QolTrackFirstWave(uint32_t track, Word word, Name name) {
  using Result=decltype(name(uint32_t{}));
  if (word(track)!=0x82A1CC78) return Result{};
  const uint32_t clips=word(track+24),nc=clips?word(clips-4):0;
  if (!nc || nc>16) return Result{};
  const uint32_t clip=word(clips);
  if (word(clip)!=0x82A1CDC0) return Result{};
  const uint32_t takes=word(clip+12),nt=takes?word(takes-4):0;
  if (!nt || nt>16) return Result{};
  const uint32_t wave=word(takes); // inline VoiceOverDialogTake, Wave at +0
  if (word(wave)!=0x82A1CCC0) return Result{};
  return name(word(wave+8));
}
