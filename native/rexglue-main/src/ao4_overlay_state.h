#pragma once
// Status shown by the launcher's overlay, filled by AO4_GetOverlayState (exported by
// ao4_main_native.dll, role_observer.cpp). Shared by both projects; keep the layout stable.
struct Ao4OverlayState {
  int show;          // main menu (FrontEnd spawned, no level partner)
  int bravo_mode;    // Bravo mode (role, look and voice swap)
  int partner_gear;  // custom AI partner gear (Partner-Customizer)
  char bravo_key[24];
  char gear_key[24];
};
using Ao4GetOverlayStateFn = void (*)(Ao4OverlayState*);
