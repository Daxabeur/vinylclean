// vinylclean — tests of the decrackler and of the full chain.
//   ./vc-tests-crackle          (returns 0 if everything passes)
//   ./vc-tests-crackle --quick  (short version for the installation)
#include "presets.h"
#include "offline.h"
#include "synth.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vc;
using namespace vctest;

static int gFail = 0, gPass = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    (ok ? gPass : gFail)++;
}

template <class P, class Prm>
static std::vector<float> run(double fs, int nch, const std::vector<float>& x, const Prm& prm, int block = 512) {
    P d;
    d.prepare(fs, nch);
    d.setParams(prm);
    return processOffline(d, x, nch, block);
}

// Synthetic crackles: short, soft impulses (1 to 6 samples, half-sine
// shape, as after the RIAA correction), added to the signal.
static std::vector<uint8_t> addCrackle(std::vector<float>& x, int nch, double fs, double rate, double dbLo, double dbHi, uint64_t seed) {
    Rng rng(seed);
    std::vector<uint8_t> truth(x.size(), 0);
    const size_t frames = x.size() / nch;
    const size_t n = size_t(rate * frames / fs) * nch;
    for (size_t k = 0; k < n; ++k) {
        const size_t pos = 100 + size_t(rng.uni() * (frames - 200));
        const int c = int(rng.uni() * nch) % nch;
        const double A = std::pow(10.0, rng.range(dbLo, dbHi) / 20) * (rng.uni() < 0.5 ? -1 : 1);
        const int L = 1 + int(rng.uni() * 6);
        for (int i = 0; i < L; ++i) x[(pos + i) * nch + c] += float(A * std::sin(kPi * (i + 0.5) / L));
        for (int i = -2; i < L + 2; ++i) truth[(pos + i) * nch + c] = 1;
    }
    return truth;
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    std::printf("=== vinylclean: decrackle and chain tests ===\n");
    std::printf("Decrackle intensity → threshold k:\n  ");
    for (int I = 1; I <= 10; ++I) std::printf("%d→%.1f  ", I, decrackleThreshold(float(I)));
    std::printf("\n");

    // 1) Chain: fixed latency, bit-exact transparency ------------------------------
    std::printf("\n1. Declick → Decrackle chain: latency and transparency\n");
    std::vector<double> rates = quick ? std::vector<double>{48000, 96000} : std::vector<double>{44100, 48000, 88200, 96000};
    for (double fs : rates) {
        auto x = synthStereo(fs, size_t(fs * 3), kAll, 11);
        addClicks(x, 2, fs, 15, 2.0, 12);
        addCrackle(x, 2, fs, 100, -60, -36, 13);
        ChainParams p = defaultChainParams();
        Chain probe; probe.prepare(fs, 2);
        const int lat = probe.latency();
        p.mode = Mode::Bypass;  auto yb = run<Chain>(fs, 2, x, p);
        p.mode = Mode::Repair;  auto yr = run<Chain>(fs, 2, x, p);
        p.mode = Mode::Removed; auto yx = run<Chain>(fs, 2, x, p);
        ChainParams z; z.declick.intensity = 0; z.decrackle.intensity = 0;
        auto y0 = run<Chain>(fs, 2, x, z);
        bool remOk = true;
        for (size_t i = 0; i < x.size(); ++i) if (yx[i] != x[i] - yr[i]) { remOk = false; break; }
        char msg[200];
        std::snprintf(msg, sizeof msg, "%5.1f kHz: latency %d samples (%.1f ms); Bypass bit-identical", fs / 1000, lat, 1000.0 * lat / fs);
        check(yb == x, msg);
        std::snprintf(msg, sizeof msg, "%5.1f kHz: intensities 0: bit-identical; Removed = input − Repair", fs / 1000);
        check(y0 == x && remOk, msg);
    }

    // 2) Independence from the block size ---------------------------------------------
    std::printf("\n2. Same result whatever the block size\n");
    {
        const double fs = 48000;
        auto x = synthStereo(fs, size_t(fs * 2), kAll, 21);
        addCrackle(x, 2, fs, 200, -60, -36, 22);
        ChainParams p = defaultChainParams();
        auto ref = run<Chain>(fs, 2, x, p, 512);
        bool same = true;
        for (int b : {1, 7, 256, 333, 4096}) same = same && (run<Chain>(fs, 2, x, p, b) == ref);
        check(same, "blocks of 1, 7, 256, 333 and 4096: bit-identical outputs");
    }

    // 3) Efficiency and harmlessness ----------------------------------------------------
    std::printf("\n3. Synthetic crackles (150/s per channel, −60 to −36 dBFS)\n");
    std::printf("   music               int. | crackles reduced    | clean music: SNR      samples changed\n");
    struct { const char* name; unsigned m; float gain; } mus[] = {
        {"quiet piano (−25 dB)", kPiano | kSurface, 0.1f}, {"realistic", kPiano | kBrass | kVoice | kSurface, 1.f}};
    double quietRed = 0, worstSnr = 1e9;
    for (auto& mu : mus) {
        const double fs = 48000;
        auto clean = synthStereo(fs, size_t(fs * (quick ? 8 : 15)), mu.m, 31);
        for (auto& v : clean) v *= mu.gain;
        auto dirty = clean;
        auto truth = addCrackle(dirty, 2, fs, 150, -60, -36, 32);
        for (int I : quick ? std::vector<int>{8} : std::vector<int>{2, 4, 6, 8, 10}) {
            DecrackleParams p; p.intensity = float(I);
            auto y = run<Decrackler>(fs, 2, dirty, p);
            auto yc = run<Decrackler>(fs, 2, clean, p);
            double e0 = 0, e1 = 0, es = 0, ed = 0;
            size_t mod = 0;
            for (size_t i = 0; i < clean.size(); ++i) {
                if (truth[i]) { e0 += double(dirty[i] - clean[i]) * (dirty[i] - clean[i]); e1 += double(y[i] - clean[i]) * (y[i] - clean[i]); }
                es += double(clean[i]) * clean[i];
                ed += double(yc[i] - clean[i]) * (yc[i] - clean[i]);
                mod += (yc[i] != clean[i]);
            }
            const double red = 10 * std::log10(e0 / std::max(e1, 1e-30));
            const double snr = ed > 0 ? 10 * std::log10(es / ed) : 200;
            std::printf("   %-20s %3d  |      %5.1f dB       |       %6.1f dB    %.3f %%\n", mu.name, I, red, std::fmin(snr, 200.0), 100.0 * mod / clean.size());
            if (I == 8 && mu.gain < 1.f) quietRed = red;
            if (I == 8) worstSnr = std::min(worstSnr, snr);
        }
    }
    check(quietRed >= 6.0, "quiet passage, intensity 8 (sensitivity 5, k ≈ 6): crackles reduced by at least 6 dB");
    check(worstSnr >= 60.0, "clean music, intensity 8: music intact (SNR ≥ 60 dB)");

    // 4) Robustness -------------------------------------------------------------------
    std::printf("\n4. Robustness (no NaN, no blow-up)\n");
    {
        const double fs = 48000;
        const size_t F = size_t(fs * 2);
        Rng rng(5);
        bool ok = true;
        for (int kind = 0; kind < 6; ++kind) {
            std::vector<float> v(F * 2);
            for (size_t i = 0; i < F; ++i) {
                float s = 0;
                switch (kind) {
                    case 0: s = 0; break;
                    case 1: s = 0.9f; break;
                    case 2: s = ((i / 24) & 1) ? 1.f : -1.f; break;
                    case 3: s = float(rng.range(-1, 1)); break;
                    case 4: s = (i & 1) ? 1.f : -1.f; break;
                    default: s = rng.uni() < 0.05 ? 1.f : 0.f;
                }
                v[2 * i] = s; v[2 * i + 1] = -s;
            }
            ChainParams p; p.declick.intensity = 10; p.decrackle.intensity = 10; p.declick.maxClickMs = 5;
            auto y = run<Chain>(fs, 2, v, p);
            for (float t : y) if (!std::isfinite(t) || std::fabs(t) > 4.f) { ok = false; break; }
        }
        check(ok, "6 extreme signals at maximum intensity: finite and bounded outputs");
    }

    std::printf("\nResult: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
