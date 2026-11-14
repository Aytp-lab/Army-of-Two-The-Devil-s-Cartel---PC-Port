#pragma once
#include "job_atomic_observer.h"
#include <cstring>

// Expand the original dispatch macro before overriding it. Its lookup,
// fallback and eventual call remain unconditional and retain the same target.
inline void Ao4OriginalIndirectCall(PPCContext& ctx, uint8_t* base, uint32_t target) {
    REX_CALL_INDIRECT_FUNC(target);
}
inline void Ao4ObserveJobDispatch(PPCContext& ctx, uint8_t* base, uint32_t target,
                                  const char* function) {
    if (std::strcmp(function, "__imp__sub_832405A0") || uint32_t(ctx.lr) != 0x83240660) return;
    static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
    if (!enabled) return;
    using Observer = void (*)(PPCContext*, uint8_t*, uint32_t);
    static Observer observer = [] {
        HMODULE module = GetModuleHandleW(L"ao4_main_native.dll");
        return module ? reinterpret_cast<Observer>(GetProcAddress(module, "AO4_RecordJobDispatch")) : nullptr;
    }();
    if (observer) observer(&ctx, base, target);
}
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(x) do { \
    const uint32_t ao4_dispatch_target_ = (uint32_t)(x); \
    Ao4ObserveJobDispatch(ctx, base, ao4_dispatch_target_, __func__); \
    Ao4OriginalIndirectCall(ctx, base, ao4_dispatch_target_); \
} while (0)
