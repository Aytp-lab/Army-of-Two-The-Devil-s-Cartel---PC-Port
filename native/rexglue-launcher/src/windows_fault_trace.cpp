// Observe the known startup fault; never change protection or swallow faults.
#include <windows.h>
#include <csignal>
#include <cstdio>
#include <atomic>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

// Prepared before guest execution. The abort observer writes diagnostics only;
// returning from SIGABRT still lets std::abort terminate normally.
static HANDLE ao4_abort_file = INVALID_HANDLE_VALUE;
static HANDLE ao4_abort_complete = nullptr;
static std::atomic_flag ao4_abort_started = ATOMIC_FLAG_INIT;

static void TraceAo4Abort(int) {
    if (ao4_abort_file == INVALID_HANDLE_VALUE) return;
    // Several workers may abort together. Let one observer finish its record
    // before another thread's CRT abort terminates the entire process.
    if (ao4_abort_started.test_and_set(std::memory_order_relaxed)) {
        if (ao4_abort_complete) WaitForSingleObject(ao4_abort_complete, 5000);
        return;
    }
    char output[4096] = {};
    DWORD written = 0;
    if (auto* thread = rex::runtime::ThreadState::Get()) {
        const auto* ctx = thread->context();
        const int length = std::snprintf(output, sizeof(output),
            "AO4 abort: thread=%u lr=%llX ctr=%llX target=%08X base=%p\n"
            "r1=%llX r2=%llX r3=%llX r4=%llX r5=%llX r6=%llX\n"
            "r7=%llX r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX\n"
            "r28=%llX r29=%llX r30=%llX r31=%llX\n",
            thread->thread_id(), ctx->lr, ctx->ctr.u64, ctx->last_indirect_target,
            thread->memory()->virtual_membase(),
            ctx->r1.u64, ctx->r2.u64, ctx->r3.u64, ctx->r4.u64, ctx->r5.u64, ctx->r6.u64,
            ctx->r7.u64, ctx->r8.u64, ctx->r9.u64, ctx->r10.u64, ctx->r11.u64, ctx->r12.u64,
            ctx->r28.u64, ctx->r29.u64, ctx->r30.u64, ctx->r31.u64);
        if (length > 0 && length < sizeof(output))
            WriteFile(ao4_abort_file, output, DWORD(length), &written, nullptr);
        // The native Main observer uses bounded slot reads and direct file I/O,
        // never logging locks or guest execution, to preserve this node history.
        if (auto module = GetModuleHandleW(L"ao4_main_native.dll")) {
            using DumpJobHistory = void (*)(uint32_t, uint32_t);
            if (auto dump = reinterpret_cast<DumpJobHistory>(GetProcAddress(module, "AO4_DumpJobHistory")))
                dump(ctx->r31.u32, uint32_t(ctx->lr));
        }
        // Observe both address views; generated Win32 PPC accesses add 0x1000
        // for guest addresses >= 0xE0000000. Never read unchecked pointers.
        for (uint32_t offset : {0u, ctx->r31.u32 >= 0xE0000000u ? 0x1000u : 0u}) {
            const auto* node = thread->memory()->virtual_membase() + ctx->r31.u32 + offset;
            uint32_t words[16] = {};
            SIZE_T read = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), node, words, sizeof(words), &read) ||
                read != sizeof(words)) continue;
            int used = std::snprintf(output, sizeof(output), "node host=%p guest=%08X offset=%X:",
                node, ctx->r31.u32, offset);
            for (const uint32_t word : words)
                used += std::snprintf(output + used, sizeof(output) - used, " %08X",
                    __builtin_bswap32(word));
            output[used++] = '\n';
            WriteFile(ao4_abort_file, output, DWORD(used), &written, nullptr);
        }
    }
    void* frames[32] = {};
    const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT index = 0; index < count; ++index) {
        HMODULE module = nullptr;
        wchar_t module_path[1024] = {};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[index]), &module);
        if (module) GetModuleFileNameW(module, module_path, 1024);
        const auto rva = reinterpret_cast<uintptr_t>(frames[index]) -
            reinterpret_cast<uintptr_t>(module);
        const int length = std::snprintf(output, sizeof(output),
            "frame %u module=%ls rva=%llX\n", index, module_path,
            static_cast<unsigned long long>(rva));
        if (length > 0 && length < sizeof(output))
            WriteFile(ao4_abort_file, output, DWORD(length), &written, nullptr);
    }
    FlushFileBuffers(ao4_abort_file);
    if (ao4_abort_complete) SetEvent(ao4_abort_complete);
}

static LONG CALLBACK TraceAo4Fault(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 ||
        record->ExceptionInformation[1] != 0x182A01F04ULL)
        return EXCEPTION_CONTINUE_SEARCH;
    HMODULE module = nullptr;
    char modulePath[MAX_PATH] = {};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(record->ExceptionAddress), &module);
    if (module) GetModuleFileNameA(module, modulePath, MAX_PATH);
    MEMORY_BASIC_INFORMATION memory = {};
    VirtualQuery(reinterpret_cast<void*>(record->ExceptionInformation[1]),
        &memory, sizeof(memory));
    REXLOG_ERROR("AO4 fault trace: module={} host_rva={:X} operation={} protection={:X} state={:X}",
        modulePath,
        reinterpret_cast<uintptr_t>(record->ExceptionAddress) - reinterpret_cast<uintptr_t>(module),
        record->ExceptionInformation[0], memory.Protect, memory.State);
    if (auto* thread = rex::runtime::ThreadState::Get()) {
        const auto* context = thread->context();
        REXLOG_ERROR("AO4 PPC fault: lr={:X} r1={:X} r11={:X} r29={:X} r28={:X} r30={:X}",
            context->lr, context->r1.u32, context->r11.u32, context->r29.u32,
            context->r28.u32, context->r30.u32);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallAo4FaultTrace() {
    AddVectoredExceptionHandler(1, TraceAo4Fault);
    wchar_t path[1024] = {};
    const DWORD length = GetEnvironmentVariableW(L"AO4_ABORT_TRACE", path, 1024);
    if (!length || length >= 1024) return;
    ao4_abort_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ao4_abort_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ao4_abort_file != INVALID_HANDLE_VALUE) std::signal(SIGABRT, TraceAo4Abort);
}
