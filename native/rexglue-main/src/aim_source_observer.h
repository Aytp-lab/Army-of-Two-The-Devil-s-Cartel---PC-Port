#pragma once
#include <cstdint>
struct PPCContext;

// Records only source getters reached inside the scoped human aim query.
void Ao4ObserveAimSource(PPCContext& ctx, uint8_t* base, uint32_t weapon, uint32_t source,
                         bool predicted);

// role_observer.cpp: true while a level HUD screen is on top of the in-level screen stack
// (false in pause, settings, popups, result screens and the front end).
bool Ao4LevelHudOnTop(uint8_t* base);
