// vinylclean — real-time declick. Original work (clean-room).
//
// Layout of one channel (linear buffer of B samples, shifted by H at every
// "hop"):
//
//   [ left context | core (H) | right context ]
//     already repaired  analyzed   upcoming (raw)
//
// At each hop: AR model estimation on a window centered on the core,
// forward/backward prediction errors, detection of the clicks that START in
// the core, musical protection, then LSAR interpolation. The H samples that
// can no longer change are sent to the output. The latency is therefore fixed.
#include "declick.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr float kSigmaMin  = 3e-6f;   // ≈ −110 dBFS: floor of the standard deviation
constexpr float kThreshAbs = 2e-5f;   // ≈ −94 dBFS: below this, inaudible
constexpr double kPi = 3.14159265358979323846;
}

float declickThreshold(float intensity) {
    if (!(intensity > 0.f)) return 0.f;
    const float I = std::min(intensity, 10.f);
    // k(1) = 30, k(10) = 4, geometric progression (≈ ×0.80 per step).
    // Scale widened in 0.6 to cover everything from "very light" (k ≈ 22)
    // to "very strong" (k ≈ 5.5); see presets.h.
    // Below k ≈ 4 the noise of the music itself starts being taken for
    // clicks.
    return 30.f * std::pow(4.f / 30.f, (I - 1.f) / 9.f);
}

// ---------------------------------------------------------------------------
// DeclickChannel
// ---------------------------------------------------------------------------

void DeclickChannel::prepare(double fs, bool crackle) {
    fs_ = fs;
    const double r = fs / 48000.0;
    p_ = (fs > 60000.0) ? 32 : 24;
    H_ = std::max(64, roundi(512.0 * r));
    N_ = 2 * H_;
    lmaxMax_ = std::max(8, roundi((crackle ? 0.5e-3 : 5e-3) * fs));
    wPre_  = crackle ? 1 : std::max(1, roundi(2.0 * r));
    wPost_ = crackle ? std::max(1, roundi(1.0 * r)) : std::max(2, roundi(4.0 * r));
    gMerge_ = std::max(3, roundi(6.0 * r));
    gBridge_ = std::max(gMerge_, roundi(0.5e-3 * fs));   // bridge inside a long click
    slack_ = gBridge_ + 8;  // a click straddling two hops is never lost
    g1_ = std::max(8, roundi(1e-3 * fs));
    W_  = std::max(32, roundi(8e-3 * fs));
    decim_ = std::max(2, roundi(4.0 * r));
    tMin_ = roundi(0.4e-3 * fs);   // up to 2.5 kHz (high notes)
    tMax_ = roundi(12e-3 * fs);
    repW_ = std::max(4, roundi(4.0 * r));
    repMin_ = std::max(16, roundi(24.0 * r));
    onsetGap_ = std::max(4, roundi(6.0 * r));
    crackle_ = crackle;
    // High-order model for long clicks: order ≤ 96 (2 ms at 48 kHz),
    // and at most 2 (≤ 50 kHz) or 1 (> 50 kHz) per hop: bounded cost.
    pHiMax_ = crackle ? p_ : std::max(p_, 96);
    maxLongPerHop_ = crackle ? 0 : ((fs > 50000.0) ? 1 : 2);
    if (crackle) {
        // A crackle is tiny: threshold barely raised on onsets, no "long
        // event on an onset" rule (it cannot be long).
        onsetGuard = 1.3f;
        strongOnset = 1e9f;
        extentK = 1.0f;       // tight extent: a crackle is a few samples long
        extentRel_ = 0.25f;
        maxGaps_ = 96;
    }
    // Crackle profile: two neighboring crackles are not merged (that would
    // re-invent the healthy music between them).
    mergeDist_ = crackle ? 3 : p_;
    softRepair_ = crackle;
    softMax_ = lmaxMax_ + wPre_ + wPost_ + 8;
    keep_.assign(softMax_, 0.f);
    longGap_ = std::max(8, roundi(12.0 * r));
    blk_ = std::max(16, roundi(64.0 * r));
    locHalf_ = roundi(4e-3 * fs);  // 8 ms local window

    const int halfPad  = (N_ - H_) / 2;
    const int detAfter = lmaxMax_ + wPost_ + 2 * gMerge_;
    const int lctx = std::max(halfPad, slack_ + wPre_ + pHiMax_) + std::max(p_, g1_ + W_) + 8;
    const int afterCore = std::max(halfPad, detAfter + 1);
    const int R = afterCore + std::max(pHiMax_, g1_ + W_) + wPost_ + 8;

    // Multiband: the correction of a gap, split into bands, spills over by
    // mbM_ (2.5 ms) on each side; the neighboring music (mbNb_ ≤ 1.5 ms)
    // is the reference. The output is delayed by mbM_ (latency + 2.5 ms).
    mbM_ = roundi(2.5e-3 * fs);
    twinHw_ = std::max(3, roundi(6.0 * r));
    twinLag_ = std::max(8, roundi(1e-3 * fs));
    jointMin_ = std::max(8, roundi(jointMinMs * 1e-3 * fs));
    xTrain_ = roundi(16e-3 * fs);
    mbNb_ = std::max(32, roundi(1.5e-3 * fs));
    C0_ = lctx;
    B_  = lctx + H_ + R;
    i0_ = C0_ - slack_ - wPre_ - mbM_;
    latency_ = B_ - 1 - i0_;
    eS_ = C0_ - halfPad;
    eE_ = C0_ + H_ + afterCore;
    dS_ = C0_ - slack_ - gBridge_;
    dE_ = C0_ + H_ + detAfter;

    raw_.assign(B_, 0.f);
    rest_.assign(B_, 0.f);
    scratch_.assign(B_, 0.f);
    ef_.assign(B_, 0.f);
    md_.assign(B_, 0.f);
    eb_.assign(B_, 0.f);
    win_.resize(N_);
    for (int i = 0; i < N_; ++i)
        win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / N_));
    xw_.assign(N_, 0.f);
    absbuf_.assign(std::max(N_, 2 * roundi(4e-3 * fs) + 2), 0.f);
    outQRaw_.assign(H_, 0.f);
    outQRest_.assign(H_, 0.f);
    env_.assign(B_ / decim_ + 2, 0.f);
    r_.assign(p_ + 1, 0.0);
    aD_.assign(p_, 0.0);
    tmp_.assign(p_, 0.0);
    c_.assign(p_ + 1, 0.0);
    aF_.assign(p_, 0.f);
    aRevF_.assign(p_, 0.f);
    cF_.assign(p_ + 1, 0.f);
    psum_.assign(B_ + 1, 0.0);
    bandAbs_.assign(B_, 0.f);
    bandBg_.assign((dE_ - dS_) / blk_ + 2, 0.f);
    bandGaps_.clear(); bandGaps_.reserve(128);
    {
        // Band-pass filters of the pre-detector (RBJ biquads, Q ≈ 0.7).
        const double bands[2][2] = {{1000.0, 4000.0}, {4000.0, std::min(12000.0, 0.45 * fs)}};
        for (int b = 0; b < 2; ++b) {
            const double f0 = std::sqrt(bands[b][0] * bands[b][1]), bw = bands[b][1] / bands[b][0];
            const double Q = std::sqrt(bw) / (bw - 1.0), w0 = 2.0 * 3.14159265358979323846 * f0 / fs, al = std::sin(w0) / (2.0 * Q);
            const double a0 = 1.0 + al;
            bandF_[b].b0 = float(al / a0); bandF_[b].b1 = 0.f; bandF_[b].b2 = float(-al / a0);
            bandF_[b].a1 = float(-2.0 * std::cos(w0) / a0); bandF_[b].a2 = float((1.0 - al) / a0);
        }
    }
    {
        const double fc[kMbCross] = {1000.0, 3000.0, 7000.0};
        for (int b = 0; b < kMbCross; ++b) {
            // 2nd-order Butterworth (RBJ, Q = 1/√2); forward-backward = 4th order, zero phase.
            const double w0 = 2.0 * kPi * std::min(fc[b], 0.45 * fs) / fs, al = std::sin(w0) / (2.0 * std::sqrt(0.5)), cw = std::cos(w0);
            const double a0 = 1.0 + al;
            mbLP_[b].b0 = float((1.0 - cw) / 2.0 / a0); mbLP_[b].b1 = float((1.0 - cw) / a0); mbLP_[b].b2 = mbLP_[b].b0;
            mbLP_[b].a1 = float(-2.0 * cw / a0); mbLP_[b].a2 = float((1.0 - al) / a0);
        }
        const int gmax = lmaxMax_ + wPre_ + wPost_ + 2 * gBridge_ + 16;
        mbWin_ = gmax + 2 * (mbM_ + 24 + mbNb_) + 8;
        for (int b = 0; b <= kMbCross; ++b) { mbX_[b].assign(mbWin_, 0.f); mbC_[b].assign(mbWin_, 0.f); }
        keep_.assign(std::max<size_t>(keep_.size(), gmax + 8), 0.f);
    }
    e2p_.assign(B_ + 1, 0.0);
    xA_.assign((2 * xK_ + 1) * (2 * xK_ + 1), 0.0);
    xb_.assign(2 * xK_ + 1, 0.0);
    xPrior_.assign(keep_.size(), 0.f);
    xWx_.assign(2 * xTrain_ + keep_.size() + 16, 0.f);
    xWr_.assign(xWx_.size(), 0.f);
    d1_.assign(B_, 0.f);
    repL_.assign(tMax_ + 2, 0.f);
    repR_.assign(tMax_ + 2, 0.f);
    gaps_.clear();
    gaps_.reserve(maxGaps_ * 4);
    gaps1_.reserve(maxGaps_ * 4);
    rej_.reserve(maxGaps_ * 4);
    locSig_.assign((dE_ - dS_) / blk_ + 2, 0.f);
    rH_.assign(pHiMax_ + 1, 0.0);
    aH_.assign(pHiMax_, 0.0);
    tmpH_.assign(pHiMax_, 0.0);
    cH_.assign(pHiMax_ + 1, 0.0);
    lsar_.prepare(lmaxMax_ + wPre_ + wPost_ + 4, pHiMax_);
    reset();
}

void DeclickChannel::reset() {
    std::fill(raw_.begin(), raw_.end(), 0.f);
    std::fill(rest_.begin(), rest_.end(), 0.f);
    std::fill(outQRaw_.begin(), outQRaw_.end(), 0.f);
    std::fill(outQRest_.begin(), outQRest_.end(), 0.f);
    inFill_ = 0;
    rp_ = 1;  // H-1 pending zeros: the latency is B-1-i0 right from the start
    doneUntil_ = 0;
    stats_ = DeclickStats{};
}

bool DeclickChannel::push(const float* in, float* outRaw, float* outRest, int m) {
    const int base = B_ - H_ + inFill_;
    std::memcpy(&raw_[base], in, sizeof(float) * m);
    std::memcpy(&rest_[base], in, sizeof(float) * m);
    const bool doHop = (inFill_ + m == H_);
    const int fromOld = doHop ? m - 1 : m;
    std::memcpy(outRaw, &outQRaw_[rp_], sizeof(float) * fromOld);
    std::memcpy(outRest, &outQRest_[rp_], sizeof(float) * fromOld);
    rp_ += fromOld;
    inFill_ += m;
    return doHop;
}

void DeclickChannel::finishHop(float* outRaw, float* outRest) {
    inFill_ = 0;
    *outRaw = outQRaw_[0];
    *outRest = outQRest_[0];
    rp_ = 1;
}

void DeclickChannel::process(const float* in, float* outRaw, float* outRest, int n,
                             const DeclickParams& prm) {
    int done = 0;
    while (done < n) {
        const int m = std::min(n - done, H_ - inFill_);
        if (push(in + done, outRaw + done, outRest + done, m)) {
            hop(prm);
            finishHop(outRaw + done + m - 1, outRest + done + m - 1);
        }
        done += m;
    }
}

void DeclickChannel::estimateAR(const float* x) {
    for (int i = 0; i < N_; ++i) xw_[i] = x[eS_ + i] * win_[i];
    autocorrelation(xw_.data(), N_, p_, r_.data());
    levinson(r_.data(), p_, aD_.data(), tmp_.data());
    c_[0] = 1.0;
    cF_[0] = 1.f;
    for (int k = 0; k < p_; ++k) {
        aF_[k] = static_cast<float>(aD_[k]);
        aRevF_[p_ - 1 - k] = aF_[k];
        c_[k + 1] = -aD_[k];
        cF_[k + 1] = -aF_[k];
    }
}

// ef: forward prediction error (one-sided).
// md: matched filter (Vaseghi & Rayner) = ef filtered by the time-reversed
//      error filter. Up to a factor, it is the "two-sided" interpolation
//      error: it peaks ON the damaged samples instead of spreading after
//      them.
// High-order model to rebuild a long click (Janssen et al. 1986; Godsill &
// Rayner: the order must grow with the gap length). Window of N samples
// centered on the click. Returns the order used.
int DeclickChannel::estimateHighOrder(const float* x, int center, int order) {
    int a0 = std::clamp(center - N_ / 2, 0, B_ - N_);
    for (int i = 0; i < N_; ++i) xw_[i] = x[a0 + i] * win_[i];
    autocorrelation(xw_.data(), N_, order, rH_.data());
    levinson(rH_.data(), order, aH_.data(), tmpH_.data());
    cH_[0] = 1.0;
    for (int k = 0; k < order; ++k) cH_[k + 1] = -aH_[k];
    return order;
}

void DeclickChannel::computeErrors(const float* x) {
    const float* ar = aRevF_.data();
    for (int n = eS_; n < eE_ + p_; ++n) ef_[n] = x[n] - dot(ar, x + n - p_, p_);
    const float* cf = cF_.data();
    const float* af = aF_.data();
    for (int n = eS_; n < eE_; ++n) {
        md_[n] = dot(cf, &ef_[n], p_ + 1);
        eb_[n] = x[n] - dot(af, x + n + 1, p_);
    }
}

float DeclickChannel::robustSigma(const float* e) {
    for (int i = 0; i < N_; ++i) absbuf_[i] = std::fabs(e[eS_ + i]);
    const int mid = N_ / 2;
    std::nth_element(absbuf_.begin(), absbuf_.begin() + mid, absbuf_.end());
    const float med = absbuf_[mid];
    const float s = 1.4826f * med;
    return (s > kSigmaMin) ? s : kSigmaMin;  // NaN → floor
}

// Robust standard deviation of md over [a, b) (clamped to the computed zone). 0 if too short.
float DeclickChannel::sideSigma(int a, int b) {
    a = std::max(a, eS_);
    b = std::min(b, eE_);
    const int L = b - a;
    if (L < 32) return 0.f;
    for (int i = 0; i < L; ++i) absbuf_[i] = std::fabs(md_[a + i]);
    std::nth_element(absbuf_.begin(), absbuf_.begin() + L / 2, absbuf_.begin() + L);
    return 1.4826f * absbuf_[L / 2];
}

bool DeclickChannel::isOnset(int s, int e, float ratio) {
    if (!energyReady_) {
        psum_[0] = 0.0;
        psum_[1] = 0.0;
        for (int i = 1; i < B_; ++i) {
            const double d = static_cast<double>(onsetSrc_[i]) - onsetSrc_[i - 1];
            psum_[i + 1] = psum_[i] + d * d;
        }
        energyReady_ = true;
    }
    // Short gap (onsetGap_): a drum hit starts right after the event;
    // a 1 ms gap missed it.
    int be = s - onsetGap_, bs = be - W_;
    int as = e + onsetGap_, ae = as + W_;
    bs = std::max(bs, 1);
    ae = std::min(ae, B_);
    if (be - bs < 8 || ae - as < 8) return false;
    const double before = (psum_[be] - psum_[bs]) / (be - bs);
    const double after  = (psum_[ae] - psum_[as]) / (ae - as);
    return after > ratio * before && after > 1e-12;
}

void DeclickChannel::computePeriodicity() {
    periodReady_ = true;
    periodic_ = false;
    period_ = 0;
    const int M = (eE_ - eS_) / decim_;
    if (M < 16) return;
    double mean = 0.0;
    for (int m = 0; m < M; ++m) {
        float s = 0.f;
        const float* e = &md_[eS_ + m * decim_];
        for (int j = 0; j < decim_; ++j) s += std::fabs(e[j]);
        env_[m] = s;
        mean += s;
    }
    mean /= M;
    // Clips the large impulses (the clicks) so that they do not hide the
    // periodicity of the music.
    const float clip = static_cast<float>(4.0 * mean);
    mean = 0.0;
    for (int m = 0; m < M; ++m) { env_[m] = std::min(env_[m], clip); mean += env_[m]; }
    mean /= M;
    for (int m = 0; m < M; ++m) env_[m] -= static_cast<float>(mean);

    const int mMin = std::max(2, tMin_ / decim_);
    const int mMax = std::min(tMax_ / decim_, M / 2);
    if (mMax <= mMin + 2) return;
    float* rho = absbuf_.data();  // reuse (large enough: mMax < N)
    float rhoMax = 0.f;
    for (int lag = mMin; lag <= mMax; ++lag) {
        const int L = M - lag;
        const float* a = env_.data();
        const float* b = env_.data() + lag;
        const float xy = dot(a, b, L), xx = dot(a, a, L), yy = dot(b, b, L);
        const float den = std::sqrt(xx * yy);
        const float v = (den > 1e-20f) ? xy / den : 0.f;
        rho[lag] = v;
        rhoMax = std::max(rhoMax, v);
    }
    if (!(rhoMax >= pitchRhoMin)) return;
    // First local maximum close to the global maximum (avoids octave errors).
    int best = mMin;
    for (int lag = mMin + 1; lag < mMax; ++lag) {
        if (rho[lag] >= 0.9f * rhoMax && rho[lag] >= rho[lag - 1] && rho[lag] >= rho[lag + 1]) {
            best = lag;
            break;
        }
    }
    periodic_ = true;
    period_ = best * decim_;
}

// Protection of periodic sounds (synth, brass, voice…), LOCAL test: the exact
// period around the event is found by waveform correlation, then we check
// that the impulse has "twins" one period before AND one period after. A
// vinyl click is unique; a musical impulse repeats identically.
bool DeclickChannel::isRepeating(int n0, int cs, int ce) {
    if (n0 < 0) return false;
    // Comparison on the DERIVATIVE of the ORIGINAL signal (raw_): the derivative
    // brings out the clicks (rich in treble) relative to the music, and the
    // original signal prevents a previous repair from erasing the twin.
    if (!d1Ready_) {
        for (int i = std::max(2, eS_ - tMax_); i < B_; ++i) d1_[i] = raw_[i] - 2.f * raw_[i - 1] + raw_[i - 2];
        d1Ready_ = true;
    }
    const int W = repW_;
    // Window of at least ~1 ms: on a window that is too short, a similarity
    // can happen by chance (background noise).
    const int a0 = std::min(cs - W, n0 - repMin_), a1 = std::max(ce + W, n0 + repMin_);
    const int Lw = a1 - a0;
    const float* A = &d1_[a0];
    const float e0 = dot(A, A, Lw);
    if (!(e0 > 1e-16f)) return false;
    // Similarity for each possible period, before and after. A real musical
    // repetition is found at the SAME period on both sides; a chance
    // similarity is not.
    const int T0 = tMin_;  // (the shifted window may overlap the event: a click does not resemble itself shifted)
    if (T0 > tMax_) return false;
    for (int si = 0; si < 2; ++si) {
        const int side = si ? 1 : -1;
        float* R = si ? repR_.data() : repL_.data();
        for (int T = T0; T <= tMax_; ++T) {
            const int b0 = a0 + side * T;
            if (b0 < std::max(2, eS_ - tMax_) || b0 + Lw >= B_) { R[T] = -1.f; continue; }
            const float* Bp = &d1_[b0];
            const float xy = dot(A, Bp, Lw);
            R[T] = (xy > 0.f) ? xy * xy / (e0 * dot(Bp, Bp, Lw) + 1e-30f) : 0.f;
        }
    }
    const float thr = repRhoCur_ * repRhoCur_;
    float bestL = 0.f, bestR = 0.f;
    for (int T = T0; T <= tMax_; ++T) {
        bestL = std::max(bestL, repL_[T]);
        bestR = std::max(bestR, repR_[T]);
        if (repL_[T] < thr) continue;
        for (int u = std::max(T0, T - 1); u <= std::min(tMax_, T + 1); ++u)
            if (repR_[u] >= thr) return true;
    }
    // Start or end of a note: the twin only exists on one side. It is accepted
    // if the similarity is very strong (chance cannot produce it over a window
    // of at least 1 ms).
    // (not for the decrackler: a tiny crackle on a held note could pass for a
    // "twin"; the two-sided rule is kept)
    if (crackle_) return false;
    const float one = repOne * repOne;
    return bestL >= one || bestR >= one;
}

bool DeclickChannel::isPitchPulse(int n0, int cs, int ce) {
    // A "musical" impulse (brass, voice…) has similar neighbors one period
    // before AND one period after. A click is isolated.
    if (n0 < 0) return false;
    const int tol = decim_ + 1;
    if (period_ <= (ce - cs) + tol) return false;  // period shorter than the event: no verdict
    // We look at the neighbors at ±1 and ±2 periods: a real musical impulse
    // repeats; at least two of them must be available.
    const float v = std::fabs(md_[n0]);
    float P = -1.f;
    int nSeen = 0;
    for (int mult = -2; mult <= 2; ++mult) {
        if (mult == 0) continue;
        const int c = n0 + mult * period_;
        if (c - tol < eS_ || c + tol >= eE_) continue;  // outside the window
        float m = 0.f;
        for (int n = c - tol; n <= c + tol; ++n)
            if (n < cs || n >= ce) m = std::max(m, std::fabs(md_[n]));
        P = (P < 0.f) ? m : std::min(P, m);
        ++nSeen;
    }
    return nSeen >= 2 && P > 0.f && v < pitchVeto * P;
}

// Band pre-detector (1–4 kHz and 4–12 kHz). A large vinyl click produces a
// very brief burst of energy in these bands, well above the local floor.
// These zones are NOT repaired directly: they are only excluded from the
// estimation of the music model (2nd pass), because a large click
// "contaminates" the AR model, which ends up predicting it (known limit of
// AR methods, Godsill & Rayner). The final decision remains that of the main
// detector, with all its protections.
// Does the burst clearly exceed the level of the surrounding music (peak
// ≥ ampK × local RMS over ± 8 ms)? A large click does; a drum hit is part of
// the music and stays at its level.
bool DeclickChannel::standsOut(int s, int e) {
    float pk = 0.f;
    for (int i = s; i < e; ++i) pk = std::max(pk, std::fabs(raw_[i]));
    const int a0 = std::max(0, s - W_), a1 = std::min(B_, e + W_);
    double sum = 0; int n = 0;
    for (int i = a0; i < a1; ++i) if (i < s || i >= e) { sum += double(raw_[i]) * raw_[i]; ++n; }
    const float rms = n ? float(std::sqrt(sum / n)) : 0.f;
    return pk > ampK * rms;
}

void DeclickChannel::bandCandidates(float k) {
    bandGaps_.clear();
    if (!(bandK > 0.f)) return;
    const int a0 = std::max(2, eS_ - 64), a1 = eE_;
    for (int bnd = 0; bnd < 2; ++bnd) {
        Biquad2 f = bandF_[bnd];
        f.z1 = f.z2 = 0.f;
        float* h = bandAbs_.data();
        for (int i = a0; i < a1; ++i) h[i] = std::fabs(f.tick(raw_[i]));
        // Local floor: (subsampled) median of |h| over ± 4 ms, per block.
        const int nBlk = (dE_ - dS_ + blk_ - 1) / blk_;
        for (int b = 0; b < nBlk; ++b) {
            const int c = dS_ + b * blk_ + blk_ / 2;
            const int w0 = std::max(eS_, c - locHalf_), w1 = std::min(eE_, c + locHalf_);
            int m = 0;
            for (int i = w0; i < w1; i += 4) absbuf_[m++] = h[i];
            std::nth_element(absbuf_.begin(), absbuf_.begin() + m / 2, absbuf_.begin() + m);
            bandBg_[b] = std::max(1.4826f * absbuf_[m / 2], kSigmaMin);
        }
        const float kb = bandK * k;
        int cs = -1, lf = -1;
        for (int n = dS_; n <= dE_; ++n) {
            const bool on = n < dE_ && h[n] > kb * bandBg_[(n - dS_) / blk_];
            if (on) {
                if (cs >= 0 && n - lf > gMerge_) {
                    if (lf + 1 - cs <= lmaxCur_ && bandGaps_.size() < bandGaps_.capacity() && !isOnset(cs, lf + 1, onsetRatio) && standsOut(cs, lf + 1))
                        bandGaps_.push_back({std::max(dS_, cs - 8), std::min(dE_, lf + 9)});
                    cs = -1;
                }
                if (cs < 0) cs = n;
                lf = n;
            } else if (cs >= 0 && (n == dE_ || n - lf > gMerge_)) {
                if (lf + 1 - cs <= lmaxCur_ && bandGaps_.size() < bandGaps_.capacity() && !isOnset(cs, lf + 1, onsetRatio) && standsOut(cs, lf + 1))
                    bandGaps_.push_back({std::max(dS_, cs - 8), std::min(dE_, lf + 9)});
                cs = -1;
            }
        }
    }
}

void DeclickChannel::detect(float k, const DeclickParams& prm) {
    gaps_.clear();
    sig_ = robustSigma(md_.data());
    sigF_ = robustSigma(ef_.data());
    sigB_ = robustSigma(eb_.data());
    const float thF = std::max(extentK * k * sigF_, kThreshAbs);
    const float thB = std::max(extentK * k * sigB_, kThreshAbs);
    // Dynamic threshold: local robust standard deviation (8 ms window), never
    // below frameFloor × that of the frame. In a dense or noisy passage
    // (cymbal, onset), the threshold rises locally; a click does not raise
    // the median.
    const int nBlk = (dE_ - dS_ + blk_ - 1) / blk_;
    for (int b = 0; b < nBlk; ++b) {
        const int c = dS_ + b * blk_ + blk_ / 2;
        const int a0 = std::max(eS_, c - locHalf_), a1 = std::min(eE_, c + locHalf_);
        const int L = a1 - a0;
        for (int i = 0; i < L; ++i) absbuf_[i] = std::fabs(md_[a0 + i]);
        std::nth_element(absbuf_.begin(), absbuf_.begin() + L / 2, absbuf_.begin() + L);
        const float sl = 1.4826f * absbuf_[L / 2];
        locSig_[b] = std::max(sl, frameFloor * sig_);
        if (!(locSig_[b] > kSigmaMin)) locSig_[b] = kSigmaMin;
    }
    const int allowed = lmaxCur_ + wPre_ + wPost_;
    DeclickStats local;

    // A group of flagged samples (small holes filled) = one click.
    rej_.clear();
    auto reject = [&](int a, int b) { if (rej_.size() < rej_.capacity()) rej_.push_back({a, b}); };
    // Music protection level: 0 = none (aggressive),
    // 1 = normal, 2 = maximum (audiophile).
    const int prot = std::clamp(prm.protect, 0, 2);
    const float og = (prot >= 2) ? 3.0f : onsetGuard;     // threshold × og on an onset
    const float so = (prot >= 2) ? 2.0f : strongOnset;    // "sharp onset"
    const float hs = (prot >= 2) ? 30.f : hugeScore;      // obvious click even on an onset
    repRhoCur_ = (prot >= 2) ? 0.6f : (prot == 1) ? repRho : 0.85f;
    auto handleCluster = [&](int cs, int ce, float score, int n0) {
        bool hfGap = false;
        // The group [cs, ce) contains the click and a bit of "smear" on each
        // side. Start = first strong forward error (it only smears after the
        // click); end = last strong backward error (it only smears before).
        // Fall back on the peak if nothing is found.
        // Thresholds relative to the click itself: the neighboring music must
        // not widen the zone.
        float pkF = 0.f, pkB = 0.f;
        for (int n = cs; n < ce; ++n) { pkF = std::max(pkF, std::fabs(ef_[n])); pkB = std::max(pkB, std::fabs(eb_[n])); }
        const float tF = std::max(thF, extentRel_ * pkF), tB = std::max(thB, extentRel_ * pkB);
        int s = n0, e = n0 + 1;
        for (int n = std::max(cs, n0 - lmaxCur_); n <= n0; ++n) if (std::fabs(ef_[n]) > tF) { s = n; break; }
        for (int n = std::min(ce, n0 + lmaxCur_) - 1; n >= n0; --n) if (std::fabs(eb_[n]) > tB) { e = n + 1; break; }
        // Zones that start in the core (with a margin on the left) and were
        // not already handled at the previous hop.
        if (s < C0_ - slack_ || s >= C0_ + H_ || cs < doneUntil_) return;
        doneUntil_ = std::max(doneUntil_, ce);
        if (e - s > lmaxCur_ || ce >= dE_) { local.tooLong++; return; }
        // Isolation test: a click is an isolated event; before AND after it,
        // the music returns to its level. The start of a noisy sound (cymbal,
        // plucked attack) leaves a high residual instead.
        const float side = std::max(sideSigma(cs - g1_ - W_, cs - g1_), sideSigma(ce + g1_, ce + g1_ + W_));
        // (score in local units: also valid for the second detector)
        const float slN0 = locSig_[std::clamp((n0 - dS_) / blk_, 0, static_cast<int>(locSig_.size()) - 1)];
        if (side > 0.f && score * slN0 < k * side) return;
        if (prot == 0) {
            // Aggressive mode: no more onset protection, except that a whole
            // drum attack is not smoothed; the (strict) repetition test is
            // kept: repairing a periodic synth sound removes no click, it only
            // damages the music.
            if (prm.pitchProtect && isRepeating(n0, cs, ce)) { local.protectedHits++; reject(cs, ce); return; }
            // Long event on a sharp onset: it may be the hit itself. It is not
            // smoothed; only its treble (> 3 kHz) is softened, where a click
            // sitting on the onset is audible.
            if (e - s > 2 * longGap_ && score < hugeScore * k && isOnset(s, e, strongOnset)) {
                if (crackle_) { local.protectedHits++; reject(cs, ce); return; }
                hfGap = true;
            }
        }
        if (prot > 0) {
            // Stereo: a click with no "twin" in the other channel is safer,
            // an event with a twin is more suspect (musical).
            const float tw = crackle_ ? -1.f : twinRatio(n0);
            float sf = (tw < 0.f) ? 1.f : (tw < stTwinLo) ? stLoK : (tw > stTwinHi) ? stHiK : 1.f;
            // Never below stFloorK: lower, the noise of the music passes for clicks.
            if (sf < 1.f) sf = std::max(sf, std::min(1.f, stFloorK / k));
            const bool lone = stOnset && prot == 1 && tw >= 0.f && tw < stTwinLo && score >= k * sf;
            const bool onset = isOnset(s, e, onsetRatio);
            // An instrument panned to one side has no twin either: on an
            // onset, the threshold is never lowered.
            if ((onset || prot >= 2) && sf < 1.f) sf = 1.f;   // nor at maximum protection
            float req = k * sf;
            if (onset) {
                // On an onset: raised threshold. On a sharp onset, a long event
                // is part of the attack (pluck, drum hit). At maximum
                // protection, the decrackler never touches an onset (a crackle
                // is masked there anyway).
                if (crackle_ && prot >= 2) { local.protectedHits++; reject(cs, ce); return; }
                req *= og;
                if (e - s > longGap_ && score < hs * k && isOnset(s, e, so)) {
                    // … except, at normal protection, a single-channel click:
                    // the onset is in both. Only its treble is softened.
                    if (!lone) { local.protectedHits++; reject(cs, ce); return; }
                    hfGap = true; local.stereoOnset++;
                }
            }
            if (!hfGap && score < req) {
                if (!(onset && lone)) { local.protectedHits++; reject(cs, ce); return; }
                hfGap = true; local.stereoOnset++;
            }
            if (isRepeating(n0, cs, ce)) { local.protectedHits++; reject(cs, ce); return; }
        }
        int gs = s - wPre_;
        const int ge = e + wPost_;
        if (!gaps_.empty()) {
            Gap& g = gaps_.back();
            if (gs - g.e < mergeDist_) {  // nearby gaps: merged if possible
                if (ge - g.s <= allowed) { g.e = std::max(g.e, ge); g.hf = g.hf && hfGap; return; }
                if (gs < g.e) gs = g.e;  // otherwise no overlap
            }
        }
        if (ge - gs <= 0) return;
        if (static_cast<int>(gaps_.size()) >= maxGaps_ * 2) { local.overBudget++; return; }
        gaps_.push_back({gs, ge, hfGap});
    };

    const float kScan = (twin_ && !crackle_ && prot == 1) ? std::max(k * std::min(1.f, stLoK), std::min(k, stFloorK)) : k;
    int cs = -1, lf = -1, n0 = -1;
    float best = 0.f;
    for (int n = dS_; n < dE_; ++n) {
        const float a = std::fabs(md_[n]);
        const float sl = locSig_[(n - dS_) / blk_];
        // Second detector: strong FORWARD error AND strong BACKWARD error
        // within a few samples. It catches clicks whose shape (impulse +
        // rebound, typical after RIAA correction) partly cancels the
        // matched filter.
        const float rel = sl / std::max(sig_, kSigmaMin);
        float vc = 0.f;
        if (comboK > 0.f) {
            const float zf = std::fabs(ef_[n]) / std::max(sigF_ * rel, kSigmaMin);
            if (zf > k * comboK) {
                float zb = 0.f;
                for (int j = std::max(eS_, n - 3); j <= std::min(eE_ - 1, n + 3); ++j) zb = std::max(zb, std::fabs(eb_[j]));
                zb /= std::max(sigB_ * rel, kSigmaMin);
                vc = std::min(zf, zb) / comboK;
            }
        }
        if (!(a > std::max(kScan * sl, kThreshAbs)) && !(vc > kScan && a > kThreshAbs)) continue;
        if (cs >= 0) {
            const int hole = n - lf - 1;
            if (!(hole <= gMerge_ || (hole <= gBridge_ && n + 1 - cs <= lmaxCur_ + 2 * p_))) {
                handleCluster(cs, lf + 1, best, n0);
                cs = -1;
            }
        }
        if (cs < 0) { cs = n; best = 0.f; n0 = n; }
        lf = n;
        const float v = std::max(a / sl, vc);
        if (v > best) { best = v; n0 = n; }
    }
    if (cs >= 0) handleCluster(cs, lf + 1, best, n0);

    pending_ = local;  // validated by hop() after the last pass
}

void DeclickChannel::zeroPhaseLP(int b, float* x, int n) {
    Biquad2 f = mbLP_[b];
    f.z1 = f.z2 = 0.f;
    for (int i = 0; i < n; ++i) x[i] = f.tick(x[i]);
    f.z1 = f.z2 = 0.f;
    for (int i = n - 1; i >= 0; --i) x[i] = f.tick(x[i]);
}

// Multiband repair (original work). The gap [gs, ge) has just been
// interpolated; keep_ holds the original. The correction c = interpolation −
// original and the signal are split into 4 complementary bands (< 1 kHz,
// 1–3, 3–7, > 7 kHz) by zero-phase filters. In each band, Wiener gain:
// g = max(0, 1 − β·E_music / E_gap), where E_gap is the energy of the
// original in the gap and E_music that of the music just before and just
// after. Where the click dominates (often the treble), the repair is
// complete; where the music dominates (often the bass), the original is
// kept: the music is no longer re-invented needlessly.
bool DeclickChannel::multibandGate(int gs, int ge, bool hfOnly) {
    const int L = ge - gs;
    const int nb = std::clamp(L, 32, mbNb_);
    const int pad = mbM_ + 24 + nb;
    const int ws = gs - pad, we = ge + pad, n = we - ws;
    if (ws < 0 || we > B_ || n > mbWin_ || L > static_cast<int>(keep_.size())) {
        if (hfOnly) { std::memcpy(&rest_[gs], keep_.data(), sizeof(float) * std::min<int>(L, keep_.size())); return false; }
        return true;  // keeps the full-band repair
    }
    float* x0 = mbX_[kMbCross].data();
    float* c0 = mbC_[kMbCross].data();
    for (int i = 0; i < n; ++i) { x0[i] = rest_[ws + i]; c0[i] = 0.f; }
    for (int i = 0; i < L; ++i) { x0[pad + i] = keep_[i]; c0[pad + i] = rest_[gs + i] - keep_[i]; }
    for (int b = 0; b < kMbCross; ++b) {
        std::memcpy(mbX_[b].data(), x0, sizeof(float) * n);
        std::memcpy(mbC_[b].data(), c0, sizeof(float) * n);
        zeroPhaseLP(b, mbX_[b].data(), n);
        zeroPhaseLP(b, mbC_[b].data(), n);
    }
    // Band k = LP_k − LP_{k−1} (LP_{−1} = 0, LP_3 = full signal).
    auto band = [&](const std::vector<float>* A, int k, int i) {
        const float hi = (k == kMbCross) ? A[kMbCross][i] : A[k][i];
        const float lo = (k == 0) ? 0.f : A[k - 1][i];
        return hi - lo;
    };
    float g[kMbCross + 1];
    bool all1 = true, all0 = true;
    for (int k = 0; k <= kMbCross; ++k) {
        double eg = 0.0, eb = 0.0;
        for (int i = pad; i < pad + L; ++i) { const double v = band(mbX_, k, i); eg += v * v; }
        for (int i = pad - 24 - nb; i < pad - 24; ++i) { const double v = band(mbX_, k, i); eb += v * v; }
        for (int i = pad + L + 24; i < pad + L + 24 + nb; ++i) { const double v = band(mbX_, k, i); eb += v * v; }
        eg /= L; eb /= 2.0 * nb;
        g[k] = (eg > 0.0) ? static_cast<float>(std::max(0.0, 1.0 - mbBeta * eb / eg)) : 0.f;
        if (hfOnly && k < 2) g[k] = 0.f;   // < 3 kHz: the original is kept
        all1 = all1 && g[k] >= 0.999f;
        all0 = all0 && g[k] <= 0.f;
    }
    if (all1) return true;
    if (all0) { std::memcpy(&rest_[gs], keep_.data(), sizeof(float) * L); return false; }
    // rest_ already holds the full correction: Σ (g_k − 1)·c_k is added
    // over ±mbM_ around the gap.
    const int a = pad - mbM_, z = pad + L + mbM_;
    for (int i = a; i < z; ++i) {
        float d = 0.f;
        for (int k = 0; k <= kMbCross; ++k) d += (g[k] - 1.f) * band(mbC_, k, i);
        rest_[ws + i] += d;
    }
    return true;
}

// Stereo: (normalized) energy of the prediction error of the OTHER channel
// around n0, at the best offset within ±1 ms (spaced microphones), relative
// to its own. Small (< 0.1): nothing similar in the other channel, it is a
// single-channel click; large: a common event, probably musical.
float DeclickChannel::twinRatio(int n0) const {
    if (!twin_) return -1.f;
    const int a = n0 - twinHw_, b = n0 + twinHw_ + 1;
    if (a < eS_ || b > eE_) return -1.f;
    const double own = e2p_[b] - e2p_[a];
    if (!(own > 0.0)) return -1.f;
    const int lo = std::max(-twinLag_, eS_ - a), hi = std::min(twinLag_, eE_ - b);
    double best = 0.0;
    for (int l = lo; l <= hi; ++l) best = std::max(best, twin_->e2p_[b + l] - twin_->e2p_[a + l]);
    return static_cast<float>(best / own);
}

// Joint interpolation of a long gap (original work): in addition to the AR
// model (LSAR), a cross predictor (filter of 2K+1 coefficients estimated on
// the neighboring music) gives an estimate taken from the other channel. The
// two are combined optimally (least squares weighted by their respective
// errors): T u + λ u = b + λ·prior, λ = σ²(AR error) / σ²(cross error).
bool DeclickChannel::jointInterp(int gs, int L, const double* cc, int order, const DeclickChannel* o) {
    const int K = xK_, n = 2 * K + 1, ge = gs + L;
    if (L > static_cast<int>(xPrior_.size())) return false;
    const float* X = rest_.data();
    const float* R = o->rest_.data();
    // Autocorrelation (Toeplitz) method on the neighboring music, with gaps and
    // clicks of either channel set to zero: ≈ 4 times less computation than
    // the full covariance, for an equivalent estimate.
    auto near = [](const std::vector<Gap>& G, int t) {
        for (const Gap& g : G) if (t >= g.s - 2 && t < g.e + 2) return true;
        return false;
    };
    const int t0 = std::max(K + 1, gs - xTrain_), t1 = std::min(B_ - K - 1, ge + xTrain_);
    const int W = t1 - t0;
    if (W <= 0 || W > static_cast<int>(xWx_.size())) return false;
    int N = 0;
    for (int i = 0; i < W; ++i) {
        const int t = t0 + i;
        const bool ok = !(t >= gs - 4 && t < ge + 4) && !near(gaps_, t) && !near(o->gaps_, t);
        xWx_[i] = ok ? X[t] : 0.f;
        xWr_[i] = ok ? R[t] : 0.f;
        N += ok;
    }
    if (N < 8 * n) return false;
    double rr[2 * 16 + 1], rx[2 * 16 + 1];
    for (int m = 0; m < n; ++m) {
        double v = 0.0;
        for (int i = 0; i + m < W; ++i) v += double(xWr_[i]) * xWr_[i + m];
        rr[m] = v;
    }
    for (int j = 0; j < n; ++j) {   // rx[j] = Σ x[t] r[t + j − K]
        const int lag = j - K;
        double v = 0.0;
        for (int i = std::max(0, -lag); i < W && i + lag < W; ++i) v += double(xWx_[i]) * xWr_[i + lag];
        rx[j] = v;
    }
    for (int i = 0; i < n; ++i) {
        xb_[i] = rx[i];
        for (int j = 0; j < n; ++j) xA_[i * n + j] = rr[std::abs(i - j)];
    }
    // Cholesky (n ≤ 33) with a small diagonal loading.
    double tr = 0.0;
    for (int i = 0; i < n; ++i) tr += xA_[i * n + i];
    const double load = 1e-6 * tr / n + 1e-30;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double v = xA_[j * n + i] + (i == j ? load : 0.0);   // upper triangle → A[i][j]
            for (int k = 0; k < j; ++k) v -= xA_[i * n + k] * xA_[j * n + k];
            if (i == j) { if (!(v > 0.0)) return false; xA_[i * n + i] = std::sqrt(v); }
            else xA_[i * n + j] = v / xA_[j * n + j];
        }
    }
    // xA_ now holds L (lower triangle); solve L Lᵀ h = b.
    for (int i = 0; i < n; ++i) { double v = xb_[i]; for (int k = 0; k < i; ++k) v -= xA_[i * n + k] * xb_[k]; xb_[i] = v / xA_[i * n + i]; }
    for (int i = n - 1; i >= 0; --i) { double v = xb_[i]; for (int k = i + 1; k < n; ++k) v -= xA_[k * n + i] * xb_[k]; xb_[i] = v / xA_[i * n + i]; }
    // Residual error of the cross predictor, recomputed on the training zone.
    double ex = 0.0;
    int nx = 0;
    for (int t = std::max(K + 1, gs - 256); t < std::min(B_ - K - 1, ge + 256); ++t) {
        if ((t >= gs - 4 && t < ge + 4) || near(gaps_, t) || near(o->gaps_, t)) continue;
        double p = 0.0;
        for (int i = 0; i < n; ++i) p += xb_[i] * R[t - K + i];
        ex += (X[t] - p) * (X[t] - p);
        ++nx;
    }
    if (nx < 32) return false;
    const double sx = ex / nx;
    // Error of the AR model used (robust: median), over 256 samples on each side.
    int na = 0;
    for (int t = std::max(order, gs - 256); t < gs; ++t) {
        double e = 0.0;
        for (int k = 0; k <= order; ++k) e += cc[k] * X[t - k];
        if (na < static_cast<int>(absbuf_.size())) absbuf_[na++] = static_cast<float>(std::fabs(e));
    }
    for (int t = ge + order; t < std::min(B_, ge + 256); ++t) {
        double e = 0.0;
        for (int k = 0; k <= order; ++k) e += cc[k] * X[t - k];
        if (na < static_cast<int>(absbuf_.size())) absbuf_[na++] = static_cast<float>(std::fabs(e));
    }
    if (na < 32) return false;
    std::nth_element(absbuf_.begin(), absbuf_.begin() + na / 2, absbuf_.begin() + na);
    const double se = 1.4826 * absbuf_[na / 2];
    const double lambda = (sx > 0.0) ? se * se / sx : 0.0;
    if (!(lambda > 0.0) || !std::isfinite(lambda)) return false;
    for (int i = 0; i < L; ++i) {
        double p = 0.0;
        for (int j = 0; j < n; ++j) p += xb_[j] * R[gs + i - K + j];
        xPrior_[i] = static_cast<float>(p);
    }
    return lsar_.solve(rest_.data(), gs, L, cc, order, xPrior_.data(), lambda);
}

void DeclickChannel::hop(const DeclickParams& prm) {
    hopAnalyze(prm);
    hopDetect(prm, nullptr);
    hopRepair(prm, nullptr);
    hopOutput();
}

void DeclickChannel::hopAnalyze(const DeclickParams& prm) {
    const float k = (prm.kOverride > 0.f && prm.intensity > 0.f) ? prm.kOverride : declickThreshold(prm.intensity);
    kCur_ = k;
    protCur_ = prm.pitchProtect ? std::clamp(prm.protect, 0, 2) : 0;
    lmaxCur_ = std::clamp(roundi(prm.maxClickMs * 1e-3 * fs_), 1, lmaxMax_);
    mbOn_ = prm.multiband;

    if (k > 0.f) {
        energyReady_ = false;
        periodReady_ = false;
        d1Ready_ = false;
        onsetSrc_ = rest_.data();
        estimateAR(rest_.data());
        computeErrors(rest_.data());
        // Stereo snapshot: normalized prediction error (1st pass), cumulated,
        // so that the other channel can look for "twins".
        sigA_ = robustSigma(ef_.data());
        e2p_[0] = 0.0;
        for (int i = 0; i < B_; ++i) {
            const double z = (i >= eS_ && i < eE_) ? ef_[i] / sigA_ : 0.0;
            e2p_[i + 1] = e2p_[i] + z * z;
        }
    }
}

void DeclickChannel::hopDetect(const DeclickParams& prm, const DeclickChannel* other) {
    const float k = kCur_;
    twin_ = (other && stereo && prm.stereo && other->kCur_ > 0.f) ? other : nullptr;
    if (twin_) {
        // Mono record (or mono passage): both channels are identical, every
        // click has a perfect twin; the test makes no sense then.
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (int i = eS_ + 1; i < eE_; ++i) {
            const double a = double(raw_[i]) - raw_[i - 1], b = double(other->raw_[i]) - other->raw_[i - 1];
            xy += a * b; xx += a * a; yy += b * b;
        }
        if (xx * yy > 0.0 && xy / std::sqrt(xx * yy) > monoRho) twin_ = nullptr;
    }
    if (k > 0.f) {
        const int doneSaved = doneUntil_;
        detect(k, prm);
        if (!crackle_) bandCandidates(k);
        if (twoPass && (!gaps_.empty() || !bandGaps_.empty())) {
            doneUntil_ = doneSaved;
            {
                // 2nd pass: model re-estimated without the clicks (Godsill & Rayner),
                // nor the bursts found by the band pre-detector.
                std::memcpy(scratch_.data(), rest_.data(), sizeof(float) * B_);
                auto bridge = [&](const Gap& g) {
                    const float a = scratch_[g.s - 1], b = scratch_[g.e];
                    const int L = g.e - g.s + 1;
                    for (int i = g.s; i < g.e; ++i)
                        scratch_[i] = a + (b - a) * static_cast<float>(i - g.s + 1) / L;
                };
                for (const Gap& g : gaps_) bridge(g);
                for (const Gap& g : bandGaps_) bridge(g);
                gaps1_.assign(gaps_.begin(), gaps_.end());  // reserved capacity: no allocation
                estimateAR(scratch_.data());
                computeErrors(rest_.data());
                periodReady_ = false;
                // Onsets measured without the clicks already found: a nearby click
                // no longer makes another click look like an onset.
                if (onsetClean) { onsetSrc_ = scratch_.data(); energyReady_ = false; }
                detect(k, prm);
                onsetSrc_ = rest_.data();
                // Union: a click found in the 1st pass stays repaired, unless the
                // 2nd pass (better model) judged it musical.
                auto overlaps = [](const Gap& a, const Gap& b) { return a.s < b.e && b.s < a.e; };
                const size_t n2 = gaps_.size();
                for (const Gap& g : gaps1_) {
                    bool skip = false;
                    for (size_t i = 0; i < n2 && !skip; ++i) skip = overlaps(g, gaps_[i]);
                    for (const Gap& r : rej_) if (!skip) skip = overlaps(g, r);
                    if (!skip && gaps_.size() < gaps_.capacity()) gaps_.push_back(g);
                }
                if (gaps_.size() > n2) {
                    std::sort(gaps_.begin(), gaps_.end(), [](const Gap& a, const Gap& b) { return a.s < b.s; });
                    size_t w = 0;
                    for (size_t i = 1; i < gaps_.size(); ++i) {
                        if (gaps_[i].s <= gaps_[w].e) { gaps_[w].e = std::max(gaps_[w].e, gaps_[i].e); gaps_[w].hf = gaps_[w].hf && gaps_[i].hf; }
                        else gaps_[++w] = gaps_[i];
                    }
                    gaps_.resize(w + 1);
                }
            }
        }
    }
}

void DeclickChannel::hopRepair(const DeclickParams& prm, const DeclickChannel* other) {
    (void)prm;
    const float k = kCur_;
    const DeclickChannel* oth = (other && stereo && prm.stereo) ? other : nullptr;
    if (k > 0.f) {
        stats_.protectedHits += pending_.protectedHits;
        stats_.tooLong += pending_.tooLong;
        stats_.overBudget += pending_.overBudget;
        stats_.stereoOnset += pending_.stereoOnset;
        // Repair, with a bounded computing budget (guaranteed real time).
        const int budget = H_ / 2;
        int used = 0, count = 0, nLong = 0;
        const float* clean = (twoPass && !gaps_.empty()) ? scratch_.data() : rest_.data();
        int prevEnd = 0;
        for (size_t gi = 0; gi < gaps_.size(); ++gi) {
            Gap g = gaps_[gi];
            const int nextStart = (gi + 1 < gaps_.size()) ? gaps_[gi + 1].s : B_;
            int L = g.e - g.s;
            if (count >= maxGaps_ || used + L > budget) { stats_.overBudget++; continue; }
            const double* cc = c_.data();
            int order = p_;
            if (L > longGap_ && nLong < maxLongPerHop_) {
                // Long click: model of order ≈ 3 × length (bounded), estimated on
                // the signal roughly cleared of clicks.
                order = estimateHighOrder(clean, (g.s + g.e) / 2, std::clamp(3 * L, p_, pHiMax_));
                cc = cH_.data();
                ++nLong;
            }
            if (g.hf && L > static_cast<int>(keep_.size())) continue;
            const int L0 = L, s0 = g.s;
            if ((softRepair_ && L <= softMax_) || ((mbOn_ || g.hf) && L <= static_cast<int>(keep_.size())))
                std::memcpy(keep_.data(), &rest_[g.s], sizeof(float) * L);
            bool solved = false;
            if (oth && !softRepair_ && !g.hf && L >= jointMin_) {
                bool clear = true;   // the other channel must be healthy at this spot
                for (const Gap& og2 : oth->gaps_) if (og2.s < g.e + xK_ && g.s - xK_ < og2.e) { clear = false; break; }
                if (clear && jointInterp(g.s, L, cc, order, oth)) { solved = true; stats_.joint++; }
            }
            if (solved || lsar_.solve(rest_.data(), g.s, L, cc, order)) {
                if (!softRepair_ && extendK > 0.f && !g.hf) {
                    // Extension: a vinyl click has a decaying "tail" (RIAA
                    // correction). After the repair, if the prediction error
                    // stays strong just after (or just before) the gap, the tail
                    // is not repaired: the gap is enlarged and the repair redone
                    // (at most 3 times).
                    const int bi = std::clamp((g.s - dS_) / blk_, 0, static_cast<int>(locSig_.size()) - 1);
                    const float loc = locSig_[bi] / std::max(sig_, kSigmaMin);
                    const float thE = std::max(extendK * kCur_ * sigF_ * loc, kThreshAbs);
                    const float thB = std::max(extendK * kCur_ * sigB_ * loc, kThreshAbs);
                    const int maxLen = std::min(lmaxCur_ + wPre_ + wPost_, lmaxMax_ + wPre_ + wPost_);
                    const float* ar = aRevF_.data();
                    const float* af = aF_.data();
                    for (int it = 0; it < 3; ++it) {
                        int ns = g.s, ne = g.e;
                        const int eLim = std::min(g.e + p_, nextStart - 1);
                        for (int n = g.e; n < eLim; ++n)
                            if (std::fabs(rest_[n] - dot(ar, &rest_[n - p_], p_)) > thE) ne = n + 1;
                        const int sLim = std::max(g.s - p_, prevEnd + 1);
                        for (int n = g.s - 1; n >= sLim; --n)
                            if (std::fabs(rest_[n] - dot(af, &rest_[n + 1], p_)) > thB) ns = n;
                        if (ne == g.e && ns == g.s) break;
                        if (ne > g.e) ne = std::min(ne + wPost_, nextStart - 1);
                        if (ns < g.s) ns = std::max(ns - wPre_, prevEnd + 1);
                        if (ne - ns > maxLen || ns - order < 0 || ne + order > B_) break;
                        if (!lsar_.solve(rest_.data(), ns, ne - ns, cc, order)) break;
                        g.s = ns; g.e = ne;
                        stats_.extended++;
                    }
                    L = g.e - g.s;
                }
                if (softRepair_ && L <= softMax_) {
                    // Soft (Wiener-type) repair: r = original − interpolation
                    // holds the crackle + the interpolation error. Only the part
                    // that exceeds this expected error is removed.
                    double R0 = 0.0;
                    for (int j = 0; j <= p_; ++j) R0 += c_[j] * c_[j];
                    const int bi = std::clamp(((g.s + g.e) / 2 - dS_) / blk_, 0, static_cast<int>(locSig_.size()) - 1);
                    const double sI = locSig_[bi] / std::max(R0, 1e-12);  // interpolation error (1 sample)
                    const double sF = sigF_ * locSig_[bi] / std::max(static_cast<double>(sig_), 1e-12);  // local prediction error
                    // Expected interpolation error: ~sI for an isolated sample,
                    // ~sF per sample for a longer gap (cautious).
                    const double En = (L == 1) ? sI * sI : L * sF * sF;
                    double Er = 0.0;
                    for (int i = 0; i < L; ++i) { const double r = keep_[i] - rest_[g.s + i]; Er += r * r; }
                    double gain = (Er > 0.0) ? std::max(0.0, 1.0 - softBeta * En / Er) : 0.0;
                    // A crackle is small: above crackleCap, it is not a
                    // crackle (drum attack, steep edge…).
                    if (protCur_ > 0) {
                        const float cap = (protCur_ >= 2) ? 0.5f * crackleCap : crackleCap;
                        for (int i = 0; i < L; ++i)
                            if (std::fabs(keep_[i] - rest_[g.s + i]) * gain > cap) { gain = 0.0; break; }
                    }
                    for (int i = 0; i < L; ++i)
                        rest_[g.s + i] = static_cast<float>(keep_[i] - gain * (keep_[i] - rest_[g.s + i]));
                    if (gain <= 0.0) {  // nothing removed: the exact original is put back
                        std::memcpy(&rest_[g.s], keep_.data(), sizeof(float) * L);
                        continue;
                    }
                }
                // Multiband (without the tail extension, which changes the gap).
                if (g.hf) {
                    if (L > static_cast<int>(keep_.size()) || !multibandGate(g.s, g.e, true)) continue;
                    stats_.hfOnly++;
                } else if (mbOn_ && g.s == s0 && L == L0 && !multibandGate(g.s, g.e, false)) continue;
                stats_.repaired++;
                used += L;
                ++count;
                prevEnd = g.e;
            }
        }
    }

}

void DeclickChannel::hopOutput() {
    std::memcpy(outQRaw_.data(), &raw_[i0_], sizeof(float) * H_);
    std::memcpy(outQRest_.data(), &rest_[i0_], sizeof(float) * H_);
    std::memmove(raw_.data(), raw_.data() + H_, sizeof(float) * (B_ - H_));
    std::memmove(rest_.data(), rest_.data() + H_, sizeof(float) * (B_ - H_));
    doneUntil_ = std::max(0, doneUntil_ - H_);
}

// ---------------------------------------------------------------------------
// Declicker (multichannel, listening modes, crossfade)
// ---------------------------------------------------------------------------

bool Declicker::prepare(double fs, int channels, bool crackle) {
    if (channels < 1 || channels > kMaxChannels || !(fs >= 8000.0 && fs <= 400000.0)) return false;
    ch_.assign(channels, DeclickChannel{});
    for (auto& c : ch_) c.prepare(fs, crackle);
    raw_.assign(static_cast<size_t>(kChunk) * channels, 0.f);
    rest_.assign(static_cast<size_t>(kChunk) * channels, 0.f);
    fadeLen_ = std::max(1, roundi(0.010 * fs));
    curMode_ = prevMode_ = prm_.mode;
    fadePos_ = 0;
    return true;
}

void Declicker::reset() {
    for (auto& c : ch_) c.reset();
    fadePos_ = 0;
}

void Declicker::process(const float* const* in, float* const* out, int n) {
    DenormalGuard dg;
    const int nch = channels();
    if (prm_.mode != curMode_) {
        prevMode_ = curMode_;
        curMode_ = prm_.mode;
        fadePos_ = fadeLen_;
    }
    auto pick = [](Mode m, float raw, float rest) {
        switch (m) {
            case Mode::Bypass:  return raw;
            case Mode::Removed: return raw - rest;
            default:            return rest;
        }
    };
    for (int off = 0; off < n; off += kChunk) {
        const int m = std::min(kChunk, n - off);
        // All channels advance together, hop by hop: each phase of a hop sees
        // the other channel at the same instant (stereo test, interpolation).
        for (int done = 0; done < m;) {
            const int step = std::min(m - done, ch_[0].hopSize() - ch_[0].fill());
            bool due = false;
            for (int c = 0; c < nch; ++c)
                due = ch_[c].push(in[c] + off + done, &raw_[c * kChunk + done], &rest_[c * kChunk + done], step) || due;
            if (due) {
                auto other = [&](int c) -> const DeclickChannel* { return nch == 2 ? &ch_[1 - c] : nullptr; };
                for (int c = 0; c < nch; ++c) ch_[c].hopAnalyze(prm_);
                for (int c = 0; c < nch; ++c) ch_[c].hopDetect(prm_, other(c));
                for (int c = 0; c < nch; ++c) ch_[c].hopRepair(prm_, other(c));
                for (int c = 0; c < nch; ++c) {
                    ch_[c].hopOutput();
                    ch_[c].finishHop(&raw_[c * kChunk + done + step - 1], &rest_[c * kChunk + done + step - 1]);
                }
            }
            done += step;
        }
        const float invFade = 1.f / fadeLen_;
        for (int c = 0; c < nch; ++c) {
            const float* rw = &raw_[c * kChunk];
            const float* rs = &rest_[c * kChunk];
            float* o = out[c] + off;
            if (fadePos_ <= 0) {
                for (int i = 0; i < m; ++i) o[i] = pick(curMode_, rw[i], rs[i]);
            } else {
                for (int i = 0; i < m; ++i) {
                    const int fp = fadePos_ - i;
                    const float vNew = pick(curMode_, rw[i], rs[i]);
                    if (fp > 0) {
                        const float g = fp * invFade;
                        o[i] = g * pick(prevMode_, rw[i], rs[i]) + (1.f - g) * vNew;
                    } else {
                        o[i] = vNew;
                    }
                }
            }
        }
        fadePos_ = std::max(0, fadePos_ - m);
    }
}

DeclickStats Declicker::stats() const {
    DeclickStats s;
    for (const auto& c : ch_) {
        const auto& t = c.stats();
        s.repaired += t.repaired;
        s.protectedHits += t.protectedHits;
        s.tooLong += t.tooLong;
        s.hfOnly += t.hfOnly;
        s.stereoOnset += t.stereoOnset;
        s.joint += t.joint;
        s.overBudget += t.overBudget;
    }
    return s;
}

} // namespace vc
