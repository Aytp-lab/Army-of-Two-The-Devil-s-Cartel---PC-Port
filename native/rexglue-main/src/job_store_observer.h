// Targeted forced include. Preserve the original volatile store.
// Capture before/after values locally; perform diagnostic I/O after the store.
#pragma once
#include "job_atomic_observer.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "job_claim_observer.h"

inline bool Ao4WatchHeaderStore(uint32_t address, const char* function) {
    if (address < 0xFFC40000 || address >= 0xFFC70000 || (address & 15) ||
        (std::strcmp(function, "__imp__sub_8323FDF8") &&
         std::strcmp(function, "__imp__sub_83240248") &&
         std::strcmp(function, "__imp__sub_83241EC0") &&
         std::strcmp(function, "__imp__sub_832406B0") &&
         std::strcmp(function, "__imp__sub_83240A60"))) return false;
    static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
    return enabled;
}
inline void Ao4ReportHeaderStore(uint32_t address, uint64_t before, uint64_t value,
                                uint32_t maximum_claim, PPCContext& ctx, uint8_t* base, const char* function, unsigned line) {
    static std::atomic<uint64_t> stores{0};
    const uint64_t ordinal = stores.fetch_add(1) + 1;
    // Positive coverage evidence: absence of a regression file alone is not a
    // proof that the forced-include observer actually ran.
    if (ordinal == 1) {
        static wchar_t coverage_path[1024] = {};
        const DWORD count = GetEnvironmentVariableW(L"AO4_JOB_TRACE_FILE", coverage_path, 1024);
        if (count && count + 11 < 1024) {
            wcscat_s(coverage_path, L".coverage");
            HANDLE coverage = CreateFileW(coverage_path, GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (coverage != INVALID_HANDLE_VALUE) {
                char text[256] = {}; DWORD written = 0;
                const int used = std::snprintf(text, sizeof(text),
                    "routine=%s source_line=%u node=%08X before=%016llX written=%016llX thread=%u\n",
                    function, line, address, before, value, unsigned(GetCurrentThreadId()));
                if (used > 0 && used < sizeof(text)) WriteFile(coverage, text, DWORD(used), &written, nullptr);
                FlushFileBuffers(coverage); CloseHandle(coverage);
            }
        }
    }
    const uint32_t old_generation = uint32_t(before >> 32) & ~1u;
    const uint32_t new_generation = uint32_t(value >> 32) & ~1u;
    const bool snapshot_regression = old_generation > 2 && old_generation < 0x100000 && new_generation < old_generation;
    const bool older_than_claim = maximum_claim > 2 && new_generation < maximum_claim;
    if ((!snapshot_regression && !older_than_claim) ||
        REX_LOAD_U32(address + 16) != 0xFFC46180) return;
    // A historical maximum mismatch must not suppress a later directly
    // observed before/store regression. Keep independent bounded evidence.
    static std::atomic_flag maximum_once = ATOMIC_FLAG_INIT;
    static std::atomic_flag regression_once = ATOMIC_FLAG_INIT;
    auto& once = snapshot_regression ? regression_once : maximum_once;
    if (once.test_and_set()) return;
    static wchar_t paths[2][1024] = {};
    auto& path = paths[snapshot_regression ? 1 : 0];
    const DWORD length = GetEnvironmentVariableW(L"AO4_JOB_TRACE_FILE", path, 1024);
    if (!length || length + 18 >= 1024) return;
    wcscat_s(path, snapshot_regression ? L".store-regression" : L".store");
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char record[1024] = {};
    const int used = std::snprintf(record, sizeof(record),
        "routine=%s line=%u thread=%u context=%p node=%08X\n"
        "before=%016llX written=%016llX observed_after=%016llX maximum_observed_claim=%08X snapshot_regression=%u\n"
        "queue=%08X next=%08X callback=%08X payload=%08X owner=%08X lr=%llX\n"
        "r6=%llX r7=%llX r8=%llX r9=%llX r10=%llX r11=%llX\n",
        function, line, unsigned(GetCurrentThreadId()), static_cast<void*>(&ctx), address, before, value, REX_LOAD_U64(address), maximum_claim, unsigned(snapshot_regression),
        ctx.r3.u32, REX_LOAD_U32(address + 4), REX_LOAD_U32(address + 8),
        REX_LOAD_U32(address + 12), REX_LOAD_U32(address + 16), ctx.lr,
        ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64, ctx.r11.u64);
    if (used > 0 && used < sizeof(record)) {
        DWORD written = 0; WriteFile(file, record, DWORD(used), &written, nullptr);
    }
    FlushFileBuffers(file); CloseHandle(file);
}
#undef REX_STORE_U64
#define REX_STORE_U64(x, y) do { \
    const uint32_t ao4_address_ = (uint32_t)(x); \
    const uint64_t ao4_value_ = (uint64_t)(y); \
    const bool ao4_watch_ = Ao4WatchHeaderStore(ao4_address_, __func__); \
    const uint64_t ao4_before_ = ao4_watch_ ? REX_LOAD_U64(ao4_address_) : 0; \
    const uint32_t ao4_claim_ = ao4_watch_ ? ao4_claim_generation[(ao4_address_ - 0xFFC40000) / 16].load(std::memory_order_relaxed) : 0; \
    (*(volatile u64*)REX_RAW_ADDR(ao4_address_)) = __builtin_bswap64(ao4_value_); \
    if (ao4_watch_) Ao4ReportHeaderStore(ao4_address_, ao4_before_, ao4_value_, ao4_claim_, ctx, base, __func__, __LINE__); \
} while (0)
