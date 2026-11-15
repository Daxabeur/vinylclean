// vinylclean — automatic tests of the declicker.
// Run: ./vc-tests        (returns 0 if everything passes)
//      ./vc-tests --full (longer: full table of intensities)
#include "declick.h"
#include "offline.h"
#include "synth.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace vc;
using namespace vctest;

static int gFail = 0, gPass = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    (ok ? gPass : gFail)++;
}

static std::vector<float> run(double fs, int nch, const std::vector<float>& x, const DeclickParams& p,
                              int block = 256, DeclickStats* st = nullptr, int* lat = nullptr) {
    Declicker d;
    d.prepare(fs, nch);
    d.setParams(p);
    auto y = processOffline(d, x, nch, block);
    if (st) *st = d.stats();
    if (lat) *lat = d.latency();
    return y;
}

struct Metrics {
    int clicks = 0, found = 0, bigClicks = 0, bigFound = 0;
    int falseRegions = 0;
    double minutes = 0, snrGainDb = 0, modifiedPct = 0;
    double medGain[3] = {0, 0, 0};  // median gain per click: short, medium, long
};

static double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Compares the output to the clean signal and to the ground truth.
static Metrics evaluate(const std::vector<float>& clean, const std::vector<float>& dirty, const std::vector<float>& out,
                        const std::vector<ClickTruth>& truth, int nch, double fs) {
    Metrics m;
    std::vector<double> gains[3];
    const size_t frames = clean.size() / nch;
    m.minutes = frames / fs / 60.0;
    std::vector<uint8_t> near(clean.size(), 0);
    const int tol = int(12 * fs / 48000.0) + 2;
    for (const auto& c : truth) {
        // Click detected if the repaired samples carry ≥ 80 % of its energy.
        double eTot = 0, eCov = 0;
        for (int i = 0; i < c.len; ++i) {
            const size_t k = (c.start + i) * nch + c.ch;
            const double d = double(dirty[k]) - clean[k];
            eTot += d * d;
            if (out[k] != dirty[k]) eCov += d * d;
        }
        const bool ok = eCov >= 0.8 * eTot;
        m.clicks++; m.found += ok;
        if (c.amp >= 0.03f) {
            // Click gain: energy of the error before / after processing.
            double ei = 0, eo = 0;
            for (size_t i = (c.start >= 12 ? c.start - 12 : 0); i < std::min(frames, c.start + c.len + 12); ++i) {
                const size_t k = i * nch + c.ch;
                ei += double(dirty[k] - clean[k]) * (dirty[k] - clean[k]);
                eo += double(out[k] - clean[k]) * (out[k] - clean[k]);
            }
            const double ms = c.len * 1000.0 / fs;
            gains[ms <= 0.13 ? 0 : ms <= 0.63 ? 1 : 2].push_back(10 * std::log10(ei / std::max(eo, 1e-20)));
        }
        if (c.amp >= 0.03f) { m.bigClicks++; m.bigFound += ok; }
        const size_t na = c.start >= size_t(tol) ? c.start - tol : 0, nb = std::min(frames, c.start + c.len + tol);
        for (size_t i = na; i < nb; ++i) near[i * nch + c.ch] = 1;
    }
    double eIn = 0, eOut = 0;
    size_t modified = 0;
    for (int c = 0; c < nch; ++c) {
        bool inRegion = false;
        for (size_t i = 0; i < frames; ++i) {
            const size_t k = i * nch + c;
            eIn += double(dirty[k] - clean[k]) * (dirty[k] - clean[k]);
            eOut += double(out[k] - clean[k]) * (out[k] - clean[k]);
            const bool mod = out[k] != dirty[k];
            modified += mod;
            const bool fals = mod && !near[k];
            if (fals && !inRegion) m.falseRegions++;
            inRegion = fals;
        }
    }
    m.snrGainDb = (eIn > 0 && eOut > 0) ? 10 * std::log10(eIn / eOut) : 0;
    m.modifiedPct = 100.0 * modified / clean.size();
    for (int b = 0; b < 3; ++b) m.medGain[b] = median(gains[b]);
    return m;
}

static bool allFinite(const std::vector<float>& y, float bound) {
    for (float v : y) if (!std::isfinite(v) || std::fabs(v) > bound) return false;
    return true;
}

int main(int argc, char** argv) {
    const bool full = argc > 1 && !std::strcmp(argv[1], "--full");
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");  // for the installation on the Pi
    std::vector<double> rates = {44100, 48000, 88200, 96000};
    if (quick) rates = {48000, 96000};
    const double secDetect = quick ? 10 : 20, secClean = quick ? 15 : 30;
    std::printf("=== vinylclean: declick tests ===\n");
    std::printf("Intensity → threshold k (multiple of the local robust standard deviation):\n  ");
    for (int I = 1; I <= 10; ++I) std::printf("%d→%.1f  ", I, declickThreshold(float(I)));
    std::printf("\n");

    // 1) Latency, null result in Bypass and at intensity 0, Removed mode ----------
    std::printf("\n1. Fixed latency and bit-exact transparency\n");
    for (double fs : rates) {
        const size_t F = size_t(fs * 3);
        auto x = synthStereo(fs, F, kAll, 11);
        addClicks(x, 2, fs, 20, 2.0, 12);
        DeclickParams p; p.intensity = 6;
        int lat = 0;
        DeclickStats st;
        p.mode = Mode::Bypass;
        auto yb = run(fs, 2, x, p, 256, nullptr, &lat);
        p.mode = Mode::Repair;
        auto yr = run(fs, 2, x, p, 256, &st);
        p.mode = Mode::Removed;
        auto yx = run(fs, 2, x, p);
        DeclickParams p0; p0.intensity = 0;
        auto y0 = run(fs, 2, x, p0);
        const bool nullB = (yb == x), null0 = (y0 == x);
        bool remOk = true;
        for (size_t i = 0; i < x.size(); ++i) if (yx[i] != x[i] - yr[i]) { remOk = false; break; }
        char msg[200];
        std::snprintf(msg, sizeof msg, "%5.1f kHz: latency %d samples (%.1f ms); Bypass bit-identical", fs / 1000, lat, 1000.0 * lat / fs);
        check(nullB, msg);
        std::snprintf(msg, sizeof msg, "%5.1f kHz: intensity 0 bit-identical", fs / 1000);
        check(null0, msg);
        std::snprintf(msg, sizeof msg, "%5.1f kHz: Removed = input − Repair (exact); %llu clicks repaired", fs / 1000, (unsigned long long)st.repaired);
        check(remOk && st.repaired > 0, msg);
    }

    // 2) Independence from the host block size -----------------------------------
    std::printf("\n2. Same result whatever the host block size\n");
    {
        const double fs = 48000;
        auto x = synthStereo(fs, size_t(fs * 2), kAll, 21);
        addClicks(x, 2, fs, 30, 2.0, 22);
        DeclickParams p; p.intensity = 7;
        auto ref = run(fs, 2, x, p, 256);
        bool same = true;
        for (int b : {1, 7, 64, 333, 1024, 4096}) same = same && (run(fs, 2, x, p, b) == ref);
        check(same, "blocks of 1, 7, 64, 333, 1024 and 4096: bit-identical outputs");
    }

    // 3) Efficiency on synthetic clicks -------------------------------------------
    std::printf("\n3. Detection and reconstruction (synthetic music + 15 clicks/s, %.0f s)\n", secDetect);
    std::printf("   \"realistic\" = piano + brass + voice + surface noise; \"dense\" = everything at once\n");
    std::printf("   (detection tested without multiband, which is tested separately in 7)\n");
    std::printf("   median gain per click: short ≤ 0.13 ms / medium ≤ 0.63 ms / long ≤ 2 ms\n");
    std::printf("   music     rate  int. pitch | detected (≥ −30 dBFS)  | false/min   | median gain s/m/l (dB)  | samples changed\n");
    Metrics mRef;
    struct { const char* name; unsigned m; } mixes[] = {{"realistic", kPiano | kBrass | kVoice | kSurface}, {"dense", kAll}};
    for (auto& mx : mixes) {
        if (quick && mx.m == kAll) continue;
        for (double fs : {48000.0, 96000.0}) {
            if (!full && fs != 48000.0) continue;
            const size_t F = size_t(fs * secDetect);
            auto clean = synthStereo(fs, F, mx.m, 31);
            auto dirty = clean;
            auto truth = addClicks(dirty, 2, fs, 15, 2.0, 32);
            std::vector<int> ints = full ? std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}
                                  : quick ? std::vector<int>{5} : std::vector<int>{3, 5, 7};
            for (int I : ints) {
                for (int pp = 1; pp >= (full ? 0 : 1); --pp) {
                    DeclickParams p; p.intensity = float(I); p.pitchProtect = pp; p.multiband = false;
                    auto y = run(fs, 2, dirty, p);
                    auto m = evaluate(clean, dirty, y, truth, 2, fs);
                    std::printf("   %-9s %4.0fk  %2d   %s  | %5.1f %%  (%5.1f %%)  |   %6.1f    |  %5.1f / %5.1f / %5.1f  | %.3f %%\n",
                                mx.name, fs / 1000, I, pp ? "yes" : "no ", 100.0 * m.found / m.clicks,
                                100.0 * m.bigFound / m.bigClicks, m.falseRegions / m.minutes,
                                m.medGain[0], m.medGain[1], m.medGain[2], m.modifiedPct);
                    if (fs == 48000 && I == 5 && pp && mx.m != kAll) mRef = m;
                }
            }
        }
    }
    check(100.0 * mRef.bigFound / mRef.bigClicks >= 90.0, "realistic, 48 kHz, intensity 5: ≥ 90 % of clicks ≥ −30 dBFS repaired");
    check(mRef.falseRegions / mRef.minutes <= 30.0, "realistic, 48 kHz, intensity 5: ≤ 30 false detections per minute");
    check(mRef.medGain[0] >= 15.0 && mRef.medGain[1] >= 8.0, "realistic, 48 kHz, intensity 5: short clicks −15 dB and medium −8 dB (medians)");

    // 4) False detections on CLEAN music, per instrument ---------------------------
    std::printf("\n4. Clean music without clicks (48 kHz, %.0f s per instrument, intensity 5)\n", secClean);
    std::printf("   instrument           | false det./min without protection | with protection (normal) | maximum\n");
    struct { const char* name; unsigned m; } instrs[] = {
        {"piano", kPiano | kSurface}, {"brass", kBrass | kSurface}, {"voice", kVoice | kSurface},
        {"percussion", kPerc | kSurface}, {"square-wave synth", kPulse | kSurface},
        {"harpsichord", kHarpsi | kSurface}, {"everything", kAll}};
    double worstProt = 0, pulseMax = 1e9, pulseNorm = 1e9;
    for (auto& in : instrs) {
        const bool informative = (in.m == (kHarpsi | kSurface) || in.m == kAll);
        if (quick && informative) continue;
        const double fs = 48000;
        auto clean = synthStereo(fs, size_t(fs * secClean), in.m, 41);
        double rate[3];
        for (int pp = 0; pp < 3; ++pp) {
            DeclickParams p; p.intensity = 5; p.protect = pp; p.multiband = false;
            auto y = run(fs, 2, clean, p);
            auto m = evaluate(clean, clean, y, {}, 2, fs);
            rate[pp] = m.falseRegions / m.minutes;
        }
        if (!informative && !(in.m & kPulse)) worstProt = std::max(worstProt, rate[1]);
        if (in.m & kPulse) { pulseMax = rate[2]; pulseNorm = rate[1]; }
        std::printf("   %-20s |        %7.1f                    |   %7.1f                |  %7.1f%s\n", in.name, rate[0], rate[1], rate[2],
                    informative ? "   (extreme case, for information)" : "");
    }
    check(worstProt <= 10.0, "normal protection: ≤ 10 false detections/min (piano, brass, voice, percussion)");
    // Square-wave synth: without protection, every edge is "repaired". What
    // remains with protection is at the change from one note to the next and
    // touches only a few samples.
    check(pulseNorm <= 100.0 && pulseMax <= 60.0, "synth with steep edges: normal protection ≤ 100/min, maximum ≤ 60/min (without: > 6000/min)");

    // 5) Numerical robustness ---------------------------------------------------------
    std::printf("\n5. Robustness (no NaN, no blow-up)\n");
    {
        const double fs = 48000;
        const size_t F = size_t(fs * 2);
        Rng rng(5);
        std::vector<std::pair<const char*, std::vector<float>>> sig;
        auto mk = [&](const char* name, auto gen) {
            std::vector<float> v(F * 2);
            for (size_t i = 0; i < F; ++i) { const float s = gen(i); v[2 * i] = s; v[2 * i + 1] = -s; }
            sig.push_back({name, v});
        };
        mk("digital silence", [](size_t) { return 0.f; });
        mk("DC 0.9", [](size_t) { return 0.9f; });
        mk("1 kHz full-scale square", [&](size_t i) { return ((i / 24) & 1) ? 1.f : -1.f; });
        mk("full-scale white noise", [&](size_t) { return float(rng.range(-1, 1)); });
        mk("±1 alternation (Nyquist)", [](size_t i) { return (i & 1) ? 1.f : -1.f; });
        mk("dense ±1 impulses", [&](size_t) { return rng.uni() < 0.02 ? (rng.uni() < 0.5 ? -1.f : 1.f) : 0.f; });
        mk("tiny values (denormals)", [&](size_t) { return float(rng.gauss() * 1e-39); });
        mk("20 Hz full-scale sine", [&](size_t i) { return float(std::sin(2 * kPi * 20 * i / fs)); });
        bool allOk = true;
        for (auto& s : sig) {
            for (int I : {1, 5, 10}) {
                DeclickParams p; p.intensity = float(I); p.maxClickMs = 5;
                auto y = run(fs, 2, s.second, p);
                const bool ok = allFinite(y, 4.f);
                if (!ok) { std::printf("    problem: %s, intensity %d\n", s.first, I); allOk = false; }
            }
        }
        check(allOk, "8 extreme signals × 3 intensities: finite and bounded outputs");
        // NaN at the input: the output must become clean again afterwards.
        auto x = synthStereo(fs, F, kAll, 51);
        for (int i = 1000; i < 1100; ++i) x[2 * i] = NAN;
        DeclickParams p; p.intensity = 8;
        Declicker d; d.prepare(fs, 2); d.setParams(p);
        auto y = processOffline(d, x, 2, 256);
        bool recovered = true;
        for (size_t i = size_t(fs * 0.5) * 2; i < y.size(); ++i) if (!std::isfinite(y[i])) { recovered = false; break; }
        check(recovered, "NaN injected at the input: the output becomes clean again (no lasting contamination)");
    }

    // 6) Mode change without a sudden jump ------------------------------------------
    std::printf("\n6. Mode change during playback\n");
    {
        const double fs = 48000;
        auto x = synthStereo(fs, size_t(fs * 2), kPiano | kSurface, 61);
        Declicker d; d.prepare(fs, 2);
        DeclickParams p; p.intensity = 5;
        std::vector<float> L(256), R(256), oL(256), oR(256);
        const float* ip[2] = {L.data(), R.data()};
        float* op[2] = {oL.data(), oR.data()};
        float maxJump = 0, prev = 0;
        for (size_t t = 0; t + 256 <= x.size() / 2; t += 256) {
            if (t == 256 * 100) { p.mode = Mode::Removed; }
            if (t == 256 * 200) { p.mode = Mode::Repair; }
            d.setParams(p);
            for (int i = 0; i < 256; ++i) { L[i] = x[2 * (t + i)]; R[i] = x[2 * (t + i) + 1]; }
            d.process(ip, op, 256);
            for (int i = 0; i < 256; ++i) { maxJump = std::max(maxJump, std::fabs(oL[i] - prev)); prev = oL[i]; }
        }
        float maxNat = 0;
        for (size_t i = 1; i < x.size() / 2; ++i) maxNat = std::max(maxNat, std::fabs(x[2 * i] - x[2 * i - 2]));
        check(maxJump <= maxNat * 1.05f + 1e-6f, "10 ms crossfade: no jump larger than those of the music");
    }


    // 7) Multiband repair ---------------------------------------------------------------
    std::printf("\n7. Multiband repair (realistic, 48 kHz, intensity 5)\n");
    {
        const double fs = 48000;
        auto clean = synthStereo(fs, size_t(fs * secDetect), kPiano | kBrass | kVoice | kPerc | kSurface, 71);
        auto dirty = clean;
        auto truth = addClicks(dirty, 2, fs, 15, 2.0, 72);
        DeclickParams p; p.intensity = 5;
        auto yOn = run(fs, 2, dirty, p);
        p.multiband = false;
        auto yOff = run(fs, 2, dirty, p);
        // Zero-phase filters (forward-backward RBJ biquads) to measure per band.
        auto zp = [&](std::vector<double> v, double fc, bool high) {
            const double w0 = 2 * 3.14159265358979 * fc / fs, al = std::sin(w0) / std::sqrt(2.0), cw = std::cos(w0), a0 = 1 + al;
            const double b0 = (high ? (1 + cw) / 2 : (1 - cw) / 2) / a0, b1 = (high ? -(1 + cw) : (1 - cw)) / a0, b2 = b0;
            const double a1 = -2 * cw / a0, a2 = (1 - al) / a0;
            for (int pass = 0; pass < 2; ++pass) {
                double z1 = 0, z2 = 0;
                for (size_t k = 0; k < v.size(); ++k) {
                    const size_t i = pass ? v.size() - 1 - k : k;
                    const double y = b0 * v[i] + z1; z1 = b1 * v[i] - a1 * y + z2; z2 = b2 * v[i] - a2 * y; v[i] = y;
                }
            }
            return v;
        };
        const size_t F = clean.size() / 2;
        double hfIn = 0, hfOn = 0, hfOff = 0, lfOn = 0, lfOff = 0, lfIn = 0;
        std::vector<double> gOn, gOff;
        for (int c = 0; c < 2; ++c) {
            std::vector<double> ed(F), eOn(F), eOff(F);
            for (size_t i = 0; i < F; ++i) {
                ed[i] = dirty[2 * i + c] - clean[2 * i + c];
                eOn[i] = yOn[2 * i + c] - clean[2 * i + c];
                eOff[i] = yOff[2 * i + c] - clean[2 * i + c];
            }
            auto hd = zp(ed, 3000, true), hOn = zp(eOn, 3000, true), hOff = zp(eOff, 3000, true);
            auto ld = zp(ed, 1000, false), lOn = zp(eOn, 1000, false), lOff = zp(eOff, 1000, false);
            for (const auto& t : truth) {
                if (t.ch != c) continue;
                const size_t a = t.start > 96 ? t.start - 96 : 0;
                const size_t b = std::min<size_t>(F, t.start + size_t(t.len) + 96);
                double d0 = 0, d1 = 0, d2 = 0;
                for (size_t i = a; i < b; ++i) {
                    d0 += hd[i] * hd[i]; d1 += hOn[i] * hOn[i]; d2 += hOff[i] * hOff[i];
                    lfIn += ld[i] * ld[i]; lfOn += lOn[i] * lOn[i]; lfOff += lOff[i] * lOff[i];
                }
                hfIn += d0; hfOn += d1; hfOff += d2;
                gOn.push_back(10 * std::log10(d0 / std::max(d1, 1e-30)));
                gOff.push_back(10 * std::log10(d0 / std::max(d2, 1e-30)));
            }
        }
        const double mOn = median(gOn), mOff = median(gOff);
        std::printf("   treble (> 3 kHz) of the click removed: median gain %.1f dB (multiband) / %.1f dB (full band)\n", mOn, mOff);
        std::printf("   bass (< 1 kHz) around the clicks, deviation from the clean music: %.1f dB (multiband) / %.1f dB (full band) / %.1f dB (no repair)\n",
                    10 * std::log10(lfOn + 1e-30), 10 * std::log10(lfOff + 1e-30), 10 * std::log10(lfIn + 1e-30));
        check(mOn >= mOff - 3.0, "multiband: the treble of the click is removed almost as well as in full band (≤ 3 dB difference)");
        // Clean music: multiband can only touch the music less.
        {
            auto cl = synthStereo(fs, size_t(fs * secClean), kPulse | kSurface, 73);
            DeclickParams q; q.intensity = 5; q.protect = 0;
            auto a = run(fs, 2, cl, q);
            q.multiband = false;
            auto b = run(fs, 2, cl, q);
            double ea = 0, eb = 0;
            for (size_t i = 0; i < cl.size(); ++i) { ea += double(a[i] - cl[i]) * (a[i] - cl[i]); eb += double(b[i] - cl[i]) * (b[i] - cl[i]); }
            std::printf("   synth without clicks, protection none: music changed %.1f dB (multiband) / %.1f dB (full band)\n",
                        10 * std::log10(ea + 1e-30), 10 * std::log10(eb + 1e-30));
            check(ea <= eb, "multiband: on music without clicks, it damages the music less than full band");
        }
        check(allFinite(yOn, 4.f), "multiband: finite and bounded output");
    }

    // 8) Stereo --------------------------------------------------------------------------
    std::printf("\n8. Stereo (both channels help each other)\n");
    {
        const double fs = 48000;
        const size_t F = size_t(fs * secDetect);
        // Mono record: identical channels, identical clicks → no change.
        {
            auto m = synthMono(fs, F, kPiano | kPerc | kSurface, 81);
            std::vector<float> x(2 * F);
            for (size_t i = 0; i < F; ++i) x[2 * i] = x[2 * i + 1] = m[i];
            std::vector<float> one(F);
            for (size_t i = 0; i < F; ++i) one[i] = x[2 * i];
            addClicks(one, 1, fs, 15, 2.0, 82);
            for (size_t i = 0; i < F; ++i) x[2 * i] = x[2 * i + 1] = one[i];
            DeclickParams q; q.intensity = 5;
            auto a = run(fs, 2, x, q);
            q.stereo = false;
            auto b = run(fs, 2, x, q);
            check(a == b, "mono record: the stereo test turns itself off (bit-identical result)");
        }
        // Real stereo: a common center + parts specific to each channel;
        // independent clicks per channel (as on real records).
        auto A = synthMono(fs, F, kPiano | kVoice | kPerc, 83), Bm = synthMono(fs, F, kBrass, 84), Cm = synthMono(fs, F, kHarpsi, 85);
        auto nL = synthMono(fs, F, kSurface, 86), nR = synthMono(fs, F, kSurface, 87);
        std::vector<float> clean(2 * F);
        for (size_t i = 0; i < F; ++i) {
            clean[2 * i] = 0.6f * A[i] + 0.25f * Bm[i] + nL[i];
            clean[2 * i + 1] = 0.55f * A[i >= 8 ? i - 8 : 0] + 0.25f * Cm[i] + nR[i];
        }
        // Independent clicks in each channel (on real records, 79 % of
        // clicks touch only one channel).
        auto dirty = clean;
        std::vector<ClickTruth> truth;
        for (int c = 0; c < 2; ++c) {
            std::vector<float> one(F);
            for (size_t i = 0; i < F; ++i) one[i] = dirty[2 * i + c];
            auto t = addClicks(one, 1, fs, 8, 2.0, 88 + c);
            for (auto& v : t) { v.ch = c; truth.push_back(v); }
            for (size_t i = 0; i < F; ++i) dirty[2 * i + c] = one[i];
        }
        DeclickParams q; q.intensity = 5;
        auto yOn = run(fs, 2, dirty, q);
        auto cOn = run(fs, 2, clean, q);
        q.protect = 2;
        auto cOn2 = run(fs, 2, clean, q);
        q.protect = 1; q.stereo = false;
        auto yOff = run(fs, 2, dirty, q);
        auto cOff = run(fs, 2, clean, q);
        q.protect = 2;
        auto cOff2 = run(fs, 2, clean, q);
        auto mOn = evaluate(clean, dirty, yOn, truth, 2, fs), mOff = evaluate(clean, dirty, yOff, truth, 2, fs);
        double dOn = 0, dOff = 0;
        for (size_t i = 0; i < clean.size(); ++i) { dOn += double(cOn[i] - clean[i]) * (cOn[i] - clean[i]); dOff += double(cOff[i] - clean[i]) * (cOff[i] - clean[i]); }
        std::printf("   clicks repaired: %.1f %% (stereo) / %.1f %% (separate channels); median gain short/medium/long %.1f/%.1f/%.1f dB / %.1f/%.1f/%.1f dB\n",
                    100.0 * mOn.found / mOn.clicks, 100.0 * mOff.found / mOff.clicks, mOn.medGain[0], mOn.medGain[1], mOn.medGain[2], mOff.medGain[0], mOff.medGain[1], mOff.medGain[2]);
        std::printf("   music without clicks (harpsichord on one side only) changed: %.1f dB (stereo) / %.1f dB (separate channels)\n", 10 * std::log10(dOn + 1e-30), 10 * std::log10(dOff + 1e-30));
        check(100.0 * mOn.found / mOn.clicks >= 100.0 * mOff.found / mOff.clicks - 1.0, "stereo: at least as many clicks repaired (within 1 point)");
        check(mOn.medGain[2] >= mOff.medGain[2] - 0.5, "stereo: long clicks repaired at least as well (help from the other channel)");
        auto countFalse = [&](const std::vector<float>& y) {
            int n = 0;
            for (int c = 0; c < 2; ++c) {
                long last = -100000;
                for (size_t i = 0; i < F; ++i)
                    if (y[2 * i + c] != clean[2 * i + c]) { if (long(i) - last > long(fs * 1e-3)) ++n; last = long(i); }
            }
            return n / (F / fs / 60.0);
        };
        const double fpmOn = countFalse(cOn), fpmOff = countFalse(cOff);
        std::printf("   false repairs: %.1f per minute (stereo) / %.1f (separate channels)\n", fpmOn, fpmOff);
        check(fpmOn <= fpmOff + 9.0, "stereo: at most a few more false repairs (≤ +9/min), even with an instrument on one side only");
        check(cOn2 == cOff2, "stereo: at maximum protection, music without clicks identical to the version without stereo");
        check(allFinite(yOn, 4.f), "stereo: finite and bounded output");
    }
    std::printf("\nResult: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
