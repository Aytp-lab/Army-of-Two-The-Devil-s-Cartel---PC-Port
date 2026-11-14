#pragma once
#include <string_view>

// Normalize identity directionally. Reapplying must not toggle Alpha back to Bravo.
inline std::string_view Ao4AppliedIdentity(std::string_view asset, bool partner, bool bravo) {
  const bool alpha = partner == bravo;
  if (asset == "Gameplay/Spawners/AlphaHeadMeshUnlockAsset" ||
      asset == "Gameplay/Spawners/BravoHeadMeshUnlockAsset") {
    return alpha ? "Gameplay/Spawners/AlphaHeadMeshUnlockAsset"
                 : "Gameplay/Spawners/BravoHeadMeshUnlockAsset";
  }
  if (partner && (asset == "AO4_Customization/Masks/AlphaMask" ||
                  asset == "AO4_Customization/Masks/BravoMask")) {
    return alpha ? "AO4_Customization/Masks/AlphaMask" : "AO4_Customization/Masks/BravoMask";
  }
  return asset;
}
inline bool Ao4DefaultPartnerBody(std::string_view asset) {
  return asset == "Gameplay/Spawners/AlphaMeshUnlockAsset" ||
         asset == "Gameplay/Spawners/BravoMeshUnlockAsset";
}
