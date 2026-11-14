#pragma once
#include <string_view>
#include <cstdint>
#include <vector>
#include <array>
#include <cstring>

struct Ao4VoParameter { uint32_t key, hash, type; };
inline bool Ao4CoopLifecycleEvent(std::string_view name) {
  return name=="Os_04" || name=="OS_04_01" || name=="OS_04_00" ||
      name=="OS_04_01_00" || name=="OS_04_00_01" || name=="OS_04_01_00_01" ||
      name=="OS_04_02" || name=="OS_04_03" ||
      // Outskirts car: driver choice and convoy lines (seat-dependent Sender branches).
      name=="OS_08_00" || name=="OS_08_01_01" || name=="OS_08_01_02" || name=="OS_08_02" ||
      name=="OS_08_02_01" || name=="OS_08_02_02" || name=="OS_08_02_03" || name=="OS_08_02_04" ||
      name=="OS_08_02_05" || name=="OS_08_02_06" || name=="OS_08_02_07" || name=="OS_08_03" ||
      name=="OS_08_04";
}
inline uint32_t Ao4VoParameterAlias(uint32_t hash, uint32_t type,
                                  const std::vector<Ao4VoParameter>& parameters) {
  uint32_t match=0;
  for (const auto& parameter:parameters) {
    if (parameter.hash!=hash || parameter.type!=type) continue;
    if (match && match!=parameter.key) return 0;
    match=parameter.key;
  }
  return match;
}

// Original Outskirts branches: Alpha boosts Bravo / Bravo boosts Alpha, followed by
// the matching departure conversation and later truck/Fiona follow-ups. Exact case is
// significant: the first chain uses Os_04/Os_05, not OS_04/OS_05.
// Pairing is proven by parent SP event wiring, not inferred from similar names.
inline std::string_view Ao4CoopRouteEvent(std::string_view name) {
  if (name == "OS_03_03") return "OS_03_02";
  if (name == "OS_03_02") return "OS_03_03";
  if (name == "OS_03_05") return "OS_03_04";
  if (name == "OS_03_04") return "OS_03_05";
  if (name == "Os_04") return "OS_04_01";
  if (name == "OS_04_01") return "Os_04";
  if (name == "Os_05") return "OS_05_00";
  if (name == "OS_05_00") return "Os_05";
  if (name == "OS_04_00") return "OS_04_01_00";
  if (name == "OS_04_01_00") return "OS_04_00";
  if (name == "OS_04_00_01") return "OS_04_01_00_01";
  if (name == "OS_04_01_00_01") return "OS_04_00_01";
  // Crane shot: counters 389/390 behind the same branch splitters 418/417
  // (Bravo shot, Alpha goes down / Alpha shot, Bravo goes down).
  if (name == "OS_04_02") return "OS_04_03";
  if (name == "OS_04_03") return "OS_04_02";
  // Ghosttown sound-layer classifiers 357/358 select these opposite actor variants.
  if (name == "GT_12_05") return "GT_12_06";
  if (name == "GT_12_06") return "GT_12_05";
  // Race triggers bound to AO2PlayerEntities Alpha vs Bravo (whoever arrives/falls
  // first) select mirrored variants; server Alpha is the human in Bravo mode.
  // Proven by audit-vo-role-selectors.py, same VOLogic file, no parameters.
  if (name == "BR_04_03_01") return "BR_04_03_02";
  if (name == "BR_04_03_02") return "BR_04_03_01";
  if (name == "DL-D_04_05_01") return "DL-D_04_05_02";
  if (name == "DL-D_04_05_02") return "DL-D_04_05_01";
  if (name == "Dl-D_04_07_01_01") return "Dl-D_04_07_02";
  if (name == "Dl-D_04_07_02") return "Dl-D_04_07_01_01";
  if (name == "Dl-D_04_08_01") return "Dl-D_04_08_02";
  if (name == "Dl-D_04_08_02") return "Dl-D_04_08_01";
  if (name == "Dl-D_04_09_01") return "Dl-D_04_09_02";
  if (name == "Dl-D_04_09_02") return "Dl-D_04_09_01";
  if (name == "GT_06_00_01") return "GT_06_00_02";
  if (name == "GT_06_00_02") return "GT_06_00_01";
  if (name == "OS_06_13") return "OS_06_14";
  if (name == "OS_06_14") return "OS_06_13";
  // MG exit follow-up: gates 244/245 are set to opposite states by checker 246,
  // then the SAME delay 243 triggers both. The enabled gate selects 15 vs 16.
  if (name == "OS_06_15") return "OS_06_16";
  if (name == "OS_06_16") return "OS_06_15";
  // DrugLab chopper crash: local-player filters behind Alpha/Bravo triggers.
  // The adjacent *_In_Chopper events also test IsAI and stay unchanged.
  if (name == "Dl_-d_06_01_03_ALPHA") return "Dl_-d_06_01_04_BRAVO";
  if (name == "Dl_-d_06_01_04_BRAVO") return "Dl_-d_06_01_03_ALPHA";
  return {};
}

// Inspect event objects first. Their names can be interned in a different allocation.
// The reader must safely validate the live object and follow its name pointer; it
// must not require that the name bytes occur in this snapshot.
template<class Read>
inline bool Ao4VoEventHasName(uint32_t event, uint32_t name, std::string_view target, Read read) {
  if (target.empty() || target.size()>=64) return false;
  std::array<uint8_t,12> live{};
  if (!read(event,live.data(),live.size())) return false;
  auto word=[&](size_t i) { return (uint32_t(live[i])<<24)|(uint32_t(live[i+1])<<16)|
      (uint32_t(live[i+2])<<8)|uint32_t(live[i+3]); };
  if (word(0)!=0x82A1CAB4 || word(8)!=name) return false;
  std::array<char,64> text{};
  if (!read(name,text.data(),target.size()+1)) return false;
  return text[target.size()]=='\0' && std::memcmp(text.data(),target.data(),target.size())==0;
}

template<class MatchesName>
inline std::vector<uint32_t> Ao4VoNamedEventsInSnapshot(const uint8_t* bytes, size_t length,
    uint32_t start, MatchesName matches_name) {
  std::vector<uint32_t> found;
  auto word = [&](size_t i) {
    return (uint32_t(bytes[i]) << 24) | (uint32_t(bytes[i+1]) << 16) |
           (uint32_t(bytes[i+2]) << 8) | uint32_t(bytes[i+3]);
  };
  const size_t first = (4 - (start & 3)) & 3;
  for (size_t i=first;i+24<=length;i+=4)
    if (word(i)==0x82A1CAB4 && matches_name(start+uint32_t(i),word(i+8)))
      found.push_back(start+uint32_t(i));
  return found;
}

// Compatibility helper for callers that already know the exact name pointer.
inline std::vector<uint32_t> Ao4VoEventsInSnapshot(const uint8_t* bytes, size_t length,
                                                uint32_t start, uint32_t name_pointer) {
  return Ao4VoNamedEventsInSnapshot(bytes,length,start,
      [&](uint32_t,uint32_t pointer){return pointer==name_pointer;});
}
