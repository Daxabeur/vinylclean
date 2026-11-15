// vinylclean — digital phono preamp: RIAA playback correction, gain and
// rumble filter, for a turntable connected WITHOUT a phono preamp.
//
// RIAA playback curve (IEC 60098): time constants 3180 µs, 318 µs,
// 75 µs; H(s) = (1 + s·318µ) / ((1 + s·3180µ)(1 + s·75µ)), 0 dB at 1 kHz.
//
// Digital implementation (original work): 3 real zeros and 3 real poles, as
// a cascade of first-order cells in double precision. For each common sample
// rate, the poles and zeros were optimized (differential evolution then
// Nelder-Mead, minimax error in dB from 20 Hz to 20 kHz) to match the analog
// curve: max deviation 0.012 dB at 44.1 kHz, 0.004 dB at 48 kHz, < 0.001 dB
// above (checked by vc-tests-riaa). A plain bilinear transform would be off
// by several dB near 20 kHz.
//
// Rumble filter: 2nd-order Butterworth high-pass at 15 Hz (−0.1 dB at 40 Hz,
// −12 dB at 7.5 Hz) against very low noises (warped record, motor), which the
// RIAA curve boosts by almost 20 dB.
#pragma once

#include <cmath>
#include <algorithm>
#include <complex>

namespace vc {

class RiaaFilter {
public:
    // Prepares the coefficients for fs (Hz).
    void design(double fs) {
        fs_ = fs;
        struct Row { double fs; double z[3], p[3]; };
        static const Row kTable[] = {
            {44100.0,  {0.9309346610600816, -0.534718709184501, -0.09739591859932302}, {0.992867082442533, 0.7378465611084036, -0.45627826161926305}},
            {48000.0,  {0.9365117262952483, -0.4622900342716722, -0.08032508602764088}, {0.9934617068812874, 0.7570569216604851, -0.3710347658236926}},
            {88200.0,  {0.9649695769795597, -0.7643072629927998, -0.07729170732302874}, {0.9964404456121185, 0.8596733817505402, -0.5392247045878176}},
            {96000.0,  {0.9677738279960899, -0.04504619799446589, -0.3092850743192052}, {0.9967296723548413, 0.8703243700413714, -0.19889361956457746}},
            {176400.0, {0.9823311155055667, -0.04648560534791739, -0.32040857083223323}, {0.998218903744537, 0.927200150624403, -0.20880617692898834}},
            {192000.0, {0.9837549898517346, -0.28922213172052613, -0.04058232331839336}, {0.9983634998410338, 0.9329119584166916, -0.17780621881366174}},
        };
        bool found = false;
        for (const Row& r : kTable)
            if (std::fabs(r.fs - fs) < 1.0) {
                for (int i = 0; i < 3; ++i) { z_[i] = r.z[i]; p_[i] = r.p[i]; }
                found = true;
            }
        if (!found) {
            // Other rate: "matched" poles and zero (exp(−T/τ)) + a fixed zero
            // compensating the aliasing near Nyquist (≈ 0.1 dB deviation).
            z_[0] = std::exp(-1.0 / (318e-6 * fs)); z_[1] = -0.12; z_[2] = 0.0;
            p_[0] = std::exp(-1.0 / (3180e-6 * fs)); p_[1] = std::exp(-1.0 / (75e-6 * fs)); p_[2] = 0.0;
            // Minimax fit (z1, z2, p2) by grid search, once (≈ 0.1 s, outside the audio thread).
            auto worst = [&]() {
                g_ = 1.0 / std::abs(response(1000.0, false));
                double w = 0.0;
                for (int i = 0; i < 120; ++i) {
                    const double f = 20.0 * std::pow(1000.0, i / 119.0);
                    if (f > 0.45 * fs) break;
                    w = std::max(w, std::fabs(20.0 * std::log10(std::abs(response(f))) - idealDb(f)));
                }
                return w;
            };
            double best = worst(), step[3] = {0.004, 0.05, 0.02};
            double* par[3] = {&z_[0], &z_[1], &p_[1]};
            for (int it = 0; it < 60; ++it) {
                bool moved = false;
                for (int k = 0; k < 3; ++k)
                    for (double d : {-step[k], step[k]}) {
                        const double old = *par[k];
                        *par[k] = old + d;
                        const double w = (std::fabs(*par[k]) < 0.9995) ? worst() : 1e9;
                        if (w < best) { best = w; moved = true; } else *par[k] = old;
                    }
                if (!moved) for (double& st : step) st *= 0.5;
            }
        }
        // Gain: 0 dB at 1 kHz.
        g_ = 1.0 / std::abs(response(1000.0, false));
        // Rumble filter: 2nd-order Butterworth, 15 Hz (bilinear transform, accurate at low frequency).
        const double w = std::tan(M_PI * 15.0 / fs), q = std::sqrt(2.0);
        const double a0 = 1.0 + q * w + w * w;
        hb0_ = 1.0 / a0; hb1_ = -2.0 / a0; hb2_ = 1.0 / a0;
        ha1_ = 2.0 * (w * w - 1.0) / a0; ha2_ = (1.0 - q * w + w * w) / a0;
        reset();
    }
    void reset() {
        for (int i = 0; i < 3; ++i) { xs_[i] = 0.0; ys_[i] = 0.0; }
        h1_ = h2_ = 0.0;
    }
    // One sample: RIAA (0 dB at 1 kHz) then optional rumble filter.
    inline double tick(double x, bool rumble) {
        double v = x * g_;
        for (int i = 0; i < 3; ++i) {   // y = v − z·v[n−1] + p·y[n−1]
            const double y = v - z_[i] * xs_[i] + p_[i] * ys_[i];
            xs_[i] = v; ys_[i] = y;
            v = y;
        }
        // The high-pass always runs (continuous state); we pick its output.
        const double h = hb0_ * v + h1_;
        h1_ = hb1_ * v - ha1_ * h + h2_;
        h2_ = hb2_ * v - ha2_ * h;
        return rumble ? h : v;
    }
    // Theoretical response of the digital filter (for the tests).
    std::complex<double> response(double f, bool withGain = true) const {
        const std::complex<double> zi = std::exp(std::complex<double>(0.0, -2.0 * M_PI * f / fs_));
        std::complex<double> h = withGain ? g_ : 1.0;
        for (int i = 0; i < 3; ++i) h *= (1.0 - z_[i] * zi) / (1.0 - p_[i] * zi);
        return h;
    }
    std::complex<double> rumbleResponse(double f) const {
        const std::complex<double> zi = std::exp(std::complex<double>(0.0, -2.0 * M_PI * f / fs_));
        return (hb0_ + hb1_ * zi + hb2_ * zi * zi) / (1.0 + ha1_ * zi + ha2_ * zi * zi);
    }
    double zero(int i) const { return z_[i]; }
    double pole(int i) const { return p_[i]; }
    double gain() const { return g_; }
    // Official (analog) curve, 0 dB at 1 kHz.
    static double idealDb(double f) {
        auto h = [](double ff) {
            const std::complex<double> s(0.0, 2.0 * M_PI * ff);
            return std::abs((1.0 + s * 318e-6) / ((1.0 + s * 3180e-6) * (1.0 + s * 75e-6)));
        };
        return 20.0 * std::log10(h(f) / h(1000.0));
    }

private:
    double fs_ = 48000.0, g_ = 1.0;
    double z_[3] = {0, 0, 0}, p_[3] = {0, 0, 0};
    double xs_[3] = {0, 0, 0}, ys_[3] = {0, 0, 0};
    double hb0_ = 1, hb1_ = 0, hb2_ = 0, ha1_ = 0, ha2_ = 0, h1_ = 0, h2_ = 0;
};

// Complete preamp for one channel: RIAA + rumble filter + gain, with fades
// (activation and gain) so the speakers never get a jump.
class PhonoStage {
public:
    void prepare(double fs) { f_.design(fs); fade_ = 1.0 / (0.05 * fs); }   // 50 ms fade
    void reset() { f_.reset(); }
    // on: RIAA active; gainDb: gain after RIAA; rumble: rumble filter.
    inline float tick(float x, bool on, double gainLin, bool rumble) {
        const double y = f_.tick(x, rumble);
        mix_ += on ? fade_ : -fade_;
        mix_ = mix_ < 0.0 ? 0.0 : mix_ > 1.0 ? 1.0 : mix_;
        g_ += (gainLin - g_) * fade_ * 4.0;      // smoothed gain (≈ 12 ms)
        return static_cast<float>(mix_ * y * g_ + (1.0 - mix_) * x);
    }
    bool settled(bool on) const { return on ? mix_ >= 1.0 : mix_ <= 0.0; }
    const RiaaFilter& filter() const { return f_; }
    void setMixNow(bool on, double gainLin) { mix_ = on ? 1.0 : 0.0; g_ = gainLin; }

private:
    RiaaFilter f_;
    double mix_ = 0.0, g_ = 1.0, fade_ = 1.0 / 2400.0;
};

} // namespace vc
