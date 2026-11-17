// vinylclean — dynamics of the side being played, measured on the ORIGINAL
// sound (turntable signal after the phono preamp, before any cleaning), from
// the moment the stylus is dropped to now. For the "loudness war" question:
// how much dynamics does this music really have?
//
//  - DR: crest factor of the loudest passages, the published "DR" method
//    (3 s blocks; RMS of each block = √(2·mean x²); DR of a channel =
//    second highest block peak / RMS of the loudest 20 % of the blocks, in dB;
//    mean of the channels). DR 14 and more: very dynamic; 8 to 13: moderate;
//    7 and less: strongly compressed.
//  - PLR: highest sample peak − integrated loudness (dB).
//  - Integrated loudness: ITU-R BS.1770-4 (K-weighting, 400 ms blocks every
//    100 ms, absolute gate −70 LUFS, relative gate −10 LU).
//
// The audio thread K-weights the original (2 small filters per sample) and,
// every period, puts the sums of each channel into a lock-free queue.
// A new side starts when the groove comes back after ≥ 6 s of "arm up" (no
// treble peak above −50 dBFS: neither music nor surface noise; the hum of the
// raised arm is ignored), or on request ("New side" button, or "I am dropping
// the stylus" of the anti-distortion).
// Original work based on the public standards.
#pragma once

#include "loudness.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vc {

struct SideItem { float kl, kr, ll, rr, pl, pr, ph; uint32_t n; };   // K-weighted Σx², Σx², peaks, treble peak

struct SideRing {                       // single-producer / single-consumer queue
    static constexpr uint32_t N = 1024;
    SideItem buf[N];
    std::atomic<uint32_t> w{0}, r{0};
    void push(const SideItem& it) {
        const uint32_t wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= N) return;   // full: skip
        buf[wi % N] = it;
        w.store(wi + 1, std::memory_order_release);
    }
    bool pop(SideItem& it) {
        const uint32_t ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        it = buf[ri % N];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
};

// Audio thread: K-weighting filters of the two channels + sums of one period.
// The treble peak (high-pass 1 kHz) tells "stylus in the groove" from "arm raised":
// the groove always brings surface noise and ticks in the treble, while with the
// arm raised there is only hum (bass) and a little hiss, whatever the gain.
struct SideTap {
    Biquad shelf[2], hp[2], tre[2];
    void prepare(double fs) {
        for (int c = 0; c < 2; ++c) {
            kWeighting(fs, shelf[c], hp[c]);
            // Butterworth high-pass, 1 kHz, 2nd order.
            const double K = std::tan(M_PI * 1000.0 / fs), Q = 0.7071067811865476, a0 = 1.0 + K / Q + K * K;
            tre[c].b0 = float(1.0 / a0); tre[c].b1 = float(-2.0 / a0); tre[c].b2 = float(1.0 / a0);
            tre[c].a1 = float(2.0 * (K * K - 1.0) / a0); tre[c].a2 = float((1.0 - K / Q + K * K) / a0);
        }
    }
    SideItem sums(const float* l, const float* r, size_t n) {
        double kl = 0, kr = 0, ll = 0, rr = 0; float pl = 0, pr = 0, ph = 0;
        for (size_t i = 0; i < n; ++i) {
            const float a = l[i], b = r[i];
            const float ka = hp[0].tick(shelf[0].tick(a)), kb = hp[1].tick(shelf[1].tick(b));
            kl += double(ka) * ka; kr += double(kb) * kb; ll += double(a) * a; rr += double(b) * b;
            pl = std::fmax(pl, std::fabs(a)); pr = std::fmax(pr, std::fabs(b));
            ph = std::fmax(ph, std::fmax(std::fabs(tre[0].tick(a)), std::fabs(tre[1].tick(b))));
        }
        return {float(kl), float(kr), float(ll), float(rr), pl, pr, ph, uint32_t(n)};
    }
};

// Main thread.
class SideMeter {
public:
    // Results (NAN = not known yet).
    double lufs = NAN, plr = NAN, dr = NAN, peakDb = NAN, seconds = 0;
    bool armUp = true;              // no signal at the moment (arm raised)
    unsigned sideCount = 0;         // number of sides started (the page sees a new side)

    void reset(unsigned rate) {
        blockLen_ = std::max(1u, rate / 10); rate_ = std::max(1u, rate);
        cur_ = Block{}; armBlocks_ = 1000; armUp = true;
        startSide();
        sideCount = 0;
    }
    void startSide() {             // "New side" (button, or stylus dropped)
        blocks_.clear(); big_.clear(); bigCur_ = Big{}; nPend_ = 0;
        lufs = plr = dr = peakDb = NAN; seconds = 0; started_ = false;
        ++sideCount;
    }
    void add(const SideItem& it) {
        cur_.k += double(it.kl) + it.kr; cur_.l += it.ll; cur_.r += it.rr;
        cur_.pl = std::max(cur_.pl, it.pl); cur_.pr = std::max(cur_.pr, it.pr); cur_.ph = std::max(cur_.ph, it.ph); cur_.n += it.n;
        if (cur_.n >= blockLen_) { block(cur_); cur_ = Block{}; }
    }
    // Once per second.
    void compute() {
        const size_t nb = blocks_.size();
        uint64_t total = 0;
        for (const Block& b : blocks_) total += b.n;
        seconds = double(total) / rate_;
        if (nb < 4) return;
        // Integrated loudness: 400 ms blocks (4 × 100 ms) every 100 ms.
        auto L = [](double e) { return e > 1e-20 ? -0.691 + 10.0 * std::log10(e) : -200.0; };
        double sum = 0; size_t cnt = 0;
        mom_.clear();
        for (size_t i = 3; i < nb; ++i) {
            const double e = (blocks_[i].k + blocks_[i - 1].k + blocks_[i - 2].k + blocks_[i - 3].k) /
                             double(blocks_[i].n + blocks_[i - 1].n + blocks_[i - 2].n + blocks_[i - 3].n);
            mom_.push_back(e);
            if (L(e) > -70.0) { sum += e; ++cnt; }
        }
        if (cnt) {
            const double rel = L(sum / cnt) - 10.0;
            double s2 = 0; size_t c2 = 0;
            for (double e : mom_) if (L(e) > -70.0 && L(e) > rel) { s2 += e; ++c2; }
            lufs = c2 ? L(s2 / c2) : NAN;
        }
        float pk = 0;
        for (const Block& b : blocks_) pk = std::max(pk, std::max(b.pl, b.pr));
        peakDb = pk > 0 ? 20.0 * std::log10(pk) : NAN;
        plr = (std::isfinite(lufs) && std::isfinite(peakDb)) ? peakDb - lufs : NAN;
        // DR: 3 s blocks of the side (+ the block in progress if it already lasts 1 s).
        // At least 4 whole blocks (12 s of music): before that, one loud and one
        // quiet block would give a meaningless figure.
        const bool withCur = bigCur_.n >= rate_;
        if (big_.size() >= 4) {
            double drs = 0; int nch = 0;
            for (int c = 0; c < 2; ++c) {
                rmsTmp_.clear(); pkTmp_.clear();
                auto put = [&](const Big& b) { rmsTmp_.push_back(std::sqrt(2.0 * (c ? b.r : b.l) / b.n)); pkTmp_.push_back(c ? b.pr : b.pl); };
                for (const Big& b : big_) put(b);
                if (withCur) put(bigCur_);
                std::sort(rmsTmp_.begin(), rmsTmp_.end(), std::greater<double>());
                std::sort(pkTmp_.begin(), pkTmp_.end(), std::greater<double>());
                const size_t top = std::max<size_t>(1, size_t(std::lround(0.2 * rmsTmp_.size())));
                double q = 0; for (size_t i = 0; i < top; ++i) q += rmsTmp_[i] * rmsTmp_[i];
                const double rms = std::sqrt(q / top), pk2 = pkTmp_[1];
                if (rms > 1e-9 && pk2 > 0) { drs += 20.0 * std::log10(pk2 / rms); ++nch; }
            }
            dr = nch ? std::max(0.0, drs / nch) : NAN;
        }
    }

private:
    struct Block { double k = 0, l = 0, r = 0; float pl = 0, pr = 0, ph = 0; uint32_t n = 0; };
    struct Big { double l = 0, r = 0; float pl = 0, pr = 0; uint32_t n = 0; };
    void block(const Block& b) {
        const double lvl = 10.0 * std::log10(std::max(1e-30, std::max(b.l, b.r) / b.n));
        // No groove: no treble peak above −50 dBFS (the hum of a raised arm does not
        // count), or nothing at all. A gap between two tracks, or the arm raised.
        if (b.ph < 3.16e-3f || lvl < -80.0) {
            if (++armBlocks_ >= kArm) { armUp = true; nPend_ = 0; }   // 6 s: arm raised, not counted
            if (!started_ || armUp) return;
            if (nPend_ < kArm) pend_[nPend_++] = b;                      // counted only if the music comes back
            return;
        }
        if (armUp && armBlocks_ >= kArm && started_) startSide();       // the stylus is dropped again: new side
        armBlocks_ = 0; armUp = false; started_ = true;
        for (int i = 0; i < nPend_; ++i) append(pend_[i]);
        nPend_ = 0;
        append(b);
    }
    void append(const Block& b) {
        if (blocks_.size() < 36000) blocks_.push_back(b);   // ≤ 1 hour
        bigCur_.l += b.l; bigCur_.r += b.r; bigCur_.pl = std::max(bigCur_.pl, b.pl); bigCur_.pr = std::max(bigCur_.pr, b.pr);
        bigCur_.n += b.n;
        if (bigCur_.n >= 30u * blockLen_) { big_.push_back(bigCur_); bigCur_ = Big{}; }
    }
    static constexpr int kArm = 60;   // 6 s without groove: arm raised (a gap between tracks is shorter)
    Block pend_[kArm];
    int nPend_ = 0;
    unsigned blockLen_ = 4800, rate_ = 48000;
    Block cur_;
    std::vector<Block> blocks_;
    std::vector<Big> big_;
    Big bigCur_;
    int armBlocks_ = 1000;
    bool started_ = false;
    std::vector<double> mom_, rmsTmp_, pkTmp_;
};

} // namespace vc
