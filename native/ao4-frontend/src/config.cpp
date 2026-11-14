#include "config.h"

#include "json_lite.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ao4cfg {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

namespace {

bool ReadText(const std::wstring& file, std::string& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}
bool WriteText(const std::wstring& file, const std::string& text) {
  // Write to a temporary file first so a crash never leaves a half-written config.
  const std::wstring tmp = file + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out) return false;
  }
  return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}
std::string Lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return s; }
bool ParseBool(const std::string& v, bool fallback) {
  const std::string x = Lower(Trim(v));
  if (x == "1" || x == "an" || x == "ein" || x == "on" || x == "true" || x == "ja" || x == "yes") return true;
  if (x == "0" || x == "aus" || x == "off" || x == "false" || x == "nein" || x == "no") return false;
  return fallback;
}

}  // namespace

Paths Paths::FromExe() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring root(exe);
  root = root.substr(0, root.find_last_of(L"\\/"));
  Paths p;
  p.root = root;
  p.bin = root + L"\\bin";
  p.config = root + L"\\config";
  p.game_disc = root + L"\\game\\disc";
  p.game_hd = root + L"\\game\\hd";
  p.profiles = root + L"\\profiles";
  p.logs = root + L"\\logs";
  p.server = root + L"\\server";
  return p;
}

// ---- launcher.ini (key = value) ------------------------------------------------------
bool LauncherSettings::Load(const std::wstring& file) {
  std::string text;
  if (!ReadText(file, text)) return false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const size_t c = line.find_first_of(";#");
    if (c != std::string::npos) line = line.substr(0, c);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = Lower(Trim(line.substr(0, eq))), v = Trim(line.substr(eq + 1));
    if (k == "language") language = (Lower(v) == "de") ? "de" : "en";
    else if (k == "iso") iso_path = Widen(v);
    else if (k == "mode") mode = std::clamp(std::atoi(v.c_str()), 0, 3);
    else if (k == "fullscreen") fullscreen = ParseBool(v, fullscreen);
    else if (k == "window_width") window_width = std::clamp(std::atoi(v.c_str()), 640, 7680);
    else if (k == "window_height") window_height = std::clamp(std::atoi(v.c_str()), 360, 4320);
    else if (k == "resolution_scale") resolution_scale = std::clamp(std::atoi(v.c_str()), 1, 3);
    else if (k == "present_effect") present_effect = (v == "cas" || v == "fsr") ? v : "bilinear";
    else if (k == "anisotropic") anisotropic = std::clamp(std::atoi(v.c_str()), 0, 5);
    else if (k == "vsync") vsync = ParseBool(v, vsync);
    else if (k == "readback") readback = (v == "exact") ? "exact" : "balanced";
    else if (k == "fps60") fps60 = ParseBool(v, fps60);
    else if (k == "audio_backend") audio_backend = (v == "directsound") ? "directsound" : "wasapi";
    else if (k == "mute") mute = ParseBool(v, mute);
    else if (k == "unlock_missions") unlock_missions = ParseBool(v, unlock_missions);
    else if (k == "unlock_extras") unlock_extras = ParseBool(v, unlock_extras);
    else if (k == "cosmetics_buyable") cosmetics_buyable = ParseBool(v, cosmetics_buyable);
    else if (k == "bug_capture") bug_capture = ParseBool(v, bug_capture);
    else if (k == "mnk_player") mnk_player = std::clamp(std::atoi(v.c_str()), 0, 1);
    else if (k == "pad_players") pad_players = v;
    else if (k == "slot1") slot1 = v;
    else if (k == "slot2") slot2 = v;
    else if (k == "host_ip") host_ip = v;
    else if (k == "join_ip") join_ip = v;
  }
  return true;
}

bool LauncherSettings::Save(const std::wstring& file) const {
  std::ostringstream o;
  o << "; AO4 native port launcher settings (written by AO4Launcher.exe)\n";
  o << "language = " << language << "\n";
  o << "iso = " << Narrow(iso_path) << "\n";
  o << "mode = " << mode << "\n";
  o << "fullscreen = " << (fullscreen ? "an" : "aus") << "\n";
  o << "window_width = " << window_width << "\n";
  o << "window_height = " << window_height << "\n";
  o << "resolution_scale = " << resolution_scale << "\n";
  o << "present_effect = " << present_effect << "\n";
  o << "anisotropic = " << anisotropic << "\n";
  o << "vsync = " << (vsync ? "an" : "aus") << "\n";
  o << "readback = " << readback << "\n";
  o << "fps60 = " << (fps60 ? "an" : "aus") << "\n";
  o << "audio_backend = " << audio_backend << "\n";
  o << "mute = " << (mute ? "an" : "aus") << "\n";
  o << "unlock_missions = " << (unlock_missions ? "an" : "aus") << "\n";
  o << "unlock_extras = " << (unlock_extras ? "an" : "aus") << "\n";
  o << "cosmetics_buyable = " << (cosmetics_buyable ? "an" : "aus") << "\n";
  o << "bug_capture = " << (bug_capture ? "an" : "aus") << "\n";
  o << "mnk_player = " << mnk_player << "\n";
  o << "pad_players = " << pad_players << "\n";
  o << "slot1 = " << slot1 << "\n";
  o << "slot2 = " << slot2 << "\n";
  o << "host_ip = " << host_ip << "\n";
  o << "join_ip = " << join_ip << "\n";
  return WriteText(file, o.str());
}

// ---- quality-of-life.ini -----------------------------------------------------------
bool QolFile::Load(const std::wstring& file) {
  std::string text;
  if (!ReadText(file, text)) return false;
  lines.clear();
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return true;
}
bool QolFile::Save(const std::wstring& file) const {
  std::string text;
  for (const auto& l : lines) text += l + "\r\n";
  return WriteText(file, text);
}
bool QolFile::Get(const std::string& key, bool fallback) const {
  for (const auto& l : lines) {
    const std::string t = Trim(l);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') continue;
    const size_t eq = t.find('=');
    if (eq != std::string::npos && Lower(Trim(t.substr(0, eq))) == key) return ParseBool(t.substr(eq + 1), fallback);
  }
  return fallback;
}
void QolFile::Set(const std::string& key, bool value) {
  for (auto& l : lines) {
    const std::string t = Trim(l);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') continue;
    const size_t eq = t.find('=');
    if (eq != std::string::npos && Lower(Trim(t.substr(0, eq))) == key) {
      l = key + " = " + (value ? "an" : "aus");
      return;
    }
  }
  lines.push_back(key + " = " + (value ? "an" : "aus"));
}

// ---- native-mouse.json -------------------------------------------------------------
bool MouseSettings::Load(const std::wstring& file) {
  std::string text;
  ao4json::Value v;
  if (!ReadText(file, text) || !ao4json::Parse(text, v)) return false;
  if (const auto* r = v.Get("radians_per_count"); r && r->type == ao4json::Value::Type::Number) radians_per_count = r->n;
  if (const auto* i = v.Get("invert_y"); i && i->type == ao4json::Value::Type::Bool) invert_y = i->b;
  if (const auto* t = v.Get("turret_horizontal"); t && t->type == ao4json::Value::Type::Number) turret_horizontal = t->n;
  if (const auto* t = v.Get("turret_vertical"); t && t->type == ao4json::Value::Type::Number) turret_vertical = t->n;
  if (const auto* t = v.Get("turret_invert_y"); t && t->type == ao4json::Value::Type::Bool) turret_invert_y = t->b;
  return true;
}
bool MouseSettings::Save(const std::wstring& file) const {
  std::string text;
  ao4json::Value v = ao4json::Value::Obj();
  if (ReadText(file, text)) ao4json::Parse(text, v);  // keep turret fields and others
  if (v.type != ao4json::Value::Type::Object) v = ao4json::Value::Obj();
  v.Set("invert_y", ao4json::Value::Bool(invert_y));
  v.Set("radians_per_count", ao4json::Value::Num(std::clamp(radians_per_count, 0.00001, 0.05)));
  v.Set("turret_horizontal", ao4json::Value::Num(std::clamp(turret_horizontal, 0.01, 10.0)));
  v.Set("turret_invert_y", ao4json::Value::Bool(turret_invert_y));
  v.Set("turret_vertical", ao4json::Value::Num(std::clamp(turret_vertical, 0.01, 10.0)));
  return WriteText(file, ao4json::Serialize(v));
}

// ---- profiles.ini: one line per profile "id|name|xuid" -----------------------------
bool ProfileStore::Load(const std::wstring& file) {
  std::string text;
  if (!ReadText(file, text)) return false;
  profiles.clear();
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == ';') continue;
    const size_t a = line.find('|'), b = line.find('|', a + 1);
    if (a == std::string::npos || b == std::string::npos) continue;
    profiles.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
  }
  return true;
}
bool ProfileStore::Save(const std::wstring& file) const {
  std::string text = "; AO4 native port profiles: id|name|xuid\n";
  for (const auto& p : profiles) text += p.id + "|" + p.name + "|" + p.xuid + "\n";
  return WriteText(file, text);
}
const Profile* ProfileStore::Find(const std::string& id) const {
  for (const auto& p : profiles) if (p.id == id) return &p;
  return nullptr;
}
Profile& ProfileStore::Create(const std::string& name) {
  // Random XUID with the console's local-profile prefix; bits 0x00C0000000000000 stay clear
  // (the game refuses profiles with them set).
  uint64_t r = 0;
  BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&r), sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  uint64_t xuid = 0xB130000000000000ull | (r & 0x000FFFFFFFFFFFFEull);  // even: slot 2 may use xuid+1
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(xuid));
  Profile p;
  p.id = std::string("p") + std::string(hex).substr(8);
  std::string clean;
  for (char c : name) if (c >= 0x20 && c != '|' && c != ';') clean += c;
  p.name = clean.substr(0, 15);
  if (p.name.empty()) p.name = "Player";
  p.xuid = hex;
  profiles.push_back(p);
  return profiles.back();
}
bool ProfileStore::Remove(const std::string& id) {
  const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.id == id; });
  if (it == profiles.end()) return false;
  profiles.erase(it);
  return true;
}

// ---- partner gear / state (Windows ini, same format as the Partner-Customizer) ------------
bool PartnerGear::Load(const std::wstring& file) {
  if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  wchar_t v[32] = {};
  GetPrivateProfileStringW(L"equipment", L"mask", L"0", v, 32, file.c_str());
  mask = uint32_t(_wcstoui64(v, nullptr, 10));
  GetPrivateProfileStringW(L"equipment", L"outfit", L"0", v, 32, file.c_str());
  outfit = uint32_t(_wcstoui64(v, nullptr, 10));
  GetPrivateProfileStringW(L"equipment", L"tattoo", L"0", v, 32, file.c_str());
  tattoo = uint32_t(_wcstoui64(v, nullptr, 10));
  return true;
}
bool PartnerGear::Save(const std::wstring& file) const {
  std::ostringstream o;
  o << "[equipment]\r\nmask=" << mask << "\r\ntattoo=" << tattoo << "\r\noutfit=" << outfit << "\r\n";
  return WriteText(file, o.str());
}
bool PartnerState::Load(const std::wstring& file) {
  if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  active = GetPrivateProfileIntW(L"state", L"active", 0, file.c_str()) != 0;
  bravo = GetPrivateProfileIntW(L"state", L"bravo", 0, file.c_str()) != 0;
  return true;
}
bool PartnerState::Save(const std::wstring& file) const {
  return WriteText(file, std::string("[state]\r\nactive=") + (active ? "1" : "0") + "\r\nbravo=" +
                             (bravo ? "1" : "0") + "\r\n");
}

bool ImportSave(const std::wstring& file, const std::wstring& profiles_root, const std::string& xuid,
                std::wstring& error) {
  auto parent = [](const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); };
  const std::wstring title = parent(parent(parent(file)));  // ...\454109AB
  const std::wstring header = title + L"\\Headers\\00000001\\PROFILE_OPTIONS.header";
  if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
    error = L"PROFILE_OPTIONS not found";
    return false;
  }
  const std::wstring user = profiles_root + L"\\" + Widen(xuid);
  const std::wstring dst_title = user + L"\\454109AB";
  const std::wstring dst_dir = dst_title + L"\\00000001\\PROFILE_OPTIONS";
  const std::wstring dst_header_dir = dst_title + L"\\Headers\\00000001";
  for (const std::wstring& d : {profiles_root, user, dst_title, dst_title + L"\\00000001", dst_dir,
                                dst_title + L"\\Headers", dst_header_dir})
    CreateDirectoryW(d.c_str(), nullptr);
  const std::wstring dst = dst_dir + L"\\PROFILE_OPTIONS";
  if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileW(dst.c_str(), (dst + L".bak").c_str(), FALSE);
  if (!CopyFileW(file.c_str(), dst.c_str(), FALSE)) {
    error = L"copy failed";
    return false;
  }
  if (GetFileAttributesW(header.c_str()) != INVALID_FILE_ATTRIBUTES)
    CopyFileW(header.c_str(), (dst_header_dir + L"\\PROFILE_OPTIONS.header").c_str(), FALSE);
  return true;
}

}  // namespace ao4cfg
