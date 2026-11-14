// Targeted forced include. Original builtin CAS executes exactly once, with
// unchanged operands. Successful CAS provides the actual replaced value.
#pragma once
#include "generated/ao4.main/ao4_main_pch.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "job_claim_observer.h"

inline void Ao4ReportAtomicRegression(uint32_t node, uint64_t before, uint64_t value,
                                     PPCContext& ctx, uint8_t* base,
                                     const char* function, unsigned line) {
    const uint32_t previous = uint32_t(before >> 32) & ~1u;
    const uint32_t next = uint32_t(value >> 32) & ~1u;
    if (previous <= 2 || previous >= 0x100000 || next >= previous) return;
    static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
    if (!enabled || REX_LOAD_U32(node + 16) != 0xFFC46180) return;
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (reported.test_and_set()) return;
    static wchar_t path[1024] = {};
    const DWORD length = GetEnvironmentVariableW(L"AO4_JOB_TRACE_FILE", path, 1024);
    if (!length || length + 5 >= 1024) return;
    wcscat_s(path, L".cas");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char record[1024] = {}; DWORD written = 0;
    const int used = std::snprintf(record, sizeof(record),
        "successful_original_cas function=%s line=%u node=%08X thread=%u context=%p\n"
        "replaced=%016llX written=%016llX observed_after=%016llX\n"
        "callback=%08X payload=%08X owner=%08X lr=%llX r3=%llX r4=%llX r5=%llX r6=%llX r7=%llX r8=%llX r9=%llX r10=%llX r11=%llX\n",
        function, line, node, unsigned(GetCurrentThreadId()), static_cast<void*>(&ctx),
        before, value, REX_LOAD_U64(node), REX_LOAD_U32(node + 8), REX_LOAD_U32(node + 12),
        REX_LOAD_U32(node + 16), ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
        ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64, ctx.r11.u64);
    if (used > 0 && used < sizeof(record)) WriteFile(file, record, DWORD(used), &written, nullptr);
    FlushFileBuffers(file); CloseHandle(file);
}
template<typename T, typename Old, typename New>
inline bool Ao4ObservedCAS(T* pointer, Old expected, New value, PPCContext& ctx,
                           uint8_t* base, const char* function, unsigned line) {
    const T old_value = T(expected), new_value = T(value);
    const bool success = __sync_bool_compare_and_swap(pointer, old_value, new_value);
    if constexpr (sizeof(T) == 8) {
        if (success && !std::strcmp(function, "__imp__sub_8323FDF8") && ctx.r11.u64 == ctx.r9.u64) {
            static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
            if (enabled) {
                using Observer = void (*)(PPCContext*, uint8_t*, uint64_t, uint64_t);
                static Observer observer = [] {
                    HMODULE module = GetModuleHandleW(L"ao4_main_native.dll");
                    return module ? reinterpret_cast<Observer>(GetProcAddress(module, "AO4_RecordQueuePublish")) : nullptr;
                }();
                if (observer) observer(&ctx, base, __builtin_bswap64(uint64_t(old_value)), __builtin_bswap64(uint64_t(new_value)));
            }
        }
        if (success && old_value != new_value && !std::strcmp(function, "__imp__sub_83240E28")) {
            static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
            if (enabled) {
                using Observer = void (*)(PPCContext*, uint8_t*, uint64_t, uint64_t);
                static Observer observer = [] {
                    HMODULE module = GetModuleHandleW(L"ao4_main_native.dll");
                    return module ? reinterpret_cast<Observer>(GetProcAddress(module, "AO4_RecordQueueDetach")) : nullptr;
                }();
                if (observer) observer(&ctx, base, __builtin_bswap64(uint64_t(old_value)), __builtin_bswap64(uint64_t(new_value)));
            }
        }
        const uintptr_t offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(base);
        if (success && offset >= 0xFFC41000ULL && offset < 0xFFC71000ULL && !(offset & 15)) {
            const uint32_t node = uint32_t(offset - 0x1000);
            if (REX_LOAD_U32(node + 16) == 0xFFC46180)
                Ao4RememberClaim(node, __builtin_bswap64(uint64_t(new_value)));
            if (old_value != new_value && !std::strcmp(function, "__imp__sub_832405A0")) {
                static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
                if (enabled) {
                    using Observer = void (*)(PPCContext*, uint8_t*, uint32_t, uint64_t, uint64_t);
                    static Observer observer = [] {
                        HMODULE module = GetModuleHandleW(L"ao4_main_native.dll");
                        return module ? reinterpret_cast<Observer>(GetProcAddress(module, "AO4_RecordJobClaim")) : nullptr;
                    }();
                    if (observer) observer(&ctx, base, node, __builtin_bswap64(uint64_t(old_value)),
                        __builtin_bswap64(uint64_t(new_value)));
                }
            }
            Ao4ReportAtomicRegression(uint32_t(offset - 0x1000), __builtin_bswap64(uint64_t(old_value)),
                __builtin_bswap64(uint64_t(new_value)), ctx, base, function, line);
        }
    }
    return success;
}
#define __sync_bool_compare_and_swap(pointer, expected, value) \
    Ao4ObservedCAS((pointer), (expected), (value), ctx, base, __func__, __LINE__)
