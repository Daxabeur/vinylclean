// vinylclean — record information for the "Levels" card: left/right balance,
// stereo / mono / phase (correlation) and background noise level.
//
// The audio thread sums L², R² and L·R of the signal after the phono preamp
// (one item per period, lock-free queue, no allocation). The main thread
// groups them into 100 ms blocks and derives, once per second:
//  - balance: 10·log10(ΣL² / ΣR²) over the music of the last 30 s;
//  - correlation: ΣLR / √(ΣL²·ΣR²) over the last 3 s of music (+1 mono, ≈0 wide
//    stereo, < 0 one channel probably inverted);
//  - noise floor: quietest 400 ms of the last 3 minutes between two passages
//    of music (the gap between two tracks), digital silence excluded.
// Original work.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace vc {

struct LevelItem { float ll, rr, lr; uint32_t n; };

struct LevelRing {                     // single-producer / single-consumer queue
    static constexpr uint32_t N = 1024;
    LevelItem buf[N];
    std::atomic<uint32_t> w{0}, r{0};
    void push(const LevelItem& it) {
        const uint32_t wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= N) return;   // full: skip
        buf[wi % N] = it;
        w.store(wi + 1, std::memory_order_release);
    }
    bool pop(LevelItem& it) {
        const uint32_t ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        it = buf[ri % N];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
};

// Audio thread: sums for one period (planar L, R).
inline LevelItem levelSums(const float* l, const float* r, size_t n) {
    double ll = 0, rr = 0, lr = 0;
    for (size_t i = 0; i < n; ++i) { ll += double(l[i]) * l[i]; rr += double(r[i]) * r[i]; lr += double(l[i]) * r[i]; }
    return {float(ll), float(rr), float(lr), uint32_t(n)};
}

// Main thread.
struct LevelStats {
    struct Block { double ll = 0, rr = 0, lr = 0; uint32_t n = 0; };
    std::deque<Block> blocks;          // 100 ms blocks, 3 minutes
    Block cur;
    uint32_t blockLen = 4800;
    // Results (NAN = not known yet).
    double balanceDb = NAN, correlation = NAN, noiseDb = NAN;
    // VU meters of the touchscreen page: L² and R² since the last takeVu().
    double vuLL = 0, vuRR = 0; uint32_t vuN = 0;

    void reset(unsigned rate) { blocks.clear(); cur = Block{}; blockLen = std::max(1u, rate / 10); balanceDb = correlation = noiseDb = NAN; vuLL = vuRR = 0; vuN = 0; }
    void add(const LevelItem& it) {
        cur.ll += it.ll; cur.rr += it.rr; cur.lr += it.lr; cur.n += it.n;
        vuLL += it.ll; vuRR += it.rr; vuN += it.n;
        if (cur.n >= blockLen) {
            blocks.push_back(cur);
            cur = Block{};
            while (blocks.size() > 1800) blocks.pop_front();
        }
    }
    // RMS level of each channel (dBFS) since the previous call (≈ 100 ms), then restart.
    void takeVu(double& l, double& r) {
        auto db = [](double ms) { return ms > 1e-12 ? 10.0 * std::log10(ms) : -120.0; };
        l = vuN ? db(vuLL / vuN) : -120.0; r = vuN ? db(vuRR / vuN) : -120.0;
        vuLL = vuRR = 0; vuN = 0;
    }
    void compute() {
        const size_t nb = blocks.size();
        auto db = [](double ms) { return 10.0 * std::log10(std::max(ms, 1e-30)); };
        // Balance: music only (blocks above −50 dBFS RMS), last 30 s.
        double sl = 0, sr = 0, slr = 0;
        size_t used = 0;
        for (size_t i = nb > 300 ? nb - 300 : 0; i < nb; ++i) {
            const Block& b = blocks[i];
            if (!b.n) continue;
            if (db((b.ll + b.rr) / (2.0 * b.n)) < -50.0) continue;
            sl += b.ll; sr += b.rr; ++used;
        }
        balanceDb = (used >= 50 && sl > 0 && sr > 0) ? db(sl / sr) : NAN;   // at least 5 s of music
        // Correlation: the last 3 s of music (within the last minute).
        sl = sr = slr = 0; used = 0;
        for (size_t i = nb; i-- > (nb > 600 ? nb - 600 : 0) && used < 30;) {
            const Block& b = blocks[i];
            if (!b.n || db((b.ll + b.rr) / (2.0 * b.n)) < -50.0) continue;
            sl += b.ll; sr += b.rr; slr += b.lr; ++used;
        }
        correlation = (used >= 10 && sl > 0 && sr > 0) ? slr / std::sqrt(sl * sr) : NAN;
        // Noise floor: quietest 400 ms window that lies between two passages of
        // music (music less than 20 s before AND after: a gap between tracks, not
        // the arm raised), digital silence (< −100 dBFS) excluded.
        std::vector<long> lastLoud(nb), nextLoud(nb);
        long lastI = -100000;
        for (size_t i = 0; i < nb; ++i) {
            const Block& b = blocks[i];
            if (b.n && db((b.ll + b.rr) / (2.0 * b.n)) > -35.0) lastI = long(i);
            lastLoud[i] = lastI;
        }
        long nextI = 100000000;
        for (size_t i = nb; i-- > 0;) {
            const Block& b = blocks[i];
            if (b.n && db((b.ll + b.rr) / (2.0 * b.n)) > -35.0) nextI = long(i);
            nextLoud[i] = nextI;
        }
        double best = INFINITY;
        for (size_t i = 1; i + 5 <= nb; ++i) {
            if (long(i) - lastLoud[i - 1] > 200 || nextLoud[i + 4] - long(i + 3) > 200) continue;
            double e = 0; uint32_t n = 0;
            for (size_t k = i; k < i + 4; ++k) { e += blocks[k].ll + blocks[k].rr; n += 2 * blocks[k].n; }
            if (!n) continue;
            const double d = db(e / n);
            if (d > -100.0 && d < best) best = d;
        }
        noiseDb = std::isfinite(best) ? best : NAN;
    }
};

} // namespace vc
