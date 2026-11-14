// ao4_icon_check <disc dir> <out dir> [<reference dir with category-id.png>]
// Builds the partner-gear icons from the game files and compares them with reference pictures.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "partner_catalog.h"
#include "partner_icons.h"

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::fwprintf(stderr, L"usage: ao4_icon_check <disc dir> <out dir> [<reference dir>]\n");
    return 2;
  }
  std::vector<ao4icons::Request> items;
  struct Cat { const wchar_t* name; const ao4partner::Item* items; size_t n; };
  const Cat cats[] = {{L"mask", ao4partner::kMasks, std::size(ao4partner::kMasks)},
                      {L"outfit", ao4partner::kOutfits, std::size(ao4partner::kOutfits)},
                      {L"tattoo", ao4partner::kTattoos, std::size(ao4partner::kTattoos)}};
  for (const auto& c : cats)
    for (size_t i = 0; i < c.n; ++i) items.push_back({c.items[i].id, c.items[i].icon});
  const DWORD t0 = GetTickCount();
  std::wstring error;
  const int built = ao4icons::Build(argv[1], argv[2], items, error);
  std::wprintf(L"icons %d/%zu in %lu ms %s\n", built, items.size(), GetTickCount() - t0, error.c_str());
  if (argc < 4) return built == int(items.size()) ? 0 : 1;
  int same = 0, maxdiff = 0, compared = 0;
  for (const auto& c : cats) {
    for (size_t i = 0; i < c.n; ++i) {
      int w1, h1, w2, h2;
      std::vector<uint8_t> a, b;
      const std::wstring ref = std::wstring(argv[3]) + L"\\" + c.name + L"-" + std::to_wstring(c.items[i].id) + L".png";
      if (!ao4icons::LoadRgba(ao4icons::IconPath(argv[2], c.items[i].id), w1, h1, a) || !ao4icons::LoadRgba(ref, w2, h2, b) ||
          w1 != w2 || h1 != h2) {
        std::wprintf(L"MISMATCH size/load %s\n", ref.c_str());
        continue;
      }
      ++compared;
      int d = 0;
      for (size_t k = 0; k < a.size(); ++k) d = (std::max)(d, std::abs(int(a[k]) - int(b[k])));
      if (d == 0) ++same;
      maxdiff = (std::max)(maxdiff, d);
    }
  }
  std::wprintf(L"compared %d, identical %d, max channel difference %d\n", compared, same, maxdiff);
  return 0;
}
