// Windows UCRT compatibility for Clang/SIMDe's C23 math calls.
// Nearest-even is explicit, independent of the caller's MXCSR rounding mode.
// https://clang.llvm.org/doxygen/smmintrin_8h.html
#include <smmintrin.h>

extern "C" __attribute__((target("sse4.1"))) float roundevenf(float value) {
    const __m128 input = _mm_set_ss(value);
    return _mm_cvtss_f32(_mm_round_ss(input, input,
        _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
}
