#pragma once
#include <atomic>
#include <cstdint>
// Cross-thread notification when mission customization data is prepared.
inline std::atomic<uint32_t> ao4_mission_appearance_revision{0};
inline std::atomic<bool> ao4_scavenge_appearance{false};
inline std::atomic<bool> ao4_scavenge_has_mask{false};
// Bravo mode (role, look and voice swap; F9 by default). role_observer.cpp owns the key and
// the saved state; soldier_observer.cpp applies the look swap at spawn only while it is on.
inline std::atomic<bool> ao4_bravo_mode{false};
// Last spawned character template was the main menu's (AO2SoldierSpawnTemplate_FrontEnd).
inline std::atomic<bool> ao4_frontend{false};
// Bravo mode chosen with the key (saved); ao4_bravo_mode takes it over in the main menu, while
// loading and when a level or checkpoint spawns the player and partner templates.
inline std::atomic<bool> ao4_bravo_wanted{false};
// The loaded level has the Drogenlabor stages (set at a fresh template load, cleared when a
// normal customize data follows): the AI gear keeps off the mission body until then.
inline std::atomic<bool> ao4_scavenge_level{false};
// Each human player spawn (mission start, checkpoint, game-over restart): opens the
// QoL equipment check window. Tutorial: its own bodies/masks belong to the level.
inline std::atomic<uint32_t> ao4_player_spawn_epoch{0};
inline std::atomic<bool> ao4_tutorial_level{false};
