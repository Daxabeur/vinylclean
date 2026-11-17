// vinylclean — clock drift correction when the input and the output are two
// DIFFERENT sound cards. Each card has its own quartz: 48 000 Hz on one is for
// example 48 004 Hz on the other (≈ 100 ppm). Without correction the output
// slowly runs dry or overflows: a small dropout every few minutes.
//
// The processed sound goes through an adaptive resampler whose ratio follows
// the real speed of the output: a slow control loop (PI) keeps the number of
// frames waiting in the output buffer around its target. The ratio moves by a
// few millionths, far below anything audible (100 ppm = 0.0017 semitone).
//
// Resampler: windowed sinc (Kaiser, β = 12), 64 taps, 512 phases with linear
// interpolation between phases; error −125 dB at 1 kHz, −114 dB at 15 kHz
// (measured by the tests). Real time: no allocation after prepare().
// Only used with two cards: with one card the sound stays bit-exact.
// Original work based on textbook methods (Smith, "Digital Audio Resampling").
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vc {

class DriftResampler {
public:
    static constexpr int kHalf = 32;          // taps on each side
    static constexpr int kPhases = 512;

    void prepare(int channels, int maxIn) {
        nch_ = channels;
        const int taps = 2 * kHalf;
        table_.assign(size_t(kPhases + 1) * taps, 0.f);
        // Low-pass at 0.45 × fs (the ratio stays within ±0.5 %, no aliasing).
        const double fc = 0.455, beta = 12.0;
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double i0b = i0(beta);
        for (int ph = 0; ph <= kPhases; ++ph) {
            const double frac = double(ph) / kPhases;
            double sum = 0;
            for (int k = 0; k < taps; ++k) {
                const double t = (k - (kHalf - 1)) - frac;   // distance to the tap, in input samples
                const double x = t / kHalf;
                const double w = std::fabs(x) >= 1.0 ? 0.0 : i0(beta * std::sqrt(1.0 - x * x)) / i0b;
                const double s = t == 0.0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
                table_[size_t(ph) * taps + k] = float(s * w);
                sum += s * w;
            }
            for (int k = 0; k < taps; ++k) table_[size_t(ph) * taps + k] = float(table_[size_t(ph) * taps + k] / sum);   // unity gain
        }
        hist_.assign(size_t(channels) * (maxIn + 2 * taps + 8), 0.f);
        histLen_ = maxIn + 2 * taps + 8;
        fill_ = taps;           // history of zeros
        pos_ = 0.0;
    }
    // in[c][i] (n frames) → out[c][…] at `step` input frames per output frame.
    // Returns the number of frames written (≤ maxOut).
    int process(const float* const* in, int n, float* const* out, int maxOut, double step) {
        const int taps = 2 * kHalf;
        for (int c = 0; c < nch_; ++c) std::copy(in[c], in[c] + n, &hist_[size_t(c) * histLen_ + fill_]);
        fill_ += n;
        int k = 0;
        // Output sample at input position pos_ + (kHalf − 1) (centre of the taps).
        while (k < maxOut && pos_ + taps <= fill_) {
            const int ip = int(pos_);
            const double fr = (pos_ - ip) * kPhases;
            const int ph = int(fr);
            const float a = float(fr - ph);
            const float* t0 = &table_[size_t(ph) * taps];
            const float* t1 = t0 + taps;
            for (int c = 0; c < nch_; ++c) {
                const float* h = &hist_[size_t(c) * histLen_ + ip];
                float s0 = 0.f, s1 = 0.f;
                for (int j = 0; j < taps; ++j) { s0 += t0[j] * h[j]; s1 += t1[j] * h[j]; }
                out[c][k] = s0 + a * (s1 - s0);
            }
            ++k;
            pos_ += step;
        }
        // Keep only the history still needed.
        const int drop = std::min(int(pos_), fill_);
        if (drop > 0) {
            for (int c = 0; c < nch_; ++c) {
                float* h = &hist_[size_t(c) * histLen_];
                std::copy(h + drop, h + fill_, h);
            }
            fill_ -= drop;
            pos_ -= drop;
        }
        return k;
    }

private:
    int nch_ = 2, histLen_ = 0, fill_ = 0;
    double pos_ = 0.0;
    std::vector<float> table_, hist_;
};

// Control loop: from the number of frames waiting in the output buffer
// (measured once per period), the step of the resampler (input frames per
// output frame). PI controller (settles in ≈ 1 min), ratio limited to ±0.5 %.
class DriftControl {
public:
    void reset(double targetFrames, double rate) {
        target_ = targetFrames; rate_ = rate;
        integ_ = 0.0; avg_ = targetFrames; step_ = 1.0; first_ = true;
    }
    // delayFrames: frames waiting in the output (snd_pcm_delay); dt: seconds since the last call.
    double update(double delayFrames, double dt) {
        if (first_) { avg_ = delayFrames; first_ = false; }
        // The measurement jumps by a period at each write: smoothed over ≈ 1 s.
        const double a = std::min(1.0, dt / 1.0);
        avg_ += a * (delayFrames - avg_);
        const double e = (avg_ - target_) / rate_;          // seconds of sound in excess in the output
        integ_ += e * dt;
        integ_ = std::clamp(integ_, -0.5, 0.5);
        // Too much waiting in the output → produce fewer frames → larger step.
        step_ = 1.0 + kp_ * e + ki_ * integ_;
        step_ = std::clamp(step_, 0.995, 1.005);
        return step_;
    }
    double step() const { return step_; }
    double ppm() const { return (step_ - 1.0) * 1e6; }

private:
    double target_ = 0, rate_ = 48000, integ_ = 0, avg_ = 0, step_ = 1.0;
    double kp_ = 0.1, ki_ = 0.006;      // ω ≈ 0.08 rad/s, damping ≈ 0.65
    bool first_ = true;
};

} // namespace vc
