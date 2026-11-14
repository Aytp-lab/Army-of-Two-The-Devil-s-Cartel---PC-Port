// I3: observe X/Y timing in the original input object and messages delivered to
// the local weapon component. Always execute the original functions unchanged.
#include "generated/ao4.main/ao4_main_init.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <map>

namespace {
bool Enabled(){
  static const bool on=[] {char value[8]{};return GetEnvironmentVariableA("AO4_WEAPON_ACTION_TRACE",value,sizeof(value))&&value[0]=='1';}();
  return on;
}
uint64_t Clock(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
bool Read32(uint8_t* base,uint32_t address,uint32_t& out){
  if(address<0x40000000||address>0xDFFFFFFC)return false;
  uint32_t raw=0;SIZE_T got=0;
  if(!ReadProcessMemory(GetCurrentProcess(),base+address,&raw,4,&got)||got!=4)return false;
  out=_byteswap_ulong(raw);return true;
}
uint32_t Read(uint8_t* base,uint32_t address){uint32_t value=0;Read32(base,address,value);return value;}
uint32_t LocalWeapons(uint8_t* base){
  const uint32_t manager=Read(base,Read(base,0x841350DC)+52);
  const uint32_t player=Read(base,manager+184);
  if(Read(base,player)!=0x82A3F850)return 0;
  const uint32_t link=Read(base,Read(base,player+1004));
  if(link<4||Read(base,link-4)!=0x82AAB0D0)return 0;
  const uint32_t weapons=Read(base,link-4+1380);
  return Read(base,weapons)==0x82AAB3B0?weapons:0;
}
std::atomic<unsigned> budget{0};
std::atomic<uint64_t> action_until{0};
bool Allowed(){return budget.fetch_add(1,std::memory_order_relaxed)<2048;}
struct Selection {uint32_t component=0,source=0,index=0,weapon=0;bool valid=false;};
Selection Selected(uint8_t* base,uint32_t component){
  Selection s;s.component=component;
  uint32_t flags=0,begin=0,end=0;
  if(!component||!Read32(base,component+12,flags)||!Read32(base,component+460,begin)||!Read32(base,component+464,end))return s;
  // Original sub_82FF4050 chooses +488 when byte +13 has bit 0x20,
  // otherwise +484; original sub_83071968 reads selector+4 as the slot.
  s.source=(((flags>>16)&0x20)!=0)?488:484;
  const uint32_t selector=Read(base,component+s.source);
  if(!selector||!Read32(base,selector+4,s.index)||s.index>=64||end<begin||end-begin>256||s.index>=(end-begin)/4)return s;
  s.valid=Read32(base,begin+s.index*4,s.weapon);return s;
}
struct PollState {uint16_t bits=0;uint64_t last=0,started[2]{};};
void ObserveInput(uint8_t* base,uint32_t object){
  static std::atomic<bool> announced{false};
  if(!announced.exchange(true))REXLOG_WARN("AO4_WEAPON_TRACE I3 title-input + local-component-messages; observation-only");
  uint32_t word=0,connected=0;
  if(!Read32(base,object+16,word)||!Read32(base,object+8,connected))return;
  const uint16_t bits=connected?uint16_t(word>>16):0;
  thread_local std::map<uint32_t,PollState> states;
  if(states.size()>=16&&states.find(object)==states.end())return;
  auto& state=states[object];const uint64_t now=Clock(),gap=state.last?now-state.last:0;state.last=now;
  constexpr uint16_t masks[2]={0x4000,0x8000};
  for(unsigned i=0;i<2;++i)if((bits^state.bits)&masks[i]){
    const bool down=(bits&masks[i])!=0;
    const uint64_t held=(!down&&state.started[i])?now-state.started[i]:0;state.started[i]=down?now:0;
    action_until.store(now+3000000,std::memory_order_relaxed);
    const auto selection=Selected(base,LocalWeapons(base));
    if(Allowed())REXLOG_WARN("AO4_WEAPON_BUTTON layer=title-input action={} down={} held_us={} poll_gap_us={} input={:08X} component={:08X} selector_offset={} slot={} weapon={:08X} selection_valid={}",
      i?"switch":"reload",down,held,gap,object,selection.component,selection.source,selection.index,selection.weapon,selection.valid);
  }
  state.bits=bits;
}
}

REX_HOOK_RAW(sub_82F86A48){
  const uint32_t input=ctx.r3.u32;
  __imp__sub_82F86A48(ctx,base);
  if(Enabled())ObserveInput(base,input);
}

REX_HOOK_RAW(sub_82FFEE60){
  if(Enabled()&&Clock()<action_until.load(std::memory_order_relaxed)){
    const uint32_t component=ctx.r3.u32,message=ctx.r4.u32;
    if(component&&component==LocalWeapons(base)){
      uint32_t id=0;
      if(Read32(base,message+4,id)&&Allowed()){
        const auto selection=Selected(base,component);
        REXLOG_WARN("AO4_WEAPON_MESSAGE component={:08X} id={:08X} message={:08X} caller={:08X} words8_20={:08X},{:08X},{:08X},{:08X} slot={} weapon={:08X} selection_valid={}",
          component,id,message,uint32_t(ctx.lr),Read(base,message+8),Read(base,message+12),Read(base,message+16),Read(base,message+20),selection.index,selection.weapon,selection.valid);
      }
    }
  }
  __imp__sub_82FFEE60(ctx,base);
}
