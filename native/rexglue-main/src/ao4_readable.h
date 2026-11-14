#pragma once
// Fast "is this host range committed and readable" check for guest reads in the mod hooks.
// VirtualQuery is expensive on the large guest mappings and the hooks call it hundreds of times
// per frame (profiled: ~30% of the game's main thread in a busy scene). Each thread caches the
// last committed regions for 50 ms: short enough that a region the game frees (level change)
// is queried again within a few frames, long enough that per-frame checks hit the cache.
#include <windows.h>

#include <cstddef>
#include <cstdint>

inline bool Ao4CommittedRange(const uint8_t* host, size_t length) {
  struct Region { uintptr_t begin = 0, end = 0; ULONGLONG at = 0; };
  thread_local Region regions[16];
  thread_local unsigned next = 0;
  const uintptr_t h = reinterpret_cast<uintptr_t>(host);
  const ULONGLONG now = GetTickCount64();
  for (const Region& r : regions) {
    if (r.end && now - r.at < 50 && h >= r.begin && h + length <= r.end) return true;
  }
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(host, &info, sizeof(info)) || info.State != MEM_COMMIT ||
      (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    return false;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
  regions[next++ % 16] = Region{begin, begin + info.RegionSize, now};
  return h + length <= begin + info.RegionSize;
}
