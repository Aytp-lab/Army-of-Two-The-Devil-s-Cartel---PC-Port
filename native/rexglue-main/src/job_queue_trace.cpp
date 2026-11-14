// Observe a suspicious free-list insertion. Preserve all original guest work.
#include "generated/ao4.main/ao4_main_init.h"
#include <rex/hook.h>
#include <windows.h>
#include <atomic>
#include "job_node_history.h"

REX_HOOK_RAW(sub_8323EAA0) {
    static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    const uint32_t queue = ctx.r3.u32, node = ctx.r4.u32, caller = uint32_t(ctx.lr);
    const bool watch = enabled && node >= 0xFFC40000 && node < 0xFFC70000;
    if (watch) Ao4RecordJob(PushEnter, node, queue, caller, ctx.r5.u32, base, reinterpret_cast<uintptr_t>(&ctx));
    if (enabled && ctx.r5.u32 == 4 && ctx.r3.u32 && ctx.r4.u32) {
        const uint32_t observed_head = REX_LOAD_U32(ctx.r3.u32 + 4);
        if (observed_head == ctx.r4.u32 && !reported.test_and_set(std::memory_order_relaxed)) {
            REXLOG_ERROR("AO4 pool observation: incoming node already equals head at function entry: queue={:08X} node={:08X} lr={:08X}",
                ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
            void* frames[24] = {};
            const USHORT count = CaptureStackBackTrace(0, 24, frames, nullptr);
            for (USHORT index = 0; index < count; ++index) {
                HMODULE module = nullptr;
                wchar_t path[MAX_PATH] = {};
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(frames[index]), &module)) continue;
                GetModuleFileNameW(module, path, MAX_PATH);
                const auto rva = reinterpret_cast<uintptr_t>(frames[index]) - reinterpret_cast<uintptr_t>(module);
                REXLOG_ERROR("AO4 pool observation frame {}: host_module={:X} rva={:X}",
                    index, reinterpret_cast<uintptr_t>(module), rva);
            }
        }
    }
    __imp__sub_8323EAA0(ctx, base);
    if (watch) Ao4RecordJob(PushDone, node, queue, caller, 0, base, reinterpret_cast<uintptr_t>(&ctx));
}
