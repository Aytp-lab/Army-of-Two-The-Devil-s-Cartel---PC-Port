// Observe the original GetData entry, without modifying guest registers/data.
#include "generated/ao4.main/ao4_main_init.h"
#include <rex/hook.h>
#include "../../diagnostics/query_trace_gate.h"
#include <array>

namespace {
bool Readable(uint8_t* base,uint32_t address,size_t size) {
  if(address<0x10000 || uint64_t(address)+size>0x100000000ull) return false;
  const auto* p=REX_RAW_ADDR(address);
  MEMORY_BASIC_INFORMATION m{};
  return VirtualQuery(p,&m,sizeof(m)) && m.State==MEM_COMMIT &&
    !(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
    uintptr_t(p)+size<=uintptr_t(m.BaseAddress)+m.RegionSize;
}
struct Snapshot {
  uint32_t query=0,type=0,pool=0,physical=0,fence=0,completed=0,fence_ptr=0,count=0;
  std::array<uint32_t,16> words{};
  bool valid=false;
};
Snapshot Observe(uint8_t* base,uint32_t query) {
  Snapshot s; s.query=query;
  if(!Readable(base,query,152)) return s;
  s.type=REX_LOAD_U32(query+4);
  if(s.type!=9) return s;
  s.pool=REX_LOAD_U32(query+28); s.fence=REX_LOAD_U32(query+24);
  s.count=REX_LOAD_U32(query+148);
  // Exactly the physical conversion and C0000000 alias used by 82CBC970.
  s.physical=(s.pool&0x1fffffff)+(((s.pool>>20)+512)&0x1000);
  uint32_t alias=s.physical+0xc0000000;
  if(!Readable(base,alias,64)) return s;
  for(unsigned i=0;i<16;++i) s.words[i]=__builtin_bswap32(REX_LOAD_U32(alias+4*i));
  uint32_t device=REX_LOAD_U32(query);
  if(Readable(base,device,11028)) {
    s.fence_ptr=REX_LOAD_U32(device+11024);
    if(Readable(base,s.fence_ptr,4)) s.completed=REX_LOAD_U32(s.fence_ptr);
  }
  s.valid=true; return s;
}
}
REX_HOOK_RAW(sub_82CBC970) {
  uint64_t window=0;
  const bool active=ao4_query_trace::Active(window);
  uint64_t start=0; Snapshot before;
  const uint32_t query=ctx.r3.u32,out=ctx.r4.u32,size=ctx.r5.u32,caller=uint32_t(ctx.lr);
  if(active) {start=ao4_query_trace::Tick();before=Observe(base,query);}
  __imp__sub_82CBC970(ctx,base);
  if(active && before.valid) {
    const uint64_t end=ao4_query_trace::Tick();
    uint32_t result=0;
    const bool output_valid=size>=4 && Readable(base,out,4);
    if(output_valid) result=REX_LOAD_U32(out);
    REXLOG_INFO("AO4_QUERY_READ window={} qpc0={} qpc1={} freq={} query={:08X} physical={:08X} caller={:08X} code={:08X} samples={} output_valid={} count={} fence={} completed={} fence_ptr={:08X} end_pass={},{} begin_pass={},{} end_total={},{} begin_total={},{}",
      window,start,end,ao4_query_trace::Frequency(),query,before.physical,caller,ctx.r3.u32,
      result,output_valid,before.count,before.fence,before.completed,before.fence_ptr,
      before.words[4],before.words[5],before.words[12],before.words[13],
      before.words[0],before.words[1],before.words[8],before.words[9]);
  }
}
