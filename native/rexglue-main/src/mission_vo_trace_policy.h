#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "ono_trace_policy.h"

// Diagnostic classification of scripted mission dialogue; never chooses or replaces a sound.
// Mission events are named <2-5 char level tag>_<digit or '-'>..., e.g. OS_08_02, Os_04,
// DL-D_04_05_01, Dl_-d_06_01_03_ALPHA. Systemic General_/Friendly_/Enemy_ events are excluded.
inline bool Ao4MissionVoEvent(std::string_view name) {
  const size_t split=name.find('_');
  if (split<2 || split>5 || split+1>=name.size()) return false;
  const char next=name[split+1];
  return (next>='0' && next<='9') || next=='-';
}
// Mission waves live under sound/vo/<level>/<speaker>/; contextual/systemic and ONO excluded.
inline bool Ao4MissionVoWave(std::string_view path) {
  const auto s=Ao4OnoLower(path);
  const size_t vo=s.find("/sound/vo/");
  if (vo==s.npos || Ao4OnoWaveKind(path)>=0) return false;
  const std::string_view rest=std::string_view(s).substr(vo+10);
  return rest.rfind("contextual/",0)!=0 && rest.rfind("systemic/",0)!=0 &&
         rest.rfind("logic/",0)!=0 && rest.find('/')!=rest.npos;
}
// Speaker folder after sound/vo/<level>/ in lower case ("alpha", "bravo", "fiona", ...).
inline std::string Ao4MissionVoSpeaker(std::string_view path) {
  if (!Ao4MissionVoWave(path)) return {};
  const auto s=Ao4OnoLower(path);
  const size_t level=s.find('/',s.find("/sound/vo/")+10);
  const size_t end=s.find('/',level+1);
  return end==s.npos ? std::string() : s.substr(level+1,end-level-1);
}
inline std::string_view Ao4VoKeyName(uint32_t hash) {
  if (hash==3337816366u) return "Sender";
  if (hash==205558026u) return "FIONA";
  if (hash==1736235134u) return "SP_AlphaPlayer";
  if (hash==3835145314u) return "SP_BravoPlayer";
  return {};
}
