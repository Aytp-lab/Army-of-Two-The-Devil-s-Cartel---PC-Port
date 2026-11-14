#include "generated/ao4.main/ao4_main_init.h"
#include "aim_source_observer.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <fmt/format.h>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <cmath>
#include "aim_source_step.h"
#include "native_mouse_angles.h"
#include "turret_mouse_pulse.h"
#include "native_mouse_settings.h"
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <map>
#include <tuple>
#include <vector>

namespace {
bool Enabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_AIM_SOURCE_TRACE", nullptr, 0) != 0;
  return enabled;
}
bool NativeMouseEnabled() {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_NATIVE_MOUSE", nullptr, 0) != 0;
  return enabled;
}
using ConsumeMouse = int (*)(uint32_t, float*, float*);
ConsumeMouse MouseBridge() {
  static ConsumeMouse bridge = reinterpret_cast<ConsumeMouse>(GetProcAddress(
      GetModuleHandleW(L"rexruntime.dll"), "rex_ao4_mouse_consume_v1"));
  return bridge;
}
// Live mouse settings. Start values come from the launcher environment; a poller thread
// (never the game thread) handles F3/F4 and picks up changes to native-mouse.json, e.g.
// from Maus-Empfindlichkeit.cmd. F3/F4 also write the file so both stay in step.
constexpr double kMinScale = 0.00001, kMaxScale = 0.05, kHotkeyStep = 1.1;
std::atomic<double> g_scale{0.002};
std::atomic<bool> g_invert{false};
std::atomic<double> g_turret_horizontal{1}, g_turret_vertical{1};
std::atomic<bool> g_turret_invert{false};
std::atomic<bool> g_direct_on{true}, g_focus{false};
std::atomic<ULONGLONG> g_burst_until{0};
std::atomic<bool> g_burst_dump_pending{false};
std::atomic<uint32_t> g_human_soldier{0};
void ApplySettings(const Ao4MouseSettings& s) {
  g_scale = s.foot; g_invert = s.invert;
  g_turret_horizontal = s.turret_horizontal;
  g_turret_vertical = s.turret_vertical;
  g_turret_invert = s.turret_invert;
}
std::wstring SettingsPath() {
  wchar_t path[32768] = {};
  return GetEnvironmentVariableW(L"AO4_MOUSE_SETTINGS", path, 32768) ? path : L"";
}
uint64_t SettingsFileTime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}
std::string SettingsText(const std::wstring& path) {
  FILE* file = _wfopen(path.c_str(), L"rb");
  if (!file) return {};
  char buffer[16384]{};
  const size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);
  return std::string(buffer, size);
}
bool ReadSettingsFile(const std::wstring& path, Ao4MouseSettings& s) {
  return Ao4ParseMouseSettings(SettingsText(path), s);
}
void WriteSettingsFile(const std::wstring& path, double scale, bool invert) {
  if (path.empty()) return;
  const std::string original = SettingsText(path);
  Ao4MouseSettings s;
  if (!Ao4ParseMouseSettings(original, s)) return;
  s.foot = scale; s.invert = invert; // F3/F4 affect foot only; retain turret/extra fields.
  const std::string encoded = Ao4EncodeMouseSettings(original, s);
  if (encoded.empty()) return;
  const std::wstring temp = path + L".game-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
  FILE* file = _wfopen(temp.c_str(), L"wb");
  if (!file) return;
  const bool written = fwrite(encoded.data(), 1, encoded.size(), file) == encoded.size();
  fclose(file);
  if (written && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) ApplySettings(s);
  else DeleteFileW(temp.c_str());
}
void StartSettingsPoller() {
  static std::once_flag once;
  std::call_once(once, [] {
    char text[64]{};
    if (GetEnvironmentVariableA("AO4_MOUSE_RADIANS_PER_COUNT", text, sizeof(text))) {
      const double scale = std::strtod(text, nullptr);
      if (std::isfinite(scale) && scale >= kMinScale && scale <= kMaxScale) g_scale = scale;
    }
    g_invert = GetEnvironmentVariableA("AO4_MOUSE_INVERT_Y", text, sizeof(text)) && text[0] == '1';
    // Split screen: start on the stick path (the direct path turns player 1's camera);
    // the poller (F2/F3/F4/F12) still runs. F2 switches as usual.
    if (GetEnvironmentVariableA("AO4_NATIVE_MOUSE_START_STICK", text, sizeof(text)) && text[0] == '1')
      g_direct_on = false;
    g_turret_horizontal = g_turret_vertical = std::clamp(g_scale.load() / .002, .01, 10.0);
    g_turret_invert = g_invert.load();
    // F3/F4 sensitivity keys: on by default (test starters); the public-test launcher sets
    // AO4_MOUSE_HOTKEYS=0 because F4 opens its port menu (slider) and F3 the runtime's overlay.
    const bool sensitivity_keys =
        !(GetEnvironmentVariableA("AO4_MOUSE_HOTKEYS", text, sizeof(text)) && text[0] == '0');
    std::thread([sensitivity_keys] {
      const std::wstring path = SettingsPath();
      Ao4MouseSettings initial;
      if (ReadSettingsFile(path, initial)) ApplySettings(initial);
      uint64_t file_time = SettingsFileTime(path);
      bool f2 = false, f3 = false, f4 = false, f12 = false;
      for (unsigned tick = 0;; ++tick) {
        Sleep(25);
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool focus = pid == GetCurrentProcessId();
        g_focus.store(focus, std::memory_order_relaxed);
        const bool down3 = sensitivity_keys && focus && (GetAsyncKeyState(VK_F3) & 0x8000);
        const bool down4 = sensitivity_keys && focus && (GetAsyncKeyState(VK_F4) & 0x8000);
        if ((down3 && !f3) || (down4 && !f4)) {
          const double old_scale = g_scale.load();
          const double scale = std::clamp(down4 ? old_scale * kHotkeyStep : old_scale / kHotkeyStep,
                                          kMinScale, kMaxScale);
          g_scale = scale;
          WriteSettingsFile(path, scale, g_invert.load());
          file_time = SettingsFileTime(path);
          REXLOG_WARN("AO4_NATIVE_MOUSE sensitivity={:.6g} ({})", scale, down4 ? "F4 up" : "F3 down");
          Beep(scale == old_scale ? 300 : (down4 ? 1000 : 600), 60);
        }
        f3 = down3;
        f4 = down4;
        const bool down2 = focus && (GetAsyncKeyState(VK_F2) & 0x8000);
        if (down2 && !f2) {
          const bool enabled = !g_direct_on.load();
          g_direct_on = enabled;
          if (!enabled) {
            if (auto bridge = MouseBridge()) { float dx, dy; bridge(0, &dx, &dy); }
          }
          REXLOG_WARN("AO4_NATIVE_MOUSE toggle={} (F2)", enabled ? "direct" : "stick fallback");
        }
        f2 = down2;
        const bool down12 = focus && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (down12 && !f12) {
          REXLOG_WARN("AO4_TEST_MARKER F12");
          if (Enabled() && !g_burst_dump_pending.load()) {
            g_burst_until = GetTickCount64() + 10000;
            g_burst_dump_pending = true;
            REXLOG_WARN("AO4_AIM_BURST start (F12, 10 s)");
          }
        }
        f12 = down12;
        if (tick % 20 == 0) {
          const uint64_t now_time = SettingsFileTime(path);
          Ao4MouseSettings settings;
          if (now_time && now_time != file_time && ReadSettingsFile(path, settings)) {
            file_time = now_time;
            ApplySettings(settings);
            REXLOG_WARN("AO4_NATIVE_MOUSE sensitivity={:.6g} invert_y={} turret_h={} turret_v={} turret_invert={} (file)",
                        settings.foot, settings.invert, settings.turret_horizontal,
                        settings.turret_vertical, settings.turret_invert);
          }
        }
      }
    }).detach();
  });
}
// F2 (direct/stick toggle) and window focus are polled by the settings thread, so the
// game thread only reads atomics here.
bool NativeMouseRequested() {
  if (!NativeMouseEnabled()) return false;
  StartSettingsPoller();
  return g_direct_on.load(std::memory_order_relaxed);
}
// VirtualQuery is slow on the large guest mappings (it measures the whole region), and
// these checks run hundreds of times per frame. Committed regions are cached for 1 s.
bool Readable(uint8_t* base, uint32_t address, uint32_t length) {
  if (!base || address < 0x40000000 || address >= 0xE0000000 ||
      length > 0xE0000000 - address) return false;
  struct Region { uintptr_t begin = 0, end = 0; ULONGLONG at = 0; };
  thread_local Region regions[8];
  thread_local unsigned next = 0;
  const uintptr_t host = reinterpret_cast<uintptr_t>(base) + address;
  const ULONGLONG now = GetTickCount64();
  for (const Region& region : regions) {
    if (region.end && now - region.at < 1000 && host >= region.begin &&
        host + length <= region.end) return true;
  }
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(reinterpret_cast<void*>(host), &info, sizeof(info)) ||
      info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
  regions[next++ % 8] = Region{begin, begin + info.RegionSize, now};
  return host + length <= begin + info.RegionSize;
}
uint32_t Read32(uint8_t* base, uint32_t address) {
  return Readable(base, address, 4) ? REX_LOAD_U32(address) : 0;
}
bool ReadFloat(uint8_t* base, uint32_t address, float& value) {
  if (!Readable(base, address, 4)) return false;
  const uint32_t bits = REX_LOAD_U32(address);
  std::memcpy(&value, &bits, sizeof(value));
  return std::isfinite(value);
}
// Resolve the actual local client player, including on a network guest.
// +0x564 follows the original camera getter: soldier+192, then +1188.
// Window focus for the mouse paths; automated background tests (AO4_TEST_KEY_FILE) feed mouse
// motion without focus. Hotkeys keep using the real focus.
bool MouseFocus() {
  static const bool test = GetEnvironmentVariableW(L"AO4_TEST_KEY_FILE", nullptr, 0) != 0;
  return test || g_focus.load(std::memory_order_relaxed);
}

// Guest user that owns keyboard/mouse (runtime export; 0 = player 1, 1 = split-screen player 2).
uint32_t MouseUser() {
  using Fn = uint32_t (*)();
  static Fn fn = [] {
    HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
    return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, "rex_ao4_mnk_user_v1")) : nullptr;
  }();
  return fn ? fn() : 0;
}

uint32_t HumanWeapons(uint8_t* base, uint32_t& soldier) {
  const uint32_t list = Read32(base, Read32(base, 0x841350DC) + 52);
  // Original local-player getter sub_82D04DA0 returns manager+184.
  // The first non-bot in the roster may be the remote host in online coop.
  // Do not fall back to that roster when the local player is absent during loading.
  uint32_t player = Read32(base, list + 184);
  // Split screen with keyboard/mouse on player 2: the other human (non-bot) client player of
  // the roster. Online the mouse user stays 0, so a remote human is never picked.
  if (MouseUser() == 1) {
    const uint32_t local1 = player;
    player = 0;
    const uint32_t begin = Read32(base, list + 152), end = Read32(base, list + 156);
    for (uint32_t slot = begin; slot && slot < end && slot < begin + 16 * 4; slot += 4) {
      const uint32_t p = Read32(base, slot);
      if (p && p != local1 && Read32(base, p) == 0x82A3F850 && Readable(base, p + 804, 1) &&
          !REX_LOAD_U8(p + 804)) {
        player = p;
        break;
      }
    }
    if (!player) return 0;
  }
  if (Read32(base, player) != 0x82A3F850 || !Readable(base, player + 804, 1) ||
      REX_LOAD_U8(player + 804)) return 0;
  const uint32_t link = Read32(base, Read32(base, player + 1004));
  if (link < 4) return 0;
  soldier = link - 4;
  if (Read32(base, soldier) != 0x82AAB0D0) return 0;
  const uint32_t weapons = Read32(base, soldier + 1380);
  if (Read32(base, weapons) != 0x82AAB3B0) return 0;
  thread_local uint32_t last_player = 0, last_soldier = 0;
  if (player != last_player || soldier != last_soldier) {
    REXLOG_WARN("AO4_NATIVE_MOUSE local_player={:08X} soldier={:08X} weapons={:08X}",
                player, soldier, weapons);
    last_player = player; last_soldier = soldier;
  }
  return weapons;
}
struct Sample {
  bool active = false;
  uint32_t weapon = 0, source = 0;
  bool predicted = false, readable = false;
  float yaw = 0, pitch = 0;
  bool pitch_step = false;
  bool native_mouse = false;
};
thread_local Sample current;
std::atomic<uint32_t> total_samples{0};
// Cost of these hooks (excluding the original game code), logged every 5 s.
std::atomic<uint64_t> perf_calls{0}, perf_human{0}, perf_hook_ticks{0}, perf_mouse_ticks{0};
struct HookTimer {
  LARGE_INTEGER start;
  HookTimer() { perf_calls.fetch_add(1, std::memory_order_relaxed); QueryPerformanceCounter(&start); }
  void Stop() { LARGE_INTEGER now; QueryPerformanceCounter(&now); perf_hook_ticks.fetch_add(uint64_t(now.QuadPart - start.QuadPart), std::memory_order_relaxed); }
  void Resume() { QueryPerformanceCounter(&start); }
  ~HookTimer() { Stop(); }
};
void LogPerf() {
  static std::atomic<ULONGLONG> last{0};
  const ULONGLONG now = GetTickCount64();
  ULONGLONG before = last.load();
  if (now - before < 5000 || !last.compare_exchange_strong(before, now)) return;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  const double us = 1e6 / double(frequency.QuadPart);
  REXLOG_WARN("AO4_AIM_PERF calls_5s={} human_5s={} hook_ms_5s={:.2f} mouse_ms_5s={:.2f}",
              perf_calls.exchange(0), perf_human.exchange(0), perf_hook_ticks.exchange(0) * us / 1000,
              perf_mouse_ticks.exchange(0) * us / 1000);
}
bool PitchStepRequested(bool pitch, uint32_t caller) {
  static const bool enabled = GetEnvironmentVariableW(L"AO4_AIM_SOURCE_STEP", nullptr, 0) != 0;
  if (!enabled || pitch || caller != 0x82FC4CCC) return false;
  DWORD foreground_pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
  thread_local bool down = false;
  const bool pressed = foreground_pid == GetCurrentProcessId() &&
                       (GetAsyncKeyState(VK_F2) & 0x8000);
  const bool edge = pressed && !down;
  down = pressed;
  return edge;
}
// F12 burst trace (turret/mode investigation): for 10 s, count every call of the aim
// getters and the original yaw/pitch setters by caller and object, then log the totals.
std::mutex g_burst_lock;
struct BurstEntry { uint64_t count = 0; float last_value = 0; };
std::map<std::tuple<const char*, uint32_t, uint32_t, uint32_t>, BurstEntry> g_burst;
void BurstDumpIfDone() {
  if (!g_burst_dump_pending.load(std::memory_order_relaxed) ||
      GetTickCount64() < g_burst_until.load()) return;
  if (!g_burst_dump_pending.exchange(false)) return;
  std::lock_guard<std::mutex> lock(g_burst_lock);
  std::vector<std::pair<std::tuple<const char*, uint32_t, uint32_t, uint32_t>, BurstEntry>> rows(
      g_burst.begin(), g_burst.end());
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
  REXLOG_WARN("AO4_AIM_BURST end entries={}", rows.size());
  for (size_t i = 0; i < rows.size() && i < 80; ++i) {
    const auto& [kind, caller, object, vtable] = rows[i].first;
    REXLOG_WARN("AO4_AIM_BURST {} caller={:08X} object={:08X} vt={:08X} count={} last={}", kind,
                caller, object, vtable, rows[i].second.count, rows[i].second.last_value);
  }
  g_burst.clear();
}
void BurstRecord(uint8_t* base, const char* kind, uint32_t caller, uint32_t object, float value) {
  if (!Enabled()) return;
  if (GetTickCount64() >= g_burst_until.load(std::memory_order_relaxed)) {
    BurstDumpIfDone();
    return;
  }
  const uint32_t vtable = Read32(base, object);
  std::lock_guard<std::mutex> lock(g_burst_lock);
  auto& entry = g_burst[{kind, caller, object, vtable}];
  ++entry.count;
  entry.last_value = value;
}
// The camera getter is also reached from job threads (seen in turret/cutscene phases).
// Only one thread may apply mouse deltas; another takes over after 2 s of silence.
bool MouseOwnerThread() {
  static std::atomic<DWORD> owner{0};
  static std::atomic<ULONGLONG> owner_seen{0};
  const DWORD self = GetCurrentThreadId();
  const ULONGLONG now = GetTickCount64();
  DWORD current = owner.load();
  if (current != self) {
    if (current && now - owner_seen.load() < 2000) return false;
    if (!owner.compare_exchange_strong(current, self)) return false;
    REXLOG_WARN("AO4_NATIVE_MOUSE owner_thread={}", self);
  }
  owner_seen = now;
  return true;
}
// The screen stack check reads guest memory through role_observer; 50 ms is fast enough
// to stop the camera when the pause menu opens.
bool HudOnTop(uint8_t* base) {
  thread_local bool on = false;
  thread_local ULONGLONG at = 0;
  const ULONGLONG now = GetTickCount64();
  if (!at || now - at >= 50) { on = Ao4LevelHudOnTop(base); at = now; }
  return on;
}
void TraceGetter(PPCContext& ctx, uint8_t* base, bool pitch,
                 void (*original)(PPCContext&, uint8_t*)) {
  if (!Enabled() && !NativeMouseEnabled()) return original(ctx, base);
  BurstRecord(base, pitch ? "get_pitch" : "get_yaw", uint32_t(ctx.lr), ctx.r3.u32, 0);
  if (current.active) return original(ctx, base);
  HookTimer timer;
  const auto call_original = [&] { timer.Stop(); original(ctx, base); timer.Resume(); };
  thread_local ULONGLONG last[2]{};
  const unsigned axis = pitch ? 1 : 0;
  const ULONGLONG now = GetTickCount64();
  // The getters run for every soldier, many times per frame. Resolving the human through
  // VirtualQuery-checked reads on each call cost most of the frame rate, so the human
  // weapons object is resolved at most every 250 ms; a match is still type-checked.
  thread_local uint32_t cached_weapons = 0, cached_soldier = 0, cached_user = 0;
  thread_local ULONGLONG cached_at = 0;
  const uint32_t mouse_user = MouseUser();
  if (!cached_at || now - cached_at >= 250 || mouse_user != cached_user) {
    cached_user = mouse_user;
    cached_weapons = HumanWeapons(base, cached_soldier);
    if (cached_weapons) g_human_soldier = cached_soldier;
    cached_at = now;
  }
  if (!cached_weapons || ctx.r3.u32 != cached_weapons ||
      Read32(base, cached_weapons) != 0x82AAB3B0) return call_original();
  const uint32_t weapons = cached_weapons, soldier = cached_soldier;
  perf_human.fetch_add(1, std::memory_order_relaxed);
  LogPerf();
  const bool step = PitchStepRequested(pitch, uint32_t(ctx.lr));
  const bool mouse = !pitch && uint32_t(ctx.lr) == 0x82FC4CCC && NativeMouseRequested();
  const bool log = Enabled() && now - last[axis] >= 200 && total_samples.load() < 2400;
  if (!step && !mouse && !log)
    return call_original();
  if (log) last[axis] = now;
  const Sample previous = current;
  current = Sample{};
  current.active = true;
  current.pitch_step = step;
  current.native_mouse = mouse;
  const uint32_t caller = uint32_t(ctx.lr);
  call_original();
  const double result = ctx.f1.f64;
  const Sample observed = current;
  current = previous;
  if (!log) return;
  total_samples.fetch_add(1);
  REXLOG_WARN("AO4_AIM_SOURCE axis={} soldier={:08X} weapons={:08X} weapon={:08X} "
              "source={:08X} kind={} readable={} yaw={} pitch={} returned={} caller={:08X}",
              pitch ? "pitch" : "yaw", soldier, weapons, observed.weapon, observed.source,
              observed.source ? (observed.predicted ? "predicted" : "interpolated") : "other",
              observed.readable, observed.yaw, observed.pitch, result, caller);
}
}  // namespace

void Ao4ObserveAimSource(PPCContext& ctx, uint8_t* base, uint32_t weapon, uint32_t source,
                         bool predicted) {
  if (!current.active) return;
  current.weapon = weapon;
  current.source = source;
  current.predicted = predicted;
  const uint32_t yaw_offset = predicted ? 12 : 4;
  const uint32_t pitch_offset = predicted ? 16 : 8;
  current.readable = source && ReadFloat(base, source + yaw_offset, current.yaw) &&
                     ReadFloat(base, source + pitch_offset, current.pitch);
  if (current.native_mouse) {
    LARGE_INTEGER mouse_start;
    QueryPerformanceCounter(&mouse_start);
    current.native_mouse = false;
    const uint32_t linked = Read32(base, source + 8);
    const uint32_t constraints = Read32(base, linked + 8);
    float base_yaw = 0, base_pitch = 0;
    // Resolve fresh on the original local-human query, never on a background
    // thread. Unsupported sources do not renew the SDK lease.
    if (predicted && current.readable && std::abs(current.pitch) <= 1.6f &&
        Readable(base, source, 40) && linked && Readable(base, linked, 24) &&
        ReadFloat(base, source + 32, base_yaw) && ReadFloat(base, source + 36, base_pitch) &&
        (!constraints || Readable(base, constraints, 16)) &&
        ctx.r1.u32 >= 0x300 && Readable(base, ctx.r1.u32 - 0x300, 0x300) &&
        MouseFocus() && HudOnTop(base) && MouseOwnerThread()) {
      auto bridge = MouseBridge();
      static std::atomic<bool> announced{false};
      if (!announced.exchange(true)) {
        REXLOG_WARN("AO4_NATIVE_MOUSE bridge={} normal-human-original-setters; zoom scale uncalibrated",
                    bridge ? "v1" : "missing: stick fallback");
      }
      float dx = 0, dy = 0;
      if (bridge && bridge(source, &dx, &dy) && (dx != 0 || dy != 0)) {
        const double scale = g_scale.load();
        const bool invert = g_invert.load();
        float yaw = 0, pitch = 0;
        if (Ao4MouseAngles(current.yaw, current.pitch, dx, dy, scale, invert, yaw, pitch)) {
          const float before_yaw = current.yaw, before_pitch = current.pitch;
          PPCContext saved = ctx;
          ctx.r1.u64 = saved.r1.u32 - 0x200;
          ctx.r3.u64 = source;
          ctx.f1.f64 = yaw;
          __imp__sub_82FBD948(ctx, base);
          ctx = saved;
          ctx.r1.u64 = saved.r1.u32 - 0x200;
          ctx.r3.u64 = source;
          ctx.f1.f64 = pitch;
          __imp__sub_82FBD970(ctx, base);
          ctx = saved;
          ReadFloat(base, source + 12, current.yaw);
          ReadFloat(base, source + 16, current.pitch);
          thread_local ULONGLONG last_mouse_log = 0;
          if (Enabled() && GetTickCount64() - last_mouse_log >= 200) {
            last_mouse_log = GetTickCount64();
            REXLOG_WARN("AO4_NATIVE_MOUSE source={:08X} dx={} dy={} yaw_before={} yaw_after={} pitch_before={} pitch_after={}",
                        source, dx, dy, before_yaw, current.yaw, before_pitch, current.pitch);
          }
        }
      }
    }
    LARGE_INTEGER mouse_end;
    QueryPerformanceCounter(&mouse_end);
    perf_mouse_ticks.fetch_add(uint64_t(mouse_end.QuadPart - mouse_start.QuadPart), std::memory_order_relaxed);
  }
  if (!current.pitch_step) return;
  current.pitch_step = false;
  float target = 0;
  if (!predicted || !current.readable || !Ao4ChoosePitchStep(current.pitch, target)) {
    REXLOG_WARN("AO4_AIM_STEP rejected source={:08X} predicted={} readable={}",
                source, predicted, current.readable);
    return;
  }
  // Use the game's setter, which also propagates to the linked aim simulation.
  // Reserve scratch stack below the current guest frame and restore CPU registers.
  if (ctx.r1.u32 < 0x300 || !Readable(base, ctx.r1.u32 - 0x300, 0x300)) {
    REXLOG_WARN("AO4_AIM_STEP rejected: no guest scratch stack");
    return;
  }
  const float before = current.pitch;
  const uint32_t linked = Read32(base, source + 8);
  PPCContext saved = ctx;
  ctx.r1.u64 = ctx.r1.u32 - 0x200;
  ctx.r3.u64 = source;
  ctx.f1.f64 = target;
  __imp__sub_82FBD970(ctx, base);
  ctx = saved;
  float after = 0;
  const bool valid = ReadFloat(base, source + 16, after);
  REXLOG_WARN("AO4_AIM_STEP original_setter=82FBD970 source={:08X} linked={:08X} "
              "pitch_before={} requested={} pitch_after={} valid={}",
              source, linked, before, target, after, valid);
  if (valid) current.pitch = after;
}
REX_HOOK_RAW(sub_82FF3F10) { TraceGetter(ctx, base, false, __imp__sub_82FF3F10); }
REX_HOOK_RAW(sub_82FF3FB0) { TraceGetter(ctx, base, true, __imp__sub_82FF3FB0); }
// Original yaw/pitch setters, only observed (our own calls use __imp__ and bypass this).
REX_HOOK_RAW(sub_82FBD948) {
  if (NativeMouseEnabled()) BurstRecord(base, "set_yaw", uint32_t(ctx.lr), ctx.r3.u32, float(ctx.f1.f64));
  __imp__sub_82FBD948(ctx, base);
}
REX_HOOK_RAW(sub_82FBD970) {
  if (NativeMouseEnabled()) BurstRecord(base, "set_pitch", uint32_t(ctx.lr), ctx.r3.u32, float(ctx.f1.f64));
  __imp__sub_82FBD970(ctx, base);
}
REX_HOOK_RAW(sub_830733D8) {
  const uint32_t weapon = ctx.r3.u32;
  if (NativeMouseEnabled()) BurstRecord(base, "source", uint32_t(ctx.lr), weapon, 0);
  __imp__sub_830733D8(ctx, base);
  Ao4ObserveAimSource(ctx, base, weapon, ctx.r3.u32, true);
}
// Soldier aim setters (store soldier+1152/+1156, clamp, push to all weapon sources through
// sub_82FF0208). Used every frame on a stationary turret, not on foot. Observed in the F12
// burst only, to find the turret's caller before steering it with the mouse.
#define AO4_OBSERVE_AIM_SETTER(name)                                                     \
  REX_HOOK_RAW(name) {                                                                   \
    if (NativeMouseEnabled()) {                                                          \
      BurstRecord(base, #name "_yaw", uint32_t(ctx.lr), ctx.r3.u32, float(ctx.f1.f64));  \
      BurstRecord(base, #name "_pitch", uint32_t(ctx.lr), ctx.r3.u32, float(ctx.f2.f64)); \
    }                                                                                    \
    __imp__##name(ctx, base);                                                            \
  }
AO4_OBSERVE_AIM_SETTER(sub_82FBDBC8)
AO4_OBSERVE_AIM_SETTER(sub_82FBFC70)
AO4_OBSERVE_AIM_SETTER(sub_82FBFD28)
// Soldier transform from a seat/turret matrix (virtual, no direct caller). With the flag set
// it also derives the soldier's aim from the matrix. Burst only: caller = turret update,
// r4 = matrix inside the turret object.
REX_HOOK_RAW(sub_82FBE8D0) {
  if (NativeMouseEnabled()) {
    BurstRecord(base, "seat_matrix", uint32_t(ctx.lr), ctx.r4.u32, float(ctx.r5.u32 & 0xFF));
    BurstRecord(base, "seat_soldier", uint32_t(ctx.lr), ctx.r3.u32, 0);
    // Once per burst: the guest call chain (back chain at *r1, caller LR at back - 8), so the
    // turret's own update shows up although every level is called virtually.
    static std::atomic<ULONGLONG> logged_for{0};
    const ULONGLONG burst = g_burst_until.load();
    if (GetTickCount64() < burst && logged_for.exchange(burst) != burst) {
      std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
      uint32_t sp = ctx.r1.u32;
      for (int depth = 0; depth < 16; ++depth) {
        const uint32_t back = Read32(base, sp);
        if (!back || back <= sp || back - sp > 0x10000) break;
        chain += fmt::format(" <- {:08X}", Read32(base, back - 8));
        sp = back;
      }
      REXLOG_WARN("AO4_AIM_BURST seat_stack soldier={:08X} matrix={:08X} chain: {}",
                  ctx.r3.u32, ctx.r4.u32, chain);
    }
  }
  __imp__sub_82FBE8D0(ctx, base);
}

// Pulse v4: no seat feedback. Only the original analog reader may drain pulses.
using StickPulse = void (*)(float, float);
StickPulse StickBridge() {
  static StickPulse bridge = reinterpret_cast<StickPulse>(GetProcAddress(
      GetModuleHandleW(L"rexruntime.dll"), "rex_ao4_stick_pulse_v1"));
  return bridge;
}
void TurretMouse(uint8_t* base, uint32_t soldier, uint32_t caller) {
  static const bool test = GetEnvironmentVariableW(L"AO4_TURRET_TEST", nullptr, 0) != 0;
  if (!NativeMouseEnabled() || !test || !soldier || soldier != g_human_soldier.load()) return;
  static std::mutex control_lock;
  std::lock_guard<std::mutex> lock(control_lock);
  static uint32_t owner = 0;
  static bool engaged = false;
  auto stick = StickBridge();
  auto stop = [&] {
    owner = 0;
    if (engaged && stick) stick(0, 0);
    engaged = false;
  };
  // Other calls of this shared transform setter do not prove a seat exit.
  // Do not erase an undelivered pulse on an unrelated invocation.
  if (caller != 0x82FBE968) return;
  if (!g_direct_on.load() || !MouseFocus() || !HudOnTop(base)) {
    stop(); return;
  }
  if (!MouseOwnerThread()) return;
  float yaw = 0, pitch = 0;
  auto mouse = MouseBridge();
  if (!mouse || !stick || !ReadFloat(base, soldier + 1152, yaw) ||
      Read32(base, soldier) != 0x82AAB0D0) { stop(); return; }
  if (owner != soldier) { stick(0, 0); owner = soldier; }
  // Pitch is diagnostic only. It is not used to maintain a target.
  const bool pitch_readable = ReadFloat(base, soldier + 1156, pitch);
  static ULONGLONG last_log = 0;
  const ULONGLONG now = GetTickCount64();
  float dx = 0, dy = 0;
  if (!mouse(soldier, &dx, &dy)) { stop(); return; }
  float x = 0, y = 0;
  if (!Ao4TurretMousePulse(dx, dy, g_turret_horizontal.load(), g_turret_vertical.load(), g_turret_invert.load(), x, y)) {
    stop(); return;
  }
  if (!engaged) {
    engaged = true;
    REXLOG_WARN("AO4_TURRET_MOUSE on control=pulse-v5 soldier={:08X} cap=1 feedback=off diagonal=ratio-preserved separate-sensitivity=on", soldier);
  }
  if (dx != 0 || dy != 0) stick(x, y);
  if (now - last_log >= 200) {
    last_log = now;
    REXLOG_WARN("AO4_TURRET_MOUSE control=pulse-v5 dx={} dy={} seat_yaw={} seat_pitch={} pitch_readable={} pulse={:.4f},{:.4f}",
                dx, dy, yaw, pitch, pitch_readable, x, y);
  }
}
REX_HOOK_RAW(sub_82FBDC28) {
  const uint32_t soldier = ctx.r3.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  if (NativeMouseEnabled()) {
    BurstRecord(base, "sub_82FBDC28", uint32_t(ctx.lr), soldier, 0);
  }
  __imp__sub_82FBDC28(ctx, base);
  TurretMouse(base, soldier, caller);
}

// Original XInput wrapper: r3=user, r4=state pointer. The analog path at
// 82F86ABC uses the axes; 83228B48 only checks the connection return code.
// role_observer.cpp: QoL partner bundle preload while a level loads (guest main thread only).
void Ao4QolLoadingTick(PPCContext& ctx, uint8_t* base);

REX_HOOK_RAW(sub_83A568F0) {
  Ao4QolLoadingTick(ctx, base);
  static const bool test = GetEnvironmentVariableW(L"AO4_TURRET_TEST", nullptr, 0) != 0;
  using Scope = uint32_t (*)(uint32_t);
  static Scope scope = reinterpret_cast<Scope>(GetProcAddress(
      GetModuleHandleW(L"rexruntime.dll"), "rex_ao4_stick_poll_scope_v1"));
  if (!NativeMouseEnabled() || !test || !scope) return __imp__sub_83A568F0(ctx, base);
  const uint32_t caller = uint32_t(ctx.lr), output = ctx.r4.u32, user = ctx.r3.u32;
  const uint32_t previous = scope(caller);
  __imp__sub_83A568F0(ctx, base);
  scope(previous);
  if (!Enabled()) return; // scope/analog handling above remains active in lean tests
  // Sparse per-caller accounting establishes what the game actually received.
  thread_local uint64_t counts[2]{}, nonzero[2]{};
  thread_local ULONGLONG last[2]{};
  const unsigned kind = caller == 0x82F86ABC ? 0 : 1;
  ++counts[kind];
  int16_t rx = 0, ry = 0;
  if (ctx.r3.u32 == 0 && Readable(base, output, 16)) {
    rx = int16_t(REX_LOAD_U16(output + 12));
    ry = int16_t(REX_LOAD_U16(output + 14));
    if (rx || ry) ++nonzero[kind];
  }
  const ULONGLONG now = GetTickCount64();
  if (now - last[kind] >= 2000) {
    last[kind] = now;
    REXLOG_WARN("AO4_TURRET_INPUT caller={:08X} user={} calls={} nonzero={} rx={} ry={} result={} sink={}",
                caller, user, counts[kind], nonzero[kind], rx, ry, ctx.r3.u32,
                kind == 0 ? "analog" : "no-drain");
    counts[kind] = nonzero[kind] = 0;
  }
}
