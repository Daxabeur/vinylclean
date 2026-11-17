// vinylclean — test of the side dynamics (sidemeter.h): integrated loudness
// (EBU Tech 3341 reference: stereo 1 kHz sine at −23 dBFS = −23 LUFS), DR of a
// sine (0 dB by definition), DR of a dynamic signal, new side after the arm
// is raised. Original work.
#include "sidemeter.h"
#include <cstdio>
#include <vector>
using namespace vc;

static int fails = 0;
static void check(const char* what, double got, double want, double tol) {
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%-52s %8.2f (expected %.2f ± %.2f)  %s\n", what, got, want, tol, ok ? "OK" : "FAIL");
    fails += !ok;
}
// Feeds `sec` seconds of a generator into the meter, 512-sample periods like the program.
template <class G> static void feed(SideTap& tap, SideMeter& m, double sec, G gen, unsigned rate = 48000) {
    const unsigned P = 512;
    std::vector<float> l(P), r(P);
    static double t = 0;
    for (double done = 0; done < sec; done += double(P) / rate) {
        for (unsigned i = 0; i < P; ++i, t += 1.0 / rate) { gen(t, l[i], r[i]); }
        m.add(tap.sums(l.data(), r.data(), P));
    }
    m.compute();
}
int main() {
    const unsigned rate = 48000;
    {
        SideTap tap; tap.prepare(rate); SideMeter m; m.reset(rate);
        const double a = std::pow(10.0, -23 / 20.0);
        feed(tap, m, 20, [&](double t, float& l, float& r) { l = r = float(a * std::sin(2 * M_PI * 1000 * t)); });
        check("stereo 1 kHz at -23 dBFS: loudness (LUFS)", m.lufs, -23.0, 0.1);
        check("same: DR of a sine (dB)", m.dr, 0.0, 0.1);
        check("same: PLR = peak - loudness (dB)", m.plr, 0.0, 0.1);
        check("same: duration of the side (s)", m.seconds, 20.0, 0.2);
    }
    {
        // Music-like signal: a loud passage (peaks 0.9, RMS ≈ −15 dBFS) and a quiet one.
        SideTap tap; tap.prepare(rate); SideMeter m; m.reset(rate);
        uint32_t s = 1;
        auto noise = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / double(1 << 24) * 2 - 1; };
        feed(tap, m, 30, [&](double t, float& l, float& r) {
            const double env = std::fmod(t, 6.0) < 3.0 ? 0.25 : 0.03;
            double v = env * noise() * 1.7;
            if (std::fmod(t, 1.5) < 1.0 / 48000) v = 0.9;     // one strong peak every 1.5 s
            l = r = float(v);
        });
        // Expected DR: loudest 20 % of the 3 s blocks = loud blocks, RMS√2 of
        // uniform noise of amplitude 0.425 = 0.425·√(2/3) → 20·log10(0.9 / 0.347) ≈ 8.3 dB.
        check("dynamic signal: DR (dB)", m.dr, 20 * std::log10(0.9 / (0.425 * std::sqrt(2.0 / 3.0))), 0.3);
        // Arm raised 9 s, then music again: a new side starts.
        const unsigned before = m.sideCount;
        feed(tap, m, 9, [&](double, float& l, float& r) { l = r = 0.f; });
        check("arm raised: duration not counted (s)", m.seconds, 30.0, 0.3);
        feed(tap, m, 5, [&](double t, float& l, float& r) { l = r = float(0.1 * std::sin(2 * M_PI * 440 * t)); });
        check("stylus dropped again: new side", double(m.sideCount - before), 1.0, 0.0);
        // A quiet passage of 3 s inside the side is NOT a new side.
        const unsigned b2 = m.sideCount;
        feed(tap, m, 3, [&](double, float& l, float& r) { l = r = 0.f; });
        feed(tap, m, 2, [&](double t, float& l, float& r) { l = r = float(0.1 * std::sin(2 * M_PI * 440 * t)); });
        check("3 s of silence inside the side: same side", double(m.sideCount - b2), 0.0, 0.0);
        check("... and the silence counts in the duration (s)", m.seconds, 10.0, 0.3);
    }
    {
        // Real conditions: the raised arm still gives HUM (50 Hz at −30 dBFS, built-in
        // preamp with a lot of gain) and a little hiss; the groove gives surface noise.
        SideTap tap; tap.prepare(rate); SideMeter m; m.reset(rate);
        uint32_t s = 7;
        auto noise = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / double(1 << 24) * 2 - 1; };
        auto music = [&](double t, float& l, float& r) { l = r = float(0.2 * std::sin(2 * M_PI * 440 * t) + 0.003 * noise()); };
        feed(tap, m, 20, music);
        const unsigned n0 = m.sideCount;
        // Gap between two tracks: only surface noise (−55 dBFS RMS) for 4 s.
        feed(tap, m, 4, [&](double, float& l, float& r) { l = r = float(0.0031 * noise()); });
        feed(tap, m, 5, music);
        check("4 s of surface noise between tracks: same side", double(m.sideCount - n0), 0.0, 0.0);
        // Arm raised: hum at −30 dBFS + hiss at −90 dBFS for 20 s, then the next side.
        feed(tap, m, 20, [&](double t, float& l, float& r) { l = r = float(0.0316 * std::sin(2 * M_PI * 50 * t) + 0.00003 * noise()); });
        check("arm raised with a loud hum: seen as arm raised", m.armUp ? 1.0 : 0.0, 1.0, 0.0);
        feed(tap, m, 6, music);
        check("... then the stylus is dropped: new side", double(m.sideCount - n0), 1.0, 0.0);
        check("... duration of the new side (s)", m.seconds, 6.0, 0.3);
    }
    std::printf(fails ? "\n%d FAILURE(S)\n" : "\nAll side dynamics tests passed.\n", fails);
    return fails ? 1 : 0;
}
