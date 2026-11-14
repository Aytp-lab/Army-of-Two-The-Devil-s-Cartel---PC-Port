#pragma once
#include <atomic>
#include <cstdint>

// Diagnostic maxima, shared by the targeted generated units in this module.
// A maximum is evidence of an earlier observed successful claim, not a proof
// of lifetime continuity: legitimate object reinitialization must be checked.
inline std::atomic<uint32_t> ao4_claim_generation[0x30000 / 16]{};
inline void Ao4RememberClaim(uint32_t node, uint64_t header) {
    const uint32_t generation = uint32_t(header >> 32) & ~1u;
    if (generation <= 2 || generation >= 0x100000) return;
    auto& maximum = ao4_claim_generation[(node - 0xFFC40000) / 16];
    uint32_t previous = maximum.load(std::memory_order_relaxed);
    while (generation > previous && !maximum.compare_exchange_weak(previous, generation,
        std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
