// 60 FPS EXPERIMENT ONLY (not a fix): FPSTEST_MODE=scan|vfi|sim60|var|<comma list>.
// A background thread finds the Frostbite settings instances in guest memory by their EBX
// payload signatures (settings_xenon.ebx, identical in all 15 levels) and logs them; depending
// on the mode it changes:
//   vfi   ClientSettings+14 VisualFrameInterpolation = 1 (render interpolated frames, sim stays 30 Hz)
//   sim60 GameTimeSettings+32 MaxSimFps = 60.0
//   var   GameTimeSettings+56 VariableSimTickTimeEnable = 1
// Without FPSTEST_MODE nothing runs.
#include "generated/ao4.main/ao4_main_init.h"
#include <rex/hook.h>
#include <rex/cvar.h>
#include "qol_settings.h"
#include <rex/system/kernel_state.h>
#include <rex/memory.h>
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <cmath>
#include <map>
#include <mutex>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

void FpsConstCountLog();  // fps_dtprobe.cpp
std::atomic<uint32_t> g_pos_soldier{0};  // posprobe: current local soldier
void FpsTimerLog(uint8_t* mbase);
void FpsListCandLog();
void FpsStackDump(const char* name, uint32_t r3, uint32_t r4, double f1);
// Logs r3 and the first `words` big-endian words at r4 of a hooked guest call, at most once per 5 s per name
// (dump-mode "argdump" only); words are also shown as floats.
void FpsArgDump(const char* name, uint32_t r3, uint32_t r4, unsigned words);

namespace {
std::atomic<DWORD> g_game_thread{0};  // main game thread (runs client GameTime::update)
// Runtime layout (from the game's typeinfo, differs from the EBX payload): +28 MaxInactiveVariableFps 10,
// +32 MaxSimFps 30, +36 MaxVariableFps 200, +40 ClampTicks 2, +44 ForceDeltaTime -1,
// +48 ForceDeltaTickCount -1, +52 TimeScale 1, +56..59/+60 bools (+60 VariableSimTickTimeEnable).
constexpr uint8_t kGameTimeSig[] = {0x41, 0x20, 0x00, 0x00, 0x41, 0xF0, 0x00, 0x00, 0x43, 0x48, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x02, 0xBF, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0x3F, 0x80, 0x00, 0x00};
constexpr uint32_t kGameTimeSigOffset = 28;
constexpr uint8_t kClientSig[] = {0x3D, 0xCC, 0xCC, 0xCD, 0x3E, 0x4C, 0xCC, 0xCD, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x3C, 0x23, 0xD7, 0x0A, 0x3C, 0xF5, 0xC2, 0x8F,
                                  0x3E, 0x99, 0x99, 0x9A, 0x3F, 0x80, 0x00, 0x00, 0x3C, 0x23, 0xD7, 0x0A,
                                  0x3C, 0xA3, 0xD7, 0x0A, 0x3C, 0x23, 0xD7, 0x0A};
constexpr uint32_t kClientSigOffset = 92;

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// Raw search guarded against pages the guest frees while we scan (no C++ objects here).
ptrdiff_t SafeFind(const uint8_t* from, const uint8_t* stop, const uint8_t* sig, size_t sig_len) {
  __try {
    for (const uint8_t* p = from; p + sig_len <= stop; ++p) {
      if (p[0] == sig[0] && std::memcmp(p, sig, sig_len) == 0) return p - from;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -2;
  }
  return -1;
}

// FPSTEST_MODE, read once (it was re-read on every hooked call, and a 128-byte buffer silently turned
// lists longer than 127 characters into "no experiment").
// Production set behind quality-of-life.ini "bildrate_60 = an" (no probes).
constexpr const char* kFps60QolMode = "hook60,rate48,tickrate,phys30,sched,tickfix,tlhalf,aiscan,aibudget,qol60";
bool QolFps60() {
  // Parsed directly (Ao4Qol() logs, and this runs during module static initialisation).
  wchar_t path[32768] = {};
  if (!GetEnvironmentVariableW(L"AO4_QOL_SETTINGS", path, 32768)) return false;
  std::string text;
  if (FILE* file = _wfopen(path, L"rb")) {
    char buffer[4096];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < 65536) text.append(buffer, read);
    fclose(file);
  }
  return Ao4ParseQolSettings(text).fps60;
}
const std::string& Mode() {
  static const std::string mode = [] {
    char value[2048] = {};
    const DWORD n = GetEnvironmentVariableA("FPSTEST_MODE", value, sizeof(value));
    if (n && n < sizeof(value)) return std::string(value, n);
    return QolFps60() ? std::string(kFps60QolMode) : std::string();
  }();
  return mode;
}

bool Has(const std::string& mode, const char* word) { return mode.find(word) != std::string::npos; }
// 60 FPS needs the guest vblank at 120 Hz (two vblanks per presented frame) and the synchronous
// readback ("fast" reads the previous frame: black spots when turning quickly at 60). The vblank
// worker reads the rate when the graphics system starts (usually before this module), so the
// starters pass --video_mode_refresh_rate=120 --readback_resolve=full; this is a fallback only.
struct Fps60CvarInit {
  Fps60CvarInit() {
    if (!Has(Mode(), "hook60")) return;
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "120");
    if (Has(Mode(), "qol60")) rex::cvar::SetFlagByName("readback_resolve", "full");
  }
} g_fps60_cvar_init;

void Scan(uint8_t* membase, const std::string& mode, std::set<uint32_t>& seen) {
  uint8_t* address = membase;
  uint8_t* const end = membase + 0x100000000ull;
  MEMORY_BASIC_INFORMATION info{};
  while (address < end && VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
    uint8_t* region = static_cast<uint8_t*>(info.BaseAddress);
    const size_t size = info.RegionSize;
    const DWORD protect = info.Protect & 0xFF;
    const bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                          (protect == PAGE_READWRITE || protect == PAGE_READONLY || protect == PAGE_EXECUTE_READWRITE ||
                           protect == PAGE_WRITECOPY);
    if (readable && region >= membase) {
      uint8_t* const stop = region + size;
      // GameTimeSettings: contiguous runtime signature.
      for (uint8_t* p = region;;) {
        const ptrdiff_t found = SafeFind(p, stop, kGameTimeSig, sizeof(kGameTimeSig));
        if (found < 0) break;
        p += found;
        if (p - region >= kGameTimeSigOffset) {
          uint8_t* obj = p - kGameTimeSigOffset;
          const uint32_t guest = uint32_t(obj - membase);
          if (seen.insert(guest * 2).second)
            REXLOG_WARN("AO4_FPSTEST GameTimeSettings guest={:08X} word0={:08X} word1={:08X} MaxSimFps={:08X} MaxVariableFps={:08X} ClampTicks={} VarSim={}",
                        guest, Be32(obj), Be32(obj + 4), Be32(obj + 32), Be32(obj + 36), Be32(obj + 40), obj[60]);
          if (Has(mode, "sim60")) PutBe32(obj + 32, 0x42700000);  // 60.0f (signature no longer matches)
          if (Has(mode, "var")) obj[60] = 1;
        }
        p += 1;
      }
      // ClientSettings: runtime offsets from typeinfo; anchor MinLatency 0.1f at +104.
      static constexpr uint8_t kAnchor[] = {0x3D, 0xCC, 0xCC, 0xCD};
      for (uint8_t* p = region;;) {
        const ptrdiff_t found = SafeFind(p, stop, kAnchor, 4);
        if (found < 0) break;
        p += found;
        uint8_t* obj = p - 104;
        if (((p - membase) & 3) == 0 && obj >= region && obj + 168 <= stop && Be32(obj + 76) == 0x3E4CCCCD &&
            Be32(obj + 68) == 0x3C23D70A && Be32(obj + 112) == 0x3C23D70A && Be32(obj + 40) == 0x3CA3D70A &&
            Be32(obj + 92) == 0x3F800000) {
          const uint32_t guest = uint32_t(obj - membase);
          if (seen.insert(guest * 2 + 1).second)
            REXLOG_WARN("AO4_FPSTEST ClientSettings guest={:08X} word0={:08X} word1={:08X} Vsync={} VisualFrameInterpolation={}",
                        guest, Be32(obj), Be32(obj + 4), obj[161], obj[162]);
          if (Has(mode, "vfi") && obj[162] == 0) {
            obj[162] = 1;
            REXLOG_WARN("AO4_FPSTEST ClientSettings guest={:08X} VisualFrameInterpolation -> 1", guest);
          }
        }
        p += 4;
      }
    }
    address = region + size;
  }
}

// probe mode: find field-name strings and the words that point at them (Frostbite typeinfo
// field entries hold name pointer + runtime offset); logs 32 bytes around each reference.
void Probe(uint8_t* membase) {
  const char* names[] = {"MaxSimFps", "VisualFrameInterpolation", "GameTimeSettings", "ClientSettings"};
  for (const char* name : names) {
    const size_t len = std::strlen(name) + 1;  // include terminator
    std::set<uint32_t> hits;
    uint8_t* address = membase;
    MEMORY_BASIC_INFORMATION info{};
    while (address < membase + 0x100000000ull && VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
      uint8_t* region = static_cast<uint8_t*>(info.BaseAddress);
      const DWORD protect = info.Protect & 0xFF;
      if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && region >= membase &&
          (protect == PAGE_READWRITE || protect == PAGE_READONLY || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY)) {
        uint8_t* p = region;
        for (;;) {
          const ptrdiff_t f = SafeFind(p, region + info.RegionSize, reinterpret_cast<const uint8_t*>(name), len);
          if (f < 0) break;
          p += f;
          hits.insert(uint32_t(p - membase));
          p += 1;
        }
      }
      address = region + info.RegionSize;
    }
    for (const uint32_t s : hits) {
      if (s >= 0xA0000000u) continue;  // physical mirrors
      REXLOG_WARN("AO4_FPSTEST string '{}' at {:08X}", name, s);
      const uint8_t ref[4] = {uint8_t(s >> 24), uint8_t(s >> 16), uint8_t(s >> 8), uint8_t(s)};
      address = membase;
      int refs = 0;
      while (refs < 6 && address < membase + 0xA0000000ull && VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
        uint8_t* region = static_cast<uint8_t*>(info.BaseAddress);
        const DWORD protect = info.Protect & 0xFF;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && region >= membase &&
            (protect == PAGE_READWRITE || protect == PAGE_READONLY || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY)) {
          uint8_t* p = region;
          while (refs < 6) {
            const ptrdiff_t f = SafeFind(p, region + info.RegionSize, ref, 4);
            if (f < 0) break;
            p += f;
            if ((uint32_t(p - membase) & 3) == 0 && p - region >= 16) {
              std::string hex;
              char cell[4];
              for (int i = -16; i < 16; ++i) { std::snprintf(cell, sizeof(cell), "%02X", p[i]); hex += cell; }
              REXLOG_WARN("AO4_FPSTEST   ref at {:08X}: {}", uint32_t(p - membase), hex);
              ++refs;
            }
            p += 1;
          }
        }
        address = region + info.RegionSize;
      }
    }
  }
}

// probe2: dump Frostbite typeinfo field tables {name ptr, runtime offset, type ptr}.
bool ValidName(uint8_t* membase, uint32_t name) {
  return name >= 0x82000000u && name < 0x84000000u && membase[name] >= 'A' && membase[name] <= 'z';
}
void DumpFields(uint8_t* membase, uint32_t known_entry, const char* cls) {
  const uint32_t table = known_entry - 16 * 12;
  for (int i = 0; i < 33; ++i) {
    const uint8_t* e = membase + table + i * 12;
    const uint32_t name = Be32(e), offset = Be32(e + 4), type = Be32(e + 8);
    if (!ValidName(membase, name)) { REXLOG_WARN("AO4_FPSTEST field {}[{}] (no name)", cls, i - 16); continue; }
    const char* s = reinterpret_cast<const char*>(membase + name);
    REXLOG_WARN("AO4_FPSTEST field {}[{}] {} offset={} type={:08X}", cls, i - 16, std::string(s, strnlen(s, 64)), offset, type);
  }
}

// Calls fn(region_begin, region_end) for committed readable host ranges inside [lo, hi). Only these
// may be read: the runtime's guest-fault handler terminates the game on any unmapped guest read.
template <typename Fn>
void ForEachReadable(uint8_t* lo, uint8_t* hi, Fn fn) {
  MEMORY_BASIC_INFORMATION info{};
  for (uint8_t* a = lo; a < hi && VirtualQuery(a, &info, sizeof(info)) == sizeof(info);) {
    uint8_t* region = static_cast<uint8_t*>(info.BaseAddress);
    uint8_t* region_end = region + info.RegionSize;
    const DWORD protect = info.Protect & 0xFF;
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (protect == PAGE_READWRITE || protect == PAGE_READONLY || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY))
      fn(std::max(region, lo), std::min(region_end, hi));
    a = region_end;
  }
}

// Runtime field offset by name: string in the image, then the typeinfo entry {name, offset, type}.
void FieldOffsets(uint8_t* membase, std::initializer_list<const char*> names) {
  for (const char* name : names) {
    const size_t len = std::strlen(name) + 1;
    ForEachReadable(membase + 0x82000000u, membase + 0x84000000u, [&](uint8_t* b, uint8_t* e) {
      for (const uint8_t* p = b;;) {
        const ptrdiff_t f = SafeFind(p, e, reinterpret_cast<const uint8_t*>(name), len);
        if (f < 0) break;
        p += f;
        const uint32_t s = uint32_t(p - membase);
        const uint8_t ref[4] = {uint8_t(s >> 24), uint8_t(s >> 16), uint8_t(s >> 8), uint8_t(s)};
        ForEachReadable(membase + 0x84000000u, membase + 0x84800000u, [&](uint8_t* b2, uint8_t* e2) {
          for (const uint8_t* q = b2;;) {
            const ptrdiff_t g = SafeFind(q, e2, ref, 4);
            if (g < 0) break;
            q += g;
            if ((uint32_t(q - membase) & 3) == 0 && q + 12 <= e2)
              REXLOG_WARN("AO4_FPSTEST name {} entry {:08X} offset={} type={:08X}", name, uint32_t(q - membase), Be32(q + 4) & 0xFFFF, Be32(q + 8));
            q += 4;
          }
        });
        p += 1;
      }
    });
  }
}

// probe4: GameTime object (+44 == live GameTimeSettings 0x6FFAD9B0) and the constants that
// GameTime::update (sub_82EED220) uses in mode 2.
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }
double AsDouble(uint64_t v) { double d; std::memcpy(&d, &v, 8); return d; }
float AsFloat(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
void ProbeGameTime(uint8_t* membase) {
  REXLOG_WARN("AO4_FPSTEST const 84076CC8={} 84076CD0={} 82A4E148(f)={} 82A00A88(f)={} 82A00C50={} 82A00C58={}",
              AsDouble(Be64(membase + 0x84076CC8u)), AsDouble(Be64(membase + 0x84076CD0u)),
              AsFloat(Be32(membase + 0x82A4E148u)), AsFloat(Be32(membase + 0x82A00A88u)),
              AsDouble(Be64(membase + 0x82A00C50u)), AsDouble(Be64(membase + 0x82A00C58u)));
  const uint8_t ref[4] = {0x6F, 0xFA, 0xD9, 0xB0};
  int n = 0;
  ForEachReadable(membase + 0x40000000u, membase + 0xA0000000u, [&](uint8_t* b, uint8_t* e) {
    for (const uint8_t* q = b; n < 12;) {
      const ptrdiff_t g = SafeFind(q, e, ref, 4);
      if (g < 0) break;
      q += g;
      const uint8_t* obj = q - 44;
      if ((uint32_t(q - membase) & 3) == 0 && obj >= b && obj + 200 <= e) {
        REXLOG_WARN("AO4_FPSTEST gametime? obj={:08X} vt={:08X} mode(+40)={} +48={} +52={} +56={} +60={} tick(+80)={} +96={}",
                    uint32_t(obj - membase), Be32(obj), Be32(obj + 40), Be32(obj + 48), Be32(obj + 52), Be32(obj + 56),
                    Be32(obj + 60), AsDouble(Be64(obj + 80)), AsDouble(Be64(obj + 96)));
        ++n;
      }
      q += 4;
    }
  });
}

// unlock: client GameTime (+44 -> GameTimeSettings, +40 mode 2 = fixed 1/29.535 s vsync tick)
// switched to mode 0 (tick = 1/MaxSimFps); MaxSimFps patched separately via "sim60".
void UnlockGameTime(uint8_t* membase, const std::set<uint32_t>& seen) {
  for (const uint32_t key : seen) {
    if (key & 1) continue;  // GameTimeSettings only
    const uint32_t settings = key / 2;
    const uint8_t ref[4] = {uint8_t(settings >> 24), uint8_t(settings >> 16), uint8_t(settings >> 8), uint8_t(settings)};
    ForEachReadable(membase + 0x40000000u, membase + 0x50000000u, [&](uint8_t* b, uint8_t* e) {
      for (uint8_t* q = b;;) {
        const ptrdiff_t g = SafeFind(q, e, ref, 4);
        if (g < 0) break;
        q += g;
        uint8_t* obj = q - 44;
        if ((uint32_t(q - membase) & 3) == 0 && obj >= b && Be32(obj + 40) == 2 && Be32(obj + 48) == 30) {
          PutBe32(obj + 40, 0);
          REXLOG_WARN("AO4_FPSTEST GameTime {:08X} mode 2 -> 0 (settings {:08X})", uint32_t(obj - membase), settings);
        }
        q += 4;
      }
    });
  }
}

// dumpimg: writes the readable parts of the guest image range 0x82000000-0x84800000 (constant
// pools of AO4.Main) to %TEMP%/ao4_image_<pid>.bin as {u32 guest, u32 size, bytes} records.
void DumpImage(uint8_t* membase) {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  wchar_t path[MAX_PATH + 64];
  swprintf(path, MAX_PATH + 64, L"%lsao4_image_%lu.bin", tmp, GetCurrentProcessId());
  FILE* f = _wfopen(path, L"wb");
  if (!f) return;
  size_t total = 0;
  ForEachReadable(membase + 0x82000000u, membase + 0x84800000u, [&](uint8_t* b, uint8_t* e) {
    const uint32_t guest = uint32_t(b - membase), size = uint32_t(e - b);
    std::fwrite(&guest, 4, 1, f); std::fwrite(&size, 4, 1, f);
    std::fwrite(b, 1, size, f); total += size;
  });
  std::fclose(f);
  REXLOG_WARN("AO4_FPSTEST image dump {} bytes", total);
}

void Run(std::string mode) {
  if (Has(mode, "dumpimg")) {
    rex::system::KernelState* k = nullptr;
    while (!(k = rex::system::KernelState::shared()) || !k->memory()) Sleep(200);
    Sleep(60000);
    DumpImage(k->memory()->virtual_membase());
    return;
  }
  if ((Has(mode, "hook") || Has(mode, "prof") || Has(mode, "ticks")) && !Has(mode, "scan")) return;  // hook modes need no memory scan
  rex::system::KernelState* kernel = nullptr;
  while (!(kernel = rex::system::KernelState::shared()) || !kernel->memory()) Sleep(200);
  uint8_t* membase = kernel->memory()->virtual_membase();
  REXLOG_WARN("AO4_FPSTEST mode={} scanning guest memory", mode);
  std::set<uint32_t> seen;
  // Three single scans only (a continuous scan stalled frames for up to 2 s).
  const ULONGLONG start = GetTickCount64();
  std::vector<ULONGLONG> schedule{60000ull, 90000ull, 120000ull};
  if (Has(mode, "early")) { schedule.clear(); for (ULONGLONG s = 10000; s <= 120000; s += 4000) schedule.push_back(s); }
  for (const ULONGLONG at : schedule) {
    const ULONGLONG now = GetTickCount64() - start;
    if (now < at) Sleep(DWORD(at - now));
    const ULONGLONG t0 = GetTickCount64();
    if (Has(mode, "probe4") && at >= 60000ull) ProbeGameTime(membase);
    if (Has(mode, "probe3") && at == 60000ull)
      FieldOffsets(membase, {"MinLatency", "MaxLatency", "PacketDrops", "AimScale", "CorruptionChance", "DuplicationChance"});
    if (Has(mode, "probe2")) {
      DumpFields(membase, 0x842E9044, "GameTimeSettings");
      DumpFields(membase, 0x842EFB80, "ClientSettings");
      REXLOG_WARN("AO4_FPSTEST probe2 done");
      return;
    }
    if (mode == "probe") { Probe(membase); REXLOG_WARN("AO4_FPSTEST probe done"); return; }
    Scan(membase, mode, seen);
    if (Has(mode, "unlock")) UnlockGameTime(membase, seen);
    REXLOG_WARN("AO4_FPSTEST scan at {} s took {} ms, objects seen {}", at / 1000, GetTickCount64() - t0, seen.size());
  }
}

struct Starter {
  Starter() {
    const std::string mode = Mode();
    if (!mode.empty()) std::thread(Run, mode).detach();
  }
} starter;
}  // namespace

// hook60 / hook<N>: directly before the original GameTime::update (sub_82EED220) the client
// GameTime's fixed vsync mode 2 (tick 1/29.535 s) is switched to mode 0 (tick = 1/MaxSimFps) and
// MaxSimFps of its settings (+44 -> +32) is set to N (default 60). Experiment only.
namespace {
uint32_t client_game_time = 0;
float HookFps() {
  static const float fps = [] {
    const std::string mode = Mode();
    const size_t at = mode.find("hook");
    if (at == std::string::npos) return 0.0f;
    const int n = std::atoi(mode.c_str() + at + 4);
    return n >= 30 && n <= 240 ? float(n) : 60.0f;
  }();
  return fps;
}
}  // namespace
double g_sim_dt_accum = 0;
REX_HOOK_RAW(sub_82EED220) {
  // Main loop thread: GameTime::update called from sub_82CD6888 (the other caller is a job).
  if (!g_game_thread.load() && uint32_t(ctx.lr) > 0x82CD6888u && uint32_t(ctx.lr) < 0x82CD7000u)
    g_game_thread.store(GetCurrentThreadId());
  const float fps = HookFps();
  if (fps > 0.0f) {
    uint8_t* membase = rex::system::KernelState::shared()->memory()->virtual_membase();
    uint8_t* game_time = membase + ctx.r3.u32;
    if (Be32(game_time + 40) == 2) {
      // var<N>: mode 4 (fixed tick from +48 = 30 Hz, frame limit MaxVariableFps = N); else mode 0.
      if (Has(Mode(), "var4")) {
        PutBe32(game_time + 40, 4);
        const uint32_t settings4 = Be32(game_time + 44);
        if (settings4 >= 0x40000000u && settings4 < 0x80000000u) {
          uint32_t bits;
          std::memcpy(&bits, &fps, 4);
          PutBe32(membase + settings4 + 36, bits);  // MaxVariableFps
        }
        // vfi: VisualFrameInterpolation (+162) of the live ClientSettings, cached by the render
        // step sub_82F72A80 at 0x840BF59C (lis -31732, -2660); only via a valid cached pointer.
        if (Has(Mode(), "vfi")) {
          const uint32_t client_settings = Be32(membase + 0x840BF59Cu);
          if (client_settings >= 0x40000000u && client_settings < 0x80000000u && membase[client_settings + 162] == 0) {
            membase[client_settings + 162] = 1;
            REXLOG_WARN("AO4_FPSTEST VisualFrameInterpolation -> 1 at ClientSettings {:08X}", client_settings);
          }
        }
        static bool logged4 = false;
        client_game_time = ctx.r3.u32;
        if (!logged4) { logged4 = true; REXLOG_WARN("AO4_FPSTEST hook: GameTime {:08X} mode 2 -> 4, MaxVariableFps {}", ctx.r3.u32, fps); }
        goto call_original;
      }
      PutBe32(game_time + 40, 0);
      const uint32_t settings = Be32(game_time + 44);
      if (settings >= 0x40000000u && settings < 0x80000000u) {
        uint32_t bits;
        std::memcpy(&bits, &fps, 4);
        PutBe32(membase + settings + 32, bits);
      }
      static bool logged = false;
      client_game_time = ctx.r3.u32;
      if (!logged) { logged = true; REXLOG_WARN("AO4_FPSTEST hook: GameTime {:08X} mode 2 -> 0, MaxSimFps {}", ctx.r3.u32, fps); }
    }
  }
call_original:
  if (fps > 0.0f && Has(Mode(), "k30")) {
    // k30: shared constant-pool float 1/30 (0x82A054C8, 31 load sites: tick timer lists that report
    // N ticks as N/30 s, per-tick clamps, next-tick times) -> 1/fps, once.
    static std::atomic<bool> k30_done{false};
    if (!k30_done.exchange(true)) {
      uint8_t* mbk = rex::system::KernelState::shared()->memory()->virtual_membase();
      uint8_t* k = mbk + 0x82A054C8u;
      const float tick = 1.0f / fps;
      uint32_t bits; std::memcpy(&bits, &tick, 4);
      DWORD old = 0;
      const BOOL prot = VirtualProtect(k, 4, PAGE_READWRITE, &old);
      const uint32_t before = Be32(k);
      if (before == 0x3D088889u) PutBe32(k, bits);
      if (prot) VirtualProtect(k, 4, old, &old);
      REXLOG_WARN("AO4_FPSTEST k30 {:08X} -> {:08X} (protect {} old {:X})", before, Be32(k), prot, old);
    }
  }
  if (fps > 0.0f && Has(Mode(), "phys") && !Has(Mode(), "phys30") && !Has(Mode(), "srv30")) {
    // phys: the physics job sub_83232500 steps the physics world once per game tick with the constant
    // 0.0332226 s (1/30.1, 0x82A8C1E8, only load site) -> physics time ran 2x at 60 ticks. Scale it by
    // 30/fps once.
    static std::atomic<bool> phys_done{false};
    if (!phys_done.exchange(true)) {
      uint8_t* mbp = rex::system::KernelState::shared()->memory()->virtual_membase();
      uint8_t* k = mbp + 0x82A8C1E8u;
      DWORD old = 0;
      const BOOL prot = VirtualProtect(k, 4, PAGE_READWRITE, &old);
      const uint32_t before = Be32(k);
      if (before == 0x3D081469u) {
        float v; std::memcpy(&v, &before, 4);
        v *= 30.0f / fps;
        uint32_t bits; std::memcpy(&bits, &v, 4);
        PutBe32(k, bits);
      }
      if (prot) VirtualProtect(k, 4, old, &old);
      REXLOG_WARN("AO4_FPSTEST phys {:08X} -> {:08X} (protect {})", before, Be32(k), prot);
    }
  }
  if (fps > 0.0f && Has(Mode(), "rate48")) {
    uint8_t* mb48 = rex::system::KernelState::shared()->memory()->virtual_membase();
    // srv30: the server GameTime (mode 3) keeps 30 ticks per second.
    const bool server30 = Has(Mode(), "srv30") && Be32(mb48 + ctx.r3.u32 + 40) == 3;
    if (!server30 && Be32(mb48 + ctx.r3.u32 + 48) == 30) PutBe32(mb48 + ctx.r3.u32 + 48, uint32_t(fps));
  }
  const uint32_t gt = ctx.r3.u32;
  uint8_t* const mbase = rex::system::KernelState::shared()->memory()->virtual_membase();
  const uint32_t ticks_before = Be32(mbase + gt + 52);
  // srv30: the server (mode 3) shares GameTimeSettings with the client; MaxSimFps 30 during its update.
  const uint32_t srv_settings = Be32(mbase + gt + 44);
  const bool srv30 = fps > 0.0f && Has(Mode(), "srv30") && Be32(mbase + gt + 40) == 3 &&
                     srv_settings >= 0x40000000u && srv_settings < 0x80000000u;
  uint32_t srv_saved = 0;
  if (srv30) { srv_saved = Be32(mbase + srv_settings + 32); PutBe32(mbase + srv_settings + 32, 0x41F00000u); }
  __imp__sub_82EED220(ctx, base);
  if (srv30) PutBe32(mbase + srv_settings + 32, srv_saved);
  // tickfix: the frame step gets "ticks this frame" = +52 - +56 (sub_82EE7560); in mode 0 the update
  // leaves that at 1 although +52 advanced by 2 when a frame needs two ticks, so tick-stepped systems
  // (movement, state counters) lost every second tick below 60 FPS. Set +56 to the count before.
  if (fps > 0.0f && Has(Mode(), "tickfix") && Be32(mbase + gt + 40) == 0) {
    const uint32_t now_ticks = Be32(mbase + gt + 52);
    if (now_ticks - ticks_before >= 1 && now_ticks - ticks_before <= 8) PutBe32(mbase + gt + 56, ticks_before);
  }
  static uint32_t tick_hist[4] = {};
  // Per GameTime object (client mode 2/0, server mode 3): updates, ticks and summed dt (+88).
  struct GtStat { uint32_t updates = 0, ticks = 0; double dt = 0, dt80 = 0; uint32_t mode = 0; };
  static std::map<uint32_t, GtStat> gt_stats;
  {
    auto& s = gt_stats[gt];
    ++s.updates; s.ticks += Be32(mbase + gt + 52) - ticks_before; s.dt += AsDouble(Be64(mbase + gt + 88)); s.dt80 += AsDouble(Be64(mbase + gt + 80)); s.mode = Be32(mbase + gt + 40);
  }
  if (gt == client_game_time || (!client_game_time && Be32(mbase + gt + 40) == 2)) {
    const uint32_t d = Be32(mbase + gt + 52) - ticks_before;
    ++tick_hist[d < 3 ? d : 3];
    // posframe: soldier displacement (copied transform, soldier+0x4F0) per frame, by ticks in the frame.
    if (Has(Mode(), "posframe")) {
      static float last_p[3] = {}; static uint32_t last_s = 0;
      static double dist_by[3] = {}; static uint32_t frames_by[3] = {};
      const uint32_t s = g_pos_soldier.load();
      if (s) {
        float p[3];
        for (int i = 0; i < 3; ++i) { const uint32_t w = Be32(mbase + s + 0x4F0 + 4 * i); std::memcpy(&p[i], &w, 4); }
        if (s == last_s) {
          const double dx = p[0] - last_p[0], dy = p[1] - last_p[1], dz = p[2] - last_p[2];
          const double dd = std::sqrt(dx * dx + dy * dy + dz * dz);
          if (dd > 1e-4 && dd < 1.0 && d >= 1 && d <= 2) { dist_by[d] += dd; ++frames_by[d]; }
        }
        std::memcpy(last_p, p, sizeof(p)); last_s = s;
      }
      static ULONGLONG last_log = 0;
      const ULONGLONG nowp = GetTickCount64();
      if (nowp - last_log >= 1000) {
        last_log = nowp;
        if (frames_by[1] + frames_by[2])
          REXLOG_WARN("AO4_FPSTEST posframe 1tick frames={} avg={:.5f} | 2tick frames={} avg={:.5f}", frames_by[1],
                      frames_by[1] ? dist_by[1] / frames_by[1] : 0.0, frames_by[2], frames_by[2] ? dist_by[2] / frames_by[2] : 0.0);
        dist_by[1] = dist_by[2] = 0; frames_by[1] = frames_by[2] = 0;
      }
    }
    if (!client_game_time) client_game_time = gt;
    g_sim_dt_accum += AsDouble(Be64(mbase + gt + 88));
  }
  if (Has(Mode(), "ticks")) {
    static ULONGLONG last_tick_log = 0;
    const ULONGLONG now_ms = GetTickCount64();
    if (now_ms - last_tick_log >= 5000) {
      last_tick_log = now_ms;
      // Simulation time: sum of per-update deltas (+88 copied to +160 accumulation) and dt (+80).
      REXLOG_WARN("AO4_FPSTEST ticks/frame 0:{} 1:{} 2:{} 3+:{} sim_time_5s={:.3f} last_dt={:.5f} +48={}", tick_hist[0], tick_hist[1], tick_hist[2], tick_hist[3],
                  g_sim_dt_accum, client_game_time ? AsDouble(Be64(mbase + client_game_time + 88)) : 0.0, client_game_time ? Be32(mbase + client_game_time + 48) : 0u);
      g_sim_dt_accum = 0;
      for (auto& [addr, s] : gt_stats)
        REXLOG_WARN("AO4_FPSTEST gt {:08X} mode={} updates={} ticks={} dt88_sum={:.3f} dt80_sum={:.3f} +48={}", addr, s.mode, s.updates, s.ticks, s.dt, s.dt80, Be32(mbase + addr + 48));
      gt_stats.clear();
      FpsConstCountLog();
      FpsTimerLog(mbase);
      FpsListCandLog();
      if (Has(Mode(), "framecall")) { extern std::atomic<uint32_t> g_fc_step, g_fc_worker, g_fc_ticks, g_fc_multi; REXLOG_WARN("AO4_FPSTEST framecall step={} ticks={} multi={} worker={}", g_fc_step.exchange(0), g_fc_ticks.exchange(0), g_fc_multi.exchange(0), g_fc_worker.exchange(0)); }
      { extern std::atomic<uint32_t> g_sched_updates; REXLOG_WARN("AO4_FPSTEST sched updates={}", g_sched_updates.exchange(0)); }
      if (Has(Mode(), "gtdump"))
        for (auto& [addr, s] : std::map<uint32_t, GtStat>{{0x4098EA20u, {}}, {0x4098ED20u, {}}}) {
          std::string hex;
          for (uint32_t o2 = 0; o2 < 0x140; o2 += 4) { char b[16]; std::snprintf(b, sizeof(b), "%s%08X", o2 % 32 ? " " : " | ", Be32(mbase + addr + o2)); hex += b; }
          REXLOG_WARN("AO4_FPSTEST gtdump {:08X}{}", addr, hex);
        }
      tick_hist[0] = tick_hist[1] = tick_hist[2] = tick_hist[3] = 0;
    }
  }
  if (fps > 0.0f && gt == client_game_time) {
    // Every 5 s: game-time counters (+104 accumulated, +112 last host time, both double) vs wall clock.
    static ULONGLONG last_wall = 0;
    static double last_acc = 0;
    const ULONGLONG wall = GetTickCount64();
    if (wall - last_wall >= 5000) {
      uint8_t* membase = rex::system::KernelState::shared()->memory()->virtual_membase();
      const double acc = AsDouble(Be64(membase + gt + 104)), tick = AsDouble(Be64(membase + gt + 80));
      if (last_wall)
        REXLOG_WARN("AO4_FPSTEST gametime {:08X} mode={} tick={:.5f} +104 delta={:.3f} over wall {:.3f} s",
                    gt, Be32(membase + gt + 40), tick, acc - last_acc, (wall - last_wall) / 1000.0);
      last_wall = wall;
      last_acc = acc;
    }
  }
}

// prof: samples the main game thread (the thread that runs GameTime::update for the client
// GameTime) every ~5 ms from 60 s to 80 s after start, unwinds up to 8 host frames and logs the
// most frequent chains as module offsets (symbolise offline with llvm-symbolizer).
namespace {
void ProfileGameThread() {
  Sleep(60000);
  const DWORD tid = g_game_thread.load();
  HANDLE thread = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : nullptr;
  if (!thread) { REXLOG_WARN("AO4_FPSTEST prof: game thread not found"); return; }
  std::map<std::vector<uint64_t>, uint32_t> chains;
  uint32_t samples = 0;
  const ULONGLONG end = GetTickCount64() + 20000;
  while (GetTickCount64() < end) {
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      std::vector<uint64_t> chain;
      if (GetThreadContext(thread, &c)) {
        for (int depth = 0; depth < 24 && c.Rip; ++depth) {
          chain.push_back(c.Rip);
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
          if (!fn) { c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); c.Rsp += 8; continue; }  // leaf
          void* handler_data = nullptr;
          DWORD64 frame = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &frame, nullptr);
        }
      }
      ResumeThread(thread);
      ++chains[chain];
      ++samples;
    }
    Sleep(5);
  }
  CloseHandle(thread);
  std::vector<std::pair<uint32_t, std::vector<uint64_t>>> sorted;
  for (auto& [chain, n] : chains) sorted.push_back({n, chain});
  std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });
  REXLOG_WARN("AO4_FPSTEST prof samples={} distinct={}", samples, sorted.size());
  for (size_t i = 0; i < sorted.size() && i < 25; ++i) {
    std::string text;
    for (uint64_t rip : sorted[i].second) {
      HMODULE module = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(rip), &module)) {
        GetModuleFileNameA(module, name, MAX_PATH);
        const char* slash = std::strrchr(name, 92);  // backslash
        char cell[160];
        std::snprintf(cell, sizeof(cell), "%s+%llx ", slash ? slash + 1 : name, (unsigned long long)(rip - uint64_t(module)));
        text += cell;
      } else {
        char cell[32];
        std::snprintf(cell, sizeof(cell), "%llx ", (unsigned long long)rip);
        text += cell;
      }
    }
    REXLOG_WARN("AO4_FPSTEST prof {} {}", sorted[i].first, text);
  }
}
struct ProfStarter {
  ProfStarter() { if (Has(Mode(), "prof") && !Has(Mode(), "profall")) std::thread(ProfileGameThread).detach(); }
} prof_starter;
}  // namespace

// profall: from 60 s to 75 s after start, samples every thread of the process every ~4 ms. Per
// thread: samples whose top frame is outside ntdll/KERNELBASE count as "busy"; for those the first
// frame in ao4_main_native/rexruntime/rexgpu-xenos (after unwinding) is histogrammed.
namespace {
struct ModRange { const char* name; uintptr_t lo = 0, hi = 0; };
ModRange Range(const wchar_t* dll, const char* name) {
  ModRange r{name};
  if (HMODULE h = GetModuleHandleW(dll)) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(h) + dos->e_lfanew);
    r.lo = reinterpret_cast<uintptr_t>(h); r.hi = r.lo + nt->OptionalHeader.SizeOfImage;
  }
  return r;
}
void ProfileAllThreads() {
  Sleep(60000);
  const ModRange mods[] = {Range(L"ao4_main_native.dll", "main"), Range(L"rexruntime.dll", "runtime"),
                           Range(L"rexgpu-xenos.dll", "gpu"), Range(L"ntdll.dll", "ntdll"), Range(L"KernelBase.dll", "kernelbase")};
  struct T { HANDLE h; DWORD id; uint32_t samples = 0, busy = 0; std::map<std::string, uint32_t> frames, idle; uint64_t cpu0 = 0; };
  auto cpu = [](HANDLE h) -> uint64_t {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
  };
  std::vector<T> threads;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
      if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))
        threads.push_back({h, te.th32ThreadID});
  CloseHandle(snap);
  auto label = [&](DWORD64 rip) -> std::string {
    for (const auto& m : mods) if (rip >= m.lo && rip < m.hi) { char b[64]; std::snprintf(b, sizeof(b), "%s+%llx", m.name, (unsigned long long)(rip - m.lo)); return b; }
    return "other";
  };
  for (auto& t : threads) t.cpu0 = cpu(t.h);
  const ULONGLONG start_ms = GetTickCount64(), end = start_ms + 15000;
  while (GetTickCount64() < end) {
    for (auto& t : threads) {
      if (SuspendThread(t.h) == DWORD(-1)) continue;
      CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
      std::string first;
      bool busy = false;
      if (GetThreadContext(t.h, &c)) {
        busy = !((c.Rip >= mods[3].lo && c.Rip < mods[3].hi) || (c.Rip >= mods[4].lo && c.Rip < mods[4].hi));
        for (int depth = 0; depth < 16 && c.Rip && first.empty(); ++depth) {
          if ((c.Rip >= mods[0].lo && c.Rip < mods[0].hi) || (c.Rip >= mods[1].lo && c.Rip < mods[1].hi) || (c.Rip >= mods[2].lo && c.Rip < mods[2].hi))
            first = label(c.Rip);
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
          if (!fn) { c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); c.Rsp += 8; continue; }
          void* hd = nullptr; DWORD64 frame = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &hd, &frame, nullptr);
        }
      }
      ResumeThread(t.h);
      ++t.samples;
      if (busy) { ++t.busy; ++t.frames[first.empty() ? "?" : first]; } else { ++t.idle[first.empty() ? "?" : first]; }
    }
    Sleep(4);
  }
  const double wall_100ns = double(GetTickCount64() - start_ms) * 10000.0;
  for (auto& t : threads) {
    const double cpu_pct = 100.0 * double(cpu(t.h) - t.cpu0) / wall_100ns;
    CloseHandle(t.h);
    if (cpu_pct < 5.0) continue;  // real CPU time >= 5 % of one core
    std::vector<std::pair<uint32_t, std::string>> top;
    for (auto& [k, n] : t.frames) top.push_back({n, k});
    std::sort(top.rbegin(), top.rend());
    std::string text;
    for (size_t i = 0; i < top.size() && i < 8; ++i) text += top[i].second + "=" + std::to_string(top[i].first) + " ";
    REXLOG_WARN("AO4_FPSTEST profall tid={} cpu={:.0f}% sampled_busy={}/{} {}", t.id, cpu_pct, t.busy, t.samples, text);
    std::vector<std::pair<uint32_t, std::string>> idle;
    for (auto& [k, n] : t.idle) idle.push_back({n, k});
    std::sort(idle.rbegin(), idle.rend());
    std::string itext;
    for (size_t i = 0; i < idle.size() && i < 6; ++i) itext += idle[i].second + "=" + std::to_string(idle[i].first) + " ";
    REXLOG_WARN("AO4_FPSTEST profall tid={} idle-in {}", t.id, itext);
  }
  REXLOG_WARN("AO4_FPSTEST profall done threads={} game_thread={}", threads.size(), g_game_thread.load());
}
struct ProfAllStarter {
  ProfAllStarter() { if (Has(Mode(), "profall")) std::thread(ProfileAllThreads).detach(); }
} prof_all_starter;
}  // namespace

void FpsArgDump(const char* name, uint32_t r3, uint32_t r4, unsigned words) {
  if (!Has(Mode(), "argdump")) return;
  static std::map<std::string, ULONGLONG> last;
  const ULONGLONG now = GetTickCount64();
  ULONGLONG& l = last[name];
  if (now - l < 5000) return;
  l = now;
  uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
  std::string s;
  for (unsigned i = 0; i < words; ++i) {
    const uint32_t w = Be32(mb + r4 + 4 * i);
    float f; std::memcpy(&f, &w, 4);
    char buf[48]; std::snprintf(buf, sizeof(buf), " %08X(%g)", w, f); s += buf;
  }
  REXLOG_WARN("AO4_FPSTEST arg {} r3={:08X} r4={:08X}{}", name, r3, r4, s);
  if (Has(Mode(), "argstack")) {
    // Host call chain (recompiled guest functions call each other natively) as main-DLL offsets.
    void* frames[28] = {};
    const USHORT count = CaptureStackBackTrace(1, 28, frames, nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FpsArgDump), &self);
    std::string st;
    for (USHORT i = 0; i < count; ++i) {
      char b[24]; std::snprintf(b, sizeof(b), " %llX", (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)self)); st += b;
    }
    REXLOG_WARN("AO4_FPSTEST argstack {} tid={}{}", name, GetCurrentThreadId(), st);
  }
}

// rate48 part 2: the world tick counters (TickTimer: +0 ticks, +4 ticks per second 30, +32 cached tick
// time float, +40 cached time double, +48 cache flag; time = ticks / rate) keep their own 30 Hz rate, so
// world time, timers, weapons and animations ran twice as fast with 60 ticks. sub_82EE7028 advances
// +0 by r4 ticks: on the first advance at rate 30 switch to 60 and double +0, so time = ticks/rate stays
// continuous (+40 unchanged); +32 becomes 1/60.
REX_HOOK_RAW(sub_82EE7028) {
  const float fps = HookFps();
  if (fps > 0.0f && Has(Mode(), "tickrate")) {
    uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
    uint8_t* timer = mb + ctx.r3.u32;
    const uint32_t rate = Be32(timer + 4), target = uint32_t(fps);
    const bool client_timer = uint32_t(ctx.lr) >= 0x82F72A80u && uint32_t(ctx.lr) < 0x82F74000u;
    if (rate == 30 && target != 30 && (client_timer || !Has(Mode(), "srv30"))) {
      const uint32_t ticks = Be32(timer + 0);
      PutBe32(timer + 0, uint32_t(uint64_t(ticks) * target / 30));
      PutBe32(timer + 4, target);
      const float tick = 1.0f / float(target);
      uint32_t bits; std::memcpy(&bits, &tick, 4);
      PutBe32(timer + 32, bits);
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 64)
        REXLOG_WARN("AO4_FPSTEST tickrate timer {:08X} ticks {} -> {} rate 30 -> {} lr={:08X}", ctx.r3.u32, ticks,
                    Be32(timer + 0), target, uint32_t(ctx.lr));
    }
  }
  __imp__sub_82EE7028(ctx, base);
}

// ttscan: every TickTimer seen by its getters (time 82EE7048, tick time 82EE70B8, set 82EE70F0), with
// rate (+4), ticks (+0), cache flag (+48), cached tick time (+32), logged every 5 s from the ticks block.
namespace {
std::mutex g_tt_mutex;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_tt;  // timer -> (getter, calls)
void NoteTimer(uint32_t timer, uint32_t getter) {
  if (!Has(Mode(), "ttscan")) return;
  std::lock_guard<std::mutex> lock(g_tt_mutex);
  auto& e = g_tt[timer]; e.first = getter; ++e.second;
}
}  // namespace
void FpsTimerLog(uint8_t* mbase) {
  if (!Has(Mode(), "ttscan")) return;
  std::lock_guard<std::mutex> lock(g_tt_mutex);
  for (auto& [timer, e] : g_tt) {
    uint8_t* p = mbase + timer;
    const uint32_t tb = Be32(p + 32); float tick; std::memcpy(&tick, &tb, 4);
    REXLOG_WARN("AO4_FPSTEST tt {:08X} getter={:08X} calls={} ticks={} rate={} flag={} tick={:.5f}", timer, e.first, e.second,
                Be32(p + 0), Be32(p + 4), p[48], tick);
  }
  g_tt.clear();
}
REX_HOOK_RAW(sub_82EE7048) { NoteTimer(ctx.r3.u32, 0x82EE7048u); __imp__sub_82EE7048(ctx, base); }
REX_HOOK_RAW(sub_82EE70B8) { NoteTimer(ctx.r3.u32, 0x82EE70B8u); __imp__sub_82EE70B8(ctx, base); }
REX_HOOK_RAW(sub_82EE70F0) { NoteTimer(ctx.r3.u32, 0x82EE70F0u); __imp__sub_82EE70F0(ctx, base); }

// stackdump: r3/r4/f1 and the host call chain (main-DLL offsets) of a hooked guest call, once per 5 s.
void FpsStackDump(const char* name, uint32_t r3, uint32_t r4, double f1) {
  if (!Has(Mode(), "stackdump")) return;
  static std::mutex m;
  static std::map<std::string, ULONGLONG> last;
  {
    std::lock_guard<std::mutex> lock(m);
    const ULONGLONG now = GetTickCount64();
    ULONGLONG& l = last[name];
    if (now - l < 5000) return;
    l = now;
  }
  void* frames[30] = {};
  const USHORT count = CaptureStackBackTrace(1, 30, frames, nullptr);
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&FpsStackDump), &self);
  std::string st;
  for (USHORT i = 0; i < count; ++i) {
    char b[24]; std::snprintf(b, sizeof(b), " %llX", (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)self)); st += b;
  }
  REXLOG_WARN("AO4_FPSTEST stack {} r3={:08X} r4={:08X} f1={:.5f} tid={}{}", name, r3, r4, f1, GetCurrentThreadId(), st);
}

// phys with srv30: physics worlds step per own tick with 0.0332226 s (sub_83232500 -> vtable+148 ->
// sub_83A24E20, f1). Per world (r3) measure calls per second; worlds stepped at the client rate get
// f1 * 30/fps, the 30 Hz server world keeps the original step.
REX_HOOK_RAW(sub_83A24E20) {
  const float fps = HookFps();
  // phys30: keep the original physics cadence instead of halving the step: every game tick steps the
  // physics world with the fixed 0.0332226 s, so at 60 ticks skip every second step per world.
  // Halved steps made bodies move half as far per step, so the engine's rest detection put ragdolls
  // to sleep at once (corpses frozen in their last, often crouched, pose).
  if (fps > 30.0f && Has(Mode(), "phys30") && std::fabs(ctx.f1.f64 - 0.0332226) < 1e-4) {
    static std::mutex m30;
    static std::map<uint32_t, uint32_t> calls30;
    bool skip;
    { std::lock_guard<std::mutex> lock(m30); skip = (++calls30[ctx.r3.u32] & 1) == 0; }
    if (skip) return;
  }
  if (fps > 0.0f && Has(Mode(), "phys") && !Has(Mode(), "phys30") && Has(Mode(), "srv30") && std::fabs(ctx.f1.f64 - 0.0332226) < 1e-4) {
    struct Rate { ULONGLONG start = 0; uint32_t calls = 0; float per_sec = 0; };
    static std::mutex m;
    static std::map<uint32_t, Rate> rates;
    float per_sec;
    {
      std::lock_guard<std::mutex> lock(m);
      Rate& r = rates[ctx.r3.u32];
      const ULONGLONG now = GetTickCount64();
      if (!r.start) r.start = now;
      ++r.calls;
      if (now - r.start >= 1000) {
        r.per_sec = r.calls * 1000.0f / float(now - r.start);
        static int logged = 0;
        if (logged < 40) { ++logged; REXLOG_WARN("AO4_FPSTEST physworld {:08X} {:.1f} steps/s", ctx.r3.u32, r.per_sec); }
        r.start = now; r.calls = 0;
      }
      per_sec = r.per_sec;
    }
    if (per_sec > 45.0f) ctx.f1.f64 = ctx.f1.f64 * 30.0 / fps;
  }
  __imp__sub_83A24E20(ctx, base);
}

// sched: the update scheduler sub_82DE7230 updates an object every ceil(30 / F) ticks, F = update
// frequency in Hz from sub_82DEA590 (object +328, default 30). With 60 ticks objects updated twice
// as often; scale F by 30/fps so the interval in real time stays original. Counts updates (82DE9028).
std::atomic<uint32_t> g_sched_updates{0};
REX_HOOK_RAW(sub_82DEA590) {
  __imp__sub_82DEA590(ctx, base);
  const float fps = HookFps();
  if (fps > 0.0f && Has(Mode(), "sched")) ctx.f1.f64 = ctx.f1.f64 * 30.0 / fps;
}
REX_HOOK_RAW(sub_82DE9028) {
  g_sched_updates.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82DE9028(ctx, base);
}

namespace {
// posprobe: every 100 ms the local soldier object (resolved like aim_source_observer: 0x841350DC
// -> +52 -> +184 player (vt 82A3F850) -> +1004 -> link-4 soldier (vt 82AAB0D0)) is copied, 0x1000
// bytes, to %TEMP%/ao4_pos_<pid>.bin as {u64 ms, u32 soldier, u32 client ticks, bytes} for 4 min,
// so movement speed can be compared between 30 and 60 FPS offline.
bool GuestReadable(uint8_t* membase, uint32_t a, size_t n) {
  if (a < 0x10000u || uint64_t(a) + n > 0x100000000ull) return false;
  MEMORY_BASIC_INFORMATION m{};
  uint8_t* p = membase + a;
  return VirtualQuery(p, &m, sizeof(m)) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
         uintptr_t(p) + n <= uintptr_t(m.BaseAddress) + m.RegionSize;
}
uint32_t SafeBe32(uint8_t* mb, uint32_t a) { return GuestReadable(mb, a, 4) ? Be32(mb + a) : 0; }
void PosProbe() {
  rex::system::KernelState* k = nullptr;
  while (!(k = rex::system::KernelState::shared()) || !k->memory()) Sleep(200);
  uint8_t* mb = k->memory()->virtual_membase();
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  wchar_t path[MAX_PATH + 64];
  swprintf(path, MAX_PATH + 64, L"%lsao4_pos_%lu.bin", tmp, GetCurrentProcessId());
  FILE* f = nullptr;
  const ULONGLONG t0 = GetTickCount64();
  ULONGLONG first = 0;
  static uint8_t buf[0x1000];
  for (;;) {
    Sleep(100);
    const uint32_t list = SafeBe32(mb, SafeBe32(mb, 0x841350DCu) + 52);
    const uint32_t player = list ? SafeBe32(mb, list + 184) : 0;
    if (!player || SafeBe32(mb, player) != 0x82A3F850u) continue;
    const uint32_t link = SafeBe32(mb, SafeBe32(mb, player + 1004));
    if (link < 4) continue;
    const uint32_t soldier = link - 4;
    if (SafeBe32(mb, soldier) != 0x82AAB0D0u || !GuestReadable(mb, soldier, sizeof(buf))) continue;
    const ULONGLONG now = GetTickCount64();
    static uint32_t last_soldier = 0, last_src = 0;
    if (soldier != last_soldier) { last_soldier = soldier; REXLOG_WARN("AO4_FPSTEST posprobe soldier {:08X}", soldier); }
    g_pos_soldier.store(soldier);
    {
      // Transform source copied by sub_82FC0DC8 into soldier+1216: *(+28), or with bit 22 of +8 an
      // entry of that array (bytes +6/+7 select it); position = source + 48.
      const uint32_t arr = Be32(mb + soldier + 28);
      uint32_t src = arr;
      if (GuestReadable(mb, arr, 8) && ((Be32(mb + soldier + 8) >> 22) & 1))
        src = arr + (uint32_t(mb[arr + 7]) * 2 + mb[arr + 6]) * 32 + 16;
      if (src != last_src) { last_src = src; REXLOG_WARN("AO4_FPSTEST possrc obj={:08X} src={:08X}", soldier, src); }
    }
    if (!first) { first = now; f = _wfopen(path, L"wb"); if (!f) return; }
    if (now - first > 240000) break;
    std::memcpy(buf, mb + soldier, sizeof(buf));
    const uint64_t ms = now - t0;
    const uint32_t ticks = client_game_time ? Be32(mb + client_game_time + 52) : 0;
    std::fwrite(&ms, 8, 1, f); std::fwrite(&soldier, 4, 1, f); std::fwrite(&ticks, 4, 1, f);
    std::fwrite(buf, 1, sizeof(buf), f);
    std::fflush(f);
  }
  if (f) std::fclose(f);
}
struct PosStarter {
  PosStarter() { if (Has(Mode(), "posprobe")) std::thread(PosProbe).detach(); }
} g_pos_starter;
}  // namespace


// framecall: calls of the frame step sub_82F72A80 (r4+8 = ticks of the frame, r4+4 dt) and of its per-tick
// worker sub_82F60010 (r5, r6 = tick index), logged every 5 s from the ticks block.
std::atomic<uint32_t> g_fc_step{0}, g_fc_worker{0}, g_fc_ticks{0}, g_fc_multi{0};
REX_HOOK_RAW(sub_82F72A80) {
  if (Has(Mode(), "framecall")) {
    uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
    const uint32_t ticks = Be32(mb + ctx.r4.u32 + 8);
    g_fc_step.fetch_add(1); g_fc_ticks.fetch_add(ticks); if (ticks > 1) g_fc_multi.fetch_add(1);
  }
  __imp__sub_82F72A80(ctx, base);
}
REX_HOOK_RAW(sub_82F60010) {
  if (Has(Mode(), "framecall")) g_fc_worker.fetch_add(1);
  __imp__sub_82F60010(ctx, base);
}

// setdump: settings registry lookup sub_82D76860 (r3 registry, r4 key = type info) -> settings object.
// Logs each new key once with 4 words of the key and the first 0x90 bytes of the object.
namespace {
void DumpSetting(uint8_t* mb, uint32_t key, uint32_t obj) {
  static std::mutex m;
  static std::set<uint32_t> seen;
  {
    std::lock_guard<std::mutex> lock(m);
    if (!seen.insert(key).second || seen.size() > 300) return;
  }
  std::string ks, os;
  if (GuestReadable(mb, key, 16)) for (int i = 0; i < 4; ++i) { char b[12]; std::snprintf(b, sizeof(b), " %08X", Be32(mb + key + 4 * i)); ks += b; }
  if (obj && GuestReadable(mb, obj, 0x90)) for (int i = 0; i < 0x90 / 4; ++i) { char b[12]; std::snprintf(b, sizeof(b), " %08X", Be32(mb + obj + 4 * i)); os += b; }
  REXLOG_WARN("AO4_FPSTEST setting key={:08X}[{} ] obj={:08X}:{}", key, ks, obj, os);
}
}  // namespace
REX_HOOK_RAW(sub_82D76860) {
  const uint32_t key = ctx.r4.u32;
  __imp__sub_82D76860(ctx, base);
  if (Has(Mode(), "setdump")) DumpSetting(rex::system::KernelState::shared()->memory()->virtual_membase(), key, ctx.r3.u32);
}

// listprobe: job list owner of sub_82E3D420 (r3): active list +1348/+1352 (vector of pointers), incoming
// list +2392/+2396. Every 5 s: sizes and a vtable histogram of the active list (first 64 items).
REX_HOOK_RAW(sub_82E3D420) {
  if (Has(Mode(), "listprobe")) {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 5000) {
      last = now;
      uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
      const uint32_t o = ctx.r3.u32;
      const uint32_t b = Be32(mb + o + 1348), e = Be32(mb + o + 1352);
      const uint32_t ib = Be32(mb + o + 2392), ie = Be32(mb + o + 2396);
      std::map<uint32_t, int> vt;
      if (e >= b && e - b < 0x10000 && GuestReadable(mb, b, e - b))
        for (uint32_t p = b; p < e && p < b + 256; p += 4) { const uint32_t it = Be32(mb + p); vt[GuestReadable(mb, it, 4) ? Be32(mb + it) : 0]++; }
      std::string s;
      for (auto& [v, n] : vt) { char x[24]; std::snprintf(x, sizeof(x), " %08X:%d", v, n); s += x; }
      REXLOG_WARN("AO4_FPSTEST list owner={:08X} active={} incoming={} vt{}", o, (e - b) / 4, (ie - ib) / 4, s);
    }
  }
  __imp__sub_82E3D420(ctx, base);
}

// soldmon: per soldier (sub_82FC4368 r3, vt 82AAB0D0) update counts. Every 10 s logs the number of
// soldiers updated and each soldier that was updated before but not in this window while still
// alive in memory (vtable intact): position (+0x4F0), speed (+0x98), seconds since its last update.
namespace {
struct SoldMon { uint32_t calls = 0; ULONGLONG last = 0; bool reported = false; void* stack[20] = {}; uint16_t depth = 0; DWORD tid = 0;
  // stucktrace
  ULONGLONG born = 0; uint32_t total = 0; float ref[3] = {}; uint32_t ref_call = 0; bool dumped_run = false, dumped_stuck = false;
  ULONGLONG stuck_at = 0; uint8_t later = 0;
  // deathprobe: +0x20 (health, also +0x9C) falls to 0 -> follow the corpse transform for 2 s.
  float health = -1; ULONGLONG died = 0; float death_pos[3] = {}; float max_drop = 0, max_move = 0; bool death_logged = false;
  // idleprobe: alive soldier that moved before and then stays within 5 cm for > 20 s.
  float idle_pos[3] = {}, first_pos[3] = {}; ULONGLONG idle_since = 0; bool moved_once = false, idle_logged = false, have_first = false; };
void DumpSoldier(uint8_t* mb, uint32_t soldier, const char* tag) {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  wchar_t path[MAX_PATH + 96];
  swprintf(path, MAX_PATH + 96, L"%lsao4_soldier_%lu_%08X_%hs.bin", tmp, GetCurrentProcessId(), soldier, tag);
  FILE* f = _wfopen(path, L"wb");
  if (!f) return;
  std::fwrite(mb + soldier, 1, 0x1000, f);
  std::fclose(f);
  REXLOG_WARN("AO4_FPSTEST stuckdump {:08X} {}", soldier, tag);
}
std::mutex g_sm_mutex;
std::map<uint32_t, SoldMon> g_sm;
// passprobe: entity update pass (sub_82D50290 r3) -> calls; owner (sub_83036FE0 r3) -> pass;
// soldier (sub_82FC4368 r3) -> owner. Thread-local current pass/owner while their update runs.
thread_local uint32_t t_pass = 0, t_owner = 0;
std::map<uint32_t, uint32_t> g_pass_calls, g_owner_pass, g_soldier_owner;
}  // namespace
REX_HOOK_RAW(sub_82FC4368) {
  if (Has(Mode(), "soldmon")) {
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG last_log = 0;
    std::lock_guard<std::mutex> lock(g_sm_mutex);
    SoldMon& s = g_sm[ctx.r3.u32];
    ++s.calls; s.last = now; s.reported = false;
    if (Has(Mode(), "bosslog") && s.calls == 2) {
      uint8_t* mbb = rex::system::KernelState::shared()->memory()->virtual_membase();
      const uint32_t w = Be32(mbb + ctx.r3.u32 + 0x20); float hp; std::memcpy(&hp, &w, 4);
      if (hp > 900.0f) REXLOG_WARN("AO4_FPSTEST boss {:08X} hp={:.0f}", ctx.r3.u32, hp);
    }
    if (Has(Mode(), "idleprobe")) {
      uint8_t* mbi = rex::system::KernelState::shared()->memory()->virtual_membase();
      const uint32_t so = ctx.r3.u32;
      uint32_t w = Be32(mbi + so + 0x20); float hp; std::memcpy(&hp, &w, 4);
      float pos[3];
      for (int i = 0; i < 3; ++i) { w = Be32(mbi + so + 0x4F0 + 4 * i); std::memcpy(&pos[i], &w, 4); }
      if (!s.have_first) { s.have_first = true; std::memcpy(s.first_pos, pos, sizeof(pos)); }
      { const float fx = pos[0] - s.first_pos[0], fz = pos[2] - s.first_pos[2]; if (fx * fx + fz * fz > 1.0f) s.moved_once = true; }
      const float dx = pos[0] - s.idle_pos[0], dz = pos[2] - s.idle_pos[2];
      if (!s.idle_since || dx * dx + dz * dz > 0.05f * 0.05f) {
        std::memcpy(s.idle_pos, pos, sizeof(pos)); s.idle_since = now; s.idle_logged = false;
      } else if (hp > 0.0f && s.moved_once && !s.idle_logged && now - s.idle_since > 20000) {
        s.idle_logged = true;
        w = Be32(mbi + so + 0x98); float v; std::memcpy(&v, &w, 4);
        REXLOG_WARN("AO4_FPSTEST idle {:08X} alive hp={:.1f} still={:.0f}s v={:.2f} pos=({:.1f},{:.1f},{:.1f})", so, hp,
                    (now - s.idle_since) / 1000.0, v, pos[0], pos[1], pos[2]);
      }
    }
    if (Has(Mode(), "deathprobe")) {
      uint8_t* mbd = rex::system::KernelState::shared()->memory()->virtual_membase();
      const uint32_t so = ctx.r3.u32;
      uint32_t w = Be32(mbd + so + 0x20); float hp; std::memcpy(&hp, &w, 4);
      float pos[3];
      for (int i = 0; i < 3; ++i) { w = Be32(mbd + so + 0x4F0 + 4 * i); std::memcpy(&pos[i], &w, 4); }
      if (s.health > 0.0f && hp <= 0.0f && !s.died) { s.died = now; std::memcpy(s.death_pos, pos, sizeof(pos)); DumpSoldier(mbd, so, "death0"); }
      s.health = hp;
      if (s.died && s.later < 3 && now - s.died >= 1500) { s.later = 3; DumpSoldier(mbd, so, "death15"); }
      if (s.died && !s.death_logged) {
        const float dx = pos[0] - s.death_pos[0], dy = pos[1] - s.death_pos[1], dz = pos[2] - s.death_pos[2];
        s.max_move = std::max(s.max_move, std::sqrt(dx * dx + dy * dy + dz * dz));
        s.max_drop = std::max(s.max_drop, -dy);
        if (now - s.died >= 2000) {
          s.death_logged = true;
          REXLOG_WARN("AO4_FPSTEST death {:08X} corpse_move={:.3f} drop={:.3f} pos=({:.1f},{:.1f},{:.1f}) calls={}", so, s.max_move, s.max_drop,
                      s.death_pos[0], s.death_pos[1], s.death_pos[2], s.total);
        }
      }
    }
    if (Has(Mode(), "stucktrace")) {
      uint8_t* mbt = rex::system::KernelState::shared()->memory()->virtual_membase();
      const uint32_t so = ctx.r3.u32;
      if (!s.born) s.born = now;
      ++s.total;
      float pos[3], v; uint32_t w;
      for (int i = 0; i < 3; ++i) { w = Be32(mbt + so + 0x4F0 + 4 * i); std::memcpy(&pos[i], &w, 4); }
      w = Be32(mbt + so + 0x98); std::memcpy(&v, &w, 4);
      const ULONGLONG age = now - s.born;
      if (age < 30000 && (s.total % 6) == 0)
        REXLOG_WARN("AO4_FPSTEST trace {:08X} t={:.2f} pos=({:.2f},{:.2f},{:.2f}) v={:.2f} tick={}", so, age / 1000.0, pos[0], pos[1], pos[2], v,
                    client_game_time ? Be32(mbt + client_game_time + 52) : 0u);
      // Follow-up dumps 3 s and 8 s after entering the still state (cover), to see which state
      // advances (30 FPS: soldier leaves cover again) and which stays.
      if (s.stuck_at && s.later < 2 && now - s.stuck_at >= (s.later == 0 ? 3000u : 8000u)) {
        DumpSoldier(mbt, so, s.later == 0 ? "stuck3" : "stuck8");
        ++s.later;
      }
      if (s.total - s.ref_call >= 60) {  // ~1 s at 60 ticks
        const float dx = pos[0] - s.ref[0], dz = pos[2] - s.ref[2];
        const float moved = std::sqrt(dx * dx + dz * dz);
        if (!s.dumped_run && v > 3.0f && moved > 2.0f) { s.dumped_run = true; DumpSoldier(mbt, so, "run"); }
        if (!s.dumped_stuck && s.ref_call && v > 1.0f && moved < 0.05f) {
          s.dumped_stuck = true; s.stuck_at = now; DumpSoldier(mbt, so, "stuck");
          // Targets of the fields that change only on stuck soldiers: +0x3D4 (shared slot), +0x6F8 list node,
          // +0x708 shared object, +0x398: vtable and first 16 words each, plus the shared slot object base.
          for (uint32_t off : {0x3D4u, 0x6F8u, 0x708u, 0x398u, 0x3D0u, 0x3DCu, 0x6F4u, 0x70Cu}) {
            const uint32_t ptr = Be32(mbt + so + off);
            std::string ws;
            if (ptr >= 0x10000u && GuestReadable(mbt, ptr & ~3u, 64))
              for (int i = 0; i < 16; ++i) { char x[12]; std::snprintf(x, sizeof(x), " %08X", Be32(mbt + (ptr & ~3u) + 4 * i)); ws += x; }
            REXLOG_WARN("AO4_FPSTEST stuckref {:08X} +{:03X} -> {:08X}:{}", so, off, ptr, ws);
          }
          REXLOG_WARN("AO4_FPSTEST stuck {:08X} v={:.2f} moved={:.3f} pos=({:.2f},{:.2f},{:.2f}) age={:.1f}s", so, v, moved, pos[0], pos[1], pos[2], age / 1000.0);
        }
        std::memcpy(s.ref, pos, sizeof(pos)); s.ref_call = s.total;
      }
    }
    if (Has(Mode(), "soldstack") && (s.calls & 15) == 1) {
      const bool first = !s.depth;
      s.depth = CaptureStackBackTrace(1, 20, s.stack, nullptr); s.tid = GetCurrentThreadId();
      if (first) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&FpsStackDump), &self);
        std::string st;
        for (uint16_t i = 0; i < s.depth; ++i) { char x[24]; std::snprintf(x, sizeof(x), " %llX", (unsigned long long)((uint8_t*)s.stack[i] - (uint8_t*)self)); st += x; }
        REXLOG_WARN("AO4_FPSTEST soldstack {:08X} tid={} first-update stack{}", ctx.r3.u32, s.tid, st);
      }
    }
    if (t_owner) g_soldier_owner[ctx.r3.u32] = t_owner;
    if (!last_log) last_log = now;
    if (now - last_log >= 10000) {
      last_log = now;
      uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
      int active = 0, stalled = 0;
      std::string detail;
      for (auto it = g_sm.begin(); it != g_sm.end();) {
        SoldMon& m = it->second;
        const bool alive = GuestReadable(mb, it->first, 0x500) && Be32(mb + it->first) == 0x82AAB0D0u;
        if (!alive) { it = g_sm.erase(it); continue; }
        if (m.died && !m.death_logged && now - m.died >= 2000) {  // corpse stopped receiving updates
          m.death_logged = true;
          REXLOG_WARN("AO4_FPSTEST death {:08X} corpse_move={:.3f} drop={:.3f} pos=({:.1f},{:.1f},{:.1f}) no-updates", it->first, m.max_move,
                      m.max_drop, m.death_pos[0], m.death_pos[1], m.death_pos[2]);
        }
        if (m.calls) ++active;
        else if (!m.reported || now - m.last < 60000) {
          ++stalled;
          float p[3], v; uint32_t w;
          for (int i = 0; i < 3; ++i) { w = Be32(mb + it->first + 0x4F0 + 4 * i); std::memcpy(&p[i], &w, 4); }
          w = Be32(mb + it->first + 0x98); std::memcpy(&v, &w, 4);
          char b[128];
          const uint32_t ow = g_soldier_owner.count(it->first) ? g_soldier_owner[it->first] : 0;
          const uint32_t ps = g_owner_pass.count(ow) ? g_owner_pass[ow] : 0;
          std::snprintf(b, sizeof(b), " %08X@(%.1f,%.1f,%.1f) v=%.2f idle=%.0fs own=%08X pass=%08X", it->first, p[0], p[1], p[2], v,
                        (now - m.last) / 1000.0, ow, ps);
          detail += b; m.reported = true;
          if (Has(Mode(), "soldstack") && m.depth) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&FpsStackDump), &self);
            std::string st;
            for (uint16_t i = 0; i < m.depth; ++i) { char x[24]; std::snprintf(x, sizeof(x), " %llX", (unsigned long long)((uint8_t*)m.stack[i] - (uint8_t*)self)); st += x; }
            REXLOG_WARN("AO4_FPSTEST soldstack {:08X} tid={} last-update stack{}", it->first, m.tid, st);
          }
        }
        m.calls = 0; ++it;
      }
      REXLOG_WARN("AO4_FPSTEST soldmon updated={} stalled={}{}", active, stalled, detail);
      if (Has(Mode(), "passprobe")) {
        std::string ps;
        for (auto& [pass, calls] : g_pass_calls) {
          int owners = 0; for (auto& [o2, p2] : g_owner_pass) if (p2 == pass) ++owners;
          char x[64]; std::snprintf(x, sizeof(x), " %08X:calls=%u,owners=%d", pass, calls, owners); ps += x;
        }
        REXLOG_WARN("AO4_FPSTEST passes{}", ps);
        g_pass_calls.clear();
      }
    }
  }
  __imp__sub_82FC4368(ctx, base);
}

// soldscan: every 10 s scan committed guest heap 0x40000000-0x50000000 (16-byte steps) for soldier
// objects (vt 82AAB0D0, +4 == 1 or 0x11000001 not checked) and log them with position, speed and
// whether sub_82FC4368 updated them in the last 10 s (soldmon map).
namespace {
void SoldScan() {
  rex::system::KernelState* k = nullptr;
  while (!(k = rex::system::KernelState::shared()) || !k->memory()) Sleep(500);
  uint8_t* mb = k->memory()->virtual_membase();
  for (;;) {
    Sleep(10000);
    std::vector<uint32_t> found;
    ForEachReadable(mb + 0x40000000u, mb + 0x50000000u, [&](uint8_t* b, uint8_t* e) {
      for (uint8_t* p = b; p + 0x500 <= e; p += 16)
        if (p[0] == 0x82 && p[1] == 0xAA && p[2] == 0xB0 && p[3] == 0xD0) found.push_back(uint32_t(p - mb));
    });
    std::string s;
    const ULONGLONG now = GetTickCount64();
    int fresh = 0;
    for (uint32_t a : found) {
      float pos[3], v; uint32_t w;
      for (int i = 0; i < 3; ++i) { w = Be32(mb + a + 0x4F0 + 4 * i); std::memcpy(&pos[i], &w, 4); }
      w = Be32(mb + a + 0x98); std::memcpy(&v, &w, 4);
      double idle = -1;
      { std::lock_guard<std::mutex> lock(g_sm_mutex); auto it = g_sm.find(a); if (it != g_sm.end()) idle = (now - it->second.last) / 1000.0; }
      if (idle >= 0 && idle < 10) ++fresh;
      if (s.size() < 3000) { char b[112]; std::snprintf(b, sizeof(b), " %08X@(%.1f,%.1f,%.1f) v=%.2f upd=%.0f", a, pos[0], pos[1], pos[2], v, idle); s += b; }
    }
    REXLOG_WARN("AO4_FPSTEST soldscan count={} updated_by_82FC4368={}{}", found.size(), fresh, s);
  }
}
struct SoldScanStarter { SoldScanStarter() { if (Has(Mode(), "soldscan")) std::thread(SoldScan).detach(); } } g_soldscan_starter;
}  // namespace

// tlprobe: update list of sub_83036FE0 (r3 owner, entries +760/+764: {s32 interval ticks (0 = every
// call), s32 countdown, component}). Every 10 s per owner: entry count, interval histogram, and entries
// whose countdown exceeds 120 ticks (would not fire for > 2 s at 60 Hz).

// tlhalf: tick timer lists (r3+760/+764, entries {s32 interval ticks, s32 countdown, target}; the list
// function subtracts info+12 (= 1) from the countdown of every entry with interval != 0 unless
// info+16 == 3, fires at <= 0 and reports interval * 1/30 s). Intervals were authored for 30 ticks per
// second, so at 60 timers fired twice as early (e.g. soldiers taken out of the update lists by
// 82FC4490 before their animation finished). On every second call per list, add the decrement back
// first, so countdowns advance at 30 per second again.
namespace {
void HalfTimerList(uint32_t list_owner, uint32_t info) {
  if (HookFps() <= 30.0f || !Has(Mode(), "tlhalf")) return;
  uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
  if (int32_t(Be32(mb + info + 16)) == 3) return;
  static std::mutex m;
  static std::map<uint32_t, uint32_t> calls;
  {
    std::lock_guard<std::mutex> lock(m);
    if ((++calls[list_owner] & 1) == 1) return;  // odd call: normal decrement
  }
  const uint32_t dec = Be32(mb + info + 12);
  const uint32_t b = Be32(mb + list_owner + 760), e = Be32(mb + list_owner + 764);
  if (e < b || e - b > 0x20000 || !GuestReadable(mb, b, e - b)) return;
  for (uint32_t p = b; p + 12 <= e; p += 12)
    if (Be32(mb + p) != 0) PutBe32(mb + p + 4, Be32(mb + p + 4) + dec);
}
}  // namespace
REX_HOOK_RAW(sub_82EF54C8) { HalfTimerList(ctx.r3.u32, ctx.r4.u32); __imp__sub_82EF54C8(ctx, base); }
REX_HOOK_RAW(sub_82EF5178) { HalfTimerList(ctx.r3.u32, ctx.r4.u32); __imp__sub_82EF5178(ctx, base); }
REX_HOOK_RAW(sub_82EF4D20) { HalfTimerList(ctx.r3.u32, ctx.r4.u32); __imp__sub_82EF4D20(ctx, base); }
REX_HOOK_RAW(sub_82EF5818) { HalfTimerList(ctx.r3.u32, ctx.r4.u32); __imp__sub_82EF5818(ctx, base); }
REX_HOOK_RAW(sub_83036FE0) {
  HalfTimerList(ctx.r3.u32, ctx.r4.u32);
  const uint32_t prev_owner = t_owner;
  t_owner = ctx.r3.u32;
  if (t_pass && Has(Mode(), "passprobe")) { std::lock_guard<std::mutex> lock(g_sm_mutex); g_owner_pass[ctx.r3.u32] = t_pass; }
  if (Has(Mode(), "tlprobe")) {
    static std::mutex m;
    static std::map<uint32_t, ULONGLONG> last;
    const ULONGLONG now = GetTickCount64();
    bool log = false;
    { std::lock_guard<std::mutex> lock(m); ULONGLONG& l = last[ctx.r3.u32]; if (now - l >= 10000) { l = now; log = true; } }
    if (log) {
      uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
      const uint32_t o = ctx.r3.u32, b = Be32(mb + o + 760), e = Be32(mb + o + 764);
      if (e >= b && e - b < 0x20000 && GuestReadable(mb, b, e - b)) {
        std::map<int32_t, int> hist; std::string big;
        for (uint32_t p = b; p + 12 <= e; p += 12) {
          const int32_t iv = int32_t(Be32(mb + p)), cd = int32_t(Be32(mb + p + 4));
          hist[iv]++;
          if ((iv != 0 || cd > 120 || cd < -120) && big.size() < 2500) { char x[48]; std::snprintf(x, sizeof(x), " %08X:%d/%d", Be32(mb + p + 8), iv, cd); big += x; }
        }
        std::string h; for (auto& [iv, n] : hist) { char x[24]; std::snprintf(x, sizeof(x), " %d:%d", iv, n); h += x; }
        if ((e - b) / 12 > 2 || !big.empty())
          REXLOG_WARN("AO4_FPSTEST tl owner={:08X} entries={} interval_hist{} far{}", o, (e - b) / 12, h, big);
      }
    }
  }
  __imp__sub_83036FE0(ctx, base);
  t_owner = prev_owner;
}
REX_HOOK_RAW(sub_82D50290) {
  const uint32_t prev = t_pass;
  t_pass = ctx.r3.u32;
  if (Has(Mode(), "passprobe")) { std::lock_guard<std::mutex> lock(g_sm_mutex); ++g_pass_calls[ctx.r3.u32]; }
  __imp__sub_82D50290(ctx, base);
  t_pass = prev;
}

// regprobe: entity update pass registration (applied in sub_82D50058 from the pending list pass+52):
// add sub_82D4FC68(r4 = pass+32, r5 = {u64 key, updater}), remove sub_82D4F320(r3 = pass+32, r4 = {u64 key}).
// Each event on a pass logs key, updater and the host call chain (main-DLL offsets), at most 3000 lines.
namespace {
void RegLog(const char* what, uint32_t list, uint32_t entry) {
  if (!Has(Mode(), "regprobe")) return;
  static std::atomic<int> lines{0};
  if (lines.fetch_add(1) > 3000) return;
  uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
  const uint32_t k0 = Be32(mb + entry), k1 = Be32(mb + entry + 4), up = Be32(mb + entry + 8);
  void* frames[24] = {};
  const USHORT count = CaptureStackBackTrace(2, 24, frames, nullptr);
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&RegLog), &self);
  std::string st;
  for (USHORT i = 0; i < count; ++i) { char b[24]; std::snprintf(b, sizeof(b), " %llX", (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)self)); st += b; }
  REXLOG_WARN("AO4_FPSTEST reg {} pass={:08X} key={:08X}{:08X} updater={:08X} tick={} stack{}", what, list - 32, k0, k1, up,
              client_game_time ? Be32(mb + client_game_time + 52) : 0u, st);
}
}  // namespace
REX_HOOK_RAW(sub_82D4FC68) { RegLog("add", ctx.r4.u32, ctx.r5.u32); __imp__sub_82D4FC68(ctx, base); }
REX_HOOK_RAW(sub_82D4F320) { RegLog("remove", ctx.r3.u32, ctx.r4.u32); __imp__sub_82D4F320(ctx, base); }

// enqprobe: update pass registration requests: add sub_82D517A8(r3 pass, r4 updater), remove
// sub_82D51880(r3 pass, r4 updater). Logs pass, updater, its vtable, client tick and the host call
// chain (main-DLL offsets; callers of the request), at most 6000 lines.
namespace {
void EnqLog(const char* what, uint32_t pass, uint32_t updater) {
  if (!Has(Mode(), "enqprobe")) return;
  static std::atomic<int> lines{0};
  if (lines.fetch_add(1) > 6000) return;
  uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
  void* frames[28] = {};
  const USHORT count = CaptureStackBackTrace(2, 28, frames, nullptr);
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&EnqLog), &self);
  std::string st;
  for (USHORT i = 0; i < count; ++i) { char b[24]; std::snprintf(b, sizeof(b), " %llX", (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)self)); st += b; }
  REXLOG_WARN("AO4_FPSTEST enq {} pass={:08X} updater={:08X} vt={:08X} tick={} stack{}", what, pass, updater,
              GuestReadable(mb, updater, 4) ? Be32(mb + updater) : 0u, client_game_time ? Be32(mb + client_game_time + 52) : 0u, st);
}
}  // namespace
REX_HOOK_RAW(sub_82D517A8) { EnqLog("add", ctx.r3.u32, ctx.r4.u32); __imp__sub_82D517A8(ctx, base); }
REX_HOOK_RAW(sub_82D51880) { EnqLog("remove", ctx.r3.u32, ctx.r4.u32); __imp__sub_82D51880(ctx, base); }

// compprobe: component list of an update owner (+760, used by sub_83036FE0): add sub_83037550(r3 owner,
// r4 component, r5) and remove sub_83037508(r3 owner, r4 component), both locked wrappers. Logs owner,
// component, its vtable, r5, client tick and the host call chain, at most 8000 lines.
namespace {
void CompLog(const char* what, uint32_t owner, uint32_t comp, uint32_t r5) {
  if (!Has(Mode(), "compprobe")) return;
  static std::atomic<int> lines{0};
  if (lines.fetch_add(1) > 8000) return;
  uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
  void* frames[28] = {};
  const USHORT count = CaptureStackBackTrace(2, 28, frames, nullptr);
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&CompLog), &self);
  std::string st;
  for (USHORT i = 0; i < count; ++i) { char b[24]; std::snprintf(b, sizeof(b), " %llX", (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)self)); st += b; }
  REXLOG_WARN("AO4_FPSTEST comp {} owner={:08X} comp={:08X} vt={:08X} r5={:08X} tick={} stack{}", what, owner, comp,
              GuestReadable(mb, comp, 4) ? Be32(mb + comp) : 0u, r5, client_game_time ? Be32(mb + client_game_time + 52) : 0u, st);
}
}  // namespace
REX_HOOK_RAW(sub_83037550) { CompLog("add", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); __imp__sub_83037550(ctx, base); }
REX_HOOK_RAW(sub_83037508) { CompLog("remove", ctx.r3.u32, ctx.r4.u32, 0); __imp__sub_83037508(ctx, base); }

// nosleep (test): the timed soldier update sub_82FC4490 puts a soldier to sleep (pose snapshot, removed
// from all update lists) when sub_830320E0 and further checks report it at rest. Called from
// 82FC4490, answer "not at rest" so soldiers are never put to sleep (hypothesis: rest detection
// counts physics/update steps and triggers twice as early at 60 ticks).
REX_HOOK_RAW(sub_830320E0) {
  if (Has(Mode(), "nosleep") && uint32_t(ctx.lr) > 0x82FC4490u && uint32_t(ctx.lr) < 0x82FC4900u) {
    static std::atomic<uint32_t> n{0};
    if ((n.fetch_add(1) & 1023) == 0) REXLOG_WARN("AO4_FPSTEST nosleep soldier {:08X} (count {})", ctx.r3.u32, n.load());
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_830320E0(ctx, base);
}

// aiscan: every 15 s scan guest heap 0x40000000-0x80000000 for AI TimingConstantData (vt 82BC0AF8,
// +12 UpdateTimePerFrame, +16.. SubsystemTimeParts) and log the floats of each instance.
namespace {
void AiScan() {
  rex::system::KernelState* k = nullptr;
  while (!(k = rex::system::KernelState::shared()) || !k->memory()) Sleep(500);
  uint8_t* mb = k->memory()->virtual_membase();
  for (int round = 0; round < 400; ++round) {
    Sleep(round < 40 ? 15000 : 60000);
    std::string s; int n = 0;
    ForEachReadable(mb + 0x40000000u, mb + 0x80000000u, [&](uint8_t* b, uint8_t* e) {
      for (uint8_t* p = b; p + 44 <= e; p += 4)
        if (p[0] == 0x82 && p[1] == 0xBC && p[2] == 0x0A && p[3] == 0xF8) {
          ++n;
          // aibudget (test): UpdateTimePerFrame (ms, 7 in SP) x2.
          if (Has(Mode(), "aibudget")) {
            const uint32_t a = uint32_t(p - mb);
            const uint32_t w = Be32(p + 12); float v; std::memcpy(&v, &w, 4);
            if (v > 0.5f && v < 10.0f) {  // original 7 ms; patched 14 is not doubled again
              v *= 2.0f; uint32_t nb; std::memcpy(&nb, &v, 4); PutBe32(p + 12, nb);
              REXLOG_WARN("AO4_FPSTEST aibudget {:08X} UpdateTimePerFrame -> {}", a, v);
            }
          }
          if (s.size() < 2000) {
            char x[256]; float f[8];
            for (int i = 0; i < 8; ++i) { const uint32_t w = Be32(p + 12 + 4 * i); std::memcpy(&f[i], &w, 4); }
            std::snprintf(x, sizeof(x), " %08X[%g | %g %g %g %g %g %g %g]", uint32_t(p - mb), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            s += x;
          }
        }
    });
    REXLOG_WARN("AO4_FPSTEST aiscan TimingConstantData count={}{}", n, s);
  }
}
struct AiScanStarter { AiScanStarter() { if (Has(Mode(), "aiscan")) std::thread(AiScan).detach(); } } g_aiscan_starter;
}  // namespace

// f12dump: on every F12 press (window-independent key state), dump 0x1000 bytes of every soldier known to
// soldmon to %TEMP%/ao4_soldier_<pid>_<soldier>_f12-<n>[a|b].bin, once immediately (a) and again 10 s later
// (b), with a log line listing soldier, idle time and position, so a stuck soldier can be diffed
// against active ones.
namespace {
void DumpAllSoldiers(uint8_t* mb, int n, char phase) {
  std::vector<std::pair<uint32_t, double>> list;
  {
    std::lock_guard<std::mutex> lock(g_sm_mutex);
    const ULONGLONG now = GetTickCount64();
    for (auto& [so, m] : g_sm) list.push_back({so, (now - m.last) / 1000.0});
  }
  std::string s;
  for (auto& [so, idle] : list) {
    if (!GuestReadable(mb, so, 0x1000) || Be32(mb + so) != 0x82AAB0D0u) continue;
    char tag[32]; std::snprintf(tag, sizeof(tag), "f12-%d%c", n, phase);
    DumpSoldier(mb, so, tag);
    float p[3]; uint32_t w;
    for (int i = 0; i < 3; ++i) { w = Be32(mb + so + 0x4F0 + 4 * i); std::memcpy(&p[i], &w, 4); }
    char x[96]; std::snprintf(x, sizeof(x), " %08X idle=%.1f@(%.1f,%.1f,%.1f)", so, idle, p[0], p[1], p[2]); s += x;
  }
  REXLOG_WARN("AO4_FPSTEST f12dump {}{}{}", n, phase, s);
}
void F12Watch() {
  rex::system::KernelState* k = nullptr;
  while (!(k = rex::system::KernelState::shared()) || !k->memory()) Sleep(500);
  uint8_t* mb = k->memory()->virtual_membase();
  bool down = false; int n = 0; ULONGLONG second = 0;
  for (;;) {
    Sleep(100);
    const bool d = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (d && !down && n < 20) { ++n; DumpAllSoldiers(mb, n, 'a'); second = GetTickCount64() + 10000; }
    down = d;
    if (second && GetTickCount64() >= second) { second = 0; DumpAllSoldiers(mb, n, 'b'); }
    // perdump: additionally every 10 s a numbered dump of all soldiers (p<k>), for 30 vs 60 comparisons.
    static ULONGLONG next_periodic = 0; static int k = 0;
    if (Has(Mode(), "perdump") && GetTickCount64() >= next_periodic && k < 60) {
      next_periodic = GetTickCount64() + 10000;
      bool any; { std::lock_guard<std::mutex> lock(g_sm_mutex); any = !g_sm.empty(); }
      if (any) { ++k; DumpAllSoldiers(mb, 100 + k, 'p'); }
    }
  }
}
struct F12Starter { F12Starter() { if (Has(Mode(), "f12dump") || Has(Mode(), "perdump")) std::thread(F12Watch).detach(); } } g_f12_starter;
}  // namespace

// listcand: candidates that touch an object's +880/+888 (target list of soldiers?). Per 10 s: calls
// with r3 = soldier (vt 82AAB0D0) and other calls, logged via FpsListCandLog from the ticks block.
namespace {
constexpr uint32_t kListCand[] = {0x832A4EA0u, 0x832A5198u, 0x833390C0u, 0x83339288u, 0x83338600u, 0x832A8988u, 0x832A9AC8u, 0x832AA798u, 0x832ABF30u, 0x82E15AF0u};
std::mutex g_msg_mutex;
std::map<uint64_t, uint32_t> g_msg_counts;
std::atomic<uint32_t> g_lc_sold[10], g_lc_other[10];
void ListCand(int i, uint32_t r3) {
  if (!Has(Mode(), "listcand")) return;
  uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
  if (r3 >= 0x40000000u && r3 < 0x80000000u && Be32(mb + r3) == 0x82AAB0D0u) g_lc_sold[i]++; else g_lc_other[i]++;
}
}  // namespace
void FpsListCandLog() {
  if (Has(Mode(), "msgprobe")) {
    std::string s;
    { std::lock_guard<std::mutex> lock(g_msg_mutex);
      for (auto& [k, c] : g_msg_counts) { char x[40]; std::snprintf(x, sizeof(x), " %08X/%08X:%u", uint32_t(k >> 32), uint32_t(k), c); s += x; }
      g_msg_counts.clear(); }
    REXLOG_WARN("AO4_FPSTEST msg{}", s);
  }
  if (!Has(Mode(), "listcand")) return;
  std::string s;
  for (int i = 0; i < 10; ++i) { char x[64]; std::snprintf(x, sizeof(x), " %08X:%u/%u", kListCand[i], g_lc_sold[i].exchange(0), g_lc_other[i].exchange(0)); s += x; }
  REXLOG_WARN("AO4_FPSTEST listcand soldier/other{}", s);
}
REX_HOOK_RAW(sub_832A4EA0) { ListCand(0, ctx.r3.u32); __imp__sub_832A4EA0(ctx, base); }
REX_HOOK_RAW(sub_832A5198) { ListCand(1, ctx.r3.u32); __imp__sub_832A5198(ctx, base); }
REX_HOOK_RAW(sub_833390C0) { ListCand(2, ctx.r3.u32); __imp__sub_833390C0(ctx, base); }
REX_HOOK_RAW(sub_83339288) { ListCand(3, ctx.r3.u32); __imp__sub_83339288(ctx, base); }
REX_HOOK_RAW(sub_83338600) { ListCand(4, ctx.r3.u32); __imp__sub_83338600(ctx, base); }
REX_HOOK_RAW(sub_832A8988) { ListCand(5, ctx.r3.u32); __imp__sub_832A8988(ctx, base); }
REX_HOOK_RAW(sub_832A9AC8) { ListCand(6, ctx.r3.u32); __imp__sub_832A9AC8(ctx, base); }
REX_HOOK_RAW(sub_832AA798) { ListCand(7, ctx.r3.u32); __imp__sub_832AA798(ctx, base); }
// msgprobe: AI component message handler 832ABF30 (r4 = message, +4/+8 type hashes): count per type.
REX_HOOK_RAW(sub_832ABF30) {
  ListCand(8, ctx.r3.u32);
  if (Has(Mode(), "msgprobe")) {
    uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
    const uint64_t key = (uint64_t(Be32(mb + ctx.r4.u32 + 4)) << 32) | Be32(mb + ctx.r4.u32 + 8);
    std::lock_guard<std::mutex> lock(g_msg_mutex);
    ++g_msg_counts[key];
  }
  __imp__sub_832ABF30(ctx, base);
}
REX_HOOK_RAW(sub_82E15AF0) { ListCand(9, ctx.r3.u32); __imp__sub_82E15AF0(ctx, base); }

// stimprobe: AI stimulus handler 833E6050 (r3 AI object, r4 message, byte +176 = on/off): per 5 s
// on/off counts and the AI objects involved, plus the host call chain of the first few calls.
namespace {
std::atomic<uint32_t> g_stim_on{0}, g_stim_off{0};
}  // namespace
REX_HOOK_RAW(sub_833E6050) {
  if (Has(Mode(), "stimprobe")) {
    uint8_t* mb = rex::system::KernelState::shared()->memory()->virtual_membase();
    const bool on = mb[ctx.r4.u32 + 176] != 0;
    (on ? g_stim_on : g_stim_off)++;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 12) FpsStackDump(on ? "stim-on" : "stim-off", ctx.r3.u32, ctx.r4.u32, 0.0);
    static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
    if (now - last >= 5000) { last = now; REXLOG_WARN("AO4_FPSTEST stim on={} off={}", g_stim_on.exchange(0), g_stim_off.exchange(0)); }
  }
  __imp__sub_833E6050(ctx, base);
}
