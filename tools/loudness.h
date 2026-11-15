// vinylclean — loudness measurement (ITU-R BS.1770-4 / EBU R128, Tech 3342),
// K-weighting, integrated loudness, loudness range (LRA) and peak-to-loudness ratio.
// Original work based on the public standards.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vc {

// The audio thread filters the output (K-weighting: 2 small filters per
// sample) and, every period, puts the mean energy into a lock-free queue.
// The main thread derives loudness and dynamic range from it.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float tick(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
static void kWeighting(double fs, Biquad& shelf, Biquad& hp) {
    // Coefficients of the standard, recomputed for any sample rate.
    const double pi = 3.14159265358979323846;
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(pi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = float((Vh + Vb * K / Q + K * K) / a0); shelf.b1 = float(2.0 * (K * K - Vh) / a0);
        shelf.b2 = float((Vh - Vb * K / Q + K * K) / a0); shelf.a1 = float(2.0 * (K * K - 1.0) / a0);
        shelf.a2 = float((1.0 - K / Q + K * K) / a0);
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(pi * f0 / fs), a0 = 1.0 + K / Q + K * K;
        hp.b0 = 1.f; hp.b1 = -2.f; hp.b2 = 1.f;
        hp.a1 = float(2.0 * (K * K - 1.0) / a0); hp.a2 = float((1.0 - K / Q + K * K) / a0);
    }
}
struct LoudItem { float ms; float peak; uint32_t n; };
struct LoudRing {                      // single-producer / single-consumer queue
    static constexpr uint32_t N = 1024;
    LoudItem buf[N];
    std::atomic<uint32_t> w{0}, r{0};
    void push(const LoudItem& it) {
        const uint32_t wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= N) return;  // full: skip
        buf[wi % N] = it;
        w.store(wi + 1, std::memory_order_release);
    }
    bool pop(LoudItem& it) {
        const uint32_t ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        it = buf[ri % N];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
};

// Main thread: 100 ms blocks over the last minute.
struct LoudnessMeter {
    std::vector<double> blocks;   // K-weighted energy of each 100 ms block
    std::vector<float> peaks;     // peak of each block
    double acc = 0; float pk = 0; uint32_t n = 0, blockLen = 4800;
    void reset(unsigned rate) { blocks.clear(); peaks.clear(); acc = 0; pk = 0; n = 0; blockLen = rate / 10; }
    void add(const LoudItem& it) {
        acc += double(it.ms) * it.n; pk = std::max(pk, it.peak); n += it.n;
        if (n >= blockLen) {
            blocks.push_back(acc / n); peaks.push_back(pk);
            if (blocks.size() > 600) { blocks.erase(blocks.begin()); peaks.erase(peaks.begin()); }
            acc = 0; pk = 0; n = 0;
        }
    }
    static double lufs(double e) { return e > 1e-20 ? -0.691 + 10.0 * std::log10(e) : -120.0; }
    // Short-term loudness (3 s), integrated loudness over the minute, loudness
    // range (LRA, EBU Tech 3342) and peak-to-loudness ratio (PLR).
    void compute(double& shortTerm, double& integrated, double& lra, double& plr, double& seconds) const {
        shortTerm = integrated = -120; lra = plr = 0; seconds = blocks.size() / 10.0;
        if (blocks.size() < 30) return;
        double e3 = 0; for (size_t i = blocks.size() - 30; i < blocks.size(); ++i) e3 += blocks[i];
        shortTerm = lufs(e3 / 30);
        // Integrated loudness (absolute −70 and relative −10 gates, BS.1770).
        double sum = 0; int cnt = 0;
        for (double b : blocks) if (lufs(b) > -70) { sum += b; ++cnt; }
        if (!cnt) return;
        const double rel = lufs(sum / cnt) - 10; sum = 0; cnt = 0;
        for (double b : blocks) if (lufs(b) > -70 && lufs(b) > rel) { sum += b; ++cnt; }
        if (cnt) integrated = lufs(sum / cnt);
        // LRA: short-term loudness (3 s windows), gates −70 and −20, P95 − P10.
        std::vector<double> st;
        double run = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            run += blocks[i];
            if (i >= 30) run -= blocks[i - 30];
            if (i >= 29) st.push_back(lufs(run / 30));
        }
        double s2 = 0; int c2 = 0;
        for (double v : st) if (v > -70) { s2 += std::pow(10.0, (v + 0.691) / 10); ++c2; }
        if (c2 >= 2) {
            const double gate = lufs(s2 / c2) - 20;
            std::vector<double> g; for (double v : st) if (v > -70 && v > gate) g.push_back(v);
            if (g.size() >= 2) {
                std::sort(g.begin(), g.end());
                lra = g[size_t(0.95 * (g.size() - 1))] - g[size_t(0.10 * (g.size() - 1))];
            }
        }
        float pmax = 0; for (float p : peaks) pmax = std::max(pmax, p);
        if (pmax > 0 && integrated > -120) plr = 20.0 * std::log10(pmax) - integrated;
    }
};


} // namespace vc
