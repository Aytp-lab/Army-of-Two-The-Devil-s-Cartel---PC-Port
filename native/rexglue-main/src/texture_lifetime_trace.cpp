// Optional lifetime evidence for Diffuse_Map sizes observed becoming invalid.
// Every intercepted original routine runs; observations do not change guest data.
#include "generated/ao4.main/ao4_main_init.h"
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <windows.h>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <memory>
#include <string>
#include <set>
#include <fmt/format.h>

// Stable debugger entry, identified by symbol rather than a build-specific RVA.
// Native diagnostic state only; the debugger observes and continues all writes.
static volatile uint32_t writer_watch_address = 0;
static volatile uintptr_t writer_watch_physical = 0;
static volatile uintptr_t writer_watch_alias = 0;
static volatile uintptr_t writer_watch_descriptor = 0;
extern "C" __declspec(dllexport) __declspec(noinline)
void Ao4TexturePayloadWatchpoint(const uint8_t* physical, const uint8_t* alias,
                                const uint32_t* descriptor, uint32_t address) {
  writer_watch_physical = uintptr_t(physical);
  writer_watch_alias = uintptr_t(alias);
  writer_watch_descriptor = uintptr_t(descriptor);
  writer_watch_address = address;
}

namespace {
struct Event {
  uint64_t tick;
  uint32_t thread, caller, vtable, observed_refs;
  int32_t returned_refs;
  const char* operation;
};
std::mutex mutex;
std::unordered_map<uint32_t, std::deque<Event>> watched;
uint32_t selected_original_texture = 0;
bool selected_owner_released = false;
std::set<std::pair<uint32_t, bool>> selected_parameters;
struct DecalRecord { uint32_t decal,instance,block,texture; };
std::unordered_map<uint32_t,DecalRecord> decal_records;
std::set<uint32_t> decal_instances, decal_allocations;
struct InstanceHistory {
  std::deque<Event> recent;
  uint64_t add_enter=0,add_return=0,release_enter=0,release_return=0;
  uint64_t destructor_enter=0,destructor_return=0;
};
std::unordered_map<uint32_t,InstanceHistory> instance_history;
struct OwnerContext { uint32_t bundle = 0, caller = 0; };
thread_local OwnerContext owner_context;
bool OwnerEnabled() {
  static const bool enabled =
      GetEnvironmentVariableW(L"AO4_TEXTURE_OWNER_TRACE", nullptr, 0) != 0;
  return enabled;
}
bool OwnerWord(uint8_t* base, uint32_t address, uint32_t& value) {
  if (address < 0x10000) return false;
  auto* pointer = REX_RAW_ADDR(address);
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(pointer,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
      (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) ||
      uintptr_t(pointer)+4>uintptr_t(info.BaseAddress)+info.RegionSize) return false;
  __try { value=REX_LOAD_U32(address); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool OwnerLabel(uint8_t* base,uint32_t address,char* out,size_t capacity) {
  if(address<0x10000 || capacity<2) return false;
  auto* pointer=REX_RAW_ADDR(address);
  MEMORY_BASIC_INFORMATION info{};
  if(!VirtualQuery(pointer,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
     (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) ||
     uintptr_t(pointer)+capacity>uintptr_t(info.BaseAddress)+info.RegionSize) return false;
  __try {
    for(size_t i=0;i<capacity-1;++i) {
      out[i]=reinterpret_cast<const char*>(pointer)[i];
      if(!out[i]) return true;
    }
  } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
  out[capacity-1]=0;
  return false;
}
std::string OwnerWords(uint8_t* base, uint32_t address, unsigned count) {
  std::string text;
  for (unsigned i=0;i<count;++i) {
    uint32_t value=0;
    if (uint64_t(address)+i*4>UINT32_MAX || !OwnerWord(base,address+i*4,value)) {
      text+=" unreadable";break;
    }
    text+=fmt::format(" +{:X}={:08X}",i*4,value);
  }
  return text;
}
bool InstanceEvent(uint8_t* base,uint32_t object,uint32_t caller,
                   const char* operation,int32_t returned=-1,bool read=true) {
  if(!OwnerEnabled()) return false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if(!decal_instances.count(object)) return false;
  }
  uint32_t vt=0,refs=0;
  if(read) {OwnerWord(base,object,vt);OwnerWord(base,object+4,refs);}
  const Event event{GetTickCount64(),GetCurrentThreadId(),caller,vt,refs,returned,operation};
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto& h=instance_history[object];
    if(!std::strcmp(operation,"add-enter")) ++h.add_enter;
    else if(!std::strcmp(operation,"add-return")) ++h.add_return;
    else if(!std::strcmp(operation,"release-enter")) ++h.release_enter;
    else if(!std::strcmp(operation,"release-return")) ++h.release_return;
    else if(!std::strcmp(operation,"destructor-enter")) ++h.destructor_enter;
    else if(!std::strcmp(operation,"destructor-return")) ++h.destructor_return;
    h.recent.push_back(event);
    if(h.recent.size()>128) h.recent.pop_front();
  }
  return true;
}
void DecalSnapshot(uint8_t* base,const char* phase) {
  if(!OwnerEnabled()) return;
  std::vector<DecalRecord> copy;
  std::vector<std::pair<uint32_t,InstanceHistory>> histories;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for(const auto& [key,record]:decal_records) copy.push_back(record);
    for(const auto& entry:instance_history) histories.push_back(entry);
  }
  for(const auto& [object,h]:histories) {
    REXLOG_INFO("AO4_DECAL_TOTAL phase={} object={:08X} add_enter={} add_return={} "
                "release_enter={} release_return={} destructor_enter={} destructor_return={}",
                phase,object,h.add_enter,h.add_return,h.release_enter,h.release_return,
                h.destructor_enter,h.destructor_return);
    for(const auto& e:h.recent)
      REXLOG_INFO("AO4_DECAL_RECENT phase={} object={:08X} tick={} thread={} caller={:08X} "
                  "operation={} original_return={} vtable={:08X} observed_refs={}",
                  phase,object,e.tick,e.thread,e.caller,e.operation,e.returned_refs,e.vtable,e.observed_refs);
  }
  for(const auto& r:copy) {
    uint32_t live=0;
    const bool readable=OwnerWord(base,r.decal+24,live);
    REXLOG_INFO("AO4_DECAL_SNAPSHOT phase={} decal={:08X} instance={:08X} "
                "texture={:08X} slot_readable={} slot_now={:08X} "
                "decal_words:{} instance_words:{} block_words:{}",
                phase,r.decal,r.instance,r.texture,readable,live,
                OwnerWords(base,r.decal,8),OwnerWords(base,r.instance,7),
                OwnerWords(base,r.block,7));
  }
}
uint32_t ManagerRow(uint8_t* base,uint32_t manager,uint32_t key,uint32_t object) {
  uint32_t begin=0,end=0;
  if(!OwnerWord(base,manager,begin) || !OwnerWord(base,manager+4,end) ||
     end<begin || (end-begin)%12 || (end-begin)/12>8192) return 0;
  uint32_t lo=0,hi=(end-begin)/12;
  while(lo<hi) {
    const uint32_t mid=lo+(hi-lo)/2;
    uint32_t found=0;
    if(!OwnerWord(base,begin+mid*12,found)) return 0;
    if(found<key) lo=mid+1;else hi=mid;
  }
  const uint32_t row=begin+lo*12;
  uint32_t actual_key=0,value=0;
  return row<end && OwnerWord(base,row,actual_key) && actual_key==key &&
    OwnerWord(base,row+4,value) && value==object ? row : 0;
}
void BuildStack(uint32_t object) {
  void* frames[16]{};
  const unsigned count=CaptureStackBackTrace(0,16,frames,nullptr);
  MEMORY_BASIC_INFORMATION own{};
  VirtualQuery(reinterpret_cast<void*>(&OwnerEnabled),&own,sizeof(own));
  for(unsigned i=0;i<count;++i) {
    MEMORY_BASIC_INFORMATION module{};
    VirtualQuery(frames[i],&module,sizeof(module));
    REXLOG_INFO("AO4_TEXTURE_BUILD_STACK object={:08X} frame={} module={} "
                "image={:016X} rva={:08X}",object,i,
                module.AllocationBase==own.AllocationBase ? "main" : "other",
                uintptr_t(module.AllocationBase),
                uintptr_t(frames[i])-uintptr_t(module.AllocationBase));
  }
}
bool PayloadEnabled() {
  static const bool enabled =
      GetEnvironmentVariableW(L"AO4_TEXTURE_PAYLOAD_TRACE", nullptr, 0) != 0;
  return enabled;
}
struct Payload {
  uint32_t words[6]{};
  uint64_t hash = 0;
  bool readable = false;
};
// One physical page only. No guessed virtual alias and no writes. This is a
// non-atomic diagnostic sample; a concurrent producer may change its bytes.
bool ReadPayload(uint8_t* base, uint32_t fetch, Payload& result) {
  __try {
    for (unsigned i = 0; i < 6; ++i)
      result.words[i] = REX_LOAD_U32(fetch + i * 4);
    if ((result.words[0] & 3) != 2) return false;
    const uint32_t address = result.words[1] & 0x1FFFF000;
    if (!address || !rex::system::kernel_state()) return true;
    auto* memory = rex::system::kernel_memory();
    auto* data = memory->TranslatePhysical(address);
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(data, &region, sizeof(region)) ||
        region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        reinterpret_cast<uintptr_t>(data) + 4096 >
            reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize)
      return true;
    result.hash = 14695981039346656037ull;
    for (unsigned i = 0; i < 4096; ++i)
      result.hash = (result.hash ^ data[i]) * 1099511628211ull;
    result.readable = true;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ArmWriterWatch(uint8_t* base, uint32_t object, const Payload& sample) {
  static const bool enabled =
      GetEnvironmentVariableW(L"AO4_TEXTURE_WRITER_WATCH", nullptr, 0) != 0;
  static const bool copy_trace =
      GetEnvironmentVariableW(L"AO4_TEXTURE_COPY_TRACE", nullptr, 0) != 0;
  // First-page fingerprint of the exact BC3 decal mapped to the bad GPU draw
  // in the preceding run. Following relocation by fingerprint avoids hardcoded
  // guest object addresses or fixed physical pages from a previous process.
  if ((!enabled && !copy_trace) || !sample.readable || sample.hash != 0xF133F7C2044E42C3ull ||
      sample.words[2] != 0x000FE0FF || (sample.words[1] & 63) != 20) return;
  const uint32_t address = sample.words[1] & 0x1FFFF000;
  uint32_t descriptor = 0;
  __try {
    if ((REX_LOAD_U32(object + 92) & 0x1FFFF000) == address)
      descriptor = object + 92;
    else if ((REX_LOAD_U32(object + 144) & 0x1FFFF000) == address)
      descriptor = object + 144;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  if (!descriptor) return;
  static uint32_t selected = 0, last = UINT32_MAX, arms = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!selected) selected = object;
    if (selected != object || last == address || arms >= 128) return;
    selected_original_texture = object;
    last = address; ++arms;
  }
  auto* memory = rex::system::kernel_memory();
  if (copy_trace) {
    wchar_t value[16]{};
    swprintf_s(value, L"%08X", address);
    SetEnvironmentVariableW(L"AO4_TEXTURE_COPY_TARGET", value);
    swprintf_s(value, L"%08X", descriptor);
    SetEnvironmentVariableW(L"AO4_TEXTURE_COPY_DESCRIPTOR", value);
    REXLOG_INFO("AO4_TEXTURE_COPY_TARGET object={:08X} physical={:08X}", object, address);
  }
  if (!enabled) return;
  const auto* physical = memory->TranslatePhysical(address);
  const auto* alias = memory->TranslateVirtual(0xA0000000u | address);
  REXLOG_INFO("AO4_TEXTURE_WRITER_ARM object={:08X} physical={:08X} "
              "native_physical={:016X} native_alias={:016X} descriptor={:08X}",
              object, address, uintptr_t(physical), uintptr_t(alias), descriptor);
  Ao4TexturePayloadWatchpoint(physical, alias,
      reinterpret_cast<const uint32_t*>(REX_RAW_ADDR(descriptor)), address);
}
void ObservePayload(uint8_t* base, uint32_t object, uint32_t fetch,
                    const char* reason, bool throttled) {
  if (!PayloadEnabled()) return;
  struct State { uint64_t time = 0; Payload last{}; unsigned count = 0; };
  static std::unordered_map<uint64_t, State> states;
  const uint64_t key = (uint64_t(object) << 32) | fetch;
  const uint64_t now = GetTickCount64();
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (states.size() >= 512 && !states.count(key)) return;
    auto& state = states[key];
    if (state.count >= 32 || (throttled && now - state.time < 250)) return;
    state.time = now;
  }
  Payload sample{};
  if (!ReadPayload(base, fetch, sample)) return;
  ArmWriterWatch(base, object, sample);
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto& state = states[key];
    if (state.count && !std::memcmp(state.last.words, sample.words, sizeof(sample.words)) &&
        state.last.hash == sample.hash && state.last.readable == sample.readable) return;
    state.last = sample;
    ++state.count;
  }
  REXLOG_INFO("AO4_TEXTURE_PAYLOAD object={:08X} tick={} reason={} fetch={:08X} "
              "words={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} "
              "physical={:08X} format={} page_readable={} page_fnv64={:016X}",
              object, now, reason, fetch, sample.words[0], sample.words[1],
              sample.words[2], sample.words[3], sample.words[4], sample.words[5],
              sample.words[1] & 0x1FFFF000, sample.words[1] & 63,
              sample.readable, sample.hash);
}
bool Enabled() {
  static const bool enabled =
      GetEnvironmentVariableW(L"AO4_MATERIAL_BIND_TRACE", nullptr, 0) != 0;
  return enabled;
}
bool Header(uint8_t* base, uint32_t object, uint32_t& vtable, uint32_t& refs) {
  __try {
    vtable = REX_LOAD_U32(object);
    refs = REX_LOAD_U32(object + 4);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Observe(uint8_t* base, uint32_t object, uint32_t caller,
             const char* operation, int32_t returned = -1, bool flush = false) {
  if (!Enabled()) return false;
  std::vector<Event> copy;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = watched.find(object);
    if (found == watched.end()) return false;
    uint32_t vtable = 0, refs = 0;
    Header(base, object, vtable, refs);
    auto& events = found->second;
    events.push_back({GetTickCount64(), GetCurrentThreadId(), caller,
                      vtable, refs, returned, operation});
    if (events.size() > 64) events.pop_front();
    if (PayloadEnabled()) copy.push_back(events.back());
    else if (flush || events.size() <= 4) copy.assign(events.begin(), events.end());
  }
  for (const auto& e : copy) {
    REXLOG_INFO("AO4_TEXTURE_LIFETIME object={:08X} tick={} thread={} op={} "
                "caller={:08X} vtable={:08X} observed_refs={} returned_refs={}",
                object, e.tick, e.thread, e.operation, e.caller,
                e.vtable, e.observed_refs, e.returned_refs);
  }
  if (PayloadEnabled()) {
    uint32_t vtable = 0, refs = 0;
    if (Header(base, object, vtable, refs) && vtable == 0x82A26788) {
      ObservePayload(base, object, object + 88, operation, false);
      ObservePayload(base, object, object + 140, operation, false);
    }
  }
  return true;
}
}  // namespace

void Ao4DumpTextureLifetime(uint8_t* base, uint32_t object, uint32_t caller) {
  Observe(base, object, caller, "invalid-material-bind", -1, true);
}

// Observe the exact pointer loaded by the original material binder at
// 82E9FDC8. r28 is a reused binding context, not a unique material asset ID.
void Ao4ObserveTextureParameter(PPCContext& ctx, uint8_t* base, uint32_t object) {
  if (!OwnerEnabled() || !object) return;
  const uint32_t parameter=ctx.r9.u32;
  bool released=false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (object!=selected_original_texture || selected_parameters.size()>=64) return;
    released=selected_owner_released;
    if (!selected_parameters.emplace(parameter,released).second) return;
  }
  const uint32_t binding=ctx.r28.u32, flags=ctx.r31.u32;
  uint32_t linked=0,block=0;
  const bool matches=OwnerWord(base,parameter+16,linked) && linked==object;
  // Original loop uses a halfword flag array at binding+708 and a parallel
  // array of property blocks at binding+716. Only read a proven bounded index.
  if (flags>=binding+708 && flags-binding-708<8 && !(flags&1))
    OwnerWord(base,binding+716+2*(flags-binding-708),block);
  REXLOG_INFO("AO4_TEXTURE_PARAMETER object={:08X} owner_released={} "
              "binding={:08X} parameter={:08X} matches={} flags={:08X} block={:08X} "
              "entry={:08X} binding_words:{} parameter_words:{} block_words:{}",
              object,released,binding,parameter,matches,flags,block,ctx.r29.u32-39,
              OwnerWords(base,binding+688,12),OwnerWords(base,parameter,12),
              OwnerWords(base,block,16));
  if(released) {
    static bool snapshotted=false;
    bool capture=false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(!snapshotted) {snapshotted=true;capture=true;}
    }
    if(capture) DecalSnapshot(base,"first-bind-after-owner-release");
  }
}

void Ao4ObserveMaterialTexture(PPCContext& ctx, uint8_t* base, uint32_t object,
                               const char* label) {
  if (!Enabled() || !object || std::strcmp(label, "Diffuse_Map")) return;
  uint32_t dimensions = 0, vtable = 0, refs = 0;
  __try { dimensions = REX_LOAD_U32(object + 20); }
  __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  if ((dimensions != 0x02000100 && dimensions != 0x01000080) ||
      !Header(base, object, vtable, refs) ||
      vtable != 0x82A26788) return;
  bool added = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (watched.size() < 128) added = watched.emplace(object, std::deque<Event>{}).second;
  }
  if (added) {
    REXLOG_INFO("AO4_TEXTURE_LIFETIME tracking object={:08X} dimensions={}x{} "
                "vtable={:08X} observed_refs={}", object,
                dimensions >> 16, dimensions & 0xFFFF, vtable, refs);
    Observe(base, object, 0x82E9FE34, "first-material-bind");
    // Controlled A/B diagnosis only. This is deliberately not a production
    // ownership policy: at most sixteen real textures get one process-lifetime
    // reference. Every original release/destructor decision remains intact.
    static const bool hold_probe =
        GetEnvironmentVariableW(L"AO4_TEXTURE_HOLD_PROBE", nullptr, 0) != 0;
    static unsigned held = 0;
    bool hold = false;
    if (hold_probe && refs > 0 && refs < 0x100000) {
      std::lock_guard<std::mutex> lock(mutex);
      if (held < 16) { ++held; hold = true; }
    }
    if (hold) {
      auto saved = std::make_unique<PPCContext>(ctx);
      ctx.r3.u64 = object;
      __imp__sub_82EC07F8(ctx, base);
      const int32_t returned = ctx.r3.s32;
      ctx = *saved;
      REXLOG_WARN("AO4_TEXTURE_HOLD_PROBE object={:08X} original_addref_return={} "
                  "scope=process-diagnostic-only max_objects=16", object, returned);
      Observe(base, object, 0x82E9FE34, "probe-addref", returned, true);
    }
  }
}

REX_HOOK_RAW(sub_82EC07F8) {
  const uint32_t object = ctx.r3.u32, caller = uint32_t(ctx.lr);
  const bool instance=InstanceEvent(base,object,caller,"add-enter");
  const bool tracked = Observe(base, object, caller, "add-enter");
  __imp__sub_82EC07F8(ctx, base);
  if (tracked) Observe(base, object, caller, "add-return", ctx.r3.s32);
  if(instance) InstanceEvent(base,object,caller,"add-return",ctx.r3.s32);
}

REX_HOOK_RAW(sub_82E76348) {
  const uint32_t object = ctx.r3.u32, caller = uint32_t(ctx.lr);
  const bool tracked = Observe(base, object, caller, "release-enter");
  __imp__sub_82E76348(ctx, base);
  if (tracked) Observe(base, object, caller, "release-return", ctx.r3.s32,
                       ctx.r3.s32 == 0);
}

REX_HOOK_RAW(sub_82E75D70) {
  const uint32_t object = ctx.r3.u32, caller = uint32_t(ctx.lr);
  Observe(base, object, caller, "destroy-enter", -1, true);
  __imp__sub_82E75D70(ctx, base);
}

// Original D3D SetTexture takes a header at XenonTexture+60 or +112. Its
// original code builds six fetch words at device+1152+slot*24. Observe only
// tracked objects and only after the original call, preserving its context.
REX_HOOK_RAW(sub_82CB5EF0) {
  const uint32_t device = ctx.r3.u32, slot = ctx.r4.u32, header = ctx.r5.u32;
  uint32_t object = 0;
  if (Enabled() && PayloadEnabled() && header && slot < 32) {
    std::lock_guard<std::mutex> lock(mutex);
    if (watched.count(header - 60)) object = header - 60;
    else if (watched.count(header - 112)) object = header - 112;
  }
  __imp__sub_82CB5EF0(ctx, base);
  if (object)
    ObservePayload(base, object, device + 1152 + slot * 24, "gpu-bind", true);
}

REX_HOOK_RAW(sub_82DCA510) {
  struct Scope {
    OwnerContext saved;
    Scope(OwnerContext next):saved(owner_context){owner_context=next;}
    ~Scope(){owner_context=saved;}
  } scope(OwnerEnabled() ? OwnerContext{ctx.r3.u32,uint32_t(ctx.lr)} : owner_context);
  __imp__sub_82DCA510(ctx,base);
}

REX_HOOK_RAW(sub_82DC27B0) {
  uint32_t slot=0,texture=0;
  const uint32_t node=ctx.r3.u32,caller=uint32_t(ctx.lr);
  bool capture=false;
  if (OwnerEnabled() && OwnerWord(base,node+16,slot) && OwnerWord(base,slot,texture)) {
    static unsigned reports=0;
    std::lock_guard<std::mutex> lock(mutex);
    if (reports<16 && selected_original_texture && texture==selected_original_texture) {
      ++reports;capture=true;
    }
  }
  if (capture) {
    REXLOG_INFO("AO4_TEXTURE_OWNER_BEGIN object={:08X} node={:08X} slot={:08X} "
                "callback_lr={:08X} bundle={:08X} bundle_lr={:08X} node_words:{} bundle_words:{}",
                texture,node,slot,caller,owner_context.bundle,owner_context.caller,
                OwnerWords(base,node,12),OwnerWords(base,owner_context.bundle,72));
    void* frames[20]{};
    const unsigned count=CaptureStackBackTrace(0,20,frames,nullptr);
    MEMORY_BASIC_INFORMATION own{};
    VirtualQuery(reinterpret_cast<void*>(&OwnerEnabled),&own,sizeof(own));
    for(unsigned i=0;i<count;++i) {
      MEMORY_BASIC_INFORMATION module{};
      VirtualQuery(frames[i],&module,sizeof(module));
      REXLOG_INFO("AO4_TEXTURE_OWNER_STACK object={:08X} frame={} module={} "
                  "image={:016X} rva={:08X}", texture,i,
                  module.AllocationBase==own.AllocationBase ? "main" : "other",
                  uintptr_t(module.AllocationBase),
                  uintptr_t(frames[i])-uintptr_t(module.AllocationBase));
    }
  }
  __imp__sub_82DC27B0(ctx,base);
  if(capture) {
    uint32_t after=UINT32_MAX,refs=UINT32_MAX;
    OwnerWord(base,slot,after);OwnerWord(base,texture+4,refs);
    {
      std::lock_guard<std::mutex> lock(mutex);
      selected_owner_released=true;
    }
    REXLOG_INFO("AO4_TEXTURE_OWNER_END object={:08X} slot={:08X} "
                "slot_after={:08X} refs_after={} original_return={}",
                texture,slot,after,refs,ctx.r3.u32);
    DecalSnapshot(base,"bundle-release");
  }
}

// Original render-data initializer. +24 is the reference returned by
// sub_82EC9830; its 28-byte wrapper owns the parameter block at +16.
REX_HOOK_RAW(sub_82EC9830) {
  const uint32_t renderer=ctx.r3.u32,descriptor=ctx.r4.u32,caller=uint32_t(ctx.lr);
  __imp__sub_82EC9830(ctx,base);
  if(!OwnerEnabled()) return;
  const uint32_t instance=ctx.r3.u32;
  bool report=false,released=false;
  {
    static unsigned early=0,late=0;
    std::lock_guard<std::mutex> lock(mutex);
    released=selected_owner_released;
    auto& count=released ? late : early;
    if(decal_instances.count(instance) && count<16) {++count;report=true;}
  }
  if(!report) return;
  uint32_t parent=0,flags_word=0,parent_flags_word=0;
  OwnerWord(base,instance+8,parent);
  const bool requested_readable=OwnerWord(base,descriptor+4,flags_word);
  const bool parent_readable=OwnerWord(base,parent+4,parent_flags_word);
  // sub_82DAA4A0 returns descriptor-16 only when lhz(descriptor+4)&0x100.
  const uint32_t requested_key=requested_readable && ((flags_word>>16)&0x100) ? descriptor-16 : 0;
  const uint32_t retained_key=parent_readable && ((parent_flags_word>>16)&0x100) ? parent-16 : 0;
  REXLOG_INFO("AO4_DECAL_CACHE instance={:08X} renderer={:08X} descriptor={:08X} "
              "parent={:08X} requested_key={:08X} retained_key={:08X} "
              "after_owner_release={} caller={:08X} requested_guid:{} retained_guid:{} "
              "requested_words:{} parent_words:{}",
              instance,renderer,descriptor,parent,requested_key,retained_key,released,caller,
              OwnerWords(base,requested_key,4),OwnerWords(base,retained_key,4),
              OwnerWords(base,descriptor,24),OwnerWords(base,parent,24));
}

REX_HOOK_RAW(sub_8301C4F0) {
  const uint32_t decal=ctx.r3.u32,caller=uint32_t(ctx.lr);
  __imp__sub_8301C4F0(ctx,base);
  if(!OwnerEnabled()) return;
  uint32_t instance=0,vt=0,block=0,header=0,counts=0;
  if(!OwnerWord(base,decal+24,instance) || !OwnerWord(base,instance,vt) ||
     vt!=0x82A2CFD8 || !OwnerWord(base,instance+16,block) ||
     !OwnerWord(base,block,header) || !OwnerWord(base,block+4,counts)) return;
  // Cached wrappers may outlive the first resolving compartment. Record later
  // lookups independently of payload hash and the 32 first-entity samples.
  bool lookup=false,released=false;
  {
    static unsigned lookups=0,late_lookups=0;
    std::lock_guard<std::mutex> lock(mutex);
    released=selected_owner_released;
    auto& count=released ? late_lookups : lookups;
    if(decal_instances.count(instance) && count<16) {++count;lookup=true;}
  }
  if(lookup) {
    uint32_t source=0,parent=0,context=UINT32_MAX;
    if(OwnerWord(base,decal+16,source) && OwnerWord(base,source+4,parent))
      OwnerWord(base,parent+56,context);
    REXLOG_INFO("AO4_DECAL_LOOKUP decal={:08X} instance={:08X} block={:08X} "
                "source={:08X} context={} after_owner_release={} caller={:08X}",
                decal,instance,block,source,context,released,caller);
  }
  const uint32_t size=header&65535,uniforms=counts>>24,textures=(counts>>16)&255;
  if(textures>32 || 16+8*(uniforms+textures)>size) return;
  for(uint32_t i=0;i<textures;++i) {
    uint32_t field=0,texture=0,texture_vt=0,dimensions=0;
    if(!OwnerWord(base,block+20+8*(uniforms+i),field) || (field>>16)+20>size ||
       !OwnerWord(base,block+(field>>16)+16,texture) ||
       !OwnerWord(base,texture,texture_vt) || texture_vt!=0x82A26788 ||
       !OwnerWord(base,texture+20,dimensions) || dimensions!=0x01000080) continue;
    Payload sample{};
    if(!ReadPayload(base,texture+88,sample) || !sample.readable ||
       sample.hash!=0xF133F7C2044E42C3ULL) continue;
    bool capture=false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(decal_records.size()<32)
        capture=decal_records.emplace(decal,DecalRecord{decal,instance,block,texture}).second;
      if(capture) {
        decal_instances.insert(instance);
        decal_allocations.insert(decal);decal_allocations.insert(instance);
        decal_allocations.insert(block);
      }
    }
    if(capture)
      REXLOG_INFO("AO4_DECAL_REGISTER decal={:08X} instance={:08X} block={:08X} "
                  "texture={:08X} caller={:08X} decal_words:{} instance_words:{}",
                  decal,instance,block,texture,caller,OwnerWords(base,decal,8),
                  OwnerWords(base,instance,7));
  }
}

REX_HOOK_RAW(sub_82D12248) {
  const uint32_t object=ctx.r3.u32,caller=uint32_t(ctx.lr);
  const bool capture=InstanceEvent(base,object,caller,"release-enter");
  __imp__sub_82D12248(ctx,base);
  if(capture) InstanceEvent(base,object,caller,"release-return",ctx.r3.s32,ctx.r3.u32!=0);
}
REX_HOOK_RAW(sub_82EC0788) {
  const uint32_t object=ctx.r3.u32,caller=uint32_t(ctx.lr);
  const bool capture=InstanceEvent(base,object,caller,"destructor-enter");
  __imp__sub_82EC0788(ctx,base);
  if(capture) InstanceEvent(base,object,caller,"destructor-return",ctx.r3.s32,false);
}
REX_HOOK_RAW(sub_82D6F4A0) {
  const uint32_t address=ctx.r3.u32,caller=uint32_t(ctx.lr);
  bool capture=false;
  if(OwnerEnabled()) {
    static unsigned events=0;
    std::lock_guard<std::mutex> lock(mutex);
    if(events<64 && decal_allocations.count(address)) {++events;capture=true;}
  }
  if(capture)
    REXLOG_INFO("AO4_DECAL_FREE address={:08X} caller={:08X} before:{}",
                address,caller,OwnerWords(base,address,7));
  __imp__sub_82D6F4A0(ctx,base);
}

// Original parameter-block builder. Capture the source asset at construction,
// before first material binding selects the object for the lifetime observer.
REX_HOOK_RAW(sub_82EA8058) {
  const uint32_t owner=ctx.r3.u32, container=ctx.r4.u32, source=ctx.r5.u32;
  const uint32_t caller=uint32_t(ctx.lr);
  const uint32_t override_array=ctx.r7.u32,override_count=ctx.r8.u32;
  __imp__sub_82EA8058(ctx,base);
  if (!OwnerEnabled()) return;
  uint32_t block=0,header=0,counts=0;
  if (!OwnerWord(base,owner+4,block) || !OwnerWord(base,block,header) ||
      !OwnerWord(base,block+4,counts)) return;
  const uint32_t size=header&0xFFFF, uniforms=counts>>24, textures=(counts>>16)&255;
  if (size<24 || textures>32 || 16+8*(uniforms+textures)>size) return;
  for(uint32_t i=0;i<textures;++i) {
    uint32_t metadata=0,object=0,vt=0,dimensions=0;
    if (!OwnerWord(base,block+20+8*(uniforms+i),metadata)) continue;
    const uint32_t offset=metadata>>16;
    if (offset+20>size || !OwnerWord(base,block+offset+16,object) ||
        !OwnerWord(base,object,vt) || vt!=0x82A26788 ||
        !OwnerWord(base,object+20,dimensions) || dimensions!=0x01000080) continue;
    Payload sample{};
    if (!ReadPayload(base,object+88,sample) || !sample.readable ||
        sample.hash!=0xF133F7C2044E42C3ULL) continue;
    bool report=false;
    {
      static std::set<std::pair<uint32_t,uint32_t>> seen;
      std::lock_guard<std::mutex> lock(mutex);
      report=seen.size()<32 && seen.emplace(owner,block).second;
    }
    if (!report) continue;
    REXLOG_INFO("AO4_TEXTURE_BUILD object={:08X} owner={:08X} block={:08X} "
                "container={:08X} source={:08X} caller={:08X} "
                "owner_words:{} source_words:{} container_words:{}",
                object,owner,block,container,source,caller,OwnerWords(base,owner,8),
                OwnerWords(base,source,8),OwnerWords(base,container,12));
    BuildStack(object);
    uint32_t array=override_array,count=override_count;
    bool array_readable=true;
    if (!override_array)
      array_readable=OwnerWord(base,source+20,array) && array>=4 &&
        OwnerWord(base,array-4,count);
    REXLOG_INFO("AO4_TEXTURE_BUILD_ARRAY object={:08X} override_array={:08X} "
                "override_count={} array={:08X} count={} readable={}",
                object,override_array,override_count,array,count,array_readable);
    if (array_readable) {
      for(uint32_t j=0;j<count && j<8;++j) {
        uint32_t name=0,reference=0;
        if (!OwnerWord(base,array+j*8,name) ||
            !OwnerWord(base,array+j*8+4,reference)) break;
        REXLOG_INFO("AO4_TEXTURE_BUILD_INPUT object={:08X} index={} count={} "
                    "name={:08X} reference={:08X} name_words:{} reference_words:{}",
                    object,j,count,name,reference,OwnerWords(base,name,16),
                    OwnerWords(base,reference,16));
      }
    }
  }
}

REX_HOOK_RAW(sub_82DC6CD8) {
  const uint32_t manager=ctx.r3.u32,key=ctx.r4.u32,caller=uint32_t(ctx.lr);
  __imp__sub_82DC6CD8(ctx,base);
  const uint32_t object=ctx.r3.u32;
  if(!OwnerEnabled() || !object) return;
  uint32_t selected=0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    selected=selected_original_texture;
  }
  if(object!=selected) {
    uint32_t vt=0,dimensions=0;
    Payload sample{};
    if(!OwnerWord(base,object,vt) || vt!=0x82A26788 ||
       !OwnerWord(base,object+20,dimensions) || dimensions!=0x01000080 ||
       !ReadPayload(base,object+88,sample) || !sample.readable ||
       sample.hash!=0xF133F7C2044E42C3ULL) return;
  }
  {
    static unsigned reports=0;
    std::lock_guard<std::mutex> lock(mutex);
    if(reports>=32) return;
    ++reports;
  }
  const uint32_t row=ManagerRow(base,manager,key,object);
  REXLOG_INFO("AO4_TEXTURE_MANAGER_ACQUIRE object={:08X} manager={:08X} "
              "key={:08X} caller={:08X} row={:08X} row_words:{}",
              object,manager,key,caller,row,OwnerWords(base,row,3));
}

// The manager's original input list contains texture pointers, not asset GUIDs.
// Snapshot its caller/queue for the selected object without locking or changing
// the game's table; the original routine performs its own synchronization.
REX_HOOK_RAW(sub_82DCAED0) {
  const uint32_t manager=ctx.r3.u32,list=ctx.r4.u32,count=ctx.r5.u32;
  const uint32_t caller=uint32_t(ctx.lr),queue=ctx.r31.u32;
  uint32_t selected=0;
  if (OwnerEnabled()) {
    std::lock_guard<std::mutex> lock(mutex);
    selected=selected_original_texture;
  }
  if (selected && count<=8192) {
    for(uint32_t i=0;i<count;++i) {
      uint32_t object=0;
      if (uint64_t(list)+4*i>UINT32_MAX || !OwnerWord(base,list+4*i,object)) break;
      if (object!=selected) continue;
      bool report=false;
      {
        static unsigned reports=0;
        std::lock_guard<std::mutex> lock(mutex);
        report=reports<8;
        if(report) ++reports;
      }
      if(report) {
        const uint32_t parent=ctx.r11.u32;
        uint32_t compartment=UINT32_MAX;
        const uint32_t offset=caller==0x82E8406C ? 160 : 0;
        if((caller==0x82E8406C || caller==0x82E840D8) &&
           uint64_t(parent)+offset<=queue && (queue-parent-offset)%12==0 &&
           (queue-parent-offset)/12<205)
          compartment=(queue-parent-offset)/12;
        REXLOG_INFO("AO4_TEXTURE_MANAGER_REMOVE object={:08X} manager={:08X} "
                    "caller={:08X} queue_r31={:08X} count={} index={} parent={:08X} "
                    "compartment={} "
                    "manager_words:{} queue_words:{}",
                    selected,manager,caller,queue,count,i,parent,compartment,
                    OwnerWords(base,manager,12),OwnerWords(base,queue,16));
      }
      break;
    }
  }
  __imp__sub_82DCAED0(ctx,base);
}

// This is the resolver actually reached by the captured Decal builder. Trace
// only the original named graffiti resource; no replacement or extra lookup.
REX_HOOK_RAW(sub_82DC4508) {
  const uint32_t compartment=ctx.r3.u32,name_address=ctx.r5.u32;
  const uint32_t caller=uint32_t(ctx.lr);
  char name[192]{};
  bool capture=false;
  if(OwnerEnabled() && OwnerLabel(base,name_address,name,sizeof(name))) {
    const char* leaf=std::strrchr(name,'/');
    leaf=leaf ? leaf+1 : name;
    if(!_stricmp(leaf,"graffiti_generic_30_d")) {
      static unsigned reports=0;
      std::lock_guard<std::mutex> lock(mutex);
      if(reports<32) {++reports;capture=true;}
    }
  }
  __imp__sub_82DC4508(ctx,base);
  if(!capture) return;
  const uint32_t slot=ctx.r3.u32;
  uint32_t object=0;
  OwnerWord(base,slot,object);
  REXLOG_INFO("AO4_TEXTURE_RESOLVE name={} compartment={} caller={:08X} "
              "slot={:08X} object={:08X} slot_words:{}",
              name,compartment,caller,slot,object,OwnerWords(base,slot,8));
  uint32_t id=compartment;
  for(unsigned i=0;i<8 && id<205;++i) {
    uint32_t bundle=0,parent=205;
    // Original sub_82DC4508 / sub_82DC2E90 table address and parent offset.
    if(!OwnerWord(base,0x840EB630+4*id,bundle) || !bundle ||
       !OwnerWord(base,bundle+76,parent)) break;
    char label[96]{};
    OwnerLabel(base,bundle+128,label,sizeof(label));
    REXLOG_INFO("AO4_TEXTURE_RESOLVE_CONTEXT index={} id={} bundle={:08X} "
                "parent={} label={} words:{}",
                i,id,bundle,parent,label,OwnerWords(base,bundle+48,8));
    if(parent==id) break;
    id=parent;
  }
}
