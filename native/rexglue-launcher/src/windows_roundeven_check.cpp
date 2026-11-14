#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <xmmintrin.h>

extern "C" float roundevenf(float);

int main() {
    const unsigned saved = _mm_getcsr();
    const float inputs[] = {0.f, -0.f, .5f, -.5f, 1.5f, -1.5f, 2.5f,
        -2.5f, 3.5f, -3.5f, .75f, -.75f, 16777216.f,
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    const float expected[] = {0.f, -0.f, 0.f, -0.f, 2.f, -2.f, 2.f,
        -2.f, 4.f, -4.f, 1.f, -1.f, 16777216.f,
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    unsigned checks = 0;
    for (unsigned mode = 0; mode < 4; ++mode) {
        const unsigned control = (saved & ~0x6000u) | (mode << 13);
        _mm_setcsr(control);
        for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
            if (std::bit_cast<uint32_t>(roundevenf(inputs[i])) !=
                std::bit_cast<uint32_t>(expected[i])) return 1;
            ++checks;
        }
        if (!std::isnan(roundevenf(std::numeric_limits<float>::quiet_NaN()))) return 2;
        ++checks;
        if ((_mm_getcsr() & 0x6000u) != (control & 0x6000u)) return 3;
        ++checks;
    }
    _mm_setcsr(saved);
    std::printf("PASS: %u roundevenf checks, four host rounding modes\n", checks);
}
