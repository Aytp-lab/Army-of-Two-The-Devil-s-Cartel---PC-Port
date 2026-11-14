// Status overlay (top left, main menu only): Bravo mode and custom AI partner gear, each with
// the key from input-bindings.json. The state comes from ao4_main_native.dll
// (AO4_GetOverlayState); the dialog exists only while the menu is shown, so in levels no ImGui
// dialog takes keyboard characters or mouse capture.
#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <windows.h>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "../../rexglue-main/src/ao4_overlay_state.h"

inline bool Ao4ReadOverlayState(Ao4OverlayState& state) {
  static Ao4GetOverlayStateFn get = nullptr;
  if (!get) {
    if (HMODULE module = GetModuleHandleW(L"ao4_main_native.dll")) {
      get = reinterpret_cast<Ao4GetOverlayStateFn>(GetProcAddress(module, "AO4_GetOverlayState"));
    }
    if (!get) return false;
  }
  state = {};
  get(&state);
  state.bravo_key[sizeof(state.bravo_key) - 1] = 0;
  state.gear_key[sizeof(state.gear_key) - 1] = 0;
  return true;
}

class Ao4StatusOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit Ao4StatusOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    Ao4OverlayState state;
    if (!Ao4ReadOverlayState(state) || !state.show) return;
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##ao4_status", nullptr, flags)) {
      ImGui::SetWindowFontScale(1.8f);
      Line("Bravo-Modus", state.bravo_mode != 0, state.bravo_key);
      Line("Custom KI-Partner-Gear", state.partner_gear != 0, state.gear_key);
    }
    ImGui::End();
  }

 private:
  static void Line(const char* label, bool on, const char* key) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(250.0f);
    ImGui::TextColored(on ? ImVec4(0.35f, 0.9f, 0.35f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                       "%s", on ? "AN" : "AUS");
    ImGui::SameLine(320.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "[%s]", key);
  }
};

// Creates and removes the overlay dialog on the UI thread as the menu state changes.
class Ao4StatusOverlayController {
 public:
  void Start(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context) {
    drawer_ = drawer;
    context_ = context;
    thread_ = std::thread([this] {
      bool shown = false;
      while (!stop_) {
        Sleep(250);
        Ao4OverlayState state;
        const bool show = Ao4ReadOverlayState(state) && state.show;
        if (show == shown) continue;
        shown = show;
        context_->CallInUIThread([this, show] {
          if (stop_) return;
          if (show && !dialog_) {
            dialog_ = std::make_unique<Ao4StatusOverlay>(drawer_);
          } else if (!show) {
            dialog_.reset();
          }
        });
      }
    });
  }

  // Without Stop (no orderly shutdown) the drawer may already be gone: do not touch it.
  ~Ao4StatusOverlayController() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    dialog_.release();
  }

  // UI thread, before the drawer goes away.
  void Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    dialog_.reset();
  }

 private:
  rex::ui::ImGuiDrawer* drawer_ = nullptr;
  rex::ui::WindowedAppContext* context_ = nullptr;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::unique_ptr<Ao4StatusOverlay> dialog_;
};
