// Exercise the actual exported PPC API against an isolated guest-memory arena.
#include <rex/ppc/context.h>
#include <windows.h>
#include <array>
#include <set>
#include <cstdio>
#include <cstring>

int wmain(int argc, wchar_t** argv) {
  if(argc<2) return 1;
  HMODULE module=LoadLibraryW(argv[1]);
  if(!module) {std::printf("LoadLibrary failed: %lu\n",GetLastError());return 1;}
  using Api=void(*)(PPCContext&,uint8_t*);
  auto api=reinterpret_cast<Api>(GetProcAddress(module,"__imp__NetDll_XNetRandom"));
  if(!api) {std::puts("Export missing");return 1;}
  auto* arena=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x40000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
  if(!arena) return 1;
  auto call=[&](uint32_t address,uint32_t count) {
    PPCContext ctx{};ctx.r3.u32=1;ctx.r4.u32=address;ctx.r5.u32=count;
    api(ctx,arena);return ctx.r3.u32;
  };
  std::set<std::array<uint8_t,16>> keys;
  bool constant_bb=false;
  for(unsigned i=0;i<256;++i) {
    if(call(0x10000,16)) return 2;
    std::array<uint8_t,16> key{};std::memcpy(key.data(),arena+0x10000,16);
    std::array<uint8_t,16> bb{};bb.fill(0xBB);
    constant_bb|=key==bb;
    keys.insert(key);
  }
  std::printf("Actual XNetRandom export: 256 calls, distinct_keys=%zu, constant_BB=%s\n",
              keys.size(),constant_bb?"true":"false");
  if(keys.size()!=256 || constant_bb) return 3;
  for(uint32_t size: {1u,3u,16u,257u,65537u}) {
    std::memset(arena,0xA5,0x40000);
    if(call(0x10000,size)) return 4;
    for(uint32_t i=0;i<0x10000;++i) if(arena[i]!=0xA5) return 5;
    for(uint32_t i=0x10000+size;i<0x40000;++i) if(arena[i]!=0xA5) return 6;
  }
  std::memset(arena,0xA5,0x40000);
  if(call(0,0)!=0 || call(0,1)==0) return 7;
  for(uint32_t i=0;i<0x40000;++i) if(arena[i]!=0xA5) return 8;
  VirtualFree(arena,0,MEM_RELEASE);
  std::puts("PASS: unique keys, write bounds for five lengths, zero length and null rejection");
  return 0;
}
