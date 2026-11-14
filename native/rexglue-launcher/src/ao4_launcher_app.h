// ao4_launcher - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include "status_overlay.h"
#include "port_menu.h"
void InstallAo4FaultTrace();

class Ao4LauncherApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  void OnPostSetup() override {
    InstallAo4FaultTrace();
    runtime()->kernel_state()->RegisterRecompiledModule(
        "ao4.main.xex", "ao4.main.xex", "ao4_main_native");
    port_menu_.HookInput(static_cast<rex::input::InputSystem*>(runtime()->input_system()));
  }
  void OnConfigureFonts(ImFontAtlas* atlas) override { ao4menu::AddMenuFont(atlas); }
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    status_overlay_.Start(drawer, &app_context());
    port_menu_.Start(drawer, &app_context(), immediate_drawer());
  }
  void OnShutdown() override {
    port_menu_.Stop();
    status_overlay_.Stop();
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ao4LauncherApp>(new Ao4LauncherApp(ctx, "ao4_launcher",
        PPCImageConfig));
  }

  Ao4StatusOverlayController status_overlay_;
  ao4menu::Controller port_menu_;

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
