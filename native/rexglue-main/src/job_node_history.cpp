// Bounded observation only. No history lock spans original guest execution.
#include "generated/ao4.main/ao4_main_init.h"
#include "job_node_history.h"
#include <rex/hook.h>
#include <windows.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <cstdio>
namespace {
constexpr uint32_t pool = 0xFFC46180;
const char* labels[] = {"pop_done", "push_enter", "push_done", "chain_enter",
    "callback_enter", "callback_done", "cancel_enter", "cancel_done", "enqueue_enter", "enqueue_done", "dispatch_enter", "claim_done", "queue_detach", "queue_publish"};
thread_local uint32_t chain_origin = 0;
struct Event {
    uint64_t sequence, header, context, replaced, written;
    uint32_t thread, stage, node, queue, head, callback, payload, owner, caller, extra, origin;
};
struct Bucket {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    uint32_t node = 0, next = 0;
    std::array<Event, 64> events{};
};
// Bounded per-node retention. Hash collisions reset that bucket, never combine
// different nodes. No lock spans guest execution, logging or file I/O.
std::array<Bucket, 2048> history;
std::atomic<uint64_t> sequence{0}, dropped{0};
// Exact address-indexed diagnostic ownership markers: unseen / allocated /
// returning. The return marker precedes original pool publication; the pop
// marker follows original removal and precedes delivery to its guest caller.
std::array<std::atomic<uint32_t>, 0x30000 / 16> ownership{};
std::atomic_flag reported = ATOMIC_FLAG_INIT;
std::atomic_flag abort_reported = ATOMIC_FLAG_INIT;
std::atomic_flag origin_reported = ATOMIC_FLAG_INIT;
bool Enabled() {
    static const bool enabled = GetEnvironmentVariableW(L"AO4_JOB_TRACE", nullptr, 0) != 0;
    return enabled;
}
bool Region(uint32_t node) { return node >= 0xFFC40000 && node < 0xFFC70000; }
void Dump(const Event& trigger, bool abort = false, bool related = false) {
    if ((related ? origin_reported : abort ? abort_reported : reported).test_and_set()) return;
    // Guest workers can reach abort with little host stack left. Separate static
    // buffers permit one pool-trigger dump and one abort dump without sharing.
    static std::array<Event, 64> dump_events[3];
    static wchar_t dump_paths[3][1024] = {};
    const unsigned index = related ? 2 : abort ? 1 : 0;
    auto& selected = dump_events[index];
    size_t count = 0;
    auto& bucket = history[(trigger.node >> 4) % history.size()];
    if (!bucket.busy.test_and_set(std::memory_order_acquire)) {
        if (bucket.node == trigger.node) for (const auto& e : bucket.events) {
            if (e.sequence && e.sequence <= trigger.sequence) selected[count++] = e;
        }
        bucket.busy.clear(std::memory_order_release);
    }
    std::sort(selected.begin(), selected.begin() + count,
        [](const Event& a, const Event& b) { return a.sequence < b.sequence; });
    auto& path = dump_paths[index];
    const DWORD size = GetEnvironmentVariableW(L"AO4_JOB_TRACE_FILE", path, 1024);
    if (abort && size && size + 8 < 1024) wcscat_s(path, related ? L".origin" : L".abort");
    HANDLE file = size && size < 1024 ? CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    if (!abort) REXLOG_ERROR("AO4 node history trigger: node={:08X} queue={:08X} records={} dropped={}",
        trigger.node, trigger.queue, count, dropped.load());
    if (file != INVALID_HANDLE_VALUE) {
        char summary[256] = {}; DWORD written = 0;
        const int used = std::snprintf(summary, sizeof(summary),
            "trigger_node=%08X lr=%08X sequence=%llu records=%zu dropped=%llu abort=%u reason=%08X host_thread=%u\n",
            trigger.node, trigger.caller, trigger.sequence, count, dropped.load(), unsigned(abort), trigger.extra, trigger.thread);
        if (used > 0 && used < sizeof(summary)) WriteFile(file, summary, DWORD(used), &written, nullptr);
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& e = selected[i]; char line[512] = {};
        const int used = std::snprintf(line, sizeof(line),
            "seq=%llu thread=%u op=%s node=%08X queue=%08X head=%08X header=%016llX callback=%08X payload=%08X owner=%08X lr=%08X extra=%08X context=%016llX replaced=%016llX written=%016llX origin=%08X\n",
            e.sequence, e.thread, labels[e.stage], e.node, e.queue, e.head,
            e.header, e.callback, e.payload, e.owner, e.caller, e.extra, e.context, e.replaced, e.written, e.origin);
        if (file != INVALID_HANDLE_VALUE && used > 0 && used < sizeof(line)) {
            DWORD written = 0; WriteFile(file, line, DWORD(used), &written, nullptr);
        }
        if (!abort) REXLOG_ERROR("AO4 node history: {}", line);
    }
    if (file != INVALID_HANDLE_VALUE) { FlushFileBuffers(file); CloseHandle(file); }
}
}
extern "C" __declspec(dllexport) void AO4_DumpJobHistory(uint32_t node, uint32_t caller) {
    Event trigger{}; trigger.node = node; trigger.caller = caller;
    trigger.sequence = sequence.load(); Dump(trigger, true);
}
void Ao4RecordJob(Ao4JobStage stage, uint32_t node, uint32_t queue,
                 uint32_t caller, uint32_t extra, uint8_t* base, uint64_t context, uint64_t replaced, uint64_t written) {
    if (!Enabled() || !Region(node)) return;
    Event e{sequence.fetch_add(1) + 1, REX_LOAD_U64(node), context, replaced, written, GetCurrentThreadId(),
        uint32_t(stage), node, queue, Region(queue) ? REX_LOAD_U32(queue + 4) : 0,
        REX_LOAD_U32(node + 8), REX_LOAD_U32(node + 12), REX_LOAD_U32(node + 16), caller, extra, chain_origin};
    auto& bucket = history[(node >> 4) % history.size()];
    if (bucket.busy.test_and_set(std::memory_order_acquire)) dropped.fetch_add(1);
    else {
        if (bucket.node != node) { bucket.node = node; bucket.next = 0; bucket.events = {}; }
        bucket.events[bucket.next++ % bucket.events.size()] = e;
        bucket.busy.clear(std::memory_order_release);
    }
    if (queue == pool && !(node & 15)) {
        auto& state = ownership[(node - 0xFFC40000) / 16];
        if (stage == PopDone && state.exchange(1) == 1) {
            e.extra = 0xD001; Dump(e);
        } else if (stage == PushEnter) {
            const uint32_t prior = state.exchange(2);
            if (prior == 2 && caller == 0x8324069C) { e.extra = 0xD002; Dump(e); }
        }
    }
    if (stage == PushEnter && queue == pool && e.head == node) Dump(e);
}
extern "C" __declspec(dllexport) void AO4_RecordJobDispatch(PPCContext* context, uint8_t* base, uint32_t target) {
    const uint32_t node = context->r31.u32, caller = uint32_t(context->lr);
    Ao4RecordJob(DispatchEnter, node, 0, caller, target, base, reinterpret_cast<uintptr_t>(context));
    if (!target && Enabled() && Region(node)) {
        Event trigger{}; trigger.node = node; trigger.caller = caller;
        trigger.sequence = sequence.load(); trigger.thread = GetCurrentThreadId();
        trigger.extra = target; Dump(trigger, true);
        if (chain_origin && chain_origin != node) {
            trigger.node = chain_origin; Dump(trigger, true, true);
        }
    }
}
REX_HOOK_RAW(sub_8323EB20) {
    const uint32_t queue = ctx.r3.u32, caller = uint32_t(ctx.lr);
    __imp__sub_8323EB20(ctx, base);
    if (queue == pool) Ao4RecordJob(PopDone, ctx.r3.u32, queue, caller, 0, base, reinterpret_cast<uintptr_t>(&ctx));
}
extern "C" __declspec(dllexport) void AO4_RecordJobClaim(PPCContext* context, uint8_t* base,
                                                       uint32_t node, uint64_t replaced, uint64_t written) {
    Ao4RecordJob(ClaimDone, node, 0, uint32_t(context->lr), context->r30.u32,
        base, reinterpret_cast<uintptr_t>(context), replaced, written);
}
extern "C" __declspec(dllexport) void AO4_RecordQueueDetach(PPCContext* context, uint8_t* base,
                                                         uint64_t replaced, uint64_t written) {
    Ao4RecordJob(QueueDetach, uint32_t(replaced), context->r3.u32, uint32_t(context->lr),
        0, base, reinterpret_cast<uintptr_t>(context), replaced, written);
}
extern "C" __declspec(dllexport) void AO4_RecordQueuePublish(PPCContext* context, uint8_t* base,
                                                         uint64_t replaced, uint64_t written) {
    Ao4RecordJob(QueuePublish, context->r4.u32, context->r3.u32, uint32_t(context->lr),
        0, base, reinterpret_cast<uintptr_t>(context), replaced, written);
}
REX_HOOK_RAW(sub_832405A0) {
    struct ChainScope { uint32_t previous; ~ChainScope() { chain_origin = previous; } } scope{chain_origin};
    chain_origin = ctx.r3.u32;
    Ao4RecordJob(ChainEnter, ctx.r3.u32, 0, uint32_t(ctx.lr), 0, base, reinterpret_cast<uintptr_t>(&ctx));
    __imp__sub_832405A0(ctx, base);
}
REX_HOOK_RAW(sub_8323F850) {
    const uint32_t node = ctx.r4.u32, caller = uint32_t(ctx.lr);
    const bool watch = caller == 0x83240660 || caller == 0x8323ED1C;
    if (watch) Ao4RecordJob(CallbackEnter, node, 0, caller, 0, base, reinterpret_cast<uintptr_t>(&ctx));
    __imp__sub_8323F850(ctx, base);
    if (watch) Ao4RecordJob(CallbackDone, node, 0, caller, 0, base, reinterpret_cast<uintptr_t>(&ctx));
}
REX_HOOK_RAW(sub_8323EC58) {
    const uint32_t node = ctx.r3.u32, caller = uint32_t(ctx.lr), generation = ctx.r4.u32;
    Ao4RecordJob(CancelEnter, node, 0, caller, generation, base, reinterpret_cast<uintptr_t>(&ctx));
    __imp__sub_8323EC58(ctx, base);
    Ao4RecordJob(CancelDone, node, 0, caller, generation, base, reinterpret_cast<uintptr_t>(&ctx));
}
REX_HOOK_RAW(sub_8323FDF8) {
    const uint32_t queue = ctx.r3.u32, node = ctx.r4.u32, caller = uint32_t(ctx.lr);
    Ao4RecordJob(EnqueueEnter, node, queue, caller, 0, base, reinterpret_cast<uintptr_t>(&ctx));
    __imp__sub_8323FDF8(ctx, base);
    Ao4RecordJob(EnqueueDone, node, queue, caller, 0, base, reinterpret_cast<uintptr_t>(&ctx));
}

REX_HOOK_RAW(sub_83A34EE0) {
    // Observe original zero fills that overlap tracked active pool nodes.
    const uint32_t begin = ctx.r3.u32, length = ctx.r5.u32, caller = uint32_t(ctx.lr);
    uint32_t overlap = 0; uint64_t before = 0; uint32_t callback = 0;
    const bool watch = Enabled() && ctx.r4.u8 == 0;
    if (watch && uint64_t(begin) + length > 0xFFC40000 && begin < 0xFFC70000) {
        const uint64_t end = std::min<uint64_t>(uint64_t(begin) + length, 0xFFC70000);
        const uint64_t first = (std::max<uint64_t>(begin, 0xFFC40000) + 15) & ~15ull;
        for (uint64_t node = first; node + 20 <= end; node += 16) {
            if (ownership[(node - 0xFFC40000) / 16].load() == 1 &&
                REX_LOAD_U32(uint32_t(node) + 16) == pool) {
                overlap = uint32_t(node); before = REX_LOAD_U64(overlap);
                callback = REX_LOAD_U32(overlap + 8); break;
            }
        }
    }
    __imp__sub_83A34EE0(ctx, base);
    if (!overlap) return;
    static std::atomic_flag reported_zero = ATOMIC_FLAG_INIT;
    if (reported_zero.test_and_set()) return;
    static wchar_t path[1024] = {};
    const DWORD size = GetEnvironmentVariableW(L"AO4_JOB_TRACE_FILE", path, 1024);
    if (!size || size + 6 >= 1024) return;
    wcscat_s(path, L".zero");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512] = {}; DWORD written = 0;
    const int used = std::snprintf(line, sizeof(line),
        "original_memset lr=%08X pool=%08X begin=%08X length=%08X node=%08X\n"
        "before=%016llX callback_before=%08X after=%016llX callback_after=%08X thread=%u\n",
        caller, pool, begin, length, overlap, before, callback, REX_LOAD_U64(overlap),
        REX_LOAD_U32(overlap + 8), unsigned(GetCurrentThreadId()));
    if (used > 0 && used < sizeof(line)) WriteFile(file, line, DWORD(used), &written, nullptr);
    FlushFileBuffers(file); CloseHandle(file);
    Event trigger{}; trigger.node = overlap; trigger.caller = caller;
    trigger.sequence = sequence.load(); trigger.extra = 0xD003;
    trigger.thread = GetCurrentThreadId(); Dump(trigger);
}

REX_HOOK_RAW(sub_83A34F80) {
    // Bound inspection to an already observed allocated job header. Preserve
    // the original copy, capture its source header without unchecked reads.
    const uint32_t begin = ctx.r3.u32, source = ctx.r4.u32, length = ctx.r5.u32;
    const uint32_t caller = uint32_t(ctx.lr);
    uint32_t overlap = 0; uint64_t before = 0, incoming = 0;
    if (Enabled() && uint64_t(begin) + length > 0xFFC40000 && begin < 0xFFC70000) {
        const uint64_t end = std::min<uint64_t>(uint64_t(begin) + length, 0xFFC70000);
        const uint64_t first = (std::max<uint64_t>(begin, 0xFFC40000) + 15) & ~15ull;
        for (uint64_t node = first; node + 8 <= end; node += 16) {
            if (ownership[(node - 0xFFC40000) / 16].load() != 1 ||
                REX_LOAD_U32(uint32_t(node) + 16) != pool) continue;
            const uint64_t prior = REX_LOAD_U64(uint32_t(node));
            const uint32_t generation = uint32_t(prior >> 32) & ~1u;
            if (generation <= 2 || generation >= 0x100000) continue;
            uint64_t raw = 0; SIZE_T read = 0;
            const uint32_t from = source + uint32_t(node - begin);
            if (!ReadProcessMemory(GetCurrentProcess(), REX_RAW_ADDR(from), &raw, sizeof(raw), &read) ||
                read != sizeof(raw)) continue;
            const uint64_t value = __builtin_bswap64(raw);
            if ((uint32_t(value >> 32) & ~1u) >= generation) continue;
            overlap = uint32_t(node); before = prior; incoming = value; break;
        }
    }
    __imp__sub_83A34F80(ctx, base);
    if (!overlap) return;
    static std::atomic_flag reported_copy = ATOMIC_FLAG_INIT;
    if (reported_copy.test_and_set()) return;
    static wchar_t path[1024] = {};
    const DWORD size = GetEnvironmentVariableW(L"AO4_JOB_TRACE_FILE", path, 1024);
    if (!size || size + 6 >= 1024) return;
    wcscat_s(path, L".copy");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512] = {}; DWORD written = 0;
    const int used = std::snprintf(line, sizeof(line),
        "original_copy lr=%08X begin=%08X source=%08X length=%08X node=%08X\n"
        "before=%016llX source_header=%016llX after=%016llX thread=%u\n",
        caller, begin, source, length, overlap, before, incoming, REX_LOAD_U64(overlap), unsigned(GetCurrentThreadId()));
    if (used > 0 && used < sizeof(line)) WriteFile(file, line, DWORD(used), &written, nullptr);
    FlushFileBuffers(file); CloseHandle(file);
    Event trigger{}; trigger.node = overlap; trigger.caller = caller;
    trigger.sequence = sequence.load(); trigger.extra = 0xD004;
    trigger.thread = GetCurrentThreadId(); Dump(trigger);
}
