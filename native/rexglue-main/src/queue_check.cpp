// Exercise the actual generated PPC queue code without starting the game.
// The callback is synthetic test data, never registered in the game process.
#include "generated/ao4.main/ao4_main_init.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <rex/hook.h>

namespace {
constexpr unsigned node_count = 128;
constexpr unsigned iterations = 10000;
constexpr unsigned max_jobs = 6 * iterations;
std::array<std::atomic<bool>, node_count> active{};
std::array<std::atomic<unsigned>, max_jobs> callbacks{};
std::array<uint32_t, max_jobs> callback_nodes{};
std::atomic<unsigned> failures{0}, completed{0};
uint32_t nodes_address = 0;
uint32_t owner_address = 0;
bool observing_returns = false;
bool actual_enqueue = false;

void TestCallback(PPCContext& ctx, uint8_t*) {
    const uint32_t id = ctx.r3.u32;
    if (id >= max_jobs || ctx.r4.u32 != callback_nodes[id] ||
        callbacks[id].fetch_add(1) != 0) {
        failures.fetch_add(1);
    }
    completed.fetch_add(1);
}

void IncrementOwner(uint8_t* base, uint32_t owner) {
    auto* word = reinterpret_cast<uint32_t*>(REX_RAW_ADDR(owner + 48));
    uint32_t prior = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    for (;;) {
        const uint32_t next = __builtin_bswap32(__builtin_bswap32(prior) + 1);
        if (__atomic_compare_exchange_n(word, &prior, next, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    }
}

bool Run(unsigned workers, bool physical, bool cancel = false) {
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100010000ULL,
        MEM_RESERVE, PAGE_READWRITE));
    if (!base) { std::puts("FAIL: arena reserve"); return false; }
    const uint32_t owner = physical ? 0xFFC46180u : 0x20000u;
    owner_address = owner;
    const uint32_t pending = owner + 0x100;
    nodes_address = owner + 0x200;
    const auto commit = [&](uint8_t* at, SIZE_T size) {
        return VirtualAlloc(at, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    };
    if (!commit(base + (owner & ~0xFFFFu), 0x20000) ||
        !commit(base + 0x100000, workers * 0x10000) ||
        !commit(base + REX_IMAGE_BASE + REX_IMAGE_SIZE, 0x1000)) {
        std::puts("FAIL: arena commit"); VirtualFree(base, 0, MEM_RELEASE); return false;
    }
    failures = 0; completed = 0;
    for (auto& state : active) state = false;
    for (auto& count : callbacks) count = 0;
    REX_LOOKUP_FUNC(base, REX_CODE_BASE) = &TestCallback;
    REX_STORE_U32(owner + 40, 4);
    PPCContext initial{};
    initial.r1.u32 = 0x10F000;
    for (unsigned id = 0; id < node_count; ++id) {
        initial.r3.u32 = owner; initial.r4.u32 = nodes_address + id * 64;
        initial.r5.u32 = 4;
        __imp__sub_8323EAA0(initial, base);
    }
    observing_returns = true;
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            PPCContext ctx{};
            ctx.r1.u32 = 0x10F000 + worker * 0x10000;
            for (unsigned i = 0; i < iterations && !failures.load(); ++i) {
                uint32_t node = 0;
                while (!node && !failures.load()) {
                    ctx.r3.u32 = owner; ctx.r4.u32 = 4;
                    __imp__sub_8323EB20(ctx, base);
                    node = ctx.r3.u32;
                    if (!node) {
                        ctx.r3.u32 = pending; ctx.r4.u32 = 0;
                        __imp__sub_83240E28(ctx, base);
                        std::this_thread::yield();
                    }
                }
                if (!node) break;
                const uint32_t id = (node - nodes_address) / 64;
                if (node < nodes_address || id >= node_count ||
                    (node - nodes_address) % 64 || active[id].exchange(true)) {
                    failures.fetch_add(1); break;
                }
                IncrementOwner(base, owner);
                const unsigned token = worker * iterations + i;
                callback_nodes[token] = node;
                const uint32_t generation = REX_LOAD_U32(node) & ~1u;
                REX_STORE_U32(node + 8, REX_CODE_BASE);
                REX_STORE_U32(node + 12, token);
                REX_STORE_U32(node + 16, owner);
                ctx.r3.u32 = pending; ctx.r4.u32 = node; ctx.r5.u32 = 4;
                if (actual_enqueue) __imp__sub_8323FDF8(ctx, base);
                else sub_8323EAA0(ctx, base);
                if (cancel && (i & 1)) {
                    ctx.r3.u32 = node; ctx.r4.u32 = generation; ctx.r5.u32 = 0;
                    __imp__sub_8323EC58(ctx, base);
                }
                ctx.r3.u32 = pending; ctx.r4.u32 = 0;
                __imp__sub_83240E28(ctx, base);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    initial.r3.u32 = pending; initial.r4.u32 = 0;
    __imp__sub_83240E28(initial, base);
    observing_returns = false;
    const unsigned expected = workers * iterations;
    const bool passed = failures == 0 && completed == expected &&
        REX_LOAD_U32(owner + 48) == 0;
    std::printf("%s: workers=%u physical=%u cancel=%u enqueue=%s completed=%u expected=%u failures=%u\n",
        passed ? "PASS" : "FAIL", workers, unsigned(physical), unsigned(cancel),
        actual_enqueue ? "8323FDF8" : "8323EAA0", completed.load(), expected, failures.load());
    std::fflush(stdout);
    VirtualFree(base, 0, MEM_RELEASE);
    return passed;
}
}

// Only linked into this isolated test executable. Return-state observation
// happens before the original function publishes a node to its free list.
REX_HOOK_RAW(sub_8323EAA0) {
    if (observing_returns && ctx.r3.u32 == owner_address) {
        const uint32_t node = ctx.r4.u32;
        const uint32_t id = (node - nodes_address) / 64;
        if (node < nodes_address || id >= node_count ||
            (node - nodes_address) % 64 || !active[id].exchange(false)) {
            failures.fetch_add(1);
        }
    }
    __imp__sub_8323EAA0(ctx, base);
}

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--job-enqueue"))) return 2;
    actual_enqueue = argc == 2;
    return Run(1, false) && Run(6, false) && Run(1, true) && Run(6, true) &&
        Run(1, false, true) && Run(6, false, true) &&
        Run(1, true, true) && Run(6, true, true) ? 0 : 1;
}
