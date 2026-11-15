// vinylclean — synthetic music and synthetic clicks for the tests.
// Instruments chosen to trap a declicker: sharp onsets (piano, harpsichord,
// percussion) and sounds with periodic impulses (brass, voice).
#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

namespace vctest {

constexpr double kPi = 3.14159265358979323846;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 0x9E3779B97F4A7C15ull) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); }
    double uni() { return next() / 2097152.0 / 2048.0; }  // [0,1)
    double range(double a, double b) { return a + (b - a) * uni(); }
    double gauss() { double u = std::max(uni(), 1e-12), v = uni(); return std::sqrt(-2 * std::log(u)) * std::cos(2 * kPi * v); }
};

enum Instr : unsigned {
    kPiano = 1, kBrass = 2, kVoice = 4, kPerc = 8, kHarpsi = 16, kSurface = 32,
    kAll = 63,
    kPulse = 64   // square-wave synth (periodic steep edges, electronic music)
};

// Two-pole resonator (formant).
struct Reso {
    double b0 = 0, a1 = 0, a2 = 0, y1 = 0, y2 = 0;
    void set(double fs, double f, double bw) {
        const double r = std::exp(-kPi * bw / fs);
        a1 = 2 * r * std::cos(2 * kPi * f / fs);
        a2 = -r * r;
        b0 = 1 - r;
    }
    double tick(double x) { const double y = b0 * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
};

// Mono, length n. Each instrument is normalized then mixed.
inline std::vector<float> synthMono(double fs, size_t n, unsigned instr, uint64_t seed) {
    Rng rng(seed);
    std::vector<double> mix(n, 0.0);
    auto addNorm = [&](std::vector<double>& v, double gain) {
        double pk = 1e-12;
        for (double x : v) pk = std::max(pk, std::fabs(x));
        for (size_t i = 0; i < n; ++i) mix[i] += v[i] * gain / pk;
    };
    const double nyq = fs * 0.45;

    if (instr & kPiano) {
        std::vector<double> v(n, 0.0);
        double t = 0.05;
        while (t < n / fs) {
            const double f0 = 440.0 * std::pow(2.0, (rng.range(40, 84) - 69) / 12.0);
            const double vel = rng.range(0.3, 1.0);
            const size_t s0 = size_t(t * fs);
            for (int h = 1; h <= 14; ++h) {
                const double f = h * f0 * std::sqrt(1 + 0.0004 * h * h);
                if (f > nyq) break;
                const double amp = vel / std::pow(h, 1.2), tau = 1.5 / std::pow(h, 0.7);
                const double ph = rng.uni() * 2 * kPi;
                const size_t len = std::min(n - s0, size_t(10 * tau * fs));
                for (size_t i = 0; i < len; ++i) {
                    const double tt = i / fs;
                    const double att = std::min(1.0, tt / 0.001) * std::min(1.0, (len - i) / (0.01 * fs));
                    v[s0 + i] += amp * att * std::exp(-tt / tau) * std::sin(2 * kPi * f * tt + ph);
                }
            }
            t += rng.range(0.15, 0.5);
        }
        addNorm(v, 0.30);
    }
    if (instr & kBrass) {
        std::vector<double> v(n, 0.0);
        Reso form; form.set(fs, 1200, 600);
        double t = 0.1;
        while (t < n / fs) {
            const double f0 = rng.range(100, 450), dur = rng.range(0.8, 2.0);
            const size_t s0 = size_t(t * fs), len = std::min(n - s0, size_t(dur * fs));
            double ph = 0;
            for (size_t i = 0; i < len; ++i) {
                const double tt = i / fs;
                const double f = f0 * (1 + 0.005 * std::sin(2 * kPi * 5.2 * tt));
                ph += 2 * kPi * f / fs;
                double s = 0;
                for (int h = 1; h * f < std::min(8000.0, nyq); ++h) s += std::sin(h * ph) / h;
                const double env = std::min(1.0, tt / 0.03) * std::min(1.0, (len - i) / (0.05 * fs));
                v[s0 + i] += env * s;
            }
            t += dur + rng.range(0.05, 0.4);
        }
        for (size_t i = 0; i < n; ++i) v[i] = 0.6 * v[i] + 2.0 * form.tick(v[i]);
        addNorm(v, 0.30);
    }
    if (instr & kVoice) {
        std::vector<double> v(n, 0.0);
        Reso f1, f2, f3;
        f1.set(fs, 600, 90); f2.set(fs, 1400, 110); f3.set(fs, 2600, 160);
        double t = 0.2;
        while (t < n / fs) {
            const double f0 = rng.range(110, 260), dur = rng.range(0.4, 1.5);
            const size_t s0 = size_t(t * fs), len = std::min(n - s0, size_t(dur * fs));
            double ph = 0;
            for (size_t i = 0; i < len; ++i) {
                const double tt = i / fs;
                ph += (f0 * (1 + 0.01 * rng.gauss() * 0.3)) / fs;
                if (ph >= 1) ph -= 1;
                // Glottal pulse (Rosenberg): opening then brief closing.
                double g = 0;
                if (ph < 0.4) g = 0.5 * (1 - std::cos(kPi * ph / 0.4));
                else if (ph < 0.56) g = std::cos(kPi * (ph - 0.4) / 0.32);
                const double env = std::min(1.0, tt / 0.04) * std::min(1.0, (len - i) / (0.06 * fs));
                v[s0 + i] += env * g;
            }
            t += dur + rng.range(0.1, 0.5);
        }
        for (size_t i = 0; i < n; ++i) { const double x = v[i]; v[i] = f1.tick(x) + 0.6 * f2.tick(x) + 0.3 * f3.tick(x); }
        // First difference: lip radiation (sharper impulses).
        for (size_t i = n - 1; i > 0; --i) v[i] -= v[i - 1];
        addNorm(v, 0.30);
    }
    if (instr & kPerc) {
        std::vector<double> v(n, 0.0);
        double t = 0.0;
        Reso hp; hp.set(fs, std::min(9000.0, nyq * 0.8), 6000);
        while (t < n / fs) {
            const size_t s0 = size_t(t * fs);
            const bool snare = rng.uni() < 0.35;
            const double tau = snare ? 0.09 : 0.035, a = rng.range(0.4, 1.0);
            const size_t len = std::min(n - s0, size_t(6 * tau * fs));
            for (size_t i = 0; i < len; ++i) {
                const double tt = i / fs, e = a * std::exp(-tt / tau) * std::min(1.0, tt / 0.0005);
                double s = rng.gauss();
                if (snare) s = 0.2 * rng.gauss() + 0.27 * std::sin(2 * kPi * 185 * tt);
                v[s0 + i] += e * s;
            }
            t += 0.25 * (1 + int(rng.uni() * 2));
        }
        for (size_t i = 0; i < n; ++i) v[i] = 3 * hp.tick(v[i]) + 0.3 * v[i];
        addNorm(v, 0.25);
    }
    if (instr & kHarpsi) {
        std::vector<double> v(n, 0.0);
        double t = 0.07;
        while (t < n / fs) {
            const double f0 = 440.0 * std::pow(2.0, (rng.range(45, 88) - 69) / 12.0);
            const int L = std::max(2, int(fs / f0));
            std::vector<double> d(L);
            for (auto& x : d) x = rng.range(-1, 1);
            const size_t s0 = size_t(t * fs), len = std::min(n - s0, size_t(1.2 * fs));
            int idx = 0;
            for (size_t i = 0; i < len; ++i) {
                const int nx = (idx + 1) % L;
                const double out = d[idx];
                d[idx] = 0.4985 * (d[idx] + d[nx]);
                idx = nx;
                v[s0 + i] += out * std::min(1.0, (len - i) / (0.01 * fs));
            }
            t += rng.range(0.12, 0.35);
        }
        addNorm(v, 0.25);
    }
    if (instr & kPulse) {
        // 30 % square wave, filtered at 5 kHz, 0.25 s notes: every edge
        // looks like a click, but repeats at every period.
        std::vector<double> v(n, 0.0);
        const int seq[6] = {72, 75, 79, 84, 79, 75};
        double ph = 0;
        const size_t step = size_t(0.25 * fs);
        for (size_t a = 0, k = 0; a < n; a += step, ++k) {
            const double f0 = 440.0 * std::pow(2.0, (seq[k % 6] - 69) / 12.0);
            for (size_t i = 0; i < step && a + i < n; ++i) {
                ph += f0 / fs; if (ph >= 1) ph -= 1;
                const double tt = i / fs;
                const double rel = std::min(1.0, double(step - i) / (0.005 * fs));  // 5 ms release
                v[a + i] = (ph < 0.3 ? 1.0 : -1.0) * std::min(1.0, tt / 0.002) * std::exp(-tt / 0.25) * rel;
            }
        }
        Reso lp; lp.set(fs, std::min(5000.0, nyq), 4000);
        for (size_t i = 0; i < n; ++i) v[i] = lp.tick(v[i]);
        addNorm(v, 0.30);
    }
    if (instr & kSurface) {
        // Surface noise: approximate pink noise, ≈ −66 dBFS RMS.
        double b0 = 0, b1 = 0, b2 = 0;
        for (size_t i = 0; i < n; ++i) {
            const double w = rng.gauss();
            b0 = 0.99765 * b0 + w * 0.0990460;
            b1 = 0.96300 * b1 + w * 0.2965164;
            b2 = 0.57000 * b2 + w * 1.0526913;
            mix[i] += (b0 + b1 + b2 + w * 0.1848) * 0.5e-3 * 0.25;
        }
    }
    std::vector<float> out(n);
    double pk = 1e-12;
    for (double x : mix) pk = std::max(pk, std::fabs(x));
    const double g = (pk > 0.6) ? 0.6 / pk : 1.0;
    for (size_t i = 0; i < n; ++i) out[i] = float(mix[i] * g);
    return out;
}

// Interleaved stereo: two slightly different mixes (panning).
inline std::vector<float> synthStereo(double fs, size_t frames, unsigned instr, uint64_t seed) {
    auto a = synthMono(fs, frames, instr, seed);
    auto b = synthMono(fs, frames, instr, seed + 7777);
    std::vector<float> out(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        out[2 * i] = 0.75f * a[i] + 0.25f * b[i];
        out[2 * i + 1] = 0.25f * a[i] + 0.75f * b[i];
    }
    return out;
}

struct ClickTruth { size_t start; int len; int ch; float amp; };

// Adds (additive) clicks to an interleaved signal; returns the ground truth.
inline std::vector<ClickTruth> addClicks(std::vector<float>& x, int nch, double fs, double ratePerSec,
                                         double maxLenMs, uint64_t seed) {
    Rng rng(seed);
    std::vector<ClickTruth> truth;
    const size_t frames = x.size() / nch;
    const double r = fs / 48000.0;
    const int maxLen = std::max(2, int(maxLenMs * 1e-3 * fs));
    const size_t minSep = size_t(4e-3 * fs);
    std::vector<size_t> last(nch, 0);
    double t = 0.05 + rng.range(0, 0.05);
    while (true) {
        t += -std::log(std::max(rng.uni(), 1e-9)) / ratePerSec;
        const size_t pos = size_t(t * fs);
        if (pos + maxLen + 16 >= frames) break;
        const double u = rng.uni();
        int len;
        if (u < 0.55) len = int(rng.range(1, 6 * r + 1));
        else if (u < 0.85) len = int(rng.range(6 * r, 30 * r));
        else len = int(rng.range(30 * r, maxLen));
        len = std::clamp(len, 1, maxLen);
        const bool both = rng.uni() < 0.4;
        const int firstCh = int(rng.uni() * nch) % nch;
        for (int c = 0; c < nch; ++c) {
            if (!both && c != firstCh) continue;
            if (pos < last[c] + minSep) continue;
            const float amp = float(std::pow(10.0, rng.range(-2.0, -0.3)));  // −40 .. −6 dBFS
            const double sign = rng.uni() < 0.5 ? -1 : 1;
            const int type = int(rng.uni() * 3);
            const double f = rng.range(3000, std::min(9000.0, fs * 0.4)), tau = rng.range(0.1e-3, 0.3e-3) * fs;
            for (int i = 0; i < len; ++i) {
                double s;
                if (type == 0) s = std::sin(kPi * (i + 0.5) / len) * (1 + 0.3 * rng.gauss());
                else if (type == 1) s = std::exp(-i / tau) * std::cos(2 * kPi * f * i / fs);
                else s = (1 + 0.2 * rng.gauss()) * ((i < len / 2 + 1) ? 1.0 : 0.7);
                x[(pos + i) * nch + c] += float(sign * amp * s);
            }
            truth.push_back({pos, len, c, amp});
            last[c] = pos + len;
        }
    }
    return truth;
}

} // namespace vctest
