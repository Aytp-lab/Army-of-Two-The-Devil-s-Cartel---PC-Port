// Settings model of the public test build. All files live next to AO4Launcher.exe:
//   bin\       ao4_launcher.exe + runtime/main DLLs (no game data)
//   config\    launcher.ini, quality-of-life.ini, native-mouse.json, input-bindings.json, profiles.ini
//   game\disc  game\hd   created by the first-run setup from the player's own ISO
//   profiles\  user data root (one folder per profile XUID: save games)
//   logs\      game and server logs
//   server\    embedded Python + ao4-blaze-server.py (online/LAN host)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ao4cfg {

struct Paths {
  std::wstring root, bin, config, game_disc, game_hd, profiles, logs, server;
  static Paths FromExe();
};

enum class Mode { Single = 0, Split = 1, OnlineHost = 2, OnlineJoin = 3 };

struct LauncherSettings {
  std::string language = "en";  // en | de (release default English)
  std::wstring iso_path;
  int mode = 0;
  // Graphics (runtime options)
  bool fullscreen = true;
  int window_width = 1280, window_height = 720;
  int resolution_scale = 1;           // 1..3
  std::string present_effect = "bilinear";  // bilinear | cas | fsr
  int anisotropic = 3;                // 0 off, 1..5 = 1x..16x (runtime anisotropic_override)
  bool vsync = true;
  // GPU resolve readback: "balanced" syncs only the small CPU-read results (luminance), the
  // rest one frame delayed; "exact" syncs every small resolve (slower, split screen -20-30%).
  std::string readback = "balanced";
  bool fps60 = false;                 // quality-of-life.ini bildrate_60 + 120 Hz vblank
  // Audio
  std::string audio_backend = "wasapi";  // wasapi | directsound
  bool mute = false;
  // Game options
  bool unlock_missions = false;    // all story missions/checkpoints (AO4_UNLOCK_MISSIONS)
  bool unlock_extras = true;       // DLC/demo/pre-order gear on the disc (AO4_UNLOCK_LICENSES)
  bool cosmetics_buyable = false;  // masks/outfits/tattoos buyable regardless of rank/story
  bool bug_capture = false;       // ao4_bugcam next to the game (F12 screenshot + last seconds)
  // Profiles
  std::string slot1 = "", slot2 = "";   // profile ids
  // Split screen devices: keyboard/mouse player (0/1), pads by connection order -> player
  // (-1 unused, 0 player 1, 1 player 2); empty pad list = keyboard player 1, pad 1 player 2.
  int mnk_player = 0;
  std::string pad_players;  // e.g. "1,1,-1,-1"
  // Online
  std::string host_ip, join_ip;

  bool Load(const std::wstring& file);
  bool Save(const std::wstring& file) const;
};

// quality-of-life.ini: key = an/aus lines; Set() rewrites the value in place and keeps comments.
struct QolFile {
  std::vector<std::string> lines;
  bool Load(const std::wstring& file);
  bool Save(const std::wstring& file) const;
  bool Get(const std::string& key, bool fallback) const;
  void Set(const std::string& key, bool value);
};

struct MouseSettings {
  double radians_per_count = 0.0012418426900000002;
  bool invert_y = false;
  // Mounted guns / turrets (own scale, read by the main DLL)
  double turret_horizontal = 1.625548755750484, turret_vertical = 2.187761623949553;
  bool turret_invert_y = false;
  bool Load(const std::wstring& file);
  bool Save(const std::wstring& file) const;
};

struct Profile {
  std::string id;       // stable id (folder key)
  std::string name;     // gamertag, max 15 characters
  std::string xuid;     // 16 hex digits, permission bits 0x00C0000000000000 clear
};

struct ProfileStore {
  std::vector<Profile> profiles;
  bool Load(const std::wstring& file);
  bool Save(const std::wstring& file) const;
  const Profile* Find(const std::string& id) const;
  Profile& Create(const std::string& name);
  bool Remove(const std::string& id);
};

// partner-equipment.ini ([equipment] mask/tattoo/outfit asset ids) and partner-active.ini
// ([state] active = custom gear on, bravo = Bravo mode).
struct PartnerGear {
  uint32_t mask = 0, outfit = 0, tattoo = 0;
  bool Load(const std::wstring& file);
  bool Save(const std::wstring& file) const;
};
struct PartnerState {
  bool active = false, bravo = false;
  bool Load(const std::wstring& file);
  bool Save(const std::wstring& file) const;
};

// Copies a save (<root>/<XUID>/454109AB/00000001/PROFILE_OPTIONS/PROFILE_OPTIONS plus its header
// <root>/<XUID>/454109AB/Headers/00000001/PROFILE_OPTIONS.header) into profiles/<xuid>/.
// An existing save is kept as PROFILE_OPTIONS.bak.
bool ImportSave(const std::wstring& profile_options_file, const std::wstring& profiles_root,
                const std::string& xuid, std::wstring& error);

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

}  // namespace ao4cfg
