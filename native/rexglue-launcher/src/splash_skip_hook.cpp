// QoL "splash_ueberspringen" (native/quality-of-life.ini, env AO4_QOL_SETTINGS).
// Default.xex's loader sub_82011520 builds "AO4" + (byte +8 of its state object ? ".Main" :
// ".Splash") + ".xex", loads it, calls export 1 with the same state object, unloads it and
// sets byte +8 = 1; the outer loop then loads Main. Setting the byte before the first pass
// loads AO4.Main directly. Main gets the identical state object; the original loader runs
// unchanged. Off unless the settings file enables it.
#include "generated/default/ao4_launcher_init.h"
#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_module.h>
#include <rex/system/user_module.h>
#include <rex/system/xtypes.h>
#include <windows.h>
#include <cstdio>
#include <string>
#include "../../rexglue-main/src/qol_settings.h"

namespace {
bool SkipSplash() {
  static const bool enabled = [] {
    wchar_t path[32768] = {};
    if (GetEnvironmentVariableW(L"AO4_QOL_SETTINGS", path, 32768) == 0) return false;
    std::string text;
    if (FILE* file = _wfopen(path, L"rb")) {
      char buffer[4096];
      size_t read = 0;
      while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < 65536) text.append(buffer, read);
      fclose(file);
    }
    return Ao4ParseQolSettings(text).skip_splash;
  }();
  return enabled;
}
}  // namespace

REX_HOOK_RAW(sub_82011520) {
  if (GetEnvironmentVariableW(L"AO4_NET_TRACE", nullptr, 0) != 0) {
    // Diagnostic only: raw loader state (Default.xex stack object) before each module load.
    auto* state = rex::system::KernelState::shared()->memory()->TranslateVirtual<const uint8_t*>(ctx.r3.u32);
    std::string hex;
    char cell[4];
    for (int i = 0; i < 432; ++i) { std::snprintf(cell, sizeof(cell), "%02X", state[i]); hex += cell; }
    REXLOG_INFO("AO4_SPLASH_STATE addr={:08X} r4={:08X} r5={:08X} bytes={}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, hex);
  }
  if (SkipSplash()) {
    auto* kernel = rex::system::KernelState::shared();
    auto* selector = kernel->memory()->TranslateVirtual<uint8_t*>(ctx.r3.u32 + 8);
    if (selector && *selector == 0) {
      *selector = 1;
      REXLOG_INFO("AO4_QOL splash_ueberspringen: AO4.Splash skipped, loading AO4.Main directly");
      // Experiment only: SPLASHTEST_SET="off=hex,..." writes single bytes the splash would set.
      wchar_t spec[512] = {};
      if (GetEnvironmentVariableW(L"SPLASHTEST_SET", spec, 512) != 0) {
        auto* state = selector - 8;
        for (wchar_t* item = spec; item && *item;) {
          wchar_t* next = wcschr(item, L',');
          if (next) *next++ = 0;
          const unsigned long offset = wcstoul(item, nullptr, 10);
          const wchar_t* eq = wcschr(item, L'=');
          if (eq && offset < 432) {
            state[offset] = uint8_t(wcstoul(eq + 1, nullptr, 16));
            REXLOG_INFO("AO4_SPLASH_TEST set +{}={:02X}", offset, state[offset]);
          }
          item = next;
        }
      }
    }
  }
  __imp__sub_82011520(ctx, base);
}

// Diagnostic only (test instances, AO4_NET_TRACE): Default.xex's export lookup wrapper; logs
// which entry address each module exports for the given ordinal. Original runs unchanged.
REX_HOOK_RAW(sub_82018AB8) {
  const uint32_t handle = ctx.r3.u32, ordinal = ctx.r4.u32;
  __imp__sub_82018AB8(ctx, base);
  if (GetEnvironmentVariableW(L"AO4_NET_TRACE", nullptr, 0) != 0)
    REXLOG_INFO("AO4_SPLASH_EXPORT handle={:08X} ordinal={} -> {:08X}", handle, ordinal, ctx.r3.u32);
}
