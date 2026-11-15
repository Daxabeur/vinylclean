// vinylclean — real-time declick (large clicks).
// Original work (clean-room) based on published methods:
//  - AR model + thresholding of the prediction error + LSAR interpolation
//    (Vaseghi & Rayner 1990; Godsill & Rayner 1998);
//  - "simple" AR detection, rated best by Özdoğru (CTU Prague, 2022);
//  - matched-filter detector (Vaseghi & Rayner): "two-sided" error that
//    peaks on the damaged samples; local dynamic threshold
//    (Rund et al., JAES 2021);
//  - click start = first strong FORWARD error, end = last strong BACKWARD
//    error (each one only "smears" to one side);
//  - protection of musical sounds: onset guard (Bello et al. 2005) and
//    rejection of periodic impulses (long-term / pitch prediction).
#pragma once

#include "vc_util.h"
#include "ar.h"
#include "lsar.h"
#include <vector>
#include <cstdint>

namespace vc {

struct DeclickParams {
    float intensity    = 5.f;    // 0 = off, 1..10 = more and more sensitive
    Mode  mode         = Mode::Repair;
    bool  pitchProtect = true;   // protects onsets and periodic sounds (false = protection 0)
    int   protect      = 1;      // 0 none (aggressive), 1 normal, 2 maximum (audiophile)
    float maxClickMs   = 2.0f;   // maximum length of a repaired click (0.2 .. 5 ms)
    float kOverride    = 0.f;    // > 0: forced threshold k (used by the decrackler)
    bool  multiband    = true;   // multiband repair (only the bands where the click dominates)
    bool  stereo       = true;   // use the other channel (detection and long clicks)
};

struct DeclickStats {
    uint64_t repaired      = 0;  // repaired clicks
    uint64_t protectedHits = 0;  // candidates rejected by pitch protection
    uint64_t tooLong       = 0;  // zones longer than the maximum length
    uint64_t overBudget    = 0;  // clicks skipped for lack of computing time
    uint64_t extended      = 0;  // enlarged repairs (click tail)
    uint64_t hfOnly        = 0;  // onsets softened in the treble only (protection none)
    uint64_t stereoOnset   = 0;  // twinless clicks repaired on an onset (stereo)
    uint64_t joint         = 0;  // long clicks interpolated with help from the other channel
};

// Detection threshold k versus intensity (multiple of the robust standard
// deviation of the prediction error). 0 => off.
float declickThreshold(float intensity);

// Single-channel processing. Fixed latency = latency() samples.
class DeclickChannel {
public:
    // crackle = true: "decrackle" profile (small, very short crackles:
    // repairs ≤ 0.5 ms, minimal widening, no high-order model).
    void prepare(double fs, bool crackle = false);  // allocates everything (outside the audio thread)
    void reset();                      // resets without allocating
    int  latency() const { return latency_; }
    int  hopSize() const { return H_; }

    // Processes n samples. outRaw = delayed input (for Bypass/Removed),
    // outRest = repaired signal, both aligned with the same latency.
    void process(const float* in, float* outRaw, float* outRest, int n,
                 const DeclickParams& prm);

    const DeclickStats& stats() const { return stats_; }

    // Internal settings (exposed for the tests).
    bool  twoPass      = true;
    float onsetRatio   = 2.5f;   // energy after / before for an onset
    float onsetGuard   = 1.6f;   // threshold multiplier on an onset
    float hugeScore    = 8.0f;   // above hugeScore × k: click even on an onset
    float bandK        = 1.0f;   // band pre-detector: threshold × k (0 = off)
    float ampK         = 3.0f;   // … and the burst must exceed 3 × the local RMS
    float comboK       = 1.3f;   // second detector (forward AND backward): threshold × comboK
    float extendK      = 0.0f;   // tail extension: threshold = extendK × k (0 = off)
    float crackleCap   = 0.06f;  // maximum amplitude removed by the decrackler (−24 dBFS)
    float softBeta     = 2.0f;   // soft repair: margin over the interpolation error
    float strongOnset  = 4.0f;   // sharp onset (+6 dB): long events are protected
    float pitchRhoMin  = 0.35f;  // minimum periodicity for protection
    float repRho       = 0.7f; 
    float repOne       = 0.94f;   // one-sided twin (start/end of a note): required similarity
      // minimum waveform similarity (1 period)
    float pitchVeto    = 2.0f;   // a real click exceeds the impulses by this factor
    float extentK      = 0.5f;   // (relative) threshold to locate start and end
    float frameFloor   = 1.0f;   // the local threshold does not drop below the frame threshold
    bool  onsetClean   = true;   // 2nd pass: onsets measured without the 1st-pass clicks
    float mbBeta       = 0.5f;
    // Stereo (original work, measured on a real record: 79 % of clicks touch
    // only one channel; a musical onset almost always has a "twin" in the
    // other channel, within ±1 ms).
    bool  stereo       = true;
    float stTwinLo     = 0.1f;   // below this ratio: no twin (single-channel click)
    float stTwinHi     = 0.5f;   // above: clear twin (probably musical)
    float stLoK        = 0.75f;  // threshold × stLoK without twin
    float stHiK        = 1.15f;  // threshold × stHiK with twin
    float stFloorK     = 6.0f;   // floor of the lowered threshold (k)
    float monoRho      = 0.95f;  // above this (L/R treble correlation): mono passage, stereo test off
    bool  stOnset      = false;  // protected onset but twinless click: repair the treble (tried: damages hard-panned instruments)
    float jointMinMs   = 0.6f;   // joint interpolation (help from the other channel) above this length

private:
    struct Gap { int s, e; bool hf = false; };  // hf: repair the treble only (> 3 kHz)
    struct Biquad2 {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        inline float tick(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };
    void bandCandidates(float k);
    bool standsOut(int s, int e);

    void hop(const DeclickParams& prm);
public:
    // Phases of a hop, called in this order for ALL channels at once
    // (Declicker): the other channel is used for the stereo test and interpolation.
    void hopAnalyze(const DeclickParams& prm);
    void hopDetect(const DeclickParams& prm, const DeclickChannel* other);
    void hopRepair(const DeclickParams& prm, const DeclickChannel* other);
    void hopOutput();
    // Fills the buffer; returns true if a hop is due (the 4 phases and
    // finishHop must then be called).
    bool push(const float* in, float* outRaw, float* outRest, int m);
    void finishHop(float* outRaw, float* outRest);
    int  fill() const { return inFill_; }
private:
    float twinRatio(int n0) const;   // energy of the "twin" in the other channel / its own
    bool  jointInterp(int gs, int L, const double* cc, int order, const DeclickChannel* o);
    bool multibandGate(int gs, int ge, bool hfOnly);   // false: nothing to remove (original restored)
    void zeroPhaseLP(int b, float* x, int n);
    void estimateAR(const float* x);
    int  estimateHighOrder(const float* x, int center, int order);
    void computeErrors(const float* x);
    void detect(float k, const DeclickParams& prm);
    float robustSigma(const float* e);
    bool isOnset(int s, int e, float ratio);
    float sideSigma(int a, int b);
    void computePeriodicity();
    bool isPitchPulse(int n0, int cs, int ce);
    bool isRepeating(int n0, int cs, int ce);

    // Sizes (depend on the sample rate).
    double fs_ = 48000.0;
    int p_ = 24, N_ = 1024, H_ = 512, B_ = 0, C0_ = 0, i0_ = 0;
    int lmaxMax_ = 240, wPre_ = 2, wPost_ = 4, gMerge_ = 6, gBridge_ = 24, slack_ = 32;
    int g1_ = 48, W_ = 384, eS_ = 0, eE_ = 0, dS_ = 0, dE_ = 0;
    int decim_ = 4, tMin_ = 48, tMax_ = 576, repW_ = 36, repMin_ = 24, onsetGap_ = 48;
    int latency_ = 0;
    int maxGaps_ = 48;
    int mergeDist_ = 24;  // two gaps closer than this are merged
    bool softRepair_ = false;
    float extentRel_ = 0.05f;  // threshold relative to the peak to locate start and end
    std::vector<float> keep_;  // original samples of a gap (soft repair)

    // Buffers (allocated in prepare).
    std::vector<float> raw_, rest_, scratch_, win_, xw_, ef_, eb_, md_, absbuf_;
    std::vector<float> outQRaw_, outQRest_, env_;
    std::vector<double> r_, aD_, tmp_, c_, psum_;
    std::vector<double> rH_, aH_, tmpH_, cH_;  // high-order model (long clicks)
    int pHiMax_ = 96, longGap_ = 12, maxLongPerHop_ = 3;
    std::vector<float> aF_, aRevF_, cF_;
    std::vector<Gap> gaps_, gaps1_, rej_;  // kept gaps, 1st pass, protected rejections
    std::vector<float> locSig_;  // local robust standard deviation, per block
    int blk_ = 64, locHalf_ = 192;
    LsarInterpolator lsar_;

    int inFill_ = 0, rp_ = 1;
    int doneUntil_ = 0;  // end of the last zone already processed (buffer coordinates)
    int lmaxCur_ = 96;
    float sig_ = 0.f, sigF_ = 0.f, sigB_ = 0.f, kCur_ = 7.f, repRhoCur_ = 0.9f;
    int protCur_ = 1;
    bool crackle_ = false;
    bool d1Ready_ = false;
    std::vector<float> d1_, repL_, repR_, bandAbs_, bandBg_;
    std::vector<Gap> bandGaps_;
    // Multiband: 3 low-pass filters (1, 3, 7 kHz) run forward-backward (zero phase)
    // split the correction and the signal into 4 complementary bands.
    static constexpr int kMbCross = 3;
    Biquad2 mbLP_[kMbCross];
    int softMax_ = 34;
    const float* onsetSrc_ = nullptr;
    // Stereo
    float sigA_ = 1.f;
    std::vector<double> e2p_;           // cumulative normalized error² (1st pass)
    const DeclickChannel* twin_ = nullptr;
    int twinHw_ = 6, twinLag_ = 48, jointMin_ = 29, xK_ = 8, xTrain_ = 768;
    std::vector<double> xA_, xb_;       // normal equations of the cross predictor
    std::vector<float> xPrior_, xWx_, xWr_;
    int mbM_ = 120, mbNb_ = 72, mbWin_ = 0;
    bool mbOn_ = true;
    std::vector<float> mbX_[kMbCross + 1], mbC_[kMbCross + 1];
    Biquad2 bandF_[2];
    bool energyReady_ = false, periodReady_ = false, periodic_ = false;
    int  period_ = 0;
    DeclickStats stats_, pending_;
};

// Multichannel declicker with the three listening modes and a 10 ms crossfade
// on mode changes (no click when switching).
class Declicker {
public:
    static constexpr int kMaxChannels = 8;

    bool prepare(double fs, int channels, bool crackle = false);
    void reset();
    int  latency() const { return ch_.empty() ? 0 : ch_[0].latency(); }
    // Internal "hop" size: using an audio period that is a multiple of this
    // value spreads the load evenly (no peaks).
    int  hopSize() const { return ch_.empty() ? 0 : ch_[0].hopSize(); }
    int  channels() const { return static_cast<int>(ch_.size()); }

    void setParams(const DeclickParams& p) { prm_ = p; }
    const DeclickParams& params() const { return prm_; }

    // in[c][i], out[c][i]. Any block size. No allocation.
    // in and out may point to the same memory.
    void process(const float* const* in, float* const* out, int n);

    DeclickStats stats() const;
    DeclickChannel& channel(int c) { return ch_[c]; }

private:
    static constexpr int kChunk = 256;
    std::vector<DeclickChannel> ch_;
    std::vector<float> raw_, rest_;   // kChunk × channels
    DeclickParams prm_;
    Mode curMode_ = Mode::Repair, prevMode_ = Mode::Repair;
    int fadeLen_ = 480, fadePos_ = 0;
};

} // namespace vc
