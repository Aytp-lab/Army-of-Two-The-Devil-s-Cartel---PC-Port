#pragma once
#include "../../../tools/rexglue-sdk-source-0.10.0/thirdparty/inja/third_party/include/nlohmann/json.hpp"
#include <cmath>
#include <algorithm>
#include <string>

struct Ao4MouseSettings {
  double foot = .002;
  bool invert = false;
  double turret_horizontal = 1, turret_vertical = 1;
  bool turret_invert = false;
};
inline bool Ao4ValidMouseSettings(const Ao4MouseSettings& s) {
  return std::isfinite(s.foot) && s.foot >= .00001 && s.foot <= .05 &&
         std::isfinite(s.turret_horizontal) && s.turret_horizontal >= .01 && s.turret_horizontal <= 10 &&
         std::isfinite(s.turret_vertical) && s.turret_vertical >= .01 && s.turret_vertical <= 10;
}
inline bool Ao4ParseMouseSettings(const std::string& text, Ao4MouseSettings& out) {
  const auto doc = nlohmann::json::parse(text, nullptr, false);
  if (!doc.is_object() || !doc.contains("radians_per_count") ||
      !doc["radians_per_count"].is_number() || !doc.contains("invert_y") ||
      !doc["invert_y"].is_boolean()) return false;
  Ao4MouseSettings s;
  s.foot = doc["radians_per_count"].get<double>();
  s.invert = doc["invert_y"].get<bool>();
  // Legacy V4 used foot*8. Migrate once to independent factors of .016;
  // writing these explicit fields prevents later foot changes altering turret.
  s.turret_horizontal = s.turret_vertical = s.foot / .002;
  s.turret_invert = s.invert;
  for (const char* key : {"turret_horizontal", "turret_vertical"})
    if (doc.contains(key) && !doc[key].is_number()) return false;
  if (doc.contains("turret_invert_y") && !doc["turret_invert_y"].is_boolean()) return false;
  if (doc.contains("turret_horizontal")) s.turret_horizontal = doc["turret_horizontal"].get<double>();
  if (doc.contains("turret_vertical")) s.turret_vertical = doc["turret_vertical"].get<double>();
  if (doc.contains("turret_invert_y")) s.turret_invert = doc["turret_invert_y"].get<bool>();
  // Old files may have foot sensitivity above the independent turret range.
  if (!doc.contains("turret_horizontal")) s.turret_horizontal = std::clamp(s.turret_horizontal, .01, 10.0);
  if (!doc.contains("turret_vertical")) s.turret_vertical = std::clamp(s.turret_vertical, .01, 10.0);
  if (!Ao4ValidMouseSettings(s)) return false;
  out = s;
  return true;
}
inline std::string Ao4EncodeMouseSettings(const std::string& original, const Ao4MouseSettings& s) {
  if (!Ao4ValidMouseSettings(s)) return {};
  auto doc = nlohmann::json::parse(original, nullptr, false);
  if (!doc.is_object()) return {}; // Never replace an invalid user's file.
  doc["radians_per_count"] = s.foot;
  doc["invert_y"] = s.invert;
  doc["turret_horizontal"] = s.turret_horizontal;
  doc["turret_vertical"] = s.turret_vertical;
  doc["turret_invert_y"] = s.turret_invert;
  return doc.dump(2) + "\n";
}
