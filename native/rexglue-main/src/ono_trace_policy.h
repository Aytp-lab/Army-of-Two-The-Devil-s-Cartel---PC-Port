#pragma once
#include <string>
#include <string_view>

// Diagnostic categories only; these never choose or replace a sound.
inline int Ao4OnoEventKind(std::string_view name) {
  if (name=="General_Hit" || name=="Friendly_Hit" || name=="General_ONO_Hit_Hard") return 1;
  if (name=="General_Need_Help_Wounded") return 3;
  if (name.rfind("Friendly_ONO_",0)!=0 && name.rfind("General_ONO_",0)!=0) return -1;
  if (name.find("Melee")!=name.npos) return 2;
  if (name.find("Wounded")!=name.npos) return 3;
  if (name.find("Hit")!=name.npos) return 1;
  return 0;
}
inline std::string Ao4OnoLower(std::string_view path) {
  std::string lower(path);
  for (char& c:lower) if (c>='A' && c<='Z') c=char(c-'A'+'a');
  return lower;
}
inline int Ao4OnoWaveKind(std::string_view path) {
  const auto s=Ao4OnoLower(path);
  if (s.find("/sound/vo/")==s.npos ||
      (s.find("_ono_")==s.npos && s.find("/ono/")==s.npos)) return -1;
  if (s.find("melee")!=s.npos) return 2;
  if (s.find("wounded")!=s.npos) return 3;
  if (s.find("pain")!=s.npos || s.find("hit")!=s.npos) return 1;
  return 0;
}
inline std::string_view Ao4OnoWaveVoice(std::string_view path) {
  const auto s=Ao4OnoLower(path);
  if (s.find("/alpha/")!=s.npos) return "Alpha";
  if (s.find("/bravo/")!=s.npos) return "Bravo";
  return "unknown";
}
