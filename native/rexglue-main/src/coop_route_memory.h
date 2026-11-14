#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <vector>

struct Ao4VoMemorySnapshot { uint32_t start; std::vector<uint8_t> bytes; };
// A nearby event can be across a VirtualQuery boundary (including a protection
// change or a gap). Limit total address span, not the search to the source region.
inline std::vector<Ao4VoMemorySnapshot> Ao4VoMemorySnapshots(
    uintptr_t base,uint32_t center,uint32_t radius=0x200000) {
  std::vector<Ao4VoMemorySnapshot> result;
  if (center<0x40000000 || center>=0xE0000000 || radius>0x200000) return result;
  uint64_t cursor=(std::max)(0x40000000ull,uint64_t(center)-radius);
  const uint64_t end=(std::min)(0xE0000000ull,uint64_t(center)+radius);
  while (cursor<end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(base+cursor),&info,sizeof(info))) break;
    const uintptr_t region_begin=reinterpret_cast<uintptr_t>(info.BaseAddress);
    const uintptr_t region_end=region_begin+info.RegionSize;
    if (region_end<=base+cursor) break;
    const uint64_t next=(std::min)(end,uint64_t(region_end-base));
    if (info.State==MEM_COMMIT && !(info.Protect&(PAGE_NOACCESS|PAGE_GUARD))) {
      Ao4VoMemorySnapshot snapshot{uint32_t(cursor),std::vector<uint8_t>(size_t(next-cursor))};
      SIZE_T read=0;
      if (ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(base+cursor),
          snapshot.bytes.data(),snapshot.bytes.size(),&read) && read==snapshot.bytes.size())
        result.push_back(std::move(snapshot));
    }
    cursor=next;
  }
  return result;
}
