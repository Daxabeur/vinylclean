// vinylclean — test of the VU meter level (levels.h, takeVu): RMS of each channel
// over the last 100 ms. A sine whose peak is at −18 dBFS must give −21.0 dBFS RMS
// (the page adds 3.01 dB: sine reference, 0 VU = −18 dBFS). Original work.
#include "levels.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace vc;
int main() {
    int fails = 0;
    auto check = [&](const char* what, double got, double want, double tol) {
        const bool ok = std::fabs(got - want) <= tol;
        std::printf("%-44s %8.2f dB (expected %.2f)  %s\n", what, got, want, ok ? "OK" : "FAIL");
        fails += !ok;
    };
    const unsigned rate = 48000, P = 512;
    LevelStats lv; lv.reset(rate);
    std::vector<float> l(P), r(P);
    double ph = 0;
    for (int k = 0; k < 10; ++k) {   // 10 periods ≈ 107 ms
        for (unsigned i = 0; i < P; ++i, ph += 2 * M_PI * 997.0 / rate) {
            l[i] = float(std::pow(10.0, -18 / 20.0) * std::sin(ph));
            r[i] = float(std::pow(10.0, -30 / 20.0) * std::sin(ph));
        }
        lv.add(levelSums(l.data(), r.data(), P));
    }
    double a, b; lv.takeVu(a, b);
    check("left: sine, peak -18 dBFS -> RMS", a, -18 - 3.0103, 0.05);
    check("right: sine, peak -30 dBFS -> RMS", b, -30 - 3.0103, 0.05);
    check("left + 3.01 - (-18) = VU reading", a + 3.0103 + 18, 0.0, 0.05);
    lv.takeVu(a, b);
    check("nothing since the last reading -> floor", a, -120, 0.0);
    for (auto& x : l) x = 0;
    for (auto& x : r) x = 0;
    lv.add(levelSums(l.data(), r.data(), P));
    lv.takeVu(a, b);
    check("digital silence -> floor", b, -120, 0.0);
    std::printf(fails ? "\n%d FAILURE(S)\n" : "\nAll VU tests passed.\n", fails);
    return fails ? 1 : 0;
}
