// vinylclean — tests of the loudness measurement (EBU Tech 3341/3342 test signals).
#include "loudness.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace vc;

static int gFail = 0, gPass = 0;
static void check(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what); (ok ? gPass : gFail)++; }

// Feeds a stereo sine of `sec` seconds at `dbfs` into the meter, in periods
// of 512 like vc-live.
static void feed(LoudnessMeter& m, Biquad* sh, Biquad* hp, double fs, double sec, double dbfs, double f = 1000) {
    static double ph = 0;
    const double a = std::pow(10.0, dbfs / 20);
    const int P = 512;
    for (long t = 0; t < long(sec * fs); t += P) {
        float ms = 0, pk = 0;
        for (int i = 0; i < P; ++i) {
            const float v = float(a * std::sin(ph)); ph += 2 * 3.14159265358979 * f / fs;
            for (int c = 0; c < 2; ++c) { const float k = hp[c].tick(sh[c].tick(v)); ms += k * k; }
            pk = std::max(pk, std::fabs(v));
        }
        m.add({ms / P, pk, uint32_t(P)});
    }
}

int main() {
    std::printf("=== vinylclean: loudness measurement tests ===\n");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        Biquad sh[2], hp[2];
        for (int c = 0; c < 2; ++c) kWeighting(fs, sh[c], hp[c]);
        LoudnessMeter m; m.reset(unsigned(fs));
        feed(m, sh, hp, fs, 20, -23);
        double s, i, lra, plr, sec;
        m.compute(s, i, lra, plr, sec);
        char msg[160];
        std::snprintf(msg, sizeof msg, "%.1f kHz: 1 kHz stereo sine at −23 dBFS → %.2f LUFS (expected −23.0 ± 0.1)", fs / 1000, i);
        check(std::fabs(i + 23) <= 0.1, msg);
        // EBU Tech 3342, case 1: 20 s at −20 dBFS then 20 s at −30 dBFS → LRA 10 LU.
        LoudnessMeter m2; m2.reset(unsigned(fs));
        feed(m2, sh, hp, fs, 20, -20); feed(m2, sh, hp, fs, 20, -30);
        m2.compute(s, i, lra, plr, sec);
        std::snprintf(msg, sizeof msg, "%.1f kHz: −20 then −30 dBFS → loudness range %.1f LU (expected 10 ± 1)", fs / 1000, lra);
        check(std::fabs(lra - 10) <= 1, msg);
        std::snprintf(msg, sizeof msg, "%.1f kHz: peak-to-loudness ratio %.1f dB (expected ≈ 2.6)", fs / 1000, plr);
        check(plr > 0 && plr < 15, msg);
    }
    // Only the last minute counts.
    {
        Biquad sh[2], hp[2]; for (int c = 0; c < 2; ++c) kWeighting(48000, sh[c], hp[c]);
        LoudnessMeter m; m.reset(48000);
        feed(m, sh, hp, 48000, 60, -40); feed(m, sh, hp, 48000, 61, -23);
        double s, i, lra, plr, sec; m.compute(s, i, lra, plr, sec);
        char msg[160]; std::snprintf(msg, sizeof msg, "one-minute sliding window: %.2f LUFS over %.0f s (expected −23)", i, sec);
        check(std::fabs(i + 23) < 0.2 && sec <= 60.01, msg);
    }
    std::printf("\nResult: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
