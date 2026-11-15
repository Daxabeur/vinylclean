// vinylclean — tests of the playback anti-distortion.
// The distortion is produced by an EXACT PHYSICAL SIMULATION (round stylus
// rolling on the groove, pivoted arm), independent of the correction model
// (which is only first-order).
#include "antidist.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using namespace vc;
static int gPass = 0, gFail = 0;
static void check(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what); (ok ? gPass : gFail)++; }
static const double PI = 3.14159265358979323846;

// Amplitude of an exact frequency (Hann window) over x[a..a+N).
static double amp(const std::vector<double>& x, size_t a, size_t N, double f, double fs) {
    std::complex<double> s = 0;
    double wsum = 0;
    for (size_t i = 0; i < N; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * PI * (i + 0.5) / N);
        s += w * x[a + i] * std::polar(1.0, -2 * PI * f * double(i) / fs);
        wsum += w;
    }
    return 2 * std::abs(s) / wsum;
}

struct Tones { double f1 = 3000, f2 = 3700, U = 0.02; };   // groove velocities, m/s
// Groove velocity and wall displacement (exact) at time t.
static double dsp(const Tones& T, double t) { return -T.U * (std::cos(2 * PI * T.f1 * t) / (2 * PI * T.f1) + std::cos(2 * PI * T.f2 * t) / (2 * PI * T.f2)); }

// Velocity read from an EXACT stylus POSITION pos(t): derivative on a grid
// 32 times finer, then low-pass (windowed sinc, 20 kHz) and back to fs.
template <class F>
static std::vector<double> readExact(F pos, double fs, size_t n) {
    const int os = 32;
    const double fsf = fs * os;
    const size_t nf = n * os + 4096;
    std::vector<double> y(nf, 0.0);
    double prev = pos(-1.0 / fsf);
    for (size_t i = 0; i < nf; ++i) { const double h = pos(double(i) / fsf); y[i] = (h - prev) * fsf; prev = h; }
    const int K = 16 * os;
    std::vector<double> lp(2 * K + 1);
    double sum = 0;
    for (int kk = -K; kk <= K; ++kk) {
        const double x = kk / fsf, sinc = (kk == 0) ? 1.0 : std::sin(2 * PI * 20000 * x) / (2 * PI * 20000 * x);
        const double w = 0.42 + 0.5 * std::cos(PI * kk / K) + 0.08 * std::cos(2 * PI * kk / K);
        lp[kk + K] = sinc * w; sum += lp[kk + K];
    }
    std::vector<double> out(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const size_t c = i * os + 2048;
        double acc = 0;
        for (int kk = -K; kk <= K; ++kk) acc += lp[kk + K] * y[c + kk];
        out[i] = acc / sum;
    }
    return out;
}

// "Round stylus of radius rho" reading (exact): height of the center = max over
// the circle of z(s + σ) + √(ρ² − σ²), on a grid 32 times finer; velocity read
// = derivative; back to fs through a low-pass (windowed sinc, 20 kHz).
static std::vector<double> traceExact(const Tones& T, double rho, double v, double fs, size_t n) {
    const int os = 32;
    const double fsf = fs * os, ds = v / fsf;   // spatial step (m)
    const int half = int(rho / ds) + 1;
    const size_t nf = n * os + 4096;
    std::vector<double> z(nf + 2 * half), h(nf);
    for (size_t i = 0; i < z.size(); ++i) z[i] = dsp(T, (double(i) - half) / fsf);
    std::vector<double> circ(2 * half + 1);
    for (int k = -half; k <= half; ++k) { const double s = k * ds; circ[k + half] = (std::fabs(s) <= rho) ? std::sqrt(rho * rho - s * s) - rho : -1e9; }
    for (size_t i = 0; i < nf; ++i) {
        double m = -1e9;
        for (int k = 0; k <= 2 * half; ++k) m = std::max(m, z[i + k] + circ[k]);
        h[i] = m;
    }
    std::vector<double> y(nf, 0.0);
    for (size_t i = 1; i < nf; ++i) y[i] = (h[i] - h[i - 1]) * fsf;
    const int K = 16 * os;   // half-length of the low-pass
    std::vector<double> lp(2 * K + 1);
    double sum = 0;
    for (int k = -K; k <= K; ++k) {
        const double x = k / fsf, sinc = (k == 0) ? 1.0 : std::sin(2 * PI * 20000 * x) / (2 * PI * 20000 * x);
        const double w = 0.42 + 0.5 * std::cos(PI * k / K) + 0.08 * std::cos(2 * PI * k / K);
        lp[k + K] = sinc * w; sum += lp[k + K];
    }
    std::vector<double> out(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const size_t c = i * os + 2048;
        double acc = 0;
        for (int k = -K; k <= K; ++k) acc += lp[k + K] * y[c + k];
        out[i] = acc / sum;
    }
    return out;   // sample i ↔ time (i·os + 2048)/fsf
}

// Measurement: 2nd and 3rd order distortion products (f2−f1, 2f1, f1+f2, 2f2, 2f1−f2, 2f2−f1, 3f1) before / after.
struct Res { double before, after; };
static Res runCase(const std::vector<double>& yVel, double fs, double S, const AntiDistParams& prm, double rhoCorr, double scaleUsed, const Tones& T) {
    // Audio as it comes out of a preamp: RIAA de-emphasis of the velocity (units: m/s ÷ S).
    RiaaFilter de; de.design(fs);
    std::vector<float> a(yVel.size());
    for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<float>(de.tick(yVel[i] / S, false));
    std::vector<float> L = a, R = a, oL(a.size()), oR(a.size());
    AntiDistortion ad; ad.prepare(fs, 2); ad.setParams(prm); ad.forceScale(scaleUsed); ad.forceRho(rhoCorr);
    const float* in[2] = {L.data(), R.data()}; float* out[2] = {oL.data(), oR.data()};
    ad.process(in, out, int(a.size()));
    const int ld = ad.latency();
    const size_t a0 = size_t(fs * 0.5), N = size_t(fs * 1.0);
    std::vector<double> x(a.begin(), a.end()), y(oL.begin(), oL.end());
    auto prod = [&](const std::vector<double>& s, size_t off) {
        double p = 0;
        for (double f : {T.f2 - T.f1, 2 * T.f1, T.f1 + T.f2, 2 * T.f2, 2 * T.f1 - T.f2, 2 * T.f2 - T.f1, 3 * T.f1}) { const double m = amp(s, off, N, f, fs); p += m * m; }
        const double fund = amp(s, off, N, T.f1, fs);
        return 10 * std::log10(p / (fund * fund));
    };
    return {prod(x, a0), prod(y, a0 + ld)};
}

int main() {
    std::printf("=== vinylclean: anti-distortion tests ===\n");

    std::printf("\n1. Arm geometry (null points, offset, overhang)\n");
    for (auto al : {Alignment::Baerwald, Alignment::LofgrenB, Alignment::Stevenson}) {
        ArmGeometry g; g.effLenMm = 222; g.align = al; g.compute();
        const double e1 = g.trackingErrorDeg(g.r1), e2 = g.trackingErrorDeg(g.r2);
        double worst = 0, wr = 0;
        for (double r = 60.325; r <= 146.05; r += 0.25) { const double e = std::fabs(g.trackingErrorDeg(r)); if (e > worst) { worst = e; wr = r; } }
        static const char* nm[] = {"Baerwald", "Löfgren B", "Stevenson"};
        char msg[200];
        std::snprintf(msg, sizeof msg, "%-9s 222 mm: nulls %.2f / %.2f mm (error %.4f° / %.4f°), offset %.2f°, overhang %.2f mm, pivot–spindle %.1f mm, max error %.2f° at %.0f mm",
                      nm[int(al)], g.r1, g.r2, e1, e2, g.offsetDeg, g.overhangMm, g.mountMm, worst, wr);
        check(std::fabs(e1) < 1e-6 && std::fabs(e2) < 1e-6 && worst < 3.0, msg);
    }
    {
        ArmGeometry g; g.effLenMm = 222; g.align = Alignment::Stevenson; g.compute();
        const double ein = g.trackingErrorDeg(60.325);
        check(std::fabs(ein) < 1e-6, "Stevenson: zero error exactly at the end of the music (60.325 mm)");
    }

    const double fs = 48000;
    const size_t n = size_t(fs * 1.6);
    const double rpm = 100.0 / 3.0;

    std::printf("\n2. Tracing distortion (elliptical stylus 0.3 × 0.7, end of side, 2 tones at 2 cm/s)\n");
    {
        const double r = 117.42;   // Stevenson null point: only the stylus shape matters here
        const double rEnd = 65.0, v = 2 * PI * rEnd * 1e-3 * rpm / 60;
        (void)r;
        Tones T;
        const double rho = 7.6e-6;
        auto y = traceExact(T, rho, v, fs, n);
        AntiDistParams p; p.enabled = true; p.autoPos = false; p.posMm = rEnd; p.intensity = 1.0;
        p.arm.effLenMm = 222; p.arm.align = Alignment::Custom; p.arm.null1 = 64.0; p.arm.null2 = 66.0;   // tracking error ≈ 0 at 65 mm
        const double S = 0.05;
        auto r0 = runCase(y, fs, S, p, rho, S, T);
        auto rLo = runCase(y, fs, S, p, rho, S * 0.5, T);
        auto rHi = runCase(y, fs, S, p, rho, S * 1.5, T);
        char msg[200];
        std::snprintf(msg, sizeof msg, "exact level: distortion products %.1f dB → %.1f dB (−%.1f dB)", r0.before, r0.after, r0.before - r0.after);
        check(r0.before - r0.after >= 12.0, msg);
        std::snprintf(msg, sizeof msg, "level estimated 2 times too low: −%.1f dB; 1.5 times too high: −%.1f dB (always better)", rLo.before - rLo.after, rHi.before - rHi.after);
        check(rLo.before - rLo.after >= 4.0 && rHi.before - rHi.after >= 3.0, msg);
    }

    std::printf("\n2b. Hard case: each stylus type, end of side (61 mm), 2 loud tones at 5 cm/s\n");
    for (auto stt : {StylusType::Conical, StylusType::Elliptical37, StylusType::Elliptical27, StylusType::Shibata, StylusType::MicroLine}) {
        static const char* nm[] = {"conical 15.2 µm", "elliptical 0.3×0.7", "elliptical 0.2×0.7", "Shibata", "micro-line"};
        const double rEnd = 61.0, v = 2 * PI * rEnd * 1e-3 * rpm / 60;
        Tones T; T.U = 0.05;
        const double rho = stylusRadiusUm(stt, 0) * 1e-6;
        auto y = traceExact(T, rho, v, fs, n);
        AntiDistParams p; p.enabled = true; p.autoPos = false; p.posMm = rEnd; p.stylus = stt;
        p.arm.effLenMm = 222; p.arm.align = Alignment::Custom; p.arm.null1 = 60.0; p.arm.null2 = 62.0;
        const double S = 0.1;
        auto r0 = runCase(y, fs, S, p, rho, S, T);
        char msg[200];
        std::snprintf(msg, sizeof msg, "%-19s : %.1f dB → %.1f dB (−%.1f dB)", nm[int(stt)], r0.before, r0.after, r0.before - r0.after);
        check(r0.before - r0.after >= 10.0, msg);
    }

    std::printf("\n3. Arm tracking error (exact simulation of the shifted reading point)\n");
    {
        ArmGeometry g; g.effLenMm = 222; g.align = Alignment::Baerwald; g.compute();
        const double r = 146.05, v = 2 * PI * r * 1e-3 * rpm / 60;
        const double phi = g.trackingErrorDeg(r) * PI / 180;
        Tones T; T.U = 0.06;   // loud passage (6 cm/s per tone): the tracking error stays small
        T.f1 = 400; T.f2 = 3000;   // a low note (large displacement) modulates a high tone
        // EXACT stylus position: x = z(t − k·√2·x) (implicit equation, mono content:
        // lateral displacement √2·x), solved by iterations on a grid 32 times finer;
        // velocity = derivative of x, then back to fs as for tracing.
        const double k = std::tan(phi) / v;
        auto y = readExact([&](double t) {
            double x = dsp(T, t);
            for (int it = 0; it < 8; ++it) x = dsp(T, t - k * std::sqrt(2.0) * x);
            return x;
        }, fs, n);
        AntiDistParams p; p.enabled = true; p.autoPos = false; p.posMm = r; p.intensity = 1.0; p.arm = g;
        const double S = 0.15;
        auto r0 = runCase(y, fs, S, p, 0.0, S, T);
        char msg[200];
        std::snprintf(msg, sizeof msg, "Baerwald, edge of the record (error %.2f°): products %.1f dB → %.1f dB (−%.1f dB)", phi * 180 / PI, r0.before, r0.after, r0.before - r0.after);
        check(r0.before - r0.after >= 15.0, msg);
    }

    std::printf("\n4. Transparency and robustness\n");
    {
        std::mt19937 gen(3); std::normal_distribution<float> nd(0, 0.1f);
        std::vector<float> L(size_t(fs * 3)), R(L.size()), oL(L.size()), oR(L.size());
        for (size_t i = 0; i < L.size(); ++i) { L[i] = nd(gen); R[i] = nd(gen); if (i % 7919 == 0) L[i] += 0.9f; }
        AntiDistortion ad; ad.prepare(fs, 2);
        AntiDistParams p; p.enabled = false; ad.setParams(p);
        const float* in[2] = {L.data(), R.data()}; float* out[2] = {oL.data(), oR.data()};
        ad.process(in, out, int(L.size()));
        bool exact = true;
        for (size_t i = ad.latency(); i < L.size(); ++i) exact = exact && oL[i] == L[i - ad.latency()] && oR[i] == R[i - ad.latency()];
        check(exact, "off: bit-identical output (fixed delay)");
        AntiDistortion ad2; ad2.prepare(fs, 2);
        p.enabled = true; p.stylus = StylusType::Conical; p.intensity = 1.5; ad2.setParams(p);
        ad2.process(in, out, int(L.size()));
        bool fin = true; float mx = 0;
        for (size_t i = 0; i < L.size(); ++i) { fin = fin && std::isfinite(oL[i]) && std::isfinite(oR[i]); mx = std::max(mx, std::fabs(oL[i])); }
        check(fin && mx < 4.f, "loud noise + clicks, maximum settings: finite and bounded output");
    }
    {
        // Position: 6 s of silence (arm raised, record flipped) then 12 s of music; a 2 s gap between tracks must not reset anything.
        AntiDistortion ad; ad.prepare(fs, 2);
        AntiDistParams p; p.enabled = true; p.sideMin = 1.0;   // one-minute side: the radius must really go down
        ad.setParams(p);
        std::vector<float> L(size_t(fs * 18), 0.f), R(L.size(), 0.f), oL(L.size()), oR(L.size());
        for (size_t i = size_t(fs * 6); i < L.size(); ++i) { const bool gap = i >= size_t(fs * 10) && i < size_t(fs * 12); L[i] = R[i] = gap ? 0.f : 0.2f * float(std::sin(2 * PI * 440 * i / fs)); }
        const float* in[2] = {L.data(), R.data()}; float* out[2] = {oL.data(), oR.data()};
        ad.process(in, out, int(L.size()));
        auto st = ad.state();
        char msg[200];
        std::snprintf(msg, sizeof msg, "stylus drop detected: %.1f s of side, estimated radius %.1f mm (start 146.05), groove %.0f cm/s", st.sideSec, st.radiusMm, st.grooveCms);
        check(st.sideSec > 11.5 && st.sideSec < 12.5 && st.radiusMm < 146.0 && st.radiusMm > 120.0, msg);
    }

    std::printf("\nResult: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
