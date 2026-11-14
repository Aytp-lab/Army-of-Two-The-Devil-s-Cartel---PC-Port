// Command-line check of the first-run setup: ao4_setup_check <iso> [<out_dir>]
// Lists the ISO, verifies the game version and, with out_dir, extracts disc + HD package to
// <out_dir>\disc and <out_dir>\hd.
#include "game_setup.h"

#include <windows.h>

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fwprintf(stderr, L"usage: ao4_setup_check <iso> [<out_dir>]\n");
    return 2;
  }
  ao4setup::IsoInfo info;
  std::wstring error;
  if (!ao4setup::ReadIso(argv[1], info, error)) {
    std::fwprintf(stderr, L"ReadIso: %ls\n", error.c_str());
    return 1;
  }
  uint64_t total = 0;
  for (const auto& f : info.files) total += f.size;
  std::wprintf(L"partition 0x%llX files %zu bytes %llu\n", info.partition_offset, info.files.size(), total);
  const auto v = ao4setup::CheckVersion(argv[1], info);
  std::wprintf(L"version ok=%d default=%hs main=%hs (%ls)\n", v.ok, v.default_xex_sha256.c_str(), v.main_xex_sha256.c_str(),
               v.message.c_str());
  if (argc < 3) return v.ok ? 0 : 3;
  const std::wstring out = argv[2];
  int last = -1;
  const bool ok = ao4setup::Extract(argv[1], info, out + L"\\disc", out + L"\\hd",
      [&](uint64_t done, uint64_t all, const std::wstring& file) {
        const int pct = all ? int(done * 100 / all) : 100;
        if (pct != last) { last = pct; std::wprintf(L"progress %d%% %ls\n", pct, file.c_str()); }
        return true;
      }, error);
  if (!ok) {
    std::fwprintf(stderr, L"Extract: %ls\n", error.c_str());
    return 1;
  }
  std::wprintf(L"installed=%d\n", ao4setup::IsInstalled(out + L"\\disc", out + L"\\hd"));
  return 0;
}
