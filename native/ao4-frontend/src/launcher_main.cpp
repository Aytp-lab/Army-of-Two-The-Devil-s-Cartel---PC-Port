// AO4Launcher.exe: first-run setup from the player's own ISO, game start for all modes,
// settings, profile manager (player slots 1 and 2), help. German / English.
#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>

#include "config.h"
#include "game_setup.h"
#include "i18n.h"
#include "json_lite.h"
#include "launch.h"
#include "partner_catalog.h"
#include "partner_icons.h"

#include <map>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef AO4_PORT_VERSION
#define AO4_PORT_VERSION "0.1.0-test1"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using namespace ao4ui;
using ao4cfg::Narrow;
using ao4cfg::Widen;

constexpr double kMouseBase = 0.0012418426900000002;  // tested default of native-mouse.json
constexpr uint64_t kInstallBytes = 7600ull * 1024 * 1024;

// ---- D3D11 -------------------------------------------------------------------------
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;

void CreateRtv() {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
void ReleaseRtv() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

bool CreateDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                             &sd, &g_swap, &g_dev, &got, &g_ctx);
  if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: software rasterizer is fine for a launcher
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                       &g_swap, &g_dev, &got, &g_ctx);
  if (FAILED(hr)) return false;
  CreateRtv();
  return true;
}
void DestroyDevice() {
  ReleaseRtv();
  if (g_swap) g_swap->Release();
  if (g_ctx) g_ctx->Release();
  if (g_dev) g_dev->Release();
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) { g_resize_w = LOWORD(lp); g_resize_h = HIWORD(lp); }
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- helpers -----------------------------------------------------------------------
bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string ReadAll(const std::wstring& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool WriteAll(const std::wstring& file, const std::string& text) {
  const std::wstring tmp = file + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
  }
  return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string GiB(uint64_t bytes) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return b;
}

void Copy(char* dst, size_t n, const std::string& src) {
  std::snprintf(dst, n, "%s", src.c_str());
}

void Tooltip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

const char* L(const char* de, const char* en) { return GetLanguage() == Lang::En ? en : de; }

// Key names understood by the runtime (keybind_* options, bind_port_menu) and, for single keys,
// by the main DLL's hotkeys. Generic Shift/Ctrl/Alt only (no left/right variants).
struct KeyName { int vk; const char* name; };
const KeyName kKeys[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"},
    {VK_BACK, "Backspace"}, {VK_TAB, "Tab"}, {VK_RETURN, "Return"}, {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"},
    {VK_MENU, "Alt"}, {VK_PAUSE, "Pause"}, {VK_CAPITAL, "CapsLock"}, {VK_SPACE, "Space"}, {VK_PRIOR, "PageUp"},
    {VK_NEXT, "PageDown"}, {VK_END, "End"}, {VK_HOME, "Home"}, {VK_LEFT, "Left"}, {VK_UP, "Up"}, {VK_RIGHT, "Right"},
    {VK_DOWN, "Down"}, {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"}, {'9', "9"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_MULTIPLY, "NumpadStar"}, {VK_ADD, "NumpadPlus"},
    {VK_SUBTRACT, "NumpadMinus"}, {VK_DIVIDE, "NumpadSlash"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F3, "F3"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMLOCK, "NumLock"}, {VK_SCROLL, "ScrollLock"},
    {VK_OEM_1, "Semicolon"}, {VK_OEM_PLUS, "Plus"}, {VK_OEM_COMMA, "Comma"}, {VK_OEM_MINUS, "Minus"},
    {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"}, {VK_OEM_3, "Backtick"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_5, "Backslash"}, {VK_OEM_6, "RBracket"}, {VK_OEM_7, "Quote"},
};
bool IsModifierVk(int vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU; }
// Single keys the main DLL accepts for its hotkeys (Bravo mode, partner gear).
bool HotkeyCapable(const std::string& n) {
  if (n.size() == 1) return true;
  if (n[0] == 'F' && n.size() <= 3 && isdigit(uint8_t(n[1]))) return true;
  if (n.rfind("Numpad", 0) == 0 && n.size() == 7) return true;
  for (const char* k : {"Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Pause", "ScrollLock", "Backspace",
                        "Tab", "Space", "Return"})
    if (n == k) return true;
  return false;
}
// Keys used by the port itself in game.
const char* ReservedKeyUse(const std::string& n) {
  if (n == "F1") return L("Splitscreen: Tastatur zwischen Spielern wechseln", "split screen: switch keyboard player");
  if (n == "F2") return L("Maus direkt / Stick-Modus", "direct mouse / stick mode");
  if (n == "F3") return L("Leistungsanzeige", "performance overlay");
  if (n == "F7") return L("Erfolge", "achievements");
  if (n == "F12") return L("Fehlerstelle markieren", "mark a bug spot");
  if (n == "Backtick") return L("Konsole", "console");
  return nullptr;
}

// Waits for a key press in the launcher window: Escape cancels; a non-modifier key takes the
// held modifiers as a chord ("Shift+Up"); a modifier released alone binds the modifier.
struct KeyCapture {
  std::string target;  // "kb:<action>" or "hk:<hotkey>"
  bool append = false, armed = false, single = false;
  int pending_mod = 0;
  void Start(const std::string& t, bool add, bool single_key) {
    target = t; append = add; armed = false; single = single_key; pending_mod = 0;
  }
  bool Active() const { return !target.empty(); }
  // Returns true when finished; result empty = cancelled.
  bool Poll(HWND hwnd, std::string& result) {
    result.clear();
    if (GetForegroundWindow() != hwnd) return false;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (!armed) {  // wait until the click that started the capture is released
      for (const auto& k : kKeys) if (down(k.vk)) return false;
      armed = true;
      return false;
    }
    if (down(VK_ESCAPE)) { target.clear(); return true; }
    for (const auto& k : kKeys) {
      if (!down(k.vk)) continue;
      if (IsModifierVk(k.vk)) { if (!single) pending_mod = k.vk; else { result = k.name; break; } continue; }
      if (!single) {
        if (down(VK_CONTROL) && k.vk != VK_CONTROL) result += "Ctrl+";
        if (down(VK_MENU) && k.vk != VK_MENU) result += "Alt+";
        if (down(VK_SHIFT) && k.vk != VK_SHIFT) result += "Shift+";
      }
      result += k.name;
      break;
    }
    if (result.empty() && pending_mod && !down(pending_mod)) {
      for (const auto& k : kKeys) if (k.vk == pending_mod) result = k.name;
    }
    if (result.empty()) return false;
    return true;
  }
};

// Game-fix switches of quality-of-life.ini shown in the settings (bildrate_60 has its own
// option; skipping the splash stays off: the splash phase loads the HD data).
struct QolEntry { const char* key; const char* de; const char* en; const char* tip_de; const char* tip_en; };
const QolEntry kQol[] = {
    {"dialogzweig_fix", "Dialogzweig-Fix", "Dialogue branch fix",
     "Manche Dialoge wählen im Original eine unpassende Variante und bleiben dann stumm (z.B. Outskirts Auto, Church, Barrio).",
     "Some dialogues pick an unsuitable variant in the original and stay silent (e.g. Outskirts car, Church, Barrio)."},
    {"mg_ansage_fix", "MG-Ansage-Fix", "Machine-gun callout fix",
     "Outskirts Auto: Alpha am Geschütz ruft sich im Original selbst die MG-Warnung zu. Mit dem Fix ruft Bravo.",
     "Outskirts car: in the original Alpha on the gun warns himself about the machine gun. With the fix Bravo calls it."},
    {"ausruestung_ladefix", "Ausrüstungs-Ladefix", "Equipment load fix",
     "Lädt fehlende Körper-/Outfit-Ressourcen beider Figuren nach Spawn und Laden nach.",
     "Reloads missing body/outfit resources of both characters after spawning and loading."},
};

// ---- application state ---------------------------------------------------------------
struct App {
  ao4cfg::Paths paths = ao4cfg::Paths::FromExe();
  ao4cfg::LauncherSettings s;
  ao4cfg::ProfileStore profiles;
  ao4cfg::QolFile qol;
  ao4cfg::MouseSettings mouse;
  ao4cfg::PartnerGear gear;
  ao4cfg::PartnerState partner;
  ao4json::Value bindings;
  KeyCapture capture;
  std::string controls_msg;
  // Partner-gear preview pictures (made from the player's own game files into game\icons).
  struct IconTex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };
  std::map<uint32_t, IconTex> icon_tex;
  int icons_ready = 0;
  std::wstring IconsDir() const { return paths.root + L"\\game\\icons"; }
  bool have_qol = false, have_bindings = false;
  bool installed = false, show_setup = false;

  // setup
  char iso[1024] = {};
  enum SetupState { Idle, Checking, CheckedOk, CheckedBad, Installing, Done, Failed };
  std::atomic<int> setup{Idle};
  std::atomic<uint64_t> done{0}, total{0};
  std::atomic<bool> cancel{false};
  std::mutex mu;
  std::wstring cur_file, setup_msg;
  ao4setup::IsoInfo info;
  std::thread worker;

  // play
  ao4launch::Session session;
  std::string status;
  std::vector<std::string> addresses;
  char join_ip[64] = {};

  // profiles
  int sel = -1;
  char new_name[32] = {}, rename[32] = {};
  std::string profile_msg;

  float saved_flash = 0;
  bool dirty = false;
  HWND hwnd = nullptr;
  int initial_tab = -1;  // --tab=N (screenshots in tests)

  std::wstring F(const wchar_t* name) const { return paths.config + L"\\" + name; }

  void Load() {
    CreateDirectoryW(paths.config.c_str(), nullptr);
    s.Load(F(L"launcher.ini"));  // release default: English
    SetLanguage(s.language == "en" ? Lang::En : Lang::De);
    profiles.Load(F(L"profiles.ini"));
    if (profiles.profiles.empty()) {
      // Neutral default name (not the Windows account name: it would end up in logs/bug reports).
      const auto& p = profiles.Create("Player 1");
      s.slot1 = p.id;
      profiles.Save(F(L"profiles.ini"));
    }
    if (!profiles.Find(s.slot1)) s.slot1 = profiles.profiles.front().id;
    if (s.slot2 == s.slot1 || !profiles.Find(s.slot2)) s.slot2.clear();
    have_qol = qol.Load(F(L"quality-of-life.ini"));
    mouse.Load(F(L"native-mouse.json"));
    gear.Load(F(L"partner-equipment.ini"));
    partner.Load(F(L"partner-active.ini"));
    have_bindings = ao4json::Parse(ReadAll(F(L"input-bindings.json")), bindings) &&
                    bindings.type == ao4json::Value::Type::Object;
    Copy(iso, sizeof(iso), Narrow(s.iso_path));
    Copy(join_ip, sizeof(join_ip), s.join_ip);
    addresses = ao4launch::LocalAddresses();
    if (s.host_ip.empty() && !addresses.empty()) s.host_ip = addresses.front();
    installed = ao4setup::IsInstalled(paths.game_disc, paths.game_hd);
    if (installed) BuildIcons();
  }

  void BuildIcons() {
    std::vector<ao4icons::Request> items;
    for (const auto& i : ao4partner::kMasks) items.push_back({i.id, i.icon});
    for (const auto& i : ao4partner::kOutfits) items.push_back({i.id, i.icon});
    for (const auto& i : ao4partner::kTattoos) items.push_back({i.id, i.icon});
    std::wstring err;
    CreateDirectoryW((paths.root + L"\\game").c_str(), nullptr);
    icons_ready = ao4icons::Build(paths.game_disc, IconsDir(), items, err);
  }

  void Save() {
    s.iso_path = Widen(iso);
    s.join_ip = join_ip;
    s.Save(F(L"launcher.ini"));
    profiles.Save(F(L"profiles.ini"));
    if (have_qol) { qol.Set("bildrate_60", s.fps60); qol.Save(F(L"quality-of-life.ini")); }
    mouse.Save(F(L"native-mouse.json"));
    if (have_bindings) WriteAll(F(L"input-bindings.json"), ao4json::Serialize(bindings));
    dirty = false;
    saved_flash = 2.5f;
  }

  void Join() { if (worker.joinable()) worker.join(); }

  void StartCheck() {
    Join();
    setup = Checking;
    const std::wstring path = Widen(iso);
    worker = std::thread([this, path] {
      ao4setup::IsoInfo i;
      std::wstring err;
      if (!ao4setup::ReadIso(path, i, err)) {
        std::lock_guard<std::mutex> g(mu);
        setup_msg = err;
        setup = CheckedBad;
        return;
      }
      const auto v = ao4setup::CheckVersion(path, i);
      std::lock_guard<std::mutex> g(mu);
      info = std::move(i);
      setup_msg = v.message;
      setup = v.ok ? CheckedOk : CheckedBad;
    });
  }

  void StartInstall() {
    Join();
    cancel = false;
    done = 0;
    total = 0;
    setup = Installing;
    s.iso_path = Widen(iso);
    s.Save(F(L"launcher.ini"));
    const std::wstring path = s.iso_path;
    worker = std::thread([this, path] {
      std::wstring err;
      const bool ok = ao4setup::Extract(
          path, info, paths.game_disc, paths.game_hd,
          [this](uint64_t d, uint64_t t, const std::wstring& f) {
            done = d;
            total = t;
            { std::lock_guard<std::mutex> g(mu); cur_file = f; }
            return !cancel.load();
          },
          err);
      std::lock_guard<std::mutex> g(mu);
      setup_msg = err;
      if (ok && ao4setup::IsInstalled(paths.game_disc, paths.game_hd)) {
        installed = true;
        setup = Done;
      } else {
        setup = Failed;
      }
    });
  }
};

// ---- pages -------------------------------------------------------------------------
void PageSetup(App& a) {
  ImGui::SeparatorText(Tr(T_SetupHeading));
  ImGui::TextWrapped("%s", Tr(T_SetupIntro));
  ImGui::Spacing();
  const int st = a.setup.load();
  const bool busy = st == App::Checking || st == App::Installing;
  ImGui::BeginDisabled(busy);
  ImGui::SetNextItemWidth(-ImGui::CalcTextSize(Tr(T_SetupBrowse)).x - ImGui::GetStyle().ItemSpacing.x * 2 -
                          ImGui::GetStyle().FramePadding.x * 2);
  if (ImGui::InputTextWithHint("##iso", Tr(T_SetupChooseIso), a.iso, sizeof(a.iso))) a.setup = App::Idle;
  ImGui::SameLine();
  if (ImGui::Button(Tr(T_SetupBrowse))) {
    wchar_t file[1024] = {};
    wcsncpy_s(file, Widen(a.iso).c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.lpstrFilter = L"Xbox 360 ISO (*.iso)\0*.iso\0*.*\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
      Copy(a.iso, sizeof(a.iso), Narrow(file));
      a.StartCheck();
    }
  }
  ImGui::EndDisabled();

  ULARGE_INTEGER free_bytes{};
  CreateDirectoryW((a.paths.root + L"\\game").c_str(), nullptr);
  GetDiskFreeSpaceExW(a.paths.root.c_str(), &free_bytes, nullptr, nullptr);
  ImGui::Text("%s: %s  (%s: %s)", Tr(T_SetupSpace), GiB(free_bytes.QuadPart).c_str(), Tr(T_SetupFolder),
              Narrow(a.paths.root + L"\\game").c_str());
  ImGui::Spacing();

  std::wstring msg;
  { std::lock_guard<std::mutex> g(a.mu); msg = a.setup_msg; }
  switch (st) {
    case App::Idle:
      ImGui::BeginDisabled(a.iso[0] == 0);
      if (ImGui::Button(Tr(T_SetupCheck))) a.StartCheck();
      ImGui::EndDisabled();
      break;
    case App::Checking:
      ImGui::TextUnformatted("...");
      break;
    case App::CheckedBad:
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1));
      ImGui::TextWrapped("%s", Tr(T_SetupVersionBad));
      if (!msg.empty()) ImGui::TextWrapped("%s", Narrow(msg).c_str());
      ImGui::PopStyleColor();
      if (ImGui::Button(Tr(T_SetupCheck))) a.StartCheck();
      break;
    case App::CheckedOk: {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.9f, 0.5f, 1));
      ImGui::TextWrapped("%s", Tr(T_SetupVersionOk));
      ImGui::PopStyleColor();
      const bool space = free_bytes.QuadPart >= kInstallBytes || a.installed;
      if (!space) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s < %s", Tr(T_SetupSpace), GiB(kInstallBytes).c_str());
      if (ImGui::Button(Tr(T_SetupInstall), ImVec2(ImGui::GetFontSize() * 10, 0))) a.StartInstall();
      break;
    }
    case App::Installing: {
      const uint64_t d = a.done, t = a.total;
      std::wstring f;
      { std::lock_guard<std::mutex> g(a.mu); f = a.cur_file; }
      ImGui::TextUnformatted(Tr(T_SetupProgress));
      char overlay[64];
      std::snprintf(overlay, sizeof(overlay), "%s / %s", GiB(d).c_str(), GiB(t).c_str());
      ImGui::ProgressBar(t ? float(double(d) / double(t)) : 0.0f, ImVec2(-1, 0), overlay);
      ImGui::TextDisabled("%s", Narrow(f).c_str());
      if (ImGui::Button(Tr(T_SetupCancel))) a.cancel = true;
      break;
    }
    case App::Done:
      ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s", Tr(T_SetupDone));
      if (ImGui::Button("OK")) { a.show_setup = false; a.setup = App::Idle; a.BuildIcons(); }
      break;
    case App::Failed:
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", Narrow(msg).c_str());
      if (ImGui::Button(Tr(T_SetupInstall))) a.StartInstall();
      break;
  }
  if (a.installed && st != App::Installing && st != App::Done) {
    ImGui::Spacing();
    if (ImGui::Button(Tr(T_SetupCancel))) a.show_setup = false;
  }
}

void StartGame(App& a) {
  a.Save();
  std::wstring err;
  if (ao4launch::Start(a.paths, a.s, a.profiles, a.session, err)) {
    a.status = Tr(T_Running);
    ShowWindow(a.hwnd, SW_MINIMIZE);
  } else {
    a.status = Narrow(err);
  }
}

// Split screen: which player keyboard/mouse and each controller (by connection order) feed.
void SplitDevices(App& a) {
  ImGui::SeparatorText(L("Geräte", "Devices"));
  const char* players = L("Spieler 1\0Spieler 2\0", "Player 1\0Player 2\0");
  const char* pad_choices = L("nicht benutzt\0Spieler 1\0Spieler 2\0", "not used\0Player 1\0Player 2\0");
  const float w = ImGui::GetFontSize() * 10;
  ImGui::SetNextItemWidth(w);
  if (ImGui::Combo(L("Tastatur/Maus", "Keyboard/mouse"), &a.s.mnk_player, players)) a.dirty = true;
  int pads[4] = {1, -1, -1, -1};  // default: first controller player 2
  if (!a.s.pad_players.empty()) {
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
      const size_t comma = a.s.pad_players.find(',', start);
      pads[i] = std::atoi(a.s.pad_players.substr(start, comma - start).c_str());
      if (comma == std::string::npos) { for (int j = i + 1; j < 4; ++j) pads[j] = -1; break; }
      start = comma + 1;
    }
  }
  // Connected Xbox controllers (XInput) as a hint; other pads are numbered by connection order too.
  using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
  static GetStateFn get_state = [] {
    HMODULE x = LoadLibraryW(L"xinput1_4.dll");
    if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
    return x ? reinterpret_cast<GetStateFn>(GetProcAddress(x, "XInputGetState")) : nullptr;
  }();
  bool changed = false;
  for (int i = 0; i < 4; ++i) {
    int choice = pads[i] + 1;
    char label[64];
    std::snprintf(label, sizeof(label), L("Controller %d", "Controller %d"), i + 1);
    ImGui::SetNextItemWidth(w);
    ImGui::PushID(i);
    if (ImGui::Combo(label, &choice, pad_choices)) { pads[i] = choice - 1; changed = true; }
    ImGui::PopID();
    uint8_t state[16] = {};
    if (get_state && get_state(DWORD(i), state) == ERROR_SUCCESS) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s", L("(Xbox-Controller verbunden)", "(Xbox controller connected)"));
    }
  }
  if (changed) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d", pads[0], pads[1], pads[2], pads[3]);
    a.s.pad_players = buf;
    a.dirty = true;
  }
  if (ImGui::SmallButton(L("Standard (Tastatur Spieler 1, Controller 1 Spieler 2)", "Default (keyboard player 1, controller 1 player 2)"))) {
    a.s.mnk_player = 0;
    a.s.pad_players.clear();
    a.dirty = true;
  }
  ImGui::TextDisabled("%s", L("Controller werden in der Reihenfolge nummeriert, in der sie angeschlossen wurden. F1 im Spiel wechselt die Tastatur zwischen den Spielern.",
                              "Controllers are numbered in the order they were connected. F1 in game switches the keyboard between players."));
}

bool ProfileCombo(const char* label, App& a, std::string& slot, bool allow_none, const std::string& exclude) {
  const auto* cur = a.profiles.Find(slot);
  bool changed = false;
  if (ImGui::BeginCombo(label, cur ? cur->name.c_str() : Tr(T_NoProfile))) {
    if (allow_none && ImGui::Selectable(Tr(T_NoProfile), cur == nullptr)) { slot.clear(); changed = true; }
    for (const auto& p : a.profiles.profiles) {
      if (p.id == exclude) continue;
      ImGui::PushID(p.id.c_str());
      if (ImGui::Selectable(p.name.c_str(), cur && cur->id == p.id)) { slot = p.id; changed = true; }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void PagePlay(App& a) {
  const bool running = a.session.Running();
  const float w = ImGui::GetFontSize() * 16;
  const ImVec2 big(ImGui::GetFontSize() * 14, ImGui::GetFontSize() * 2.2f);
  ImGui::BeginDisabled(running);

  ImGui::SetNextItemWidth(w);
  if (ProfileCombo(Tr(T_Slot1), a, a.s.slot1, false, "")) {
    if (a.s.slot2 == a.s.slot1) a.s.slot2.clear();
    a.dirty = true;
  }

  // ---- 1) local: alone or split screen
  ImGui::SeparatorText(L("Spielen (allein oder Splitscreen)", "Play (alone or split screen)"));
  bool split = a.s.mode == int(ao4cfg::Mode::Split);
  if (ImGui::Checkbox(L("Splitscreen mit 2 Spielern an diesem PC", "Split screen with 2 players on this PC"), &split)) {
    a.s.mode = int(split ? ao4cfg::Mode::Split : ao4cfg::Mode::Single);
    a.dirty = true;
  }
  if (split) {
    ImGui::Indent();
    ImGui::SetNextItemWidth(w);
    if (ProfileCombo(Tr(T_Slot2), a, a.s.slot2, true, a.s.slot1)) a.dirty = true;
    ImGui::TextWrapped("%s", Tr(T_SplitHint));
    SplitDevices(a);
    ImGui::Unindent();
  }
  const bool can_local = a.installed && a.profiles.Find(a.s.slot1);
  ImGui::BeginDisabled(!can_local);
  if (ImGui::Button(L("Spielen", "Play"), big)) {
    a.s.mode = int(split ? ao4cfg::Mode::Split : ao4cfg::Mode::Single);
    StartGame(a);
  }
  ImGui::EndDisabled();

  // ---- 2) online / LAN co-op
  ImGui::Spacing();
  ImGui::SeparatorText(L("Online-Koop (LAN oder virtuelles LAN)", "Online co-op (LAN or virtual LAN)"));
  static int role = 0;  // 0 host, 1 join (remembered via mode once used)
  if (a.s.mode == int(ao4cfg::Mode::OnlineJoin)) role = 1;
  if (a.s.mode == int(ao4cfg::Mode::OnlineHost)) role = 0;
  ImGui::RadioButton(L("Spiel hosten", "Host a game"), &role, 0);
  ImGui::SameLine();
  ImGui::RadioButton(L("Spiel beitreten", "Join a game"), &role, 1);
  ImGui::SetNextItemWidth(w);
  if (ImGui::BeginCombo(Tr(T_HostAddress), a.s.host_ip.c_str())) {
    for (const auto& ip : a.addresses)
      if (ImGui::Selectable(ip.c_str(), ip == a.s.host_ip)) { a.s.host_ip = ip; a.dirty = true; }
    if (ImGui::Selectable("127.0.0.1", a.s.host_ip == "127.0.0.1")) { a.s.host_ip = "127.0.0.1"; a.dirty = true; }
    ImGui::EndCombo();
  }
  if (role == 1) {
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint(Tr(T_JoinAddress), "192.168.0.10", a.join_ip, sizeof(a.join_ip))) a.dirty = true;
  }
  ImGui::TextWrapped("%s", Tr(T_OnlineHint));
  if (role == 0) ImGui::TextDisabled("%s", Tr(T_FirewallHint));
  const bool can_online = can_local && (role == 0 || a.join_ip[0] != 0);
  ImGui::BeginDisabled(!can_online);
  if (ImGui::Button(role == 0 ? L("Online-Koop hosten", "Host online co-op") : L("Online-Koop beitreten", "Join online co-op"), big)) {
    a.s.mode = int(role == 0 ? ao4cfg::Mode::OnlineHost : ao4cfg::Mode::OnlineJoin);
    StartGame(a);
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (running) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s", Tr(T_Running));
  else if (!a.status.empty()) ImGui::TextWrapped("%s", a.status.c_str());
}

void PageSettings(App& a) {
  ImGui::SeparatorText(Tr(T_SectionLanguage));
  const float w = ImGui::GetFontSize() * 14;
  ImGui::SetNextItemWidth(w);
  int ui = a.s.language == "en" ? 1 : 0;
  if (ImGui::Combo(Tr(T_UiLanguage), &ui, "Deutsch\0English\0")) {
    a.s.language = ui ? "en" : "de";
    SetLanguage(ui ? Lang::En : Lang::De);
    a.dirty = true;
  }
  if (a.have_bindings) {
    static const char* kCodes[] = {"de", "en", "it"};
    const auto* l = a.bindings.Get("language");
    int gl = 0;
    for (int k = 0; k < 3; ++k) if (l && l->s == kCodes[k]) gl = k;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo(Tr(T_GameLanguage), &gl, "Deutsch\0English\0Italiano\0")) {
      a.bindings.Set("language", ao4json::Value::Str(kCodes[gl]));
      a.dirty = true;
    }
  }

  ImGui::SeparatorText(Tr(T_SectionGraphics));
  if (ImGui::Checkbox(Tr(T_Fullscreen), &a.s.fullscreen)) a.dirty = true;
  static const int kSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
  char cur[32];
  std::snprintf(cur, sizeof(cur), "%d x %d", a.s.window_width, a.s.window_height);
  ImGui::SetNextItemWidth(w);
  if (ImGui::BeginCombo(Tr(T_WindowSize), cur)) {
    for (const auto& sz : kSizes) {
      char t[32];
      std::snprintf(t, sizeof(t), "%d x %d", sz[0], sz[1]);
      if (ImGui::Selectable(t, a.s.window_width == sz[0] && a.s.window_height == sz[1])) {
        a.s.window_width = sz[0];
        a.s.window_height = sz[1];
        a.dirty = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(w);
  if (ImGui::SliderInt(Tr(T_ResolutionScale), &a.s.resolution_scale, 1, 3, "%dx")) a.dirty = true;
  int fx = a.s.present_effect == "cas" ? 1 : a.s.present_effect == "fsr" ? 2 : 0;
  ImGui::SetNextItemWidth(w);
  if (ImGui::Combo(Tr(T_PresentEffect), &fx, "Bilinear\0AMD CAS\0AMD FSR\0")) {
    a.s.present_effect = fx == 1 ? "cas" : fx == 2 ? "fsr" : "bilinear";
    a.dirty = true;
  }
  ImGui::SetNextItemWidth(w);
  if (ImGui::Combo(Tr(T_Anisotropic), &a.s.anisotropic, L("Aus\0" "1x\0" "2x\0" "4x\0" "8x\0" "16x\0", "Off\0" "1x\0" "2x\0" "4x\0" "8x\0" "16x\0")))
    a.dirty = true;
  if (ImGui::Checkbox(Tr(T_Vsync), &a.s.vsync)) a.dirty = true;
  int rb = a.s.readback == "exact" ? 1 : 0;
  ImGui::SetNextItemWidth(w);
  if (ImGui::Combo(L("Bildgenauigkeit", "Image accuracy"), &rb,
                   L("Ausgewogen (schneller)\0Genau (langsamer)\0", "Balanced (faster)\0Exact (slower)\0"))) {
    a.s.readback = rb ? "exact" : "balanced";
    a.dirty = true;
  }
  Tooltip(L("Wie die GPU-Ergebnisse an das Spiel zurückgegeben werden. Falls schwarze Blöcke oder falsche Helligkeit auftreten: \"Genau\" wählen.",
            "How GPU results are returned to the game. If black blocks or wrong brightness appear, choose \"Exact\"."));
  if (ImGui::Checkbox(Tr(T_Fps60), &a.s.fps60)) a.dirty = true;
  ImGui::Indent();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1));
  ImGui::TextWrapped("%s", Tr(T_Fps60Hint));
  ImGui::PopStyleColor();
  ImGui::Unindent();

  ImGui::SeparatorText(Tr(T_SectionAudio));
  int ab = a.s.audio_backend == "directsound" ? 1 : 0;
  ImGui::SetNextItemWidth(w);
  if (ImGui::Combo(Tr(T_AudioBackend), &ab, "WASAPI\0DirectSound\0")) {
    a.s.audio_backend = ab ? "directsound" : "wasapi";
    a.dirty = true;
  }
  if (ImGui::Checkbox(Tr(T_Mute), &a.s.mute)) a.dirty = true;

  ImGui::SeparatorText(L("Spiel", "Game"));
  if (ImGui::Checkbox(L("Alle Story-Missionen und Kapitel freischalten",
                        "Unlock all story missions and chapters"), &a.s.unlock_missions))
    a.dirty = true;
  Tooltip(L("Einzelspieler und Splitscreen. Der echte Fortschritt im Spielstand bleibt unverändert.",
            "Single player and split screen. The real progress in the save stays unchanged."));
  if (ImGui::Checkbox(L("DLC-, Demo- und Vorbesteller-Extras (Ausrüstung auf der Disc) freischalten",
                        "Unlock DLC, demo and pre-order extras (gear on the disc)"), &a.s.unlock_extras))
    a.dirty = true;
  if (ImGui::Checkbox(L("Alle Masken, Outfits und Tattoos sofort kaufbar (ohne Rang/Story)",
                        "All masks, outfits and tattoos buyable right away (no rank/story)"), &a.s.cosmetics_buyable))
    a.dirty = true;
  Tooltip(L("Nur kosmetisch: Die Gegenstände erscheinen im Laden und kosten weiterhin Geld; nichts wird geschenkt.",
            "Cosmetic only: the items show up in the shop and still cost money; nothing is given away."));
  if (ImGui::Checkbox(L("Fehleraufnahme: F12 speichert Bildschirmfoto + die letzten 6 Sekunden",
                        "Bug capture: F12 saves a screenshot + the last 6 seconds"), &a.s.bug_capture))
    a.dirty = true;
  Tooltip(L("Für Fehlerberichte. Nimmt nur das Spielfenster auf (ca. 150 MB Arbeitsspeicher) und speichert nach "
            "logs\\captures. Plötzlich grüne Bilder werden zusätzlich automatisch gespeichert.",
            "For bug reports. Records only the game window (about 150 MB of memory) and saves to logs\\captures. "
            "Sudden green frames are also saved automatically."));

  if (a.have_qol) {
    ImGui::SeparatorText(Tr(T_SectionGame));
    ImGui::TextDisabled("%s", Tr(T_Fixes));
    for (const auto& e : kQol) {
      bool v = a.qol.Get(e.key, true);
      if (ImGui::Checkbox(L(e.de, e.en), &v)) { a.qol.Set(e.key, v); a.dirty = true; }
      Tooltip(L(e.tip_de, e.tip_en));
    }
  }

  ImGui::Spacing();
  if (ImGui::Button(Tr(T_Save), ImVec2(ImGui::GetFontSize() * 8, 0))) a.Save();
  ImGui::SameLine();
  if (a.saved_flash > 0) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s", Tr(T_Saved));
  else ImGui::TextDisabled("%s", Tr(T_RestartNote));
}

struct BindAction { const char* key; const char* de; const char* en; };
const BindAction kBindActions[] = {
    {"lstick_up", "Vorwärts", "Forward"}, {"lstick_down", "Rückwärts", "Back"},
    {"lstick_left", "Links", "Left"}, {"lstick_right", "Rechts", "Right"},
    {"lstick_press", "Linker Stick drücken (L3)", "Left stick press (L3)"},
    {"rstick_up", "Kamera hoch", "Camera up"}, {"rstick_down", "Kamera runter", "Camera down"},
    {"rstick_left", "Kamera links", "Camera left"}, {"rstick_right", "Kamera rechts", "Camera right"},
    {"rstick_press", "Rechter Stick drücken (R3)", "Right stick press (R3)"},
    {"left_trigger", "LT (Zielen)", "LT (aim)"}, {"right_trigger", "RT (Schießen)", "RT (fire)"},
    {"left_shoulder", "LB", "LB"}, {"right_shoulder", "RB", "RB"},
    {"a", "A", "A"}, {"b", "B", "B"}, {"x", "X", "X"}, {"y", "Y", "Y"},
    {"dpad_up", "Steuerkreuz hoch", "D-pad up"}, {"dpad_down", "Steuerkreuz runter", "D-pad down"},
    {"dpad_left", "Steuerkreuz links", "D-pad left"}, {"dpad_right", "Steuerkreuz rechts", "D-pad right"},
    {"back", "Back", "Back"}, {"start", "Start", "Start"},
};
struct HotkeyAction { const char* key; const char* de; const char* en; const char* fallback; };
const HotkeyAction kHotkeys[] = {
    {"port_menu", "Port-Menü im Spiel", "Port menu in game", "F4"},
    {"bravo_mode", "Bravo-Modus an/aus", "Bravo mode on/off", "F9"},
    {"partner_gear", "Eigene Partner-Ausrüstung an/aus", "Custom partner gear on/off", "F6"},
};

ao4json::Value& JsonObject(ao4json::Value& parent, const char* key) {
  if (const auto* v = parent.Get(key); !v || v->type != ao4json::Value::Type::Object) parent.Set(key, ao4json::Value::Obj());
  for (auto& [k, v] : parent.obj) if (k == key) return v;
  return parent;  // not reached
}

void BindRow(App& a, const char* id, const char* label, std::string value, bool hotkey) {
  ImGui::PushID(id);
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(label);
  ImGui::TableNextColumn();
  const std::string target = std::string(hotkey ? "hk:" : "kb:") + id;
  if (a.capture.target == target) ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "%s", L("Taste drücken... (Esc = abbrechen)", "Press a key... (Esc = cancel)"));
  else ImGui::TextUnformatted(value.empty() ? L("(nicht belegt)", "(unbound)") : value.c_str());
  ImGui::TableNextColumn();
  ImGui::BeginDisabled(a.capture.Active());
  if (ImGui::SmallButton(L("Ändern", "Change"))) a.capture.Start(target, false, hotkey);
  if (!hotkey) {
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) a.capture.Start(target, true, false);
    Tooltip(L("Weitere Taste für dieselbe Aktion", "Another key for the same action"));
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      JsonObject(a.bindings, "keybinds").Set(id, ao4json::Value::Str(""));
      a.dirty = true;
    }
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

void PageControls(App& a) {
  const float w = ImGui::GetFontSize() * 14;
  ImGui::SeparatorText(L("Maus", "Mouse"));
  float factor = float(a.mouse.radians_per_count / kMouseBase);
  ImGui::SetNextItemWidth(w);
  if (ImGui::SliderFloat(Tr(T_MouseSensitivity), &factor, 0.2f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
    a.mouse.radians_per_count = double(factor) * kMouseBase;
    a.dirty = true;
  }
  if (ImGui::Checkbox(Tr(T_InvertY), &a.mouse.invert_y)) a.dirty = true;
  ImGui::SeparatorText(L("Geschütze (MG, Fahrzeuge)", "Mounted guns (MG, vehicles)"));
  float th = float(a.mouse.turret_horizontal), tv = float(a.mouse.turret_vertical);
  ImGui::SetNextItemWidth(w);
  if (ImGui::SliderFloat(L("Geschütz horizontal", "Gun horizontal"), &th, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
    a.mouse.turret_horizontal = th;
    a.dirty = true;
  }
  ImGui::SetNextItemWidth(w);
  if (ImGui::SliderFloat(L("Geschütz vertikal", "Gun vertical"), &tv, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
    a.mouse.turret_vertical = tv;
    a.dirty = true;
  }
  if (ImGui::Checkbox(L("Geschütz Y-Achse umkehren", "Invert gun Y axis"), &a.mouse.turret_invert_y)) a.dirty = true;
  if (a.have_bindings) {
    const auto* st = a.bindings.Get("mnk_sensitivity");
    float stick = st && st->type == ao4json::Value::Type::Number ? float(st->n) : 3.0f;
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderFloat(L("Empfindlichkeit im Stick-Modus (F2)", "Sensitivity in stick mode (F2)"), &stick, 0.5f, 10.0f, "%.1f")) {
      a.bindings.Set("mnk_sensitivity", ao4json::Value::Num(std::round(stick * 10) / 10));
      a.dirty = true;
    }
  }

  if (!a.have_bindings) return;
  // Capture in progress: apply the pressed key.
  if (a.capture.Active()) {
    std::string key;
    const std::string target = a.capture.target;
    if (a.capture.Poll(a.hwnd, key)) {
      if (!key.empty()) {
        const std::string id = target.substr(3);
        if (target.rfind("hk:", 0) == 0) {
          if (!HotkeyCapable(key)) a.controls_msg = L("Diese Taste geht hier nicht (nur einzelne Tasten wie F5, G, 7, Einfg).", "This key does not work here (single keys like F5, G, 7, Insert only).");
          else {
            JsonObject(a.bindings, "hotkeys").Set(id, ao4json::Value::Str(key));
            a.controls_msg = ReservedKeyUse(key) ? std::string(L("Achtung, Taste ist schon belegt: ", "Note, key already used: ")) + ReservedKeyUse(key) : "";
          }
        } else {
          auto& kb = JsonObject(a.bindings, "keybinds");
          const auto* cur = kb.Get(id);
          std::string v = a.capture.append && cur && cur->type == ao4json::Value::Type::String ? cur->s : "";
          if (!v.empty()) v += ",";
          kb.Set(id, ao4json::Value::Str(v + key));
          a.controls_msg = ReservedKeyUse(key) ? std::string(L("Achtung, Taste ist schon belegt: ", "Note, key already used: ")) + ReservedKeyUse(key) : "";
        }
        a.dirty = true;
      }
      a.capture.target.clear();
    }
  }
  ImGui::SeparatorText(L("Tasten (Port)", "Keys (port)"));
  if (ImGui::BeginTable("hotkeys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    auto& hk = JsonObject(a.bindings, "hotkeys");
    for (const auto& h : kHotkeys) {
      const auto* v = hk.Get(h.key);
      BindRow(a, h.key, L(h.de, h.en), v && v->type == ao4json::Value::Type::String && !v->s.empty() ? v->s : h.fallback, true);
    }
    ImGui::EndTable();
  }
  ImGui::TextDisabled("%s", L("Fest: F1 Splitscreen-Tastatur, F2 Maus direkt/Stick, F3 Leistungsanzeige, F12 Fehlerstelle markieren.",
                              "Fixed: F1 split-screen keyboard, F2 direct mouse/stick, F3 performance overlay, F12 mark a bug spot."));
  ImGui::SeparatorText(L("Tastenbelegung (Controller-Tasten)", "Key bindings (controller buttons)"));
  if (ImGui::BeginTable("keybinds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    auto& kb = JsonObject(a.bindings, "keybinds");
    for (const auto& b : kBindActions) {
      const auto* v = kb.Get(b.key);
      BindRow(a, b.key, L(b.de, b.en), v && v->type == ao4json::Value::Type::String ? v->s : "", false);
    }
    ImGui::EndTable();
  }
  if (!a.controls_msg.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "%s", a.controls_msg.c_str());
  ImGui::Spacing();
  if (ImGui::Button(Tr(T_Save), ImVec2(ImGui::GetFontSize() * 8, 0))) a.Save();
  ImGui::SameLine();
  if (a.saved_flash > 0) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s", Tr(T_Saved));
  else ImGui::TextDisabled("%s", Tr(T_RestartNote));
}

template <size_t N>
bool GearCombo(const char* label, uint32_t& id, const ao4partner::Item (&items)[N]) {
  const char* cur = "?";
  for (const auto& it : items) if (it.id == id) cur = it.name;
  bool changed = false;
  if (ImGui::BeginCombo(label, cur, ImGuiComboFlags_HeightLarge)) {
    for (const auto& it : items) {
      ImGui::PushID(int(it.id));
      if (ImGui::Selectable(it.name, it.id == id)) { id = it.id; changed = true; }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

ImTextureID IconTexture(App& a, uint32_t id, ImVec2& size) {
  auto it = a.icon_tex.find(id);
  if (it == a.icon_tex.end()) {
    App::IconTex t;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (ao4icons::LoadRgba(ao4icons::IconPath(a.IconsDir(), id), w, h, rgba)) {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = UINT(w);
      d.Height = UINT(h);
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_IMMUTABLE;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA init{rgba.data(), UINT(w * 4), 0};
      ID3D11Texture2D* tex = nullptr;
      if (SUCCEEDED(g_dev->CreateTexture2D(&d, &init, &tex))) {
        g_dev->CreateShaderResourceView(tex, nullptr, &t.srv);
        tex->Release();
        t.w = w;
        t.h = h;
      }
    }
    it = a.icon_tex.emplace(id, t).first;
  }
  if (!it->second.srv) return ImTextureID{};
  size.y = size.x * float(it->second.h) / float(it->second.w);
  return reinterpret_cast<ImTextureID>(it->second.srv);
}

// Picture grid of one gear category; falls back to names without pictures.
template <size_t N>
bool GearGrid(App& a, const char* label, uint32_t& id, const ao4partner::Item (&items)[N]) {
  ImGui::SeparatorText(label);
  bool changed = false;
  const float cell = ImGui::GetFontSize() * 4.2f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const ImGuiStyle& st = ImGui::GetStyle();
  const int per_row = std::max(1, int((avail + st.ItemSpacing.x) / (cell + st.FramePadding.x * 2 + st.ItemSpacing.x)));
  const char* current = "?";
  for (size_t i = 0; i < N; ++i) {
    const auto& it = items[i];
    if (it.id == id) current = it.name;
    ImGui::PushID(int(it.id));
    ImVec2 size(cell, cell);
    const ImTextureID tex = IconTexture(a, it.id, size);
    const bool selected = it.id == id;
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.95f, 0.62f, 0.25f, 1) : ImVec4(0.16f, 0.16f, 0.18f, 1));
    bool clicked;
    if (tex) clicked = ImGui::ImageButton("##g", tex, size);
    else clicked = ImGui::Button(it.name, ImVec2(cell, cell));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.name);
    if (clicked) { id = it.id; changed = true; }
    ImGui::PopID();
    if ((i + 1) % size_t(per_row) != 0 && i + 1 < N) ImGui::SameLine();
  }
  ImGui::Text("%s: %s", L("Gewählt", "Selected"), current);
  return changed;
}

void PagePartner(App& a) {
  ImGui::SeparatorText(L("Bravo-Modus", "Bravo mode"));
  if (ImGui::Checkbox(L("Als Bravo spielen (KI-Partner ist Alpha)", "Play as Bravo (AI partner is Alpha)"), &a.partner.bravo))
    a.partner.Save(a.F(L"partner-active.ini"));
  ImGui::TextDisabled("%s", L("Nur Einzelspieler. Im Spiel auch mit F9 bzw. im F4-Menü; gilt ab dem nächsten Laden.",
                              "Single player only. In game also F9 or the F4 menu; applies from the next load."));
  ImGui::SeparatorText(L("Ausrüstung des KI-Partners", "AI partner gear"));
  if (ImGui::Checkbox(L("Eigene Ausrüstung verwenden", "Use custom gear"), &a.partner.active))
    a.partner.Save(a.F(L"partner-active.ini"));
  ImGui::TextDisabled("%s", L("Wird sofort gespeichert; ein laufendes Spiel übernimmt die Auswahl nach etwa einer Sekunde (F6 = an/aus).",
                              "Saved right away; a running game picks it up after about a second (F6 = on/off)."));
  if (a.icons_ready == 0)
    ImGui::TextDisabled("%s", L("Vorschaubilder entstehen nach der Ersteinrichtung aus deinen Spieldateien.",
                                "Preview pictures are made from your game files after the first-time setup."));
  bool changed = false;
  changed |= GearGrid(a, L("Maske", "Mask"), a.gear.mask, ao4partner::kMasks);
  changed |= GearGrid(a, L("Outfit", "Outfit"), a.gear.outfit, ao4partner::kOutfits);
  changed |= GearGrid(a, L("Tattoo", "Tattoo"), a.gear.tattoo, ao4partner::kTattoos);
  if (changed) a.gear.Save(a.F(L"partner-equipment.ini"));
}

void ImportSaveDialog(App& a, const ao4cfg::Profile& p) {
  wchar_t file[1024] = {};
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.lpstrFilter = L"PROFILE_OPTIONS\0PROFILE_OPTIONS\0*.*\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = 1024;
  ofn.lpstrTitle = L"PROFILE_OPTIONS (...\\454109AB\\00000001\\PROFILE_OPTIONS\\PROFILE_OPTIONS)";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return;
  std::wstring err;
  a.profile_msg = ao4cfg::ImportSave(file, a.paths.profiles, p.xuid, err)
                      ? std::string(L("Spielstand übernommen für ", "Save imported for ")) + p.name
                      : std::string(L("Import fehlgeschlagen: ", "Import failed: ")) + Narrow(err);
}

void PageProfiles(App& a) {
  ImGui::SeparatorText(Tr(T_TabProfiles));
  if (ImGui::BeginTable("profiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH,
                        ImVec2(0, ImGui::GetFontSize() * 10))) {
    ImGui::TableSetupColumn(Tr(T_ProfileName));
    ImGui::TableSetupColumn(Tr(T_ProfileXuid));
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (int i = 0; i < int(a.profiles.profiles.size()); ++i) {
      const auto& p = a.profiles.profiles[size_t(i)];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      if (ImGui::Selectable(p.name.c_str(), a.sel == i, ImGuiSelectableFlags_SpanAllColumns)) {
        a.sel = i;
        Copy(a.rename, sizeof(a.rename), p.name);
        a.profile_msg.clear();
      }
      ImGui::PopID();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", p.xuid.c_str());
      ImGui::TableNextColumn();
      if (p.id == a.s.slot1) ImGui::TextUnformatted(Tr(T_Slot1));
      else if (p.id == a.s.slot2) ImGui::TextUnformatted(Tr(T_Slot2));
    }
    ImGui::EndTable();
  }
  const float w = ImGui::GetFontSize() * 12;
  ImGui::SetNextItemWidth(w);
  ImGui::InputTextWithHint("##new", Tr(T_ProfileName), a.new_name, 16);
  ImGui::SameLine();
  ImGui::BeginDisabled(a.new_name[0] == 0);
  if (ImGui::Button(Tr(T_ProfileNew))) {
    a.profiles.Create(a.new_name);
    a.new_name[0] = 0;
    a.profiles.Save(a.F(L"profiles.ini"));
  }
  ImGui::EndDisabled();
  if (a.sel >= 0 && a.sel < int(a.profiles.profiles.size())) {
    auto& p = a.profiles.profiles[size_t(a.sel)];
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("##rename", a.rename, 16);
    ImGui::SameLine();
    ImGui::BeginDisabled(a.rename[0] == 0);
    if (ImGui::Button(Tr(T_ProfileRename))) {
      std::string clean;
      for (const char* c = a.rename; *c; ++c) if (uint8_t(*c) >= 0x20 && *c != '|' && *c != ';') clean += *c;
      if (!clean.empty()) p.name = clean.substr(0, 15);
      a.profiles.Save(a.F(L"profiles.ini"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(L("Spielstand importieren...", "Import save..."))) ImportSaveDialog(a, p);
    Tooltip(L("Datei PROFILE_OPTIONS eines anderen Spielstands wählen (z.B. aus Xenia: content\\<XUID>\\454109AB\\00000001\\PROFILE_OPTIONS). Der alte Spielstand bleibt als .bak erhalten.",
              "Choose the PROFILE_OPTIONS file of another save (e.g. from Xenia: content\\<XUID>\\454109AB\\00000001\\PROFILE_OPTIONS). The old save is kept as .bak."));
    ImGui::SameLine();
    if (ImGui::Button(Tr(T_ProfileDelete))) {
      if (p.id == a.s.slot1 || p.id == a.s.slot2) {
        a.profile_msg = Tr(T_ProfileInUse);
      } else {
        a.profiles.Remove(p.id);
        a.profiles.Save(a.F(L"profiles.ini"));
        a.sel = -1;
        a.profile_msg = Tr(T_ProfileKeepSaves);
      }
    }
  }
  if (!a.profile_msg.empty()) ImGui::TextWrapped("%s", a.profile_msg.c_str());
  ImGui::Spacing();
  ImGui::TextDisabled("%s: %s", Tr(T_ProfileSaves), Narrow(a.paths.profiles).c_str());
}

void PackLogs(App& a) {
  // logs + config (no profiles: they hold player names) into a ZIP next to the launcher.
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf(name, 64, L"ao4-bugreport-%04u%02u%02u-%02u%02u.zip", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
  const std::wstring zip = a.paths.root + L"\\" + name;
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  CreateDirectoryW(a.paths.logs.c_str(), nullptr);
  std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -a -c -f \"" + zip + L"\" -C \"" + a.paths.root +
                     L"\" logs config";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  if (FileExists(zip)) {
    a.status = std::string(Tr(T_LogsPacked)) + ": " + Narrow(zip);
    const std::wstring arg = L"/select,\"" + zip + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
  }
}

void PageHelp(App& a) {
  ImGui::SeparatorText(Tr(T_TabHelp));
  ImGui::Text("%s %s", Tr(T_HelpVersion), AO4_PORT_VERSION);
  ImGui::Spacing();
  if (ImGui::Button(Tr(T_HelpReadme))) {
    const std::wstring f = a.paths.root + (GetLanguage() == Lang::En ? L"\\README.txt" : L"\\LIESMICH.txt");
    ShellExecuteW(nullptr, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::SameLine();
  if (ImGui::Button(Tr(T_HelpLogs))) {
    CreateDirectoryW(a.paths.logs.c_str(), nullptr);
    ShellExecuteW(nullptr, L"explore", a.paths.logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::SameLine();
  if (ImGui::Button(Tr(T_HelpZip))) PackLogs(a);
  if (!a.status.empty()) ImGui::TextWrapped("%s", a.status.c_str());
  ImGui::Spacing();
  ImGui::BeginDisabled(a.session.Running());
  if (ImGui::Button(Tr(T_SetupReinstall))) { a.show_setup = true; a.setup = App::Idle; }
  ImGui::EndDisabled();
  ImGui::Spacing();
  if (ImGui::CollapsingHeader(L("Startparameter (für Fehlerberichte)", "Start options (for bug reports)"))) {
    std::string line;
    for (const auto& arg : ao4launch::BuildArguments(a.paths, a.s, a.profiles, a.paths.logs + L"\\game.log"))
      line += Narrow(arg) + " ";
    ImGui::TextWrapped("%s", line.c_str());
  }
}

void Frame(App& a) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::TextUnformatted(Tr(T_Title));
  ImGui::Separator();
  if (a.session.game && !a.session.Running()) {
    a.session.Close();
    a.status = Tr(T_GameExited);
  }
  if (!a.installed || a.show_setup) {
    PageSetup(a);
  } else if (ImGui::BeginTabBar("tabs")) {
    int index = 0;
    auto tab = [&](const char* name, void (*page)(App&)) {
      const bool select = a.initial_tab == index++;
      if (ImGui::BeginTabItem(name, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0)) {
        ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None);
        page(a);
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    };
    tab(Tr(T_TabPlay), PagePlay);
    tab(Tr(T_TabSettings), PageSettings);
    tab(L("Steuerung", "Controls"), PageControls);
    tab(L("Partner", "Partner"), PagePartner);
    tab(Tr(T_TabProfiles), PageProfiles);
    tab(Tr(T_TabHelp), PageHelp);
    a.initial_tab = -1;
    ImGui::EndTabBar();
  }
  ImGui::End();
  if (a.saved_flash > 0) a.saved_flash -= ImGui::GetIO().DeltaTime;
}

void Style(float scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowPadding = ImVec2(18, 14);
  st.FramePadding = ImVec2(8, 5);
  st.ItemSpacing = ImVec2(10, 8);
  st.FrameRounding = 4;
  st.TabRounding = 4;
  st.GrabRounding = 4;
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.11f, 1);
  c[ImGuiCol_Button] = ImVec4(0.55f, 0.32f, 0.12f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.70f, 0.42f, 0.16f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.48f, 0.18f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.62f, 0.25f, 1);
  c[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.52f, 0.20f, 1);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.62f, 0.25f, 1);
  c[ImGuiCol_Tab] = ImVec4(0.22f, 0.16f, 0.11f, 1);
  c[ImGuiCol_TabHovered] = ImVec4(0.70f, 0.42f, 0.16f, 1);
  c[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.32f, 0.12f, 1);
  c[ImGuiCol_Header] = ImVec4(0.40f, 0.25f, 0.12f, 1);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.32f, 0.12f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.20f, 1);
  st.ScaleAllSizes(scale);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int show) {
  // --autostart: start the game with the saved settings right away (automated tests).
  const bool autostart = cmdline && wcsstr(cmdline, L"--autostart");
  ImGui_ImplWin32_EnableDpiAwareness();
  // One launcher at a time (two would write the same config files).
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\AO4NativePortLauncher");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(L"AO4NativeLauncher", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
    return 0;
  }

  const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW),
                 nullptr, nullptr, L"AO4NativeLauncher", nullptr};
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"Army of Two: The Devil's Cartel - Native Port", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, int(900 * scale), int(700 * scale), nullptr, nullptr, inst,
                            nullptr);
  if (!CreateDevice(hwnd)) {
    MessageBoxW(hwnd, L"Direct3D 11 konnte nicht initialisiert werden.\nDirect3D 11 could not be initialized.",
                L"AO4 Launcher", MB_ICONERROR);
    DestroyDevice();
    return 1;
  }
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  Style(scale);
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  const std::string font = Narrow(std::wstring(windir) + L"\\Fonts\\segoeui.ttf");
  if (FileExists(Widen(font))) io.Fonts->AddFontFromFileTTF(font.c_str(), 19.0f * scale);  // Latin-1: umlauts
  else io.FontGlobalScale = scale;
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_dev, g_ctx);

  App app;
  app.hwnd = hwnd;
  if (const wchar_t* t = cmdline ? wcsstr(cmdline, L"--tab=") : nullptr) app.initial_tab = _wtoi(t + 6);
  app.Load();
  if (autostart && app.installed && app.profiles.Find(app.s.slot1)) StartGame(app);

  bool quit = false;
  while (!quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) quit = true;
    }
    if (quit) break;
    if (IsIconic(hwnd)) {  // minimized while the game runs: no rendering
      Sleep(50);
      if (app.session.game && !app.session.Running()) { app.session.Close(); app.status = Tr(T_GameExited); ShowWindow(hwnd, SW_RESTORE); }
      continue;
    }
    if (g_resize_w) {
      ReleaseRtv();
      g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
      g_resize_w = g_resize_h = 0;
      CreateRtv();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Frame(app);
    ImGui::Render();
    const float clear[4] = {0.10f, 0.10f, 0.11f, 1};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
  }

  app.cancel = true;
  app.Join();
  if (app.dirty) app.Save();
  // A running game keeps going; the host's Blaze server ends with its own time limit.
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  DestroyDevice();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  if (single) CloseHandle(single);
  return 0;
}
