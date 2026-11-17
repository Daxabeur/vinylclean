// vinylclean — Decrackle (small, dense crackles) and full chain.
// Original work (clean-room).
//
// The decrackler reuses the declick AR engine with a dedicated profile:
//  - very short repairs only (≤ 0.5 ms): it cannot damage a musical attack
//    in depth;
//  - lower threshold (faint crackles);
//  - SOFT Wiener-type repair: crackle is additive; only the part that
//    exceeds the expected interpolation error is removed. A false detection
//    changes almost nothing.
// A wavelet + median-filter approach (Rajmic & Klimek, adaptive median
// filtering) was tried: it detected well but damaged the treble more than
// it removed crackle; this AR profile was kept.
#pragma once

#include "declick.h"
#include "antidist.h"

namespace vc {

struct DecrackleParams {
    float intensity    = 4.f;   // 0 = off, 1..10
    Mode  mode         = Mode::Repair;
    bool  pitchProtect = true;
    int   protect      = 1;     // 0 none, 1 normal, 2 maximum
    float kOverride    = 0.f;   // > 0: forced threshold k (tuning tools)
    bool  multiband    = true;  // multiband repair
    bool  stereo       = true;  // use the other channel
    float warp         = 0.f;   // warped prediction for the detection (0 = classic)
};

// Decrackle threshold k: 8 (intensity 1) → 3.5 (intensity 10).
float decrackleThreshold(float intensity);

class Decrackler {
public:
    bool prepare(double fs, int channels) { return eng_.prepare(fs, channels, true); }
    void reset() { eng_.reset(); }
    int  latency() const { return eng_.latency(); }
    int  hopSize() const { return eng_.hopSize(); }
    int  channels() const { return eng_.channels(); }
    void setParams(const DecrackleParams& p);
    void process(const float* const* in, float* const* out, int n) { eng_.process(in, out, n); }
    DeclickStats stats() const { return eng_.stats(); }
    void setWorker(ChannelWorker* w) { eng_.setWorker(w); }
private:
    Declicker eng_;
};

// Declick → Decrackle chain, with a global listening mode:
//  Bypass = input delayed by the total latency, Removed = input − output,
//  Repair = output. 10 ms crossfade on mode changes.
struct ChainParams {
    DeclickParams   declick;     // its mode field is ignored (global mode)
    DecrackleParams decrackle;   // same
    AntiDistParams  dist;        // playback anti-distortion (after the repairs)
    Mode mode = Mode::Repair;
};

class Chain {
public:
    static constexpr int kMaxChannels = 8;
    bool prepare(double fs, int channels);
    void reset();
    int  latency() const { return dc_.latency() + cr_.latency() + ad_.latency(); }
    int  hopSize() const { return dc_.hopSize(); }
    int  channels() const { return nch_; }
    void setParams(const ChainParams& p);
    // repaired (optional): also receives the cleaned signal (Repair), whatever the
    // listening mode — used to record the cleaned sound.
    void process(const float* const* in, float* const* out, int n, float* const* repaired = nullptr);
    DeclickStats declickStats() const { return dc_.stats(); }
    DeclickStats decrackleStats() const { return cr_.stats(); }
    AntiDistState distState() const { return ad_.state(); }
    void distStartSide() { ad_.forceSideSec(0.0); }   // "the stylus was just dropped"
    // Second channel on a helper thread ("one core per channel"), nullptr = off. Same result.
    void setWorker(ChannelWorker* w) { dc_.setWorker(w); cr_.setWorker(w); }
private:
    static constexpr int kChunk = 256;
    Declicker dc_;
    Decrackler cr_;
    AntiDistortion ad_;
    int nch_ = 0;
    // Delay line of the "original" path (total latency).
    std::vector<float> delay_;
    int dlen_ = 0, dpos_ = 0;
    std::vector<float> a_, b_, raw_;  // kChunk × channels
    Mode cur_ = Mode::Repair, prev_ = Mode::Repair;
    int fadeLen_ = 480, fadePos_ = 0;
    ChainParams prm_;
};

} // namespace vc
