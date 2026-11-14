// In-game port menu on F4 (public test build, env AO4_PORT_MENU=1 from AO4Launcher.exe).
// Replaces the runtime's generic settings dialog on F4. While open, game input (keyboard,
// mouse, controller) is paused and the mouse cursor is free; the game itself keeps running.
//   immediately:        mouse sensitivity, invert Y (native-mouse.json, read by the main DLL),
//                       mute (runtime option audio_mute)
//   info:               Bravo mode / custom partner gear state and keys
//   next game start:    game fixes and 60 FPS (quality-of-life.ini)
// Texts German / English (AO4_LAUNCHER_LANG).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include "../../ao4-frontend/src/config.h"
#include "../../ao4-frontend/src/partner_catalog.h"
#include "../../ao4-frontend/src/partner_icons.h"

#include <map>
#include <rex/ui/immediate_drawer.h>
#include "status_overlay.h"

namespace ao4menu {

inline bool Enabled() {
  wchar_t v[8] = {};
  return GetEnvironmentVariableW(L"AO4_PORT_MENU", v, 8) && v[0] == L'1';
}
inline bool English() {
  wchar_t v[8] = {};
  return GetEnvironmentVariableW(L"AO4_LAUNCHER_LANG", v, 8) && v[0] == L'e';
}
inline const char* L(const char* de, const char* en) {
  static const bool en_ = English();
  return en_ ? en : de;
}
inline std::wstring Env(const wchar_t* name) {
  wchar_t v[32768] = {};
  return GetEnvironmentVariableW(name, v, 32768) ? std::wstring(v) : std::wstring();
}

inline ImFont*& MenuFont() {
  static ImFont* font = nullptr;
  return font;
}
// Called from ReXApp::OnConfigureFonts: Segoe UI (Latin-1, umlauts) for the menu only.
inline void AddMenuFont(ImFontAtlas* atlas) {
  if (!Enabled()) return;
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  const std::string path = ao4cfg::Narrow(std::wstring(windir) + L"\\Fonts\\segoeui.ttf");
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0};
  MenuFont() = atlas->AddFontFromFileTTF(path.c_str(), 26.0f, nullptr, ranges);
}

constexpr double kMouseBase = 0.0012418426900000002;

struct QolEntry { const char* key; const char* de; const char* en; };
inline const QolEntry kQol[] = {
    {"dialogzweig_fix", "Dialogzweig-Fix", "Dialogue branch fix"},
    {"mg_ansage_fix", "MG-Ansage-Fix", "Machine-gun callout fix"},
    {"ausruestung_ladefix", "Ausrüstungs-Ladefix", "Equipment load fix"},
};

class Menu : public rex::ui::ImGuiDialog {
 public:
  Menu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, std::function<void()> close)
      : ImGuiDialog(drawer), immediate_(immediate), close_(std::move(close)) {
    icon_dir_ = Env(L"AO4_ICON_DIR");
    mouse_path_ = Env(L"AO4_MOUSE_SETTINGS");
    qol_path_ = Env(L"AO4_QOL_SETTINGS");
    if (!mouse_path_.empty()) mouse_.Load(mouse_path_);
    have_qol_ = !qol_path_.empty() && qol_.Load(qol_path_);
    gear_path_ = Env(L"AO4_PARTNER_CONFIG");
    if (!gear_path_.empty() && !gear_.Load(gear_path_)) gear_path_.clear();
    mute_ = rex::cvar::GetFlagByName("audio_mute") == "true";
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (MenuFont()) ImGui::PushFont(MenuFont());
    // Scale with the window height (font made for 1080p), so the menu fits 720p and 4K.
    const float scale = std::clamp(io.DisplaySize.y / 1080.0f, 0.55f, 2.0f);
    const float fs = ImGui::GetFontSize() * scale;
    const float width = std::min(fs * 26, io.DisplaySize.x * 0.95f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, io.DisplaySize.y * 0.95f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (ImGui::Begin(L("Port-Menü###ao4menu", "Port menu###ao4menu"), &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::SetWindowFontScale(scale);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", L("Eingaben ans Spiel sind pausiert, das Spiel läuft weiter (vorher mit Start pausieren).",
                                  "Game input is paused, the game keeps running (pause with Start first)."));
      ImGui::PopTextWrapPos();

      if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem(L("Allgemein", "General"))) {
          DrawGeneral(fs);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(L("Partner", "Partner"))) {
          DrawPartner(fs);
          ImGui::EndTabItem();
        }
        if (have_qol_ && ImGui::BeginTabItem(L("Ab Neustart", "Next start"))) {
          for (const auto& e : kQol) {
            bool v = qol_.Get(e.key, true);
            if (ImGui::Checkbox(L(e.de, e.en), &v)) { qol_.Set(e.key, v); qol_.Save(qol_path_); }
          }
          bool fps = qol_.Get("bildrate_60", false);
          if (ImGui::Checkbox(L("60 FPS (experimentell)", "60 FPS (experimental)"), &fps)) {
            qol_.Set("bildrate_60", fps);
            qol_.Save(qol_path_);
          }
          ImGui::TextDisabled("%s", L("Weitere Optionen im Launcher.", "More options in the launcher."));
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
      ImGui::Spacing();
      if (ImGui::Button(L("Weiterspielen (F4)", "Resume (F4)"), ImVec2(fs * 10, 0))) open = false;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
    }
    ImGui::End();
    if (MenuFont()) ImGui::PopFont();
    if (!open && close_) close_();
  }

 private:
  void DrawGeneral(float fs) {
    ImGui::SeparatorText(L("Maus", "Mouse"));
    float factor = float(mouse_.radians_per_count / kMouseBase);
    ImGui::SetNextItemWidth(fs * 12);
    if (ImGui::SliderFloat(L("Maus-Empfindlichkeit", "Mouse sensitivity"), &factor, 0.2f, 5.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic)) {
      mouse_.radians_per_count = double(factor) * kMouseBase;
      mouse_dirty_ = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveMouse();
    if (ImGui::Checkbox(L("Maus Y-Achse umkehren", "Invert mouse Y axis"), &mouse_.invert_y)) {
      mouse_dirty_ = true;
      SaveMouse();
    }
    ImGui::SeparatorText(L("Geschütze", "Mounted guns"));
    float th = float(mouse_.turret_horizontal), tv = float(mouse_.turret_vertical);
    ImGui::SetNextItemWidth(fs * 12);
    if (ImGui::SliderFloat(L("Horizontal", "Horizontal"), &th, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
      mouse_.turret_horizontal = th;
      mouse_dirty_ = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveMouse();
    ImGui::SetNextItemWidth(fs * 12);
    if (ImGui::SliderFloat(L("Vertikal", "Vertical"), &tv, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
      mouse_.turret_vertical = tv;
      mouse_dirty_ = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveMouse();
    if (ImGui::Checkbox(L("Geschütz Y-Achse umkehren", "Invert gun Y axis"), &mouse_.turret_invert_y)) {
      mouse_dirty_ = true;
      SaveMouse();
    }
    ImGui::SeparatorText(L("Ton", "Audio"));
    if (ImGui::Checkbox(L("Ton stumm", "Mute audio"), &mute_))
      rex::cvar::SetFlagByName("audio_mute", mute_ ? "true" : "false");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", L("F2: Maus direkt / Stick-Modus   F3: Leistungsanzeige   F12: Fehlerstelle markieren",
                                "F2: direct mouse / stick mode   F3: performance overlay   F12: mark a bug spot"));
    ImGui::PopTextWrapPos();
  }

  void DrawPartner(float fs) {
    Ao4OverlayState st;
    if (!Ao4ReadOverlayState(st)) return;
    static auto toggle = reinterpret_cast<int (*)(int)>(
        GetProcAddress(GetModuleHandleW(L"ao4_main_native.dll"), "AO4_ToggleFeature"));
    bool bravo = st.bravo_mode != 0, gear = st.partner_gear != 0;
    ImGui::BeginDisabled(!toggle);
    if (ImGui::Checkbox(L("Bravo-Modus", "Bravo mode"), &bravo) && toggle) toggle(1);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "[%s]", st.bravo_key);
    ImGui::TextDisabled("%s", L("Gilt ab dem nächsten Laden (Checkpoint, Level).", "Applies from the next load (checkpoint, level)."));
    if (ImGui::Checkbox(L("Eigene Ausrüstung für den KI-Partner", "Custom gear for the AI partner"), &gear) && toggle) toggle(2);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "[%s]", st.gear_key);
    ImGui::EndDisabled();
    if (gear_path_.empty()) return;
    bool changed = false;
    ImGui::BeginChild("##gear", ImVec2(0, fs * 13), ImGuiChildFlags_None);
    changed |= GearGrid(L("Maske", "Mask"), gear_.mask, ao4partner::kMasks, fs);
    changed |= GearGrid(L("Outfit", "Outfit"), gear_.outfit, ao4partner::kOutfits, fs);
    changed |= GearGrid(L("Tattoo", "Tattoo"), gear_.tattoo, ao4partner::kTattoos, fs);
    ImGui::EndChild();
    if (changed) gear_.Save(gear_path_);
  }

  // Preview picture as a runtime texture (UI thread), made by the launcher from the game files.
  ImTextureID Icon(uint32_t id, ImVec2& size) {
    auto it = icons_.find(id);
    if (it == icons_.end()) {
      Tex t;
      int w = 0, h = 0;
      std::vector<uint8_t> rgba;
      if (immediate_ && !icon_dir_.empty() && ao4icons::LoadRgba(ao4icons::IconPath(icon_dir_, id), w, h, rgba)) {
        t.tex = immediate_->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                          rgba.data());
        t.w = w;
        t.h = h;
      }
      it = icons_.emplace(id, std::move(t)).first;
    }
    if (!it->second.tex) return ImTextureID{};
    size.y = size.x * float(it->second.h) / float(it->second.w);
    return reinterpret_cast<ImTextureID>(it->second.tex.get());
  }

  template <size_t N>
  bool GearGrid(const char* label, uint32_t& id, const ao4partner::Item (&items)[N], float fs) {
    ImGui::SeparatorText(label);
    bool changed = false;
    const float cell = fs * 2.6f;
    const ImGuiStyle& st = ImGui::GetStyle();
    const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + st.ItemSpacing.x) /
                                        (cell + st.FramePadding.x * 2 + st.ItemSpacing.x)));
    for (size_t i = 0; i < N; ++i) {
      const auto& it = items[i];
      ImGui::PushID(int(it.id));
      ImVec2 size(cell, cell);
      const ImTextureID tex = Icon(it.id, size);
      const bool selected = it.id == id;
      ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.95f, 0.65f, 0.2f, 1) : ImVec4(0.12f, 0.12f, 0.12f, 1));
      const bool clicked = tex ? ImGui::ImageButton("##g", tex, size) : ImGui::Button(it.name, ImVec2(cell, cell));
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.name);
      if (clicked) { id = it.id; changed = true; }
      ImGui::PopID();
      if ((i + 1) % size_t(per_row) != 0 && i + 1 < N) ImGui::SameLine();
    }
    return changed;
  }

  template <size_t N>
  static bool GearCombo(const char* label, uint32_t& id, const ao4partner::Item (&items)[N]) {
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
  void SaveMouse() {
    if (mouse_dirty_ && !mouse_path_.empty()) mouse_.Save(mouse_path_);
    mouse_dirty_ = false;
  }

  std::function<void()> close_;
  struct Tex { std::unique_ptr<rex::ui::ImmediateTexture> tex; int w = 0, h = 0; };
  rex::ui::ImmediateDrawer* immediate_ = nullptr;
  std::wstring icon_dir_;
  std::map<uint32_t, Tex> icons_;
  std::wstring mouse_path_, qol_path_, gear_path_;
  ao4cfg::PartnerGear gear_;
  ao4cfg::MouseSettings mouse_;
  ao4cfg::QolFile qol_;
  bool have_qol_ = false, mouse_dirty_ = false, mute_ = false;
};

// Owns the menu; F4 bind replaces the runtime's "bind_settings" (F4) when enabled.
class Controller {
 public:
  void Start(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context, rex::ui::ImmediateDrawer* immediate) {
    if (!Enabled()) return;
    immediate_ = immediate;
    drawer_ = drawer;
    context_ = context;
    rex::ui::UnregisterBind("bind_settings");
    rex::ui::RegisterBind("bind_port_menu", "F4", "Toggle the port menu", [this] { Toggle(); });
    // Automated background tests (no window focus, so no key events): AO4_PORT_MENU_TEST=<s>
    // opens the menu once after that many seconds.
    wchar_t t[16] = {};
    if (GetEnvironmentVariableW(L"AO4_PORT_MENU_TEST", t, 16) && _wtoi(t) > 0) {
      const DWORD ms = DWORD(_wtoi(t)) * 1000;
      std::thread([this, ms] {
        Sleep(ms);
        context_->CallInUIThread([this] { if (!menu_) Toggle(); });
      }).detach();
    }
  }
  // After the runtime exists (OnPostSetup): game input is inactive while the menu is open.
  void HookInput(rex::input::InputSystem* input) {
    if (!Enabled() || !input || !drawer_) return;
    input->SetActiveCallback([this] {
      if (open_.load()) return false;
      if (!drawer_->HasDialogs()) return true;
      return !drawer_->GetIO().WantCaptureMouse;
    });
  }
  void Stop() { menu_.reset(); open_ = false; }
  ~Controller() { menu_.release(); }  // without Stop the drawer may already be gone

 private:
  void Toggle() {  // UI thread (key bind)
    if (menu_) {
      menu_.reset();
      open_ = false;
      return;
    }
    menu_ = std::make_unique<Menu>(drawer_, immediate_, [this] {
      context_->CallInUIThreadDeferred([this] { menu_.reset(); open_ = false; });
    });
    open_ = true;
  }

  rex::ui::ImGuiDrawer* drawer_ = nullptr;
  rex::ui::ImmediateDrawer* immediate_ = nullptr;
  rex::ui::WindowedAppContext* context_ = nullptr;
  std::unique_ptr<Menu> menu_;
  std::atomic<bool> open_{false};
};

}  // namespace ao4menu
