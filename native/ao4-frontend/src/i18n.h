// German / English UI texts of the launcher and the in-game F4 menu. Tr(id) returns the text in
// the current language (SetLanguage). Keep both columns in sync when adding entries.
#pragma once

#include <cstddef>

namespace ao4ui {

enum class Lang { De = 0, En = 1 };
void SetLanguage(Lang lang);
Lang GetLanguage();

enum Text : int {
  T_Title, T_TabPlay, T_TabSettings, T_TabProfiles, T_TabHelp,
  T_SetupHeading, T_SetupIntro, T_SetupChooseIso, T_SetupBrowse, T_SetupCheck, T_SetupInstall,
  T_SetupVersionOk, T_SetupVersionBad, T_SetupProgress, T_SetupDone, T_SetupCancel, T_SetupSpace,
  T_ModeSingle, T_ModeSplit, T_ModeOnlineHost, T_ModeOnlineJoin, T_Start, T_Running,
  T_Slot1, T_Slot2, T_HostAddress, T_JoinAddress, T_SplitHint, T_OnlineHint,
  T_SectionGame, T_SectionGraphics, T_SectionAudio, T_SectionControls, T_SectionLanguage,
  T_Fullscreen, T_WindowSize, T_ResolutionScale, T_PresentEffect, T_Anisotropic, T_Vsync,
  T_Fps60, T_Fps60Hint, T_AudioBackend, T_Mute, T_MouseSensitivity, T_InvertY, T_KeybindHint,
  T_Save, T_Saved, T_ProfileNew, T_ProfileRename, T_ProfileDelete, T_ProfileName, T_ProfileSaves,
  T_ProfileInUse, T_HelpLogs, T_HelpZip, T_HelpReadme, T_HelpVersion, T_RestartNote,
  T_MenuTitle, T_MenuResume, T_MenuLive,
  T_UiLanguage, T_GameLanguage, T_ProfileXuid, T_ProfileKeepSaves, T_NoProfile, T_FirewallHint,
  T_GameExited, T_SetupReinstall, T_SetupFolder, T_Fixes, T_LogsPacked, T_Count
};

const char* Tr(Text id);

}  // namespace ao4ui
