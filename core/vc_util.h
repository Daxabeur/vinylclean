// vinylclean — shared helpers for the real-time DSP code.
// Original work (clean-room). License: see LICENSE.
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace vc {

// The three listening modes shared by all processors.
enum class Mode : int {
    Bypass  = 0,  // original sound, delayed by the same latency
    Removed = 1,  // what is removed: input − output
    Repair  = 2   // cleaned music
};

inline Mode modeFromInt(int m) {
    if (m <= 0) return Mode::Bypass;
    if (m == 1) return Mode::Removed;
    return Mode::Repair;
}

// Flushes denormal numbers to zero for the lifetime of the object.
//  - AArch64 (64-bit Raspberry Pi): bit 24 (FZ) of the FPCR register; on ARM it
//    covers both inputs and outputs (equivalent to DAZ + FTZ).
//  - x86 (tests on a PC): FTZ (15) and DAZ (6) bits of MXCSR.
// Put at the start of every audio processing call. No system call.
class DenormalGuard {
public:
    DenormalGuard() {
#if defined(__aarch64__)
        uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        fpcr |= (uint64_t(1) << 24);
        asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__SSE__) || defined(__x86_64__)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);
#endif
    }
    ~DenormalGuard() {
#if defined(__aarch64__)
        uint64_t fpcr = saved_;
        asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__SSE__) || defined(__x86_64__)
        _mm_setcsr(static_cast<unsigned>(saved_));
#endif
    }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;
private:
    uint64_t saved_ = 0;
};

// Float dot product with 4 accumulators (NEON / SSE vectorizable).
inline float dot(const float* a, const float* b, int n) {
    float s0 = 0.f, s1 = 0.f, s2 = 0.f, s3 = 0.f;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; ++i) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

inline int roundi(double v) { return static_cast<int>(std::lround(v)); }

} // namespace vc
