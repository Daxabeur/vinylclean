// vinylclean — tests of the digital phono preamp (RIAA).
#include "riaa.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using namespace vc;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    (ok ? gPass : gFail)++;
}

// Amplitude measured by really passing a sine through the filter.
static double measuredDb(double fs, double f, bool rumble) {
    RiaaFilter r; r.design(fs);
    const int warm = int(fs * 2), n = int(fs * 1);
    double acc = 0, ref = 0;
    for (int i = 0; i < warm + n; ++i) {
        const double x = std::sin(2 * M_PI * f * i / fs);
        const double y = r.tick(x, rumble);
        if (i >= warm) { acc += y * y; ref += x * x; }
    }
    return 10 * std::log10(acc / ref);
}

int main() {
    std::printf("=== vinylclean: phono preamp (RIAA) tests ===\n");
    std::printf("\nOfficial curve (reminder): 20 Hz %+.2f dB, 100 Hz %+.2f, 1 kHz 0, 10 kHz %+.2f, 20 kHz %+.2f\n",
                RiaaFilter::idealDb(20), RiaaFilter::idealDb(100), RiaaFilter::idealDb(10000), RiaaFilter::idealDb(20000));

    std::printf("\n1. Accuracy to the RIAA curve (20 Hz – 20 kHz, 2000 frequencies)\n");
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 32000.0}) {
        RiaaFilter r; r.design(fs);
        double worst = 0, wf = 0;
        for (int i = 0; i < 2000; ++i) {
            const double f = 20.0 * std::pow(1000.0, i / 1999.0);
            if (f > 0.45 * fs) break;
            const double e = std::fabs(20 * std::log10(std::abs(r.response(f))) - RiaaFilter::idealDb(f));
            if (e > worst) { worst = e; wf = f; }
        }
        const bool table = fs != 32000.0;
        char msg[160];
        std::snprintf(msg, sizeof msg, "%6.1f kHz: max deviation %.4f dB (at %.0f Hz)%s", fs / 1000, worst, wf, table ? "" : " — rate not in the table");
        check(worst < (table ? 0.02 : 0.5), msg);   // not in the table: up to 0.45 × fs, near Nyquist
    }

    std::printf("\n2. Real measurement (sines passed through the filter, 48 kHz)\n");
    {
        bool ok = true;
        for (double f : {20.0, 100.0, 1000.0, 5000.0, 15000.0}) {
            const double m = measuredDb(48000, f, false), t = RiaaFilter::idealDb(f);
            std::printf("     %6.0f Hz: measured %+7.2f dB, curve %+7.2f dB\n", f, m, t);
            ok = ok && std::fabs(m - t) < 0.03;
        }
        check(ok, "the sample-by-sample computation gives the curve (deviation < 0.03 dB)");
    }

    std::printf("\n3. Rumble filter (15 Hz high-pass)\n");
    {
        RiaaFilter r; r.design(48000);
        const double a40 = 20 * std::log10(std::abs(r.rumbleResponse(40))), a75 = 20 * std::log10(std::abs(r.rumbleResponse(7.5)));
        const double a1k = 20 * std::log10(std::abs(r.rumbleResponse(1000)));
        char msg[160];
        std::snprintf(msg, sizeof msg, "40 Hz %.2f dB (music intact), 7.5 Hz %.1f dB (record warp attenuated), 1 kHz %.3f dB", a40, a75, a1k);
        check(a40 > -0.2 && a75 < -10 && std::fabs(a1k) < 0.01, msg);
    }

    std::printf("\n4. Robustness\n");
    {
        RiaaFilter r; r.design(44100);
        std::mt19937 g(1); std::normal_distribution<double> n(0, 0.3);
        bool fin = true; double mx = 0;
        for (int i = 0; i < 44100 * 20; ++i) { const double y = r.tick(n(g), true); fin = fin && std::isfinite(y); mx = std::max(mx, std::fabs(y)); }
        check(fin && mx < 50, "20 s of loud noise: finite and bounded output (stable)");
    }
    {
        // Switching the RIAA on / off and changing the gain during playback: no jump.
        const double fs = 48000;
        PhonoStage p; p.prepare(fs);
        float prev = 0, maxJump = 0, maxNat = 0, prevx = 0;
        const double h3k = std::abs(p.filter().response(3000));
        for (int i = 0; i < int(fs * 3); ++i) {
            const float x = float(0.05 * std::sin(2 * M_PI * 3000 * i / fs));
            const bool on = (i > fs * 0.5 && i < fs * 2);
            const double g = (i > fs) ? 10.0 : 1.0;   // +20 dB at once
            const float y = p.tick(x, on, g, true);
            if (i > 0) { maxJump = std::max(maxJump, std::fabs(y - prev)); maxNat = std::max(maxNat, std::fabs(x - prevx)); }
            prev = y; prevx = x;
        }
        char msg[160];
        const double bound = maxNat * std::max(1.0, 10.0 * h3k) * 1.05;   // largest step of the music itself
        std::snprintf(msg, sizeof msg, "on / off / +20 dB: largest step %.4f, that of the music %.4f (no jump)", maxJump, bound / 1.05);
        check(maxJump <= bound, msg);
    }

    std::printf("\nResult: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
