// vinylclean — tests of the loudness measurement (EBU Tech 3341/3342 test signals)
// and of the record information (balance, stereo / mono, background noise).
#include "loudness.h"
#include "levels.h"
#include <random>
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
    std::printf("\nRecord information (balance, stereo / mono, background noise)\n");
    {
        // 10 s of music (L 1 dB louder than R), 3 s gap at −60 dBFS RMS, 10 s of music, then the arm raised (−80 dBFS).
        const double fs = 48000; const int P = 512;
        auto run = [&](double corrMix, bool invert, LevelStats& st) {
            std::mt19937 g(1); std::normal_distribution<float> nd(0.f, 1.f);
            st.reset(unsigned(fs));
            std::vector<float> l(P), r(P);
            double ph = 0;
            auto seg = [&](double sec, double musicDb, double noiseRms) {
                const double a = std::pow(10.0, musicDb / 20);
                for (long t = 0; t < long(sec * fs); t += P) {
                    for (int i = 0; i < P; ++i) {
                        const double m = a * std::sin(ph); ph += 2 * 3.14159265358979 * 440 / fs;
                        const double u = a * 0.7 * nd(g);   // part not shared by the two channels
                        l[i] = float(m * std::pow(10.0, 0.5 / 20) * corrMix + u * (1 - corrMix) + noiseRms * nd(g));
                        r[i] = float((invert ? -1 : 1) * (m * std::pow(10.0, -0.5 / 20) * corrMix + 0.7 * a * nd(g) * (1 - corrMix)) + noiseRms * nd(g));
                    }
                    st.add(levelSums(l.data(), r.data(), P));
                }
            };
            seg(10, -20, 0.001); seg(3, -200, 0.001); seg(10, -20, 0.001); seg(5, -200, 0.0001);
            st.compute();
        };
        LevelStats a, b, c;
        run(1.0, false, a); run(0.5, false, b); run(1.0, true, c);
        char msg[200];
        std::snprintf(msg, sizeof msg, "balance %.2f dB (expected +1.0, left louder)", a.balanceDb);
        check(std::fabs(a.balanceDb - 1.0) < 0.15, msg);
        std::snprintf(msg, sizeof msg, "correlation: mono-like %.2f, stereo %.2f, one channel inverted %.2f", a.correlation, b.correlation, c.correlation);
        check(a.correlation > 0.95 && b.correlation > 0.1 && b.correlation < 0.9 && c.correlation < -0.9, msg);
        std::snprintf(msg, sizeof msg, "background noise between tracks %.1f dBFS (expected −60; the raised arm at −80 is ignored)", a.noiseDb);
        check(std::fabs(a.noiseDb + 60) < 1.0, msg);
    }
    std::printf("\nResult: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
