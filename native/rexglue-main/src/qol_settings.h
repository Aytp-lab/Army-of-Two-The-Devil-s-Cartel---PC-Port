#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Quality-of-life switches (native/quality-of-life.ini). They fix original-game
// defects and apply in Alpha and swapped (Bravo) mode alike. Unknown keys are ignored,
// a missing file keeps the defaults.
struct Ao4QolSettings {
  // VO logic nodes with FlowMode_One pick one relationship at random before testing its
  // condition; a false pick plays nothing (e.g. Outskirts car OS_08_*). Pick only among
  // candidates whose condition can play.
  bool dialog_branch_fix = true;
  // Outskirts car, Bravo drives / Alpha on the gun: the original OS_08_02_01 has the gunner
  // Alpha warn and instruct himself. Let the driver Bravo call "Mounted machine gun!" instead
  // (his original line from the other branch).
  bool mg_callout_fix = true;
  // Recheck actual client appearance after every spawn/reload, even when the
  // customization object's cached selection has not changed.
  bool gear_reload_fix = true;
  // Start directly with AO4.Main: Default.xex loads AO4.Splash (logos, missing intro.wmv,
  // three audio device switches) before Main; skipping it removes that crash-prone phase.
  // Read by the launcher (splash_skip_hook.cpp); off unless the file enables it.
  bool skip_splash = false;
  // Real 60 FPS (game logic at 60 ticks/s with the 30 Hz assumptions corrected, see
  // analysis/FPS_60_ANALYSE_20261102.md). Read by fps_experiment.cpp at module load; off by default.
  bool fps60 = false;
};
const Ao4QolSettings& Ao4Qol();

// Conversation track replacement: the conversation whose first track starts with target_wave
// plays the donor conversation's track that starts with donor_wave instead (lower-case
// substrings of SoundWaveAsset names).
struct Ao4QolTrackSwap { std::string_view target_wave, donor_wave; };
inline constexpr Ao4QolTrackSwap kAo4QolMgCallout{"vo_os_08_02_01_alpha_heads_up", "vo_os_08_02_bravo_mounted_machine_gun"};
inline bool Ao4QolWaveIs(std::string_view name, std::string_view wanted) {
  std::string lower(name);
  for (char& c : lower) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return !wanted.empty() && lower.find(wanted) != lower.npos;
}

inline std::string Ao4QolTrim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
  std::string out(s.substr(a, b - a));
  for (char& c : out) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}
// Returns -1 for an unrecognised value so a typo never silently flips a switch.
inline int Ao4QolBool(std::string_view value) {
  const std::string v = Ao4QolTrim(value);
  if (v == "1" || v == "an" || v == "ein" || v == "on" || v == "true" || v == "ja" || v == "yes") return 1;
  if (v == "0" || v == "aus" || v == "off" || v == "false" || v == "nein" || v == "no") return 0;
  return -1;
}
// INI text: "Schluessel = Wert", comments start with ';' or '#', [Sections] ignored.
inline Ao4QolSettings Ao4ParseQolSettings(std::string_view text, std::vector<std::string>* problems = nullptr) {
  Ao4QolSettings s;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == text.npos) end = text.size();
    std::string_view line = text.substr(start, end - start);
    start = end + 1;
    if (const size_t c = line.find_first_of(";#"); c != line.npos) line = line.substr(0, c);
    const std::string trimmed = Ao4QolTrim(line);
    if (trimmed.empty() || trimmed.front() == '[') continue;
    const size_t eq = line.find('=');
    if (eq == line.npos) {
      if (problems) problems->push_back("ohne '=': " + trimmed);
      continue;
    }
    const std::string key = Ao4QolTrim(line.substr(0, eq));
    const int value = Ao4QolBool(line.substr(eq + 1));
    bool* target = key == "dialogzweig_fix" ? &s.dialog_branch_fix
                 : key == "mg_ansage_fix" ? &s.mg_callout_fix
                 : key == "ausruestung_ladefix" ? &s.gear_reload_fix
                 : key == "splash_ueberspringen" ? &s.skip_splash
                 : key == "bildrate_60" ? &s.fps60 : nullptr;
    if (target) {
      if (value < 0) { if (problems) problems->push_back("ungueltiger Wert: " + trimmed); }
      else *target = value == 1;
    } else if (problems) {
      problems->push_back("unbekannter Schluessel: " + key);
    }
    if (end == text.size()) break;
  }
  return s;
}

// Map a random draw over the playable candidates back to the original relationship index.
// Returns -1 when the original pick should stand (all or none playable).
inline int Ao4QolPlayableIndex(const std::vector<bool>& playable, uint32_t draw) {
  std::vector<int> ok;
  for (size_t i = 0; i < playable.size(); ++i) if (playable[i]) ok.push_back(int(i));
  if (ok.empty() || ok.size() == playable.size() || draw >= ok.size()) return -1;
  return ok[draw];
}
