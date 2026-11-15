// vinylclean — Decrackle and chain. Original work.
#include "decrackle.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace vc {

float decrackleThreshold(float intensity) {
    if (!(intensity > 0.f)) return 0.f;
    const float I = std::min(intensity, 10.f);
    // Same scale as the declicker since 0.6: k(1) = 30, k(10) = 4.
    return 30.f * std::pow(4.f / 30.f, (I - 1.f) / 9.f);
}

void Decrackler::setParams(const DecrackleParams& p) {
    DeclickParams q;
    q.intensity = p.intensity;          // 0 => off (transparent)
    q.kOverride = p.kOverride > 0.f ? p.kOverride : decrackleThreshold(p.intensity);
    q.maxClickMs = 0.5f;
    q.mode = p.mode;
    q.pitchProtect = p.pitchProtect;
    q.protect = p.protect;
    q.multiband = p.multiband;
    q.stereo = p.stereo;
    eng_.setParams(q);
}

bool Chain::prepare(double fs, int channels) {
    if (channels < 1 || channels > kMaxChannels) return false;
    if (!dc_.prepare(fs, channels) || !cr_.prepare(fs, channels) || !ad_.prepare(fs, channels)) return false;
    nch_ = channels;
    dlen_ = dc_.latency() + cr_.latency() + ad_.latency();
    delay_.assign(static_cast<size_t>(dlen_ + 1) * channels, 0.f);
    dpos_ = 0;
    a_.assign(static_cast<size_t>(kChunk) * channels, 0.f);
    b_.assign(static_cast<size_t>(kChunk) * channels, 0.f);
    raw_.assign(static_cast<size_t>(kChunk) * channels, 0.f);
    fadeLen_ = std::max(1, roundi(0.010 * fs));
    cur_ = prev_ = prm_.mode;
    fadePos_ = 0;
    setParams(prm_);
    return true;
}

void Chain::reset() {
    dc_.reset();
    cr_.reset();
    ad_.reset();
    std::fill(delay_.begin(), delay_.end(), 0.f);
    dpos_ = 0;
    fadePos_ = 0;
}

void Chain::setParams(const ChainParams& p) {
    prm_ = p;
    DeclickParams d = p.declick;
    d.mode = Mode::Repair;
    dc_.setParams(d);
    DecrackleParams c = p.decrackle;
    c.mode = Mode::Repair;
    cr_.setParams(c);
    ad_.setParams(p.dist);
}

void Chain::process(const float* const* in, float* const* out, int n, float* const* repaired) {
    DenormalGuard dg;
    if (prm_.mode != cur_) { prev_ = cur_; cur_ = prm_.mode; fadePos_ = fadeLen_; }
    const float* ain[kMaxChannels];
    const float* bin[kMaxChannels];
    float* aout[kMaxChannels];
    float* bout[kMaxChannels];
    auto pick = [](Mode m, float raw, float rest) {
        switch (m) {
            case Mode::Bypass:  return raw;
            case Mode::Removed: return raw - rest;
            default:            return rest;
        }
    };
    const int D = dlen_ + 1;  // ring: read the sample written dlen_ steps ago
    for (int off = 0; off < n; off += kChunk) {
        const int m = std::min(kChunk, n - off);
        for (int c = 0; c < nch_; ++c) {
            ain[c] = in[c] + off;
            aout[c] = &a_[c * kChunk];
            bout[c] = &b_[c * kChunk];
            bin[c] = aout[c];
        }
        // Original path first (in and out may be the same memory).
        for (int i = 0; i < m; ++i) {
            const int rd = (dpos_ + 1) % D;
            for (int c = 0; c < nch_; ++c) {
                float* line = &delay_[static_cast<size_t>(c) * D];
                line[dpos_] = ain[c][i];
                raw_[c * kChunk + i] = line[rd];
            }
            dpos_ = rd;
        }
        dc_.process(ain, aout, m);   // declick (Repair)
        cr_.process(bin, bout, m);   // decrackle (Repair)
        const float* cin[kMaxChannels];
        for (int c = 0; c < nch_; ++c) cin[c] = bout[c];
        ad_.process(cin, bout, m);  // playback anti-distortion (signal already cleaned)
        for (int c = 0; c < nch_; ++c) {
            const float* rw = &raw_[c * kChunk];
            const float* rs = bout[c];
            float* o = out[c] + off;
            if (repaired) std::copy(rs, rs + m, repaired[c] + off);   // cleaned signal, whatever the listening mode
            for (int i = 0; i < m; ++i) {
                const float vNew = pick(cur_, rw[i], rs[i]);
                const int fp = fadePos_ - i;
                if (fp > 0) {
                    const float g = float(fp) / fadeLen_;
                    o[i] = g * pick(prev_, rw[i], rs[i]) + (1.f - g) * vNew;
                } else {
                    o[i] = vNew;
                }
            }
        }
        fadePos_ = std::max(0, fadePos_ - m);
    }
}

} // namespace vc
