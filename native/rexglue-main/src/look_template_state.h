#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

// Names are the original assets, including the intentionally different RR paths.
struct Ao4LookVariant {
  const char *player_template, *partner_template, *player_customize, *partner_customize;
};
inline constexpr Ao4LookVariant kAo4LookVariants[] = {
  {"Gameplay/Spawners/AO2SoldierSpawnTemplate", "OldObjects/Soldiers/Templates/SpawnTemplate_Partner",
   "Gameplay/Spawners/AO2PlayerCustomizeData", "Gameplay/Spawners/PartnerCustomizeAIData"},
  {"Gameplay/Spawners/AO2SoldierSpawnTemplate_RR", "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RR",
   "AO4JawBreaker/Gameplay/Spawners/AO2DefaultPlayerCustomizeData_RR", "Gameplay/Spawners/PartnerCustomizeAIData_RR"},
  {"Gameplay/Spawners/AO2SoldierSpawnTemplate_RRChopper", "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_RRChopper",
   "Gameplay/Spawners/AO2DefaultPlayerCustomizeData_RRChopper", "Gameplay/Spawners/PartnerCustomizeAIData_RRChopper"},
  {"Gameplay/Spawners/AO2SoldierSpawnTemplate_Tutorial", "OldObjects/Soldiers/Templates/SpawnTemplate_Partner_Tutorial",
   "Gameplay/Spawners/AO2PlayerCustomizeData_Tutorial", "Gameplay/Spawners/PAI_CustomizeAIData_Tutorial"}
};
template<class Read>
inline bool Ao4LookWord(uint32_t address, uint32_t& value, Read read) {
  std::array<uint8_t,4> bytes{};
  if (!read(address, bytes.data(), bytes.size())) return false;
  value=(uint32_t(bytes[0])<<24)|(uint32_t(bytes[1])<<16)|(uint32_t(bytes[2])<<8)|bytes[3];
  return true;
}
// A readable address alone is insufficient: level allocations are reused. Check both
// object names AND the current template->customize link before rollback or traversal.
template<class Read, class Name>
inline bool Ao4LookSideCurrent(const Ao4LookVariant& variant, bool partner,
    uint32_t tpl, uint32_t customize, Read read, Name name) {
  if (!tpl || !customize || tpl>0xFFFFFFD3u || customize>0xFFFFFF77u) return false;
  uint32_t linked=0;
  std::array<uint8_t,0x88> bytes{};
  return name(tpl)==std::string_view(partner?variant.partner_template:variant.player_template) &&
      Ao4LookWord(tpl+12,linked,read) && linked==customize &&
      read(customize,bytes.data(),bytes.size()) &&
      name(customize)==std::string_view(partner?variant.partner_customize:variant.player_customize);
}
template<class Read>
inline bool Ao4LookArray(uint32_t address, uint32_t limit,
                        std::vector<uint32_t>& values, Read read) {
  values.clear();
  uint32_t count=0;
  if (address<4 || !limit || limit>16 || !Ao4LookWord(address-4,count,read) ||
      !count || count>limit || uint64_t(address)+count*4>0x100000000ull) return false;
  std::array<uint8_t,64> bytes{};
  if (!read(address,bytes.data(),count*4)) return false;
  for (uint32_t i=0;i<count;++i)
    values.push_back((uint32_t(bytes[i*4])<<24)|(uint32_t(bytes[i*4+1])<<16)|
        (uint32_t(bytes[i*4+2])<<8)|bytes[i*4+3]);
  return true;
}
