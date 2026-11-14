// Opt-in original reader observation. No guest writes, timers, stop overrides or
// replacement callbacks. The original routines always run exactly once.
#include "generated/ao4.main/ao4_main_init.h"
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/memory.h>
#include "../../../tools/rexglue-sdk-source-0.10.0/include/rex/audio/xma/lifetime_trace.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <mutex>
#include <unordered_map>
#include <xmmintrin.h>

namespace {
namespace diag=rex::audio::diagnostic;
struct Bridge {
  bool(*enabled)()=nullptr;
  void(*record)(const diag::XmaLifeEvent&)=nullptr;
  Bridge(){
    auto module=GetModuleHandleW(L"rexruntime.dll");
    const auto size=reinterpret_cast<uint32_t(*)()>(GetProcAddress(module,"rex_ao4_audio_sample_trace_event_size"));
    if(!size||size()!=sizeof(diag::XmaLifeEvent))return;
    enabled=reinterpret_cast<bool(*)()>(GetProcAddress(module,"?XmaLifeEnabled@diagnostic@audio@rex@@YA_NXZ"));
    record=reinterpret_cast<void(*)(const diag::XmaLifeEvent&)>(GetProcAddress(module,"?XmaLifeRecord@diagnostic@audio@rex@@YAXAEBUXmaLifeEvent@123@@Z"));
  }
};
const Bridge& Recorder(){static const Bridge bridge;return bridge;}
struct Count {std::atomic_flag busy=ATOMIC_FLAG_INIT;uint32_t owner=0,epoch=0;uint64_t calls=0,wanted=0,returned=0;};
std::array<Count,8192> counts{};
std::atomic<uint32_t> count_epoch{0},count_dropped{0};
struct Totals {uint32_t epoch=0,valid=0,dropped=0;uint64_t calls=0,wanted=0,returned=0;};
Totals CountRead(uint32_t owner,uint32_t wanted,uint32_t returned,bool update,bool erase=false){
  // Bounded diagnostic table, not an audio buffer. No waits or allocations.
  const uint32_t hash=(owner>>5)&8191;
  for(uint32_t i=0;i<16;++i){
    auto& c=counts[(hash+i)&8191];
    if(c.busy.test_and_set(std::memory_order_acquire)){
      Totals t;t.dropped=update?count_dropped.fetch_add(1)+1:count_dropped.load();return t;
    }
    if(!c.owner&&update){c.owner=owner;c.epoch=count_epoch.fetch_add(1)+1;c.calls=c.wanted=c.returned=0;}
    if(c.owner==owner){
      if(update){++c.calls;c.wanted+=wanted;c.returned+=returned;}
      Totals t{c.epoch,1,count_dropped.load(),c.calls,c.wanted,c.returned};
      // Keep keys anchored: deleting a collided key could otherwise create a
      // second counter for an owner stored farther down the probe chain.
      if(erase){c.epoch=count_epoch.fetch_add(1)+1;c.calls=c.wanted=c.returned=0;}
      c.busy.clear(std::memory_order_release);return t;
    }
    c.busy.clear(std::memory_order_release);
  }
  Totals t;t.dropped=update?count_dropped.fetch_add(1)+1:count_dropped.load();return t;
}
bool Enabled(){
  static const bool enabled=[] {wchar_t value[4]{};
    return GetEnvironmentVariableW(L"AO4_AUDIO_SAMPLE_TRACE",value,4)>0&&value[0]==L'1';}();
  if(!enabled)return false;
  const auto& bridge=Recorder();return bridge.enabled&&bridge.record&&bridge.enabled();
}
uint64_t Ns(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
  std::chrono::steady_clock::now().time_since_epoch()).count());}
template<size_t N> bool Words(uint32_t address,std::array<uint32_t,N>& words){
  if(address<0x10000||uint64_t(address)+N*4>UINT32_MAX)return false;
  auto* memory=REX_KERNEL_MEMORY();
  const uint32_t end=address+uint32_t(N*4)-1;
  auto* heap=memory->LookupHeap(address);
  if(!heap||heap!=memory->LookupHeap(end)||heap->QueryRangeAccess(address,end)==rex::memory::PageAccess::kNoAccess)return false;
  const auto* p=memory->TranslateVirtual<const uint8_t*>(address);
  __try {
    for(size_t i=0;i<N;++i)words[i]=(uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|p[i*4+3];
    return true;
  } __except(EXCEPTION_EXECUTE_HANDLER){words.fill(0);return false;}
}
// A14b helpers (defined early for the read hook).
uint64_t ThreadDecodeNs(){
  using Fn=void(*)(uint64_t*,uint64_t*,uint64_t*);
  static const Fn fn=reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"rexruntime.dll"),"rex_ao4_xma_thread_cost"));
  uint64_t d=0;if(fn)fn(&d,nullptr,nullptr);return d;
}
thread_local uint64_t tl_read_ns=0,tl_reads=0;
bool ProvenanceOn(){
  static const bool on=[] {wchar_t v[4]{};return GetEnvironmentVariableW(L"AO4_AUDIO_COST_PROVENANCE",v,4)>0&&v[0]==L'1';}();
  return on;
}
struct Snapshot {
  std::array<uint32_t,32> owner{};
  std::array<uint32_t,8> channel{};
  uint32_t valid=0;
};
Snapshot Read(uint32_t owner){
  Snapshot s;
  if(Words(owner,s.owner)){
    s.valid=1;
    if(s.owner[18]>0&&s.owner[18]<=16&&Words(s.owner[14],s.channel))s.valid|=2;
  }
  return s;
}
void Record(diag::XmaLifeKind kind,uint32_t owner,uint32_t caller,uint32_t argument,
            uint32_t wanted,uint32_t returned,uint32_t ordinal,const Snapshot& before,
            const Snapshot& after,uint64_t begin,uint64_t end,uint64_t probe,const Totals& total){
  diag::XmaLifeEvent e{};e.kind=kind;e.id=UINT32_MAX;e.caller=caller;
  e.sample_owner=owner;e.sample_argument=argument;e.sample_wanted=wanted;e.sample_returned=returned;
  e.sample_ordinal=ordinal;e.sample_before_valid=before.valid;e.sample_after_valid=after.valid;
  e.sample_begin_ns=begin;e.sample_end_ns=end;e.sample_probe_ns=probe;
  e.sample_calls=total.calls;e.sample_wanted_total=total.wanted;e.sample_returned_total=total.returned;
  e.sample_epoch=total.epoch;e.sample_counter_valid=total.valid;e.sample_counter_dropped=total.dropped;
  e.sample_before=before.owner;e.sample_after=after.owner;
  e.sample_channel_before=before.channel;e.sample_channel_after=after.channel;
  if((after.valid&1)&&Words(after.owner[5],e.sample_source))e.sample_after_valid|=4;
  Recorder().record(e);
}
void Control(diag::XmaLifeKind kind,uint32_t sound,uint32_t root,uint32_t slot,const PPCContext& c){
  const auto begin=Ns();diag::XmaLifeEvent e{};e.kind=kind;e.id=UINT32_MAX;
  e.sample_subject_type=1;e.sample_owner=sound;e.caller=uint32_t(c.lr);
  e.sample_argument=c.r5.u32;e.sample_release_root=root;e.sample_release_slot=slot;e.sample_release_row=sound;
  if(Words(sound,e.sample_before))e.sample_before_valid|=1;
  if(Words(sound,e.sample_source))e.sample_release_valid|=8;
  if(Words(root,e.sample_release_root_words))e.sample_release_valid|=1;
  if(Words(slot,e.sample_release_slot_words))e.sample_release_valid|=2;
  if(Words(sound,e.sample_release_row_words))e.sample_release_valid|=4;
  e.sample_release_gpr={c.r0.u32,c.r1.u32,c.r2.u32,c.r3.u32,c.r4.u32,c.r5.u32,c.r6.u32,c.r7.u32,
    c.r8.u32,c.r9.u32,c.r10.u32,c.r11.u32,c.r12.u32,c.r13.u32,c.r14.u32,c.r15.u32,
    c.r16.u32,c.r17.u32,c.r18.u32,c.r19.u32,c.r20.u32,c.r21.u32,c.r22.u32,c.r23.u32,
    c.r24.u32,c.r25.u32,c.r26.u32,c.r27.u32,c.r28.u32,c.r29.u32,c.r30.u32,c.r31.u32};
  uint32_t frame=c.r1.u32;e.sample_release_sp[0]=frame;e.sample_release_lr[0]=uint32_t(c.lr);
  for(size_t i=1;i<e.sample_release_sp.size();++i){
    std::array<uint32_t,1> next{},lr{};
    if(!Words(frame,next)||next[0]<=frame||next[0]-frame>0x100000||(next[0]&15)||!Words(next[0]-8,lr))break;
    e.sample_release_sp[i]=next[0];e.sample_release_lr[i]=lr[0];frame=next[0];
  }
  e.sample_begin_ns=begin;e.sample_end_ns=Ns();e.sample_probe_ns=e.sample_end_ns-begin;
  Recorder().record(e);
}
}

REX_HOOK_RAW(sub_83870960){
  if(!Enabled())return __imp__sub_83870960(ctx,base);
  const uint32_t owner=ctx.r3.u32,wanted=ctx.r5.u32,argument=ctx.r4.u32,caller=uint32_t(ctx.lr);
  const bool prov=ProvenanceOn();const uint32_t sp=ctx.r1.u32;
  const uint64_t dec0=prov?ThreadDecodeNs():0;
  const auto probe_begin=Ns();const auto before=Read(owner);const auto begin=Ns();
  __imp__sub_83870960(ctx,base);
  const auto end=Ns();const uint32_t returned=ctx.r3.u32;const auto after=Read(owner);
  const auto total=CountRead(owner,wanted,returned,true);
  tl_read_ns+=end-begin;++tl_reads;
  if(prov){
    // A14b: stack + inline decode time of this read; sampled (decode>0: 1/8, else 1/64).
    const uint64_t dec=ThreadDecodeNs()-dec0;
    thread_local uint32_t prov_n=0;++prov_n;
    if((dec&&(prov_n&7)==0)||(prov_n&63)==0){
      diag::XmaLifeEvent e{};e.kind=static_cast<diag::XmaLifeKind>(33);e.id=UINT32_MAX;e.caller=caller;
      e.sample_subject_type=4;e.sample_owner=owner;e.sample_wanted=wanted;e.sample_returned=returned;
      e.sample_argument=uint32_t(dec/1000);e.sample_calls=dec;e.sample_wanted_total=end-begin;
      e.sample_release_root=GetCurrentThreadId();
      uint32_t frame=sp;e.sample_release_sp[0]=frame;e.sample_release_lr[0]=caller;
      for(size_t i=1;i<e.sample_release_sp.size();++i){
        std::array<uint32_t,1> next{},lr{};
        if(!Words(frame,next)||next[0]<=frame||next[0]-frame>0x100000||(next[0]&15)||!Words(next[0]-8,lr))break;
        e.sample_release_sp[i]=next[0];e.sample_release_lr[i]=lr[0];frame=next[0];
      }
      e.sample_begin_ns=begin;e.sample_end_ns=end;
      Recorder().record(e);
    }
  }
  const auto probe=(begin-probe_begin)+(Ns()-end);
  // First encounter per thread/hash slot, all short reads/state transitions, and
  // one normal read in32. Collisions only add observations; no guest state used.
  thread_local std::array<uint32_t,256> seen{};
  thread_local uint32_t ordinal=0;
  const uint32_t slot=(owner>>5)&255;const bool first=seen[slot]!=owner;
  seen[slot]=owner;++ordinal;
  if(first||returned!=wanted||!total.valid||total.calls==1||(total.calls&31)==0||before.owner[22]!=after.owner[22]||probe>=100000)
    Record(diag::XmaLifeKind::SampleRead,owner,caller,argument,wanted,returned,ordinal,before,after,begin,end,probe,total);
}

REX_HOOK_RAW(sub_83870680){
  if(!Enabled())return __imp__sub_83870680(ctx,base);
  const uint32_t owner=ctx.r3.u32,caller=uint32_t(ctx.lr),argument=ctx.r4.u32;
  const auto probe_begin=Ns();const auto before=Read(owner);const auto begin=Ns();
  __imp__sub_83870680(ctx,base);
  const auto end=Ns();const auto after=Read(owner);
  Record(diag::XmaLifeKind::SampleReconfigure,owner,caller,argument,0,ctx.r3.u32,0,before,after,begin,end,(begin-probe_begin)+(Ns()-end),CountRead(owner,0,0,false));
}

REX_HOOK_RAW(sub_8386BAF8){
  if(!Enabled())return __imp__sub_8386BAF8(ctx,base);
  const uint32_t owner=ctx.r3.u32,caller=uint32_t(ctx.lr),argument=ctx.r4.u32;
  const auto probe_begin=Ns();const auto before=Read(owner);const auto begin=Ns();
  const auto total=CountRead(owner,0,0,false);
  __imp__sub_8386BAF8(ctx,base);
  const auto end=Ns();const auto after=Read(owner);
  // This callback returns channel nodes to a pool. It is not named VoiceStop:
  // the initiating original caller and copied state still need interpretation.
  Record(diag::XmaLifeKind::SampleRetireCallback,owner,caller,argument,0,ctx.r3.u32,0,before,after,begin,end,(begin-probe_begin)+(Ns()-end),total);
  CountRead(owner,0,0,false,true);
}

REX_HOOK_RAW(sub_838A2920){
  if(!Enabled())return __imp__sub_838A2920(ctx,base);
  const auto begin=Ns();
  const uint32_t owner=ctx.r3.u32,caller=uint32_t(ctx.lr),sp=ctx.r1.u32;
  const auto before=Read(owner);const auto total=CountRead(owner,0,0,false);
  diag::XmaLifeEvent e{};e.kind=diag::XmaLifeKind::SampleReleaseRequest;
  e.id=UINT32_MAX;e.caller=caller;e.sample_owner=owner;e.sample_argument=ctx.r4.u32;
  e.sample_before=before.owner;e.sample_channel_before=before.channel;e.sample_before_valid=before.valid;
  e.sample_calls=total.calls;e.sample_wanted_total=total.wanted;e.sample_returned_total=total.returned;
  e.sample_epoch=total.epoch;e.sample_counter_valid=total.valid;e.sample_counter_dropped=total.dropped;
  if((before.valid&1)&&Words(before.owner[5],e.sample_source))e.sample_before_valid|=4;
  e.sample_release_gpr={ctx.r0.u32,ctx.r1.u32,ctx.r2.u32,ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,
    ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,ctx.r11.u32,ctx.r12.u32,ctx.r13.u32,ctx.r14.u32,ctx.r15.u32,
    ctx.r16.u32,ctx.r17.u32,ctx.r18.u32,ctx.r19.u32,ctx.r20.u32,ctx.r21.u32,ctx.r22.u32,ctx.r23.u32,
    ctx.r24.u32,ctx.r25.u32,ctx.r26.u32,ctx.r27.u32,ctx.r28.u32,ctx.r29.u32,ctx.r30.u32,ctx.r31.u32};
  e.sample_release_sp[0]=sp;e.sample_release_lr[0]=caller;
  uint32_t frame=sp;
  for(size_t i=1;i<e.sample_release_sp.size();++i){
    std::array<uint32_t,1> next{},lr{};
    if(!Words(frame,next)||next[0]<=frame||next[0]-frame>0x100000||(next[0]&15)||!Words(next[0]-8,lr))break;
    e.sample_release_sp[i]=next[0];e.sample_release_lr[i]=lr[0];frame=next[0];
  }
  // These exact layouts follow the seven original direct callers, not a guessed
  // universal object type. Unknown callers retain registers and raw frames only.
  std::array<uint32_t,1> root{},slot_index{};
  if(caller==0x83876458||caller==0x83876504){
    const uint32_t root_offset=caller==0x83876458?212:148;
    const uint32_t index_offset=caller==0x83876458?220:156;
    if(Words(sp+root_offset,root)&&Words(sp+index_offset,slot_index)&&slot_index[0]<256){
      e.sample_release_root=root[0];
      if(Words(root[0],e.sample_release_root_words)){
        e.sample_release_valid|=1;
        const auto& rw=e.sample_release_root_words;
        e.sample_release_slot=root[0]+(rw[117]>>16)+slot_index[0]*48;
        e.sample_release_row=rw[25]+slot_index[0]*80;
      }
    }
  }else if(caller==0x8387B020||caller==0x838880CC||caller==0x838836C8||caller==0x8388125C){
    e.sample_release_root=ctx.r31.u32;
    if(Words(e.sample_release_root,e.sample_release_root_words))e.sample_release_valid|=1;
    if(caller==0x8387B020)e.sample_release_slot=ctx.r27.u32;
    if(caller==0x838880CC)e.sample_release_slot=ctx.r23.u32;
  }else if(caller==0x838A2DD4&&Words(sp+124,root)){
    e.sample_release_root=root[0];
    if(Words(root[0],e.sample_release_root_words))e.sample_release_valid|=1;
  }
  if(e.sample_release_slot&&Words(e.sample_release_slot,e.sample_release_slot_words))e.sample_release_valid|=2;
  if(e.sample_release_row&&Words(e.sample_release_row,e.sample_release_row_words))e.sample_release_valid|=4;
  e.sample_begin_ns=begin;e.sample_end_ns=Ns();e.sample_probe_ns=e.sample_end_ns-begin;
  Recorder().record(e);
  __imp__sub_838A2920(ctx,base);
}

REX_HOOK_RAW(sub_8386FF30){
  if(!Enabled())return __imp__sub_8386FF30(ctx,base);
  const PPCContext input=ctx;
  __imp__sub_8386FF30(ctx,base);
  if(ctx.r3.u32)Control(diag::XmaLifeKind::SampleSoundCreated,ctx.r3.u32,ctx.r3.u32,input.r4.u32,input);
}
REX_HOOK_RAW(sub_82E2E8F8){
  if(Enabled()){
    std::array<uint32_t,1> sound{};
    if(Words(ctx.r4.u32+4,sound)&&sound[0])
      Control(diag::XmaLifeKind::SampleSoundStopRequest,sound[0],ctx.r3.u32,ctx.r4.u32,ctx);
  }
  __imp__sub_82E2E8F8(ctx,base);
}
REX_HOOK_RAW(sub_83870188){
  if(Enabled()){
    uint32_t root=ctx.r3.u32,slot=0;
    if(uint32_t(ctx.lr)==0x82E2E918){root=ctx.r28.u32;slot=ctx.r29.u32;}
    else if(uint32_t(ctx.lr)==0x838BE608){root=ctx.r31.u32;slot=ctx.r31.u32;}
    else if(uint32_t(ctx.lr)==0x838D5834){root=ctx.r30.u32;slot=ctx.r30.u32+556;}
    Control(diag::XmaLifeKind::SampleSoundDestroyQueued,ctx.r3.u32,root,slot,ctx);
  }
  __imp__sub_83870188(ctx,base);
}
REX_HOOK_RAW(sub_8386E750){
  if(Enabled()){
    std::array<uint32_t,1> sound{};
    if(Words(ctx.r3.u32+4,sound)&&sound[0])
      Control(diag::XmaLifeKind::SampleSoundDestroyExecute,sound[0],sound[0],ctx.r3.u32,ctx);
  }
  __imp__sub_8386E750(ctx,base);
}

// A12: original sound budget limiter (sets controller flag +272&0x10000000, which
// sub_82E3BD80 turns into an early stop). Observation only: the original runs once,
// no guest writes, no guest calls. Serialized as kind "unknown", sample_subject_type=2.
// Return r3 is the original median (every exit does mr r3,r30); f12/f11 hold the
// getter result +40/+36 and f9/f5 the limit products, untouched after loading.
namespace {
constexpr auto kSampleBudgetLimit=static_cast<diag::XmaLifeKind>(31);
constexpr uint32_t kBudgetFlag=0x10000000;
uint32_t FloatBits(double v){const float f=float(v);uint32_t u;std::memcpy(&u,&f,4);return u;}
uint32_t Trunc(double v){return std::isfinite(v)&&v>-2147483648.0&&v<4294967296.0?uint32_t(int64_t(v)):0xFFFFFFFFu;}
std::atomic<uint32_t> budget_calls{0};
// A13: runtime per-thread inline decode / lock wait totals (rexruntime export).
struct ThreadCost {uint64_t decode=0,lock=0,kicks=0;bool valid=false;};
// A14 (solo provenance only): focus controller from the limiter, opt-in via
// AO4_AUDIO_COST_PROVENANCE=1. Reads Sound+60 of all focus slots around originals.
std::atomic<uint32_t> g_focus_ctrl{0};
bool ProvenanceEnabled(){
  static const bool on=[] {wchar_t v[4]{};return GetEnvironmentVariableW(L"AO4_AUDIO_COST_PROVENANCE",v,4)>0&&v[0]==L'1';}();
  return on;
}
struct FocusCosts {std::array<uint32_t,64> sound{},cost{};uint32_t n=0;};
void ReadFocus(FocusCosts& f){
  f.n=0;const uint32_t ctrl=g_focus_ctrl.load(std::memory_order_relaxed);
  std::array<uint32_t,1> slots{};std::array<uint32_t,1> cnt{};
  if(!ctrl||!Words(ctrl+232,slots)||!Words(ctrl+264,cnt))return;
  const uint32_t nslots=cnt[0]&0xFFFF;
  for(uint32_t i=0;i<nslots&&i<64;++i){
    std::array<uint32_t,1> s{},c{};
    if(Words(slots[0]+i*24+4,s)&&s[0]&&Words(s[0]+60,c)){f.sound[f.n]=s[0];f.cost[f.n]=c[0];++f.n;}
  }
}
uint32_t CountChanged(const FocusCosts& before,uint32_t& delta_abs){
  uint32_t changed=0;delta_abs=0;
  for(uint32_t i=0;i<before.n;++i){std::array<uint32_t,1> c{};
    if(Words(before.sound[i]+60,c)&&c[0]!=before.cost[i]){++changed;delta_abs+=c[0]>before.cost[i]?c[0]-before.cost[i]:before.cost[i]-c[0];}}
  return changed;
}
ThreadCost QueryThreadCost(){
  using Fn=void(*)(uint64_t*,uint64_t*,uint64_t*);
  static const Fn fn=reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"rexruntime.dll"),"rex_ao4_xma_thread_cost"));
  ThreadCost c;if(fn){fn(&c.decode,&c.lock,&c.kicks);c.valid=true;}return c;
}
}
// HEARING EXPERIMENT ONLY (not a fix): AO4_AUDIO_BUDGET_DIVISOR=2..16 divides the
// measured cost sum before the original limiter stores it in its median ring, which
// acts like a proportionally higher limit. Unset/1 = original behaviour.
uint32_t BudgetDivisor(){
  static const uint32_t divisor=[] {wchar_t value[8]{};
    if(GetEnvironmentVariableW(L"AO4_AUDIO_BUDGET_DIVISOR",value,8)==0)return 1u;
    const unsigned long v=wcstoul(value,nullptr,10);return v>=2&&v<=16?uint32_t(v):1u;}();
  return divisor;
}
REX_HOOK_RAW(sub_82E300E0){
  const uint32_t measured_sum=ctx.r5.u32,divisor=BudgetDivisor();
  if(divisor>1)ctx.r5.u32=measured_sum/divisor;
  if(!Enabled())return __imp__sub_82E300E0(ctx,base);
  const auto begin=Ns();
  const PPCContext input=ctx;
  const uint32_t manager=ctx.r3.u32,list=ctx.r4.u32;
  diag::XmaLifeEvent e{};e.kind=kSampleBudgetLimit;e.id=UINT32_MAX;e.caller=uint32_t(ctx.lr);
  e.sample_subject_type=2;e.sample_owner=manager;e.sample_release_root=manager;e.sample_release_slot=list;
  e.sample_argument=ctx.r5.u32;e.sample_wanted=ctx.r6.u32;
  std::array<uint32_t,2> range{};
  std::array<uint32_t,256> controllers{};uint32_t count=0;
  if(Words(list,range)&&range[1]>=range[0]&&((range[1]-range[0])&3)==0){
    count=(range[1]-range[0])/4;e.sample_release_valid|=1;
    const uint32_t n=count<256?count:256;
    for(uint32_t i=0;i<n;++i){std::array<uint32_t,1> p{};if(Words(range[0]+i*4,p))controllers[i]=p[0];}
  }
  e.sample_channel_before[0]=range[0];e.sample_channel_before[1]=range[1];
  const uint32_t n=count<256?count:256;
  std::array<uint32_t,256> pre_flags{};uint32_t flagged_pre=0,cost_sum=0;
  for(uint32_t i=0;i<n;++i){
    std::array<uint32_t,10> w{};  // controller +236..+275
    if(!controllers[i]||!Words(controllers[i]+236,w))continue;
    pre_flags[i]=w[9];flagged_pre+=(w[9]&kBudgetFlag)?1:0;cost_sum+=w[3];
    if(i<32){
      e.sample_release_root_words[i*4]=controllers[i];e.sample_release_root_words[i*4+1]=w[3];
      e.sample_release_root_words[i*4+2]=w[9];e.sample_release_root_words[i*4+3]=w[0];
      e.sample_before[i]=w[4];
    }
  }
  std::array<uint32_t,2> mgr_pre{};const bool mgr_ok=Words(manager+2632,mgr_pre);
  std::array<uint32_t,1> global{},vt{},fn{},focus{};
  if(Words(0x8412DEE8u,global)&&global[0]&&Words(global[0],vt)&&Words(vt[0]+128,fn))e.sample_release_valid|=2;
  Words(manager+2620,focus);
  if(focus[0])g_focus_ctrl.store(focus[0],std::memory_order_relaxed);
  e.sample_release_gpr={input.r0.u32,input.r1.u32,input.r2.u32,input.r3.u32,input.r4.u32,input.r5.u32,input.r6.u32,input.r7.u32,
    input.r8.u32,input.r9.u32,input.r10.u32,input.r11.u32,input.r12.u32,input.r13.u32,input.r14.u32,input.r15.u32,
    input.r16.u32,input.r17.u32,input.r18.u32,input.r19.u32,input.r20.u32,input.r21.u32,input.r22.u32,input.r23.u32,
    input.r24.u32,input.r25.u32,input.r26.u32,input.r27.u32,input.r28.u32,input.r29.u32,input.r30.u32,input.r31.u32};
  uint32_t frame=input.r1.u32;e.sample_release_sp[0]=frame;e.sample_release_lr[0]=uint32_t(input.lr);
  for(size_t i=1;i<e.sample_release_sp.size();++i){
    std::array<uint32_t,1> next{},lr{};
    if(!Words(frame,next)||next[0]<=frame||next[0]-frame>0x100000||(next[0]&15)||!Words(next[0]-8,lr))break;
    e.sample_release_sp[i]=next[0];e.sample_release_lr[i]=lr[0];frame=next[0];
  }
  const auto pre_end=Ns();
  __imp__sub_82E300E0(ctx,base);
  const auto call_end=Ns();
  const uint32_t median=ctx.r3.u32;e.sample_returned=median;
  uint32_t flagged_post=0,changed=0;
  for(uint32_t i=0;i<n;++i){
    std::array<uint32_t,1> f{};
    if(!controllers[i]||!Words(controllers[i]+272,f))continue;
    flagged_post+=(f[0]&kBudgetFlag)?1:0;
    if((f[0]^pre_flags[i])&kBudgetFlag)++changed;
    if(i<32)e.sample_after[i]=f[0];
  }
  std::array<uint32_t,2> mgr_post{};Words(manager+2632,mgr_post);
  std::array<uint32_t,10> ring{};if(Words(manager+2544,ring))e.sample_release_valid|=4;
  const uint32_t limit=Trunc(ctx.f9.f64),release=Trunc(ctx.f5.f64);
  auto& s=e.sample_source;
  s[0]=global[0];s[1]=vt[0];s[2]=fn[0];s[3]=FloatBits(ctx.f12.f64);s[4]=FloatBits(ctx.f11.f64);
  s[5]=limit;s[6]=release;s[7]=FloatBits(ctx.f0.f64);s[8]=FloatBits(ctx.f13.f64);
  s[9]=mgr_ok?mgr_pre[0]:0;s[10]=mgr_ok?mgr_pre[1]:0;s[11]=mgr_post[0];s[12]=mgr_post[1];
  s[13]=focus[0];s[14]=flagged_pre;s[15]=flagged_post;s[16]=cost_sum;s[17]=count;s[18]=changed;
  for(size_t i=0;i<10;++i)s[19+i]=ring[i];
  e.sample_release_slot_words[0]=measured_sum;e.sample_release_slot_words[1]=divisor;
  {
    const auto tc=QueryThreadCost();
    e.sample_release_slot_words[2]=uint32_t(tc.decode/1000);e.sample_release_slot_words[3]=uint32_t(tc.lock/1000);
    e.sample_release_slot_words[4]=uint32_t(tc.kicks);e.sample_release_slot_words[5]=tc.valid?1:0;
    e.sample_release_slot_words[6]=GetCurrentThreadId();
    // Focus controller slots: +232 array of 24-byte slots, +266 u16 count; slot+4 sound.
    std::array<uint32_t,1> slots{};std::array<uint32_t,10> cw{};
    if(focus[0]&&Words(focus[0]+232,slots)&&Words(focus[0]+236,cw)){
      const uint32_t nslots=cw[7]&0xFFFF;e.sample_release_row_words[19]=nslots;
      uint32_t k=0;
      for(uint32_t i=0;i<nslots&&i<64&&k<6;++i){
        std::array<uint32_t,1> sound{};std::array<uint32_t,4> sw{};
        if(!Words(slots[0]+i*24+4,sound)||!sound[0]||!Words(sound[0]+60,sw))continue;
        e.sample_release_row_words[k*3]=sound[0];e.sample_release_row_words[k*3+1]=sw[0];
        e.sample_release_row_words[k*3+2]=sw[3];++k;  // sw[3]=word at +72 (state byte high)
      }
      e.sample_release_row_words[18]=k;
    }
  }
  const uint32_t call=budget_calls.fetch_add(1)+1;e.sample_ordinal=call;e.sample_calls=call;
  // Bounded: record on any flag change, median above the release threshold, or 1 in 32.
  if(changed||flagged_pre!=flagged_post||(release!=0xFFFFFFFFu&&median>release)||call==1||(call&31)==0){
    e.sample_begin_ns=begin;e.sample_end_ns=Ns();
    e.sample_probe_ns=(pre_end-begin)+(e.sample_end_ns-call_end);
    Recorder().record(e);
  }
}

// A13: original audio engine frame (audio thread). Observation only. Aggregates 64
// calls: host time, inline XMA decode and lock waits on this thread (runtime export),
// and the engine's own timebase fields +244/+248/+256 written by the original.
// Serialized as kind "unknown", sample_subject_type=3.
namespace {
struct FrameAgg {uint64_t calls=0,host_ns=0,host_max=0,decode_ns=0,decode_max=0,lock_ns=0,kicks=0;
  uint64_t e244=0,e248=0,e256=0,e248_max=0;uint32_t engine=0;
  uint64_t prov_frames=0,prov_changed=0,prov_delta=0,prov_sounds=0,sub_changed=0,sub_delta=0,sub_calls=0;};
thread_local FrameAgg frame_agg{};
}
REX_HOOK_RAW(sub_83868B30){
  if(!Enabled())return __imp__sub_83868B30(ctx,base);
  const uint32_t engine=ctx.r3.u32;
  const bool prov=ProvenanceEnabled();FocusCosts fc;if(prov)ReadFocus(fc);
  const auto c0=QueryThreadCost();const auto t0=Ns();
  __imp__sub_83868B30(ctx,base);
  const auto t1=Ns();const auto c1=QueryThreadCost();
  if(prov&&fc.n){uint32_t d=0;const uint32_t ch=CountChanged(fc,d);
    ++frame_agg.prov_frames;frame_agg.prov_changed+=ch;frame_agg.prov_delta+=d;frame_agg.prov_sounds+=fc.n;}
  std::array<uint32_t,4> ew{};Words(engine+244,ew);  // +244,+248,+252,+256
  auto& a=frame_agg;a.engine=engine;++a.calls;
  const uint64_t host=t1-t0,dec=c1.decode-c0.decode;
  a.host_ns+=host;a.host_max=host>a.host_max?host:a.host_max;
  a.decode_ns+=dec;a.decode_max=dec>a.decode_max?dec:a.decode_max;
  a.lock_ns+=c1.lock-c0.lock;a.kicks+=c1.kicks-c0.kicks;
  a.e244+=ew[0];a.e248+=ew[1];a.e256+=ew[3];a.e248_max=ew[1]>a.e248_max?ew[1]:a.e248_max;
  if(a.calls<64)return;
  diag::XmaLifeEvent e{};e.kind=static_cast<diag::XmaLifeKind>(32);e.id=UINT32_MAX;e.caller=uint32_t(ctx.lr);
  e.sample_subject_type=3;e.sample_owner=engine;e.sample_ordinal=uint32_t(a.calls);
  e.sample_calls=a.calls;e.sample_wanted_total=a.host_ns;e.sample_returned_total=a.decode_ns;
  auto& s=e.sample_source;
  s[0]=uint32_t(a.host_ns/1000);s[1]=uint32_t(a.host_max/1000);s[2]=uint32_t(a.decode_ns/1000);s[3]=uint32_t(a.decode_max/1000);
  s[4]=uint32_t(a.lock_ns/1000);s[5]=uint32_t(a.kicks);s[6]=uint32_t(a.e244);s[7]=uint32_t(a.e248);s[8]=uint32_t(a.e256);
  s[9]=uint32_t(a.e248_max);s[10]=c1.valid?1:0;s[11]=GetCurrentThreadId();
  s[12]=uint32_t(a.prov_frames);s[13]=uint32_t(a.prov_changed);s[14]=uint32_t(a.prov_delta);s[15]=uint32_t(a.prov_sounds);
  s[16]=uint32_t(a.sub_calls);s[17]=uint32_t(a.sub_changed);s[18]=uint32_t(a.sub_delta);
  e.sample_begin_ns=t0;e.sample_end_ns=Ns();e.sample_probe_ns=0;
  Recorder().record(e);
  a=FrameAgg{};
}

// A14: timed voice processing inside the engine frame (called with r4=0 and r4=1).
REX_HOOK_RAW(sub_8386AFF8){
  if(!Enabled()||!ProvenanceEnabled())return __imp__sub_8386AFF8(ctx,base);
  FocusCosts fc;ReadFocus(fc);
  __imp__sub_8386AFF8(ctx,base);
  if(fc.n){uint32_t d=0;const uint32_t ch=CountChanged(fc,d);
    ++frame_agg.sub_calls;frame_agg.sub_changed+=ch;frame_agg.sub_delta+=d;}
}

// A14c: original voice-processing pass (writes Voice+60 = mftb elapsed per voice).
// Observation only: aggregates 64 passes of host time vs. time inside original sample
// reads (which include the inline XMA decode). Kind "unknown", sample_subject_type=5.
namespace {
struct VoiceAgg {uint64_t calls=0,host_ns=0,host_max=0,read_ns=0,read_max=0,reads=0;};
thread_local VoiceAgg voice_agg{};
}
// TIMING EXPERIMENT ONLY (not a fix): AO4_AUDIO_VOICE_FTZ=1 sets host FTZ|DAZ for
// the duration of the original voice pass without touching the cached guest csr, so
// scalar code that only calls disableFlushMode() keeps flushing denormals. Afterwards
// the host MXCSR flush bits are made consistent with the cached state again.
bool VoiceFtz(){
  static const bool on=[] {wchar_t v[4]{};return GetEnvironmentVariableW(L"AO4_AUDIO_VOICE_FTZ",v,4)>0&&v[0]==L'1';}();
  return on;
}
// A15 PROFILER (diagnostic only): AO4_AUDIO_PROFILE_DIR=<dir> samples the RIP of the
// threads currently inside the original voice pass about every 1ms (Suspend/GetContext/
// Resume), histograms module+offset and rewrites <dir>/voice-profile-<pid>.txt every 5s.
// No guest state is read or written.
namespace {
struct ProfSlot {std::atomic<HANDLE> handle{nullptr};std::atomic<DWORD> tid{0};std::atomic<int> active{0};};
std::array<ProfSlot,16> prof_slots{};
std::wstring ProfDir(){wchar_t v[512]{};const DWORD n=GetEnvironmentVariableW(L"AO4_AUDIO_PROFILE_DIR",v,512);return n&&n<512?std::wstring(v,n):std::wstring();}
const std::wstring& ProfileDir(){static const std::wstring d=ProfDir();return d;}
struct Mod {uintptr_t base=0,size=0;const char* name="";};
Mod ModInfo(const wchar_t* dll,const char* name){
  Mod m;m.name=name;const auto h=reinterpret_cast<uintptr_t>(GetModuleHandleW(dll));if(!h)return m;
  const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
  const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(h+dos->e_lfanew);
  m.base=h;m.size=nt->OptionalHeader.SizeOfImage;return m;
}
DWORD WINAPI ProfThread(LPVOID){
  const Mod mods[]={ModInfo(L"ao4_main_native.dll","main"),ModInfo(L"rexruntime.dll","runtime"),
                    ModInfo(L"ntdll.dll","ntdll"),ModInfo(L"KernelBase.dll","kernelbase"),ModInfo(L"ucrtbase.dll","ucrtbase")};
  std::unordered_map<uint64_t,uint32_t> hist;uint64_t samples=0,other=0,ticks=0;
  HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002/*HIGH_RESOLUTION*/,TIMER_ALL_ACCESS);
  if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
  auto last=GetTickCount64();
  wchar_t file[600];swprintf(file,600,L"%ls/voice-profile-%lu.txt",ProfileDir().c_str(),GetCurrentProcessId());
  {wchar_t mark[600];swprintf(mark,600,L"%ls/voice-profile-started-%lu.txt",ProfileDir().c_str(),GetCurrentProcessId());
   FILE* f=_wfopen(mark,L"w");if(f){std::fprintf(f,"timer %p main %llx\n",(void*)timer,(unsigned long long)mods[0].base);std::fclose(f);}}
  for(;;){
    LARGE_INTEGER due;due.QuadPart=-10000;SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE);
    WaitForSingleObject(timer,INFINITE);++ticks;
    for(auto& s:prof_slots){
      HANDLE h=s.handle.load();if(!h||!s.active.load(std::memory_order_acquire))continue;
      if(SuspendThread(h)==DWORD(-1))continue;
      CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL;const bool ok=GetThreadContext(h,&c)!=0;
      const bool still=s.active.load(std::memory_order_acquire)!=0;
      ResumeThread(h);
      if(!ok||!still)continue;
      ++samples;bool found=false;
      for(uint32_t m=0;m<5;++m)if(mods[m].base&&c.Rip>=mods[m].base&&c.Rip<mods[m].base+mods[m].size){
        ++hist[(uint64_t(m)<<40)|uint64_t(c.Rip-mods[m].base)];found=true;break;}
      if(!found)++other;
    }
    const auto now=GetTickCount64();
    if(now-last>=5000){
      last=now;FILE* f=_wfopen(file,L"w");
      if(f){
        std::fprintf(f,"# samples %llu other %llu ticks %llu\n",(unsigned long long)samples,(unsigned long long)other,(unsigned long long)ticks);
        for(uint32_t m=0;m<5;++m)std::fprintf(f,"# module %u %s base %llx size %llx\n",m,mods[m].name,(unsigned long long)mods[m].base,(unsigned long long)mods[m].size);
        for(const auto& [k,v]:hist)std::fprintf(f,"%s %llx %u\n",mods[k>>40].name,(unsigned long long)(k&0xFFFFFFFFFFull),v);
        std::fclose(f);
      }
    }
  }
}
ProfSlot* ProfRegister(){
  if(ProfileDir().empty())return nullptr;
  thread_local ProfSlot* mine=nullptr;thread_local bool tried=false;
  if(mine||tried)return mine;tried=true;
  static std::once_flag start;std::call_once(start,[]{CloseHandle(CreateThread(nullptr,0,ProfThread,nullptr,0,nullptr));});
  const DWORD tid=GetCurrentThreadId();
  for(auto& s:prof_slots){DWORD expected=0;
    if(s.tid.compare_exchange_strong(expected,tid)){
      s.handle.store(OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,tid));
      mine=&s;break;}}
  return mine;
}
}
REX_HOOK_RAW(sub_838A6780){
  const bool ftz=VoiceFtz();const uint32_t mx=_mm_getcsr();
  if(ftz)_mm_setcsr(mx|0x8040u);
  if(!Enabled()){
    __imp__sub_838A6780(ctx,base);
    if(ftz)_mm_setcsr((_mm_getcsr()&~0x8040u)|(uint32_t(ctx.fpscr.csr)&0x8040u));
    return;
  }
  const uint64_t r0=tl_read_ns,n0=tl_reads;
  ProfSlot* ps=ProfRegister();if(ps)ps->active.store(1,std::memory_order_release);
  const auto t0=Ns();
  __imp__sub_838A6780(ctx,base);
  const auto t1=Ns();
  if(ps)ps->active.store(0,std::memory_order_release);
  if(ftz)_mm_setcsr((_mm_getcsr()&~0x8040u)|(uint32_t(ctx.fpscr.csr)&0x8040u));
  auto& a=voice_agg;++a.calls;
  const uint64_t host=t1-t0,rd=tl_read_ns-r0;
  a.host_ns+=host;a.host_max=host>a.host_max?host:a.host_max;
  a.read_ns+=rd;a.read_max=rd>a.read_max?rd:a.read_max;a.reads+=tl_reads-n0;
  if(a.calls<64)return;
  diag::XmaLifeEvent e{};e.kind=static_cast<diag::XmaLifeKind>(34);e.id=UINT32_MAX;e.caller=uint32_t(ctx.lr);
  e.sample_subject_type=5;e.sample_owner=ctx.r3.u32?ctx.r3.u32:1;e.sample_calls=a.calls;
  auto& s=e.sample_source;
  s[0]=uint32_t(a.host_ns/1000);s[1]=uint32_t(a.host_max/1000);s[2]=uint32_t(a.read_ns/1000);s[3]=uint32_t(a.read_max/1000);
  s[4]=uint32_t(a.reads);s[5]=GetCurrentThreadId();s[6]=ftz?1:0;
  e.sample_begin_ns=t0;e.sample_end_ns=Ns();
  Recorder().record(e);
  a=VoiceAgg{};
}
