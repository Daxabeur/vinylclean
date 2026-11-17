// vinylclean — test of the clock drift correction (drift.h): resampler quality
// (sine through a ratio of 1.0002: noise + distortion) and control loop
// (simulated input and output cards whose clocks differ by up to ±500 ppm:
// the output buffer never runs dry nor overflows). Original work.
#include "drift.h"
#include <cstdio>
#include <vector>
using namespace vc;
static int fails = 0;
static void check(const char* what, double got, double lo, double hi) {
    const bool ok = got >= lo && got <= hi;
    std::printf("%-58s %10.2f  [%g .. %g]  %s\n", what, got, lo, hi, ok ? "OK" : "FAIL");
    fails += !ok;
}
int main() {
    // ---- 1) Quality: 1 kHz then 15 kHz sine, step 1.0002, compared with the exact sine.
    for (double f : {1000.0, 15000.0}) {
        const double fs = 48000, step = 1.0002;
        DriftResampler rs; rs.prepare(1, 512);
        std::vector<float> in(512), out(1024);
        const float* ip[1] = {in.data()}; float* op[1] = {out.data()};
        double t = 0, errE = 0, sigE = 0; long nOut = 0;
        for (int blk = 0; blk < 400; ++blk) {
            for (int i = 0; i < 512; ++i, t += 1) in[i] = float(0.5 * std::sin(2 * M_PI * f * t / fs));
            const int k = rs.process(ip, 512, op, 1024, step);
            for (int i = 0; i < k; ++i, ++nOut) {
                // Output nOut sits at input position nOut·step − (kHalf − 1)... − the initial zeros (2·kHalf).
                const double x = nOut * step + (DriftResampler::kHalf - 1) - 2 * DriftResampler::kHalf;
                if (nOut < 2000) continue;
                const double ref = 0.5 * std::sin(2 * M_PI * f * x / fs);
                errE += (out[i] - ref) * (out[i] - ref); sigE += ref * ref;
            }
        }
        char w[96]; std::snprintf(w, sizeof w, "resampler, %g Hz sine, ratio 1.0002: error (dB)", f);
        check(w, 10 * std::log10(errE / sigE), -200, -110);
    }
    // ---- 2) Control loop: period 512, buffer 4 periods, target 2 periods.
    for (double ppm : {-500.0, -100.0, 0.0, 37.0, 250.0, 500.0}) {
        const double fi = 48000, fo = 48000 * (1 + ppm * 1e-6), P = 512, cap = 4 * P, target = 2 * P;
        DriftResampler rs; rs.prepare(2, 512);
        DriftControl ctl; ctl.reset(target, fi);
        std::vector<float> a(512, 0.f), b(512, 0.f), oa(1024), ob(1024);
        const float* ip[2] = {a.data(), b.data()}; float* op[2] = {oa.data(), ob.data()};
        double fill = target, minF = 1e9, maxF = -1e9, t = 0;
        for (int k = 0; k < int(600 * fi / P); ++k) {       // 10 minutes
            const double dt = P / fi; t += dt;
            fill -= fo * dt;                                  // the output card plays
            if (t > 120) { minF = std::min(minF, fill); }
            const int n = rs.process(ip, int(P), op, 1024, ctl.step());
            fill += n;                                        // we write what the resampler gave
            if (t > 120) { maxF = std::max(maxF, fill); }
            const double meas = fill - std::fmod(k * 37.0, 64.0);   // measurement with a little jitter
            ctl.update(meas, dt);
            if (fill < 0 || fill > cap) { std::printf("  xrun at %.1f s (fill %.0f)\n", t, fill); break; }
        }
        char w[96];
        std::snprintf(w, sizeof w, "clocks %+.0f ppm: lowest output fill after 2 min (frames)", ppm);
        check(w, minF, P * 0.5, cap);
        std::snprintf(w, sizeof w, "clocks %+.0f ppm: highest output fill (frames)", ppm);
        check(w, maxF, 0, cap - P * 0.5);
        std::snprintf(w, sizeof w, "clocks %+.0f ppm: correction found (ppm)", ppm);
        check(w, -ctl.ppm(), ppm - 3, ppm + 3);
    }
    std::printf(fails ? "\n%d FAILURE(S)\n" : "\nAll drift tests passed.\n", fails);
    return fails ? 1 : 0;
}
