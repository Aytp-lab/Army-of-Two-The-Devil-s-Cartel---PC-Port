// Partner-gear preview pictures, made on the player's PC from their own game files (nothing is
// shipped): the menu icons of masks, outfits and tattoos are textures in the front-end
// superbundle (AO4_Levels/AO4Frontend/AO4Frontend.sb, texture headers) and TexturesOnDemand
// (pixel data, found through the XOR-obfuscated TexturesOnDemand.toc). They are untiled,
// BC3-decoded and stored as PNG in <game>\icons\<asset id>.png.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ao4icons {

struct Request {
  uint32_t id;       // partner catalog asset id (file name)
  const char* icon;  // texture asset name, e.g. ao4_customization/masks/thumbnails/mask_alpha
};

// Builds every missing icon. Returns the number of icons present afterwards.
int Build(const std::wstring& disc_dir, const std::wstring& out_dir, const std::vector<Request>& items,
          std::wstring& error);

std::wstring IconPath(const std::wstring& out_dir, uint32_t id);

// RGBA8 pixels of a PNG (for the launcher's preview textures).
bool LoadRgba(const std::wstring& file, int& width, int& height, std::vector<uint8_t>& rgba);

}  // namespace ao4icons
