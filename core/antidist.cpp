// vinylclean — playback anti-distortion. Original work (see antidist.h).
#include "antidist.h"
#include "vc_util.h"
#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr double kPiD = 3.14159265358979323846;
}

void ArmGeometry::nullPoints(Alignment a, double& n1, double& n2) {
    // Published null points for the IEC groove radii (60.325 → 146.05 mm).
    switch (a) {
        case Alignment::Baerwald:  n1 = 66.00;  n2 = 120.90; break;   // Löfgren A / Baerwald
        case Alignment::LofgrenB:  n1 = 70.29;  n2 = 116.60; break;
        case Alignment::Stevenson: n1 = 60.325; n2 = 117.42; break;
        default: break;
    }
}

void ArmGeometry::compute() {
    r1 = null1; r2 = null2;
    if (align != Alignment::Custom) nullPoints(align, r1, r2);
    if (r1 > r2) std::swap(r1, r2);
    const double L = std::max(effLenMm, r2 + 1.0);
    const double sb = std::min(0.99, (r1 + r2) / (2.0 * L));
    offsetDeg = std::asin(sb) * 180.0 / kPiD;
    mountMm = std::sqrt(std::max(1.0, L * L - r1 * r2));
    overhangMm = L - mountMm;
}

double ArmGeometry::trackingErrorDeg(double r) const {
    const double L = std::max(effLenMm, r2 + 1.0), d = mountMm;
    const double s = std::clamp((L * L + r * r - d * d) / (2.0 * L * r), -1.0, 1.0);
    return (std::asin(s) - offsetDeg * kPiD / 180.0) * 180.0 / kPiD;
}

double stylusRadiusUm(StylusType t, double customUm) {
    switch (t) {
        case StylusType::Conical:      return 15.2;   // 0.6 mil
        case StylusType::Elliptical37: return 7.6;    // 0.3 × 0.7 mil
        case StylusType::Elliptical27: return 5.1;    // 0.2 × 0.7 mil
        case StylusType::Shibata:      return 6.0;    // typical value ("line" contact)
        case StylusType::MicroLine:    return 2.8;    // micro-line
        default:                       return std::clamp(customUm, 1.0, 25.0);
    }
}

// Doubled ring buffer: every sample is written twice, so reading the last
// "taps" samples is contiguous (no modulo in the loops).
static inline void pushHist(std::vector<float>& h, int& pos, float v) {
    const int n = int(h.size() / 2);
    h[pos] = v; h[pos + n] = v;
    pos = (pos + 1 == n) ? 0 : pos + 1;
}

bool AntiDistortion::prepare(double fs, int channels) {
    if (channels < 1 || channels > 8) return false;
    fs_ = fs;
    nch_ = channels;
    ref_.design(fs);
    // Half-band filter at double rate: sinc × Kaiser window (β = 8).
    taps_ = 47;
    ld_ = (taps_ - 1) / 2;   // 23 base samples: up + down (2 × 23 at double rate)
    h_.assign(taps_, 0.0);
    auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    const double beta = 8.0, c = (taps_ - 1) / 2.0;
    // Continuous kernel H(n); its exact derivative H'(n) gives a 2nd filter that
    // provides du/dt perfectly aligned with u (no finite difference, no half-sample offset).
    auto H = [&](double n) {
        const double x = n / c;
        const double sinc = (std::fabs(n) < 1e-12) ? 0.5 : std::sin(0.5 * kPiD * n) / (kPiD * n);
        return sinc * bessel0(beta * std::sqrt(std::max(0.0, 1 - x * x))) / bessel0(beta);
    };
    g_.assign(taps_, 0.0);
    double sum = 0;
    for (int i = 0; i < taps_; ++i) {
        const double n = i - c, e = 1e-4;
        h_[i] = H(n);
        g_[i] = (i == 0 || i == taps_ - 1) ? (H(n + (i == 0 ? e : 0)) - H(n - (i == 0 ? 0 : e))) / e
                                            : (H(n + e) - H(n - e)) / (2 * e);
        sum += h_[i];
    }
    for (auto& v : h_) v /= sum;
    for (auto& v : g_) v /= sum;
    // Half-band: odd-index coefficients are zero except the center one.
    hEven_.clear(); gEven_.clear(); gOdd_.clear();
    for (int i = 0; i < taps_; i += 2) { hEven_.push_back(h_[i]); gEven_.push_back(g_[i]); }
    for (int i = 1; i < taps_; i += 2) gOdd_.push_back(g_[i]);
    hCenter_ = h_[(taps_ - 1) / 2];
    ch_.assign(channels, Chan{});
    for (auto& ch : ch_) {
        ch.de.design(fs);
        ch.upHist.assign(2 * taps_, 0.f);   // doubled buffers: contiguous read, no modulo
        ch.dnHist.assign(2 * taps_, 0.f);
        ch.dry.assign(ld_ + 1, 0.f);
    }
    leak_ = std::exp(-2.0 * kPiD * 5.0 / (2.0 * fs));   // integrator (displacement): leaks below 5 Hz
    blkSamples_ = std::max(256, roundi(1024.0 * fs / 48000.0));
    blkN_ = std::max(64, roundi(60.0 * fs / blkSamples_));
    blkMax_.assign(2 * blkN_ + 4, 0.f);   // half: memory; half: copy for sorting
    reset();
    return true;
}

void AntiDistortion::reset() {
    for (auto& c : ch_) {
        c.de.reset();
        for (int i = 0; i < 3; ++i) { c.pz[i] = 0; c.pp[i] = 0; }
        std::fill(c.upHist.begin(), c.upHist.end(), 0.f);
        std::fill(c.dnHist.begin(), c.dnHist.end(), 0.f);
        std::fill(c.dry.begin(), c.dry.end(), 0.f);
        c.upPos = c.dnPos = c.dryPos = 0;
        c.prevU2 = 0; c.disp = 0;
    }
    gainNow_ = 0; blkFill_ = 0; blkPos_ = 0; blkCount_ = 0; curMax_ = 0; blkPeak_ = 0; scale_ = 0; p90max_ = 0;
    silentSec_ = 0; sideT_ = 0; sideKnown_ = false;
}

// New side (or new record): time and level start again from zero.
void AntiDistortion::newSide() {
    sideKnown_ = true; sideT_ = 0;
    blkCount_ = 0; blkPos_ = 0; p90max_ = 0; scale_ = 0;
    st_.levelReady = false;
}

void AntiDistortion::setParams(const AntiDistParams& p) {
    const bool wasOn = p_.enabled;
    p_ = p;
    p_.arm.compute();
    if (p.enabled && !wasOn && gainNow_ <= 0.0)
        for (auto& c : ch_) { c.de.reset(); for (int i = 0; i < 3; ++i) { c.pz[i] = 0; c.pp[i] = 0; } c.prevU2 = 0; c.disp = 0;
                              std::fill(c.upHist.begin(), c.upHist.end(), 0.f); std::fill(c.dnHist.begin(), c.dnHist.end(), 0.f); }
    updateGeometry();
}

// Pre-emphasis = exact inverse of the digital RIAA curve (groove velocity).
double AntiDistortion::preEmph(Chan& c, double x) {
    double v = x / ref_.gain();
    for (int i = 0; i < 3; ++i) {   // y = v − p·v[n−1] + z·y[n−1]
        const double y = v - ref_.pole(i) * c.pp[i] + ref_.zero(i) * c.pz[i];
        c.pp[i] = v; c.pz[i] = y;
        v = y;
    }
    return v;
}

void AntiDistortion::updateGeometry() {
    const DiscGeometry& g = p_.disc;
    double r;
    if (!p_.autoPos) r = p_.posMm;
    else if (!sideKnown_) r = g.rStart();   // no drop seen: cautious assumption (start of side)
    else r = g.rStart() - (g.rStart() - g.rEnd()) * std::min(1.0, sideT_ / std::max(60.0, p_.sideMin * 60.0));
    r = std::clamp(r, g.rEnd(), g.rStart());
    const double phi = p_.arm.trackingErrorDeg(r) * kPiD / 180.0;
    v_ = 2.0 * kPiD * r * 1e-3 * g.rpm() / 60.0;
    tanPhi_ = std::tan(phi);
    rho_ = forcedRho_ >= 0 ? forcedRho_ : stylusRadiusUm(p_.stylus, p_.stylusUm) * 1e-6;
    st_.radiusMm = r;
    st_.trackDeg = phi * 180.0 / kPiD;
    st_.grooveCms = v_ * 100.0;
    st_.sideSec = sideKnown_ ? sideT_ : -1.0;
}

void AntiDistortion::process(const float* const* in, float* const* out, int n) {
    const double target = p_.enabled ? std::clamp(p_.intensity, 0.0, 1.5) : 0.0;
    const double fs2 = 2.0 * fs_;
    const double fade = 1.0 / (0.2 * fs_);   // 200 ms fade
    const int nc = nch_;
    for (int i = 0; i < n; ++i) {
        float dryv[8];
        float pk = 0.f, umax = 0.f;
        double u[8];
        for (int c = 0; c < nc; ++c) {
            Chan& ch = ch_[c];
            ch.dry[ch.dryPos] = in[c][i];              // direct signal, delayed by ld_ (always)
            ch.dryPos = (ch.dryPos + 1) % (ld_ + 1);
            dryv[c] = ch.dry[ch.dryPos];
            pk = std::max(pk, std::fabs(in[c][i]));
            u[c] = preEmph(ch, in[c][i]);               // groove velocity (digital units)
            umax = std::max(umax, float(std::fabs(u[c])));
        }
        blkPeak_ = std::max(blkPeak_, pk);
        curMax_ = std::max(curMax_, umax);
        if (sideKnown_) sideT_ += 1.0 / fs_;
        if (++blkFill_ >= blkSamples_) {
            blkFill_ = 0;
            // Stylus drop: ≥ 5 s of silence (< −50 dBFS, arm raised: flipping a record takes
            // longer; a gap between two tracks, less) then music.
            const double bs = double(blkSamples_) / fs_;
            if (blkPeak_ < 3.2e-3f) silentSec_ += bs;
            else { if (silentSec_ >= 5.0) newSide(); silentSec_ = 0; }
            // Level (estimated real velocity): per-block maxima, 90th percentile over 60 s.
            if (blkPeak_ >= 3.2e-3f && curMax_ > 0.f) {
                blkMax_[blkPos_] = curMax_;
                blkPos_ = (blkPos_ + 1) % blkN_;
                blkCount_ = std::min(blkCount_ + 1, blkN_);
                if (blkCount_ >= 32 && (blkCount_ % 16 == 0 || blkCount_ == blkN_)) {
                    float* tmp = &blkMax_[blkN_];
                    std::copy(blkMax_.begin(), blkMax_.begin() + blkCount_, tmp);
                    std::nth_element(tmp, tmp + blkCount_ * 9 / 10, tmp + blkCount_);
                    const float p90 = tmp[blkCount_ * 9 / 10];
                    // The reference level is that of the LOUD passages of the side: it only
                    // drops very slowly (−3 dB in 10 min), so a quiet passage stays quiet.
                    p90max_ = std::max(p90max_, double(p90));
                    if (p90max_ > 0.0) scale_ = kRefVel / p90max_;
                }
            }
            blkPeak_ = 0.f; curMax_ = 0.f;
            p90max_ *= std::pow(10.0, -3.0 / 20.0 * bs / 600.0);
            st_.levelReady = double(blkCount_) * blkSamples_ >= 20.0 * fs_;   // 20 s of music
            st_.levelCms = kRefVel * 100.0;
            updateGeometry();
        }
        gainNow_ = (gainNow_ < target) ? std::min(target, gainNow_ + fade) : std::max(target, gainNow_ - fade);
        const double S = forcedScale_ > 0 ? forcedScale_ : (st_.levelReady ? scale_ : 0.0);
        if (gainNow_ <= 0.0 || S <= 0.0) {
            for (int c = 0; c < nc; ++c) {   // the filters still follow (re-enable without a jump)
                Chan& ch = ch_[c];
                pushHist(ch.upHist, ch.upPos, static_cast<float>(u[c]));
                ch.de.tick(0.0, false);
                out[c][i] = dryv[c];
            }
            continue;
        }
        const double cT = -(rho_ / (v_ * v_)) * S;     // tracing distortion (stylus radius)
        const double cK = (tanPhi_ / v_) * S;          // arm tracking error
        const double lim = kLimit * kRefVel / S;       // limit, in digital units
        double u2[8][2], du2[8][2];
        for (int c = 0; c < nc; ++c) {                 // ×2 oversampling (polyphase)
            Chan& ch = ch_[c];
            pushHist(ch.upHist, ch.upPos, static_cast<float>(u[c]));
            const float* x = &ch.upHist[ch.upPos + taps_ - 1];   // x[−k] = sample delayed by k
            const int ne = int(hEven_.size()), no = int(gOdd_.size());
            double e0 = 0, d0 = 0, d1 = 0;
            for (int k = 0; k < ne; ++k) { e0 += hEven_[k] * x[-k]; d0 += gEven_[k] * x[-k]; }
            for (int k = 0; k < no; ++k) d1 += gOdd_[k] * x[-k];
            const double e1 = hCenter_ * x[-((taps_ - 1) / 2 - 1) / 2];   // only non-zero odd coefficient
            u2[c][0] = 2.0 * e0; u2[c][1] = 2.0 * e1;
            du2[c][0] = 2.0 * d0 * fs2; du2[c][1] = 2.0 * d1 * fs2;
        }
        double corr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int ph = 0; ph < 2; ++ph) {
            double dl = 0.0, ul = 0.0;
            for (int c = 0; c < nc; ++c) { Chan& ch = ch_[c]; ch.disp = leak_ * ch.disp + 0.5 * (u2[c][ph] + ch.prevU2) / fs2; ch.prevU2 = u2[c][ph]; dl += ch.disp; ul += u2[c][ph]; }
            dl /= std::sqrt(2.0);   // lateral stylus displacement (45/45: sum of the walls / √2)
            ul /= std::sqrt(2.0);   // and its velocity
            for (int c = 0; c < nc; ++c) {
                Chan& ch = ch_[c];
                const double uu = u2[c][ph], du = du2[c][ph];
                // Wall read early by τ = k·D (D: lateral displacement): position z(t − k·D),
                // velocity read = u − k·d(D·u)/dt = u − k·(D·du/dt + dD/dt·u).
                const double D = nc >= 2 ? dl : ch.disp, dD = nc >= 2 ? ul : uu;
                double c2 = cT * uu * du + cK * (D * du + dD * uu);
                // Soft limit (x / √(1 + x²)): a leftover click or a badly estimated level cannot
                // create a huge "correction"; ≈ no effect on real distortion.
                const double xr = c2 / lim;
                c2 = lim * xr / std::sqrt(1.0 + xr * xr);
                pushHist(ch.dnHist, ch.dnPos, static_cast<float>(c2));
                if (ph == 0) {   // output at the base rate, aligned (2 × 23 samples at double rate)
                    const float* y2 = &ch.dnHist[ch.dnPos + taps_ - 1];
                    double y = hCenter_ * y2[-(taps_ - 1) / 2];
                    for (int k = 0; k < int(hEven_.size()); ++k) y += hEven_[k] * y2[-2 * k];
                    corr[c] = y;
                }
            }
        }
        for (int c = 0; c < nc; ++c) {
            const double e = ch_[c].de.tick(corr[c], false);   // back to the audio domain (RIAA)
            const double o = dryv[c] + gainNow_ * e;
            out[c][i] = std::isfinite(o) ? static_cast<float>(o) : dryv[c];
        }
    }
}

} // namespace vc
