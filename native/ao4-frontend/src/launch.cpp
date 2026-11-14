#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "launch.h"

#include "json_lite.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace ao4launch {
namespace {

using ao4cfg::Mode;
using ao4cfg::Narrow;
using ao4cfg::Widen;

std::wstring Stamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t buf[64];
  swprintf(buf, 64, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return buf;
}

std::string ReadAll(const std::wstring& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Options from config\input-bindings.json (same rules as native/Read-InputBindings.ps1).
void InputBindingArgs(const std::wstring& file, std::vector<std::wstring>& args) {
  ao4json::Value v;
  if (!ao4json::Parse(ReadAll(file), v) || v.type != ao4json::Value::Type::Object) return;
  // Console language (text and speech) on the disc: de, en, it.
  static const std::map<std::string, std::pair<int, int>> kLang = {{"de", {3, 24}}, {"en", {1, 103}}, {"it", {6, 50}}};
  std::string lang = "en";
  if (const auto* l = v.Get("language"); l && l->type == ao4json::Value::Type::String && kLang.count(l->s)) lang = l->s;
  args.push_back(L"--user_language=" + std::to_wstring(kLang.at(lang).first));
  args.push_back(L"--user_country=" + std::to_wstring(kLang.at(lang).second));
  if (const auto* s = v.Get("mnk_sensitivity"); s && s->type == ao4json::Value::Type::Number && s->n > 0) {
    wchar_t buf[64];
    swprintf(buf, 64, L"--mnk_sensitivity=%g", s->n);
    args.push_back(buf);
  }
  static const char* kAllowed[] = {"a", "b", "x", "y", "left_trigger", "right_trigger", "left_shoulder", "right_shoulder",
                                   "lstick_up", "lstick_down", "lstick_left", "lstick_right", "lstick_press", "rstick_up",
                                   "rstick_down", "rstick_left", "rstick_right", "rstick_press", "dpad_up", "dpad_down",
                                   "dpad_left", "dpad_right", "back", "start", "guide"};
  if (const auto* kb = v.Get("keybinds"); kb && kb->type == ao4json::Value::Type::Object) {
    for (const auto& [name, val] : kb->obj) {
      if (val.type != ao4json::Value::Type::String) continue;
      bool ok = false;
      for (const char* a : kAllowed) if (name == a) ok = true;
      if (!ok) continue;
      // An empty value is rejected by the option parser; "," = two empty alternatives (unbound).
      args.push_back(L"--keybind_" + Widen(name) + L"=" + Widen(val.s.empty() ? std::string(",") : val.s));
    }
  }
  // Port menu key (runtime key bind "bind_port_menu"); Bravo/partner keys are read by the main DLL.
  if (const auto* hk = v.Get("hotkeys"); hk && hk->type == ao4json::Value::Type::Object) {
    if (const auto* m = hk->Get("port_menu"); m && m->type == ao4json::Value::Type::String && !m->s.empty())
      args.push_back(L"--bind_port_menu=" + Widen(m->s));
  }
}

std::wstring Quote(const std::wstring& a) {
  // CommandLineToArgvW rules: backslashes before a quote are doubled.
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t bs = 0;
  for (const wchar_t c : a) {
    if (c == L'\\') { ++bs; continue; }
    if (c == L'"') { out.append(bs * 2 + 1, L'\\'); bs = 0; out += c; continue; }
    out.append(bs, L'\\');
    bs = 0;
    out += c;
  }
  out.append(bs * 2, L'\\');
  return out + L"\"";
}

std::wstring EnvBlock(const std::map<std::wstring, std::wstring>& set) {
  // Inherit the launcher's environment minus every AO4_*/FPSTEST_* variable, then add ours.
  std::map<std::wstring, std::wstring, std::less<>> env;
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
      std::wstring entry(p);
      const size_t eq = entry.find(L'=', 1);
      if (eq == std::wstring::npos) continue;
      const std::wstring name = entry.substr(0, eq);
      // Only the automated-test variables AO4_TEST_KEY_FILE / AO4_PORT_MENU_TEST are passed on.
      if ((name.rfind(L"AO4_", 0) == 0 && name != L"AO4_TEST_KEY_FILE" && name != L"AO4_PORT_MENU_TEST" &&
           name != L"AO4_PROFILE_WRITE_TRACE" && name != L"AO4_MSR_THREAD_LOCAL" &&
           name != L"AO4_READBACK_POLICY_FILE") || name.rfind(L"FPSTEST_", 0) == 0 ||
          name == L"SDL_AUDIO_DRIVER")
        continue;
      env[name] = entry.substr(eq + 1);
    }
    FreeEnvironmentStringsW(block);
  }
  for (const auto& [k, v] : set) env[k] = v;
  std::wstring out;
  for (const auto& [k, v] : env) out += k + L"=" + v + L'\0';
  out += L'\0';
  return out;
}

const ao4cfg::Profile* Slot(const ao4cfg::ProfileStore& p, const std::string& id) {
  if (const auto* x = p.Find(id)) return x;
  return p.profiles.empty() ? nullptr : &p.profiles.front();
}

}  // namespace

bool Session::Running() const { return game && WaitForSingleObject(game, 0) == WAIT_TIMEOUT; }

void Session::Close() {
  if (server) {
    if (WaitForSingleObject(server, 0) == WAIT_TIMEOUT) TerminateProcess(server, 0);
    CloseHandle(server);
    server = nullptr;
  }
  if (game) { CloseHandle(game); game = nullptr; }
}

std::vector<std::string> LocalAddresses() {
  std::vector<std::string> out;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a,
                           &size) == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             a, &size) != NO_ERROR) return out;
  }
  for (; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      const uint8_t* b = reinterpret_cast<const uint8_t*>(&sin->sin_addr);
      if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) continue;
      char ip[32];
      std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
      out.push_back(ip);
    }
  }
  return out;
}

std::vector<std::wstring> BuildArguments(const ao4cfg::Paths& paths, const ao4cfg::LauncherSettings& s,
                                         const ao4cfg::ProfileStore& profiles, const std::wstring& log) {
  const Mode mode = Mode(s.mode);
  std::vector<std::wstring> a = {
      L"--game_data_root", paths.game_disc, L"--update_data_root", paths.game_hd, L"--user_data_root", paths.profiles,
      L"--log_file", log, L"--log_level=info", L"--gpu_plugin=xenos", L"--mnk_mode", L"--mnk_mouse",
      // Tested rendering/audio options of the normal starters.
      L"--readback_resolve=full", L"--readback_resolve_max_bytes=32768", L"--audio_maxqframes=16",
      L"--depth_test_only_keeps_color_alias_mode=2"};
  // All modes: RTV path (also on Intel, where the runtime would pick the slower ROV path) with the
  // original vertex extent computation (accepted split-screen state E1: no wrapped EDRAM ranges,
  // no corrupted mission pictures/shadows; the user saw the same corruption in single player).
  // Without _with_scissor (2026-11-14): estimating the height of scissored unclipped draws from the
  // vertices on the CPU under-estimated explosion particle draws, so the lower rows of the HDR
  // target were not converted (one frame with rainbow garbage below a horizontal edge, the "green
  // flicker" before explosions). Mission pictures/shadows stay correct without it (SP and split).
  a.insert(a.end(), {L"--render_target_path_d3d12=rtv", L"--execute_unclipped_draw_vs_on_cpu=true"});
  if (mode == Mode::OnlineHost || mode == Mode::OnlineJoin) a.push_back(L"--audio_callback_min_spacing_us=4000");
  InputBindingArgs(paths.config + L"\\input-bindings.json", a);
  // Mouse on the stick path (split-screen player 2, F2 stick mode): small motions must still get
  // past the game's right-stick dead zone.
  a.push_back(L"--mnk_mouse_stick_deadzone=8000");
  // Graphics and audio settings.
  a.push_back(std::wstring(L"--fullscreen=") + (s.fullscreen ? L"true" : L"false"));
  a.push_back(L"--window_width=" + std::to_wstring(s.window_width));
  a.push_back(L"--window_height=" + std::to_wstring(s.window_height));
  a.push_back(L"--resolution_scale=" + std::to_wstring(s.resolution_scale));
  a.push_back(L"--present_effect=" + Widen(s.present_effect));
  a.push_back(L"--anisotropic_override=" + std::to_wstring(s.anisotropic));
  a.push_back(std::wstring(L"--vsync=") + (s.vsync ? L"true" : L"false"));
  // 1 ms Windows timer: guest sleeps of 1 ms otherwise last up to 15.6 ms (main thread waits).
  a.push_back(L"--timer_resolution_1ms=true");
  // Balanced readback: only the 4 KB resolves (luminance) stay synchronous; measured at the
  // split-screen hotspot (Cause & Effect, ch. 6): 29.5 FPS instead of 24-26 with all synchronous.
  if (s.readback != "exact") a.push_back(L"--readback_resolve_full_lengths=4096");
  // GPU stalls (GPU DLL gpu10+): memexport data is applied before the guest can observe GPU
  // progress instead of awaiting the GPU after every export draw, and occlusion query results are
  // published as they complete instead of awaiting them at the end of every command buffer.
  // Split-screen hotspot: 20.4-22.6 -> 22.8-25.5 FPS, frames > 50 ms per 5 s 12-42 -> 0-3.
  a.push_back(L"--readback_memexport_deferred=true");
  a.push_back(L"--occlusion_query_publish_async=true");
  // GPU gpu14+: at guest fences the recorded work is submitted at once but not awaited; the data
  // is applied as soon as the GPU has finished it. Split hotspot (3 alternating pairs):
  // 23.4-26.8 vs 20.1-21.2 FPS, frames > 50 ms 23-81 vs 541-726.
  a.push_back(L"--readback_deferred_lazy=true");
  // GPU gpu15+: submit the recorded work every 128 draws so the GPU runs in parallel with the
  // command processor (the remaining synchronous 4 KB resolves then only wait for a short tail).
  // Split hotspot, alternating pairs: 29.4/29.6 vs 27.4/27.8 FPS (16 draws: overhead, worse).
  a.push_back(L"--submit_every_draws=128");
  a.push_back(std::wstring(L"--audio_mute=") + (s.mute ? L"true" : L"false"));
  if (s.fps60) a.push_back(L"--video_mode_refresh_rate=120");
  // Profiles: slot 1 always, slot 2 in split screen (runtime options user2_*).
  if (const auto* p1 = Slot(profiles, s.slot1)) {
    a.push_back(L"--user_name=" + Widen(p1->name));
    a.push_back(L"--user_xuid=" + Widen(p1->xuid));
  }
  if (mode == Mode::Split) {
    a.insert(a.end(), {L"--local_users=2", L"--gamepad_first_user=1", L"--mnk_user_toggle=true"});
    // Manual device -> player assignment (launcher, split screen).
    a.push_back(L"--input_mnk_user=" + std::to_wstring(s.mnk_player));
    if (!s.pad_players.empty()) a.push_back(L"--input_pad_users=" + Widen(s.pad_players));
    if (const auto* p2 = profiles.Find(s.slot2)) {
      a.push_back(L"--user2_name=" + Widen(p2->name));
      a.push_back(L"--user2_xuid=" + Widen(p2->xuid));
    }
  } else {
    a.push_back(L"--local_users=1");
  }
  if (mode == Mode::OnlineHost || mode == Mode::OnlineJoin) {
    const std::string self = s.host_ip.empty() ? std::string("127.0.0.1") : s.host_ip;
    const std::string server = mode == Mode::OnlineHost ? self : s.join_ip;
    a.push_back(L"--online_signin=true");
    a.push_back(L"--xlsp_server=" + Widen(server));
    a.push_back(L"--net_bind_ip=" + Widen(self));
  } else {
    a.push_back(L"--online_signin=false");
  }
  // Comparison runs: AO4_TEST_EXTRA_ARGS (space separated, appended last so they win).
  wchar_t extra[2048] = {};
  if (GetEnvironmentVariableW(L"AO4_TEST_EXTRA_ARGS", extra, 2048)) {
    std::wistringstream in(extra);
    for (std::wstring arg; in >> arg;) a.push_back(arg);
  }
  return a;
}

bool Start(const ao4cfg::Paths& paths, const ao4cfg::LauncherSettings& s, const ao4cfg::ProfileStore& profiles,
           Session& session, std::wstring& error) {
  const Mode mode = Mode(s.mode);
  CreateDirectoryW(paths.logs.c_str(), nullptr);
  CreateDirectoryW(paths.profiles.c_str(), nullptr);
  const std::wstring stamp = Stamp();
  session.log = paths.logs + L"\\game-" + stamp + L".log";

  std::map<std::wstring, std::wstring> env = {
      {L"AO4_QOL_SETTINGS", paths.config + L"\\quality-of-life.ini"},
      {L"AO4_INPUT_BINDINGS", paths.config + L"\\input-bindings.json"},
      {L"AO4_PARTNER_CONFIG", paths.config + L"\\partner-equipment.ini"},
      {L"AO4_MOUSE_SETTINGS", paths.config + L"\\native-mouse.json"},
      {L"AO4_ABORT_TRACE", session.log + L".abort.txt"},
      {L"AO4_NATIVE_MOUSE", L"1"}, {L"AO4_TURRET_TEST", L"1"},
      // F4 = port menu (sensitivity slider there), F3 = runtime overlay: no F3/F4 key steps.
      {L"AO4_MOUSE_HOTKEYS", L"0"}, {L"AO4_PORT_MENU", L"1"},
      {L"AO4_LAUNCHER_LANG", Widen(s.language)},
      {L"AO4_ICON_DIR", paths.root + L"\\game\\icons"},  // partner-gear pictures for the F4 menu
      {L"AO4_XMA_VALID_FIX", L"1"}, {L"AO4_XMA_TAIL_FLUSH", L"0"},
      {L"SDL_AUDIO_DRIVER", Widen(s.audio_backend)},
  };
  if (s.cosmetics_buyable) env[L"AO4_COSMETICS_BUYABLE"] = L"1";
  if (s.unlock_extras) env[L"AO4_UNLOCK_LICENSES"] = L"1";
  ao4cfg::MouseSettings mouse;
  mouse.Load(paths.config + L"\\native-mouse.json");
  wchar_t buf[64];
  swprintf(buf, 64, L"%.17g", mouse.radians_per_count);
  env[L"AO4_MOUSE_RADIANS_PER_COUNT"] = buf;
  env[L"AO4_MOUSE_INVERT_Y"] = mouse.invert_y ? L"1" : L"0";
  if (mode == Mode::Single) {
    // Bravo mode / role swap / custom partner gear capabilities of the normal player starter
    // (toggled in game: F9 Bravo mode, F6 partner gear).
    for (const wchar_t* k : {L"AO4_LOOK_SWAP", L"AO4_CONTROL_SWAP", L"AO4_PLAYER1_PAI", L"AO4_VO_GAMESTATE_SWAP",
                             L"AO4_VO_SOURCE_SWAP", L"AO4_AI_ALPHA_OUTFIT", L"AO4_SKIN_BRAVO"}) env[k] = L"1";
    if (s.unlock_missions) env[L"AO4_UNLOCK_MISSIONS"] = L"1";
  } else if (mode == Mode::Split) {
    env[L"AO4_LOCAL_COOP"] = L"1";
    if (s.unlock_missions) env[L"AO4_UNLOCK_MISSIONS"] = L"1";
  } else {
    env[L"AO4_ONLINE_ROLE_ISOLATION"] = L"1";
    env[L"AO4_ONLINE_VOICE_REBIND"] = L"1";
    env[L"AO4_XMA_TAIL_DEMAND"] = L"1";
  }

  // quality-of-life.ini carries the 60 FPS switch (bildrate_60) next to the other fixes.
  ao4cfg::QolFile qol;
  if (qol.Load(paths.config + L"\\quality-of-life.ini")) {
    if (qol.Get("bildrate_60", false) != s.fps60) {
      qol.Set("bildrate_60", s.fps60);
      qol.Save(paths.config + L"\\quality-of-life.ini");
    }
  }

  if (mode == Mode::OnlineHost) {
    // Mini Blaze server on the chosen LAN/VPN address (embedded Python, our own script).
    const std::string ip = s.host_ip.empty() ? std::string("127.0.0.1") : s.host_ip;
    const std::wstring python = paths.server + L"\\python\\python.exe";
    const std::wstring script = paths.server + L"\\ao4-blaze-server.py";
    std::wstring cmd = Quote(python) + L" -I " + Quote(script) + L" --bind " + Widen(ip) + L" --public-host " + Widen(ip) +
                       L" --allow " + Widen(ip.substr(0, ip.rfind('.')) + ".0/24,25.0.0.0/8,100.64.0.0/10,127.0.0.0/8") +
                       L" --seconds 14400 --log " + Quote(paths.logs + L"\\blaze-" + stamp + L".log");
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, paths.server.c_str(), &si,
                        &pi)) {
      error = L"Koop-Server konnte nicht gestartet werden (server\\python fehlt?).";
      return false;
    }
    CloseHandle(pi.hThread);
    session.server = pi.hProcess;
    Sleep(1500);
    if (WaitForSingleObject(session.server, 0) != WAIT_TIMEOUT) {
      error = L"Koop-Server hat sich sofort beendet (Port 42124 belegt?).";
      session.Close();
      return false;
    }
  }

  const std::wstring exe = paths.bin + L"\\ao4_launcher.exe";
  std::wstring cmd = Quote(exe);
  for (const auto& arg : BuildArguments(paths, s, profiles, session.log)) cmd += L" " + Quote(arg);
  std::wstring env_block = EnvBlock(env);
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env_block.data(),
                      paths.bin.c_str(), &si, &pi)) {
    error = L"Spiel konnte nicht gestartet werden (bin\\ao4_launcher.exe fehlt?).";
    session.Close();
    return false;
  }
  CloseHandle(pi.hThread);
  session.game = pi.hProcess;
  // Optional bug capture (window capture only, no hooks): F12 saves a screenshot plus the last
  // seconds as images to logs\captures; sudden green frames are saved automatically.
  const std::wstring bugcam = paths.bin + L"\\ao4_bugcam.exe";
  if (s.bug_capture && GetFileAttributesW(bugcam.c_str()) != INVALID_FILE_ATTRIBUTES) {
    const std::wstring captures = paths.logs + L"\\captures";
    CreateDirectoryW(captures.c_str(), nullptr);
    std::wstring cam = Quote(bugcam) + L" " + std::to_wstring(pi.dwProcessId) + L" " + Quote(exe) + L" " +
                       Quote(captures) + L" 6";
    STARTUPINFOW csi{sizeof(csi)};
    PROCESS_INFORMATION cpi{};
    if (CreateProcessW(bugcam.c_str(), cam.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, paths.bin.c_str(), &csi, &cpi)) {
      CloseHandle(cpi.hThread);
      CloseHandle(cpi.hProcess);  // ends by itself with the game
    }
  }
  return true;
}

}  // namespace ao4launch
