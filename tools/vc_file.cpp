// vinylclean — applies the Declick → Decrackle chain to a WAV file
// (exactly the same code as in real time).
#include "presets.h"
#include "riaa.h"
#include "offline.h"
#include "wav.h"
#include <chrono>
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace vc;

static void usage() {
    std::printf(
        "Usage: vc-file INPUT.wav OUTPUT.wav [options]\n"
        "  --sens N            sensitivity 0..5, declick and decrackle together (default 3, 0 = off)\n"
        "  --declick-sens N    sensitivity of the declicker only 0..5\n"
        "  --decrackle-sens N  sensitivity of the decrackler only 0..5\n"
        "  --declick X      (advanced) internal declick intensity 0..10\n"
        "  --decrackle X    (advanced) internal decrackle intensity 0..10\n"
        "  --maxclick MS    maximum click length in ms (0.2 .. 5)\n"
        "  --protect N      music protection: 0 none, 1 normal, 2 maximum\n"
        "  --pitch on|off   protection of onsets and periodic sounds (default on)\n"
        "  --multiband on|off  multiband repair (default on)\n"
        "  --stereo on|off  use the other channel: detection and long clicks (default on)\n"
        "  --riaa           apply the RIAA correction (file recorded WITHOUT a phono preamp)\n"
        "  --phono-gain DB  gain after the RIAA (default 0)\n"
        "  --rumble off     no rumble filter (with --riaa)\n"
        "  --antidist on    playback anti-distortion (file = ONE side, from its start: the position is estimated)\n"
        "  --arm-length MM  --align baerwald|lofgren|stevenson  --nulls R1,R2  --stylus conical|elliptical|fine-elliptical|shibata|microline|µm\n"
        "  --disc 33|45|maxi  --side-min MIN  --radius MM (fixed position)  --dist-int X (0..1.5)\n"
        "  --mode M         repair (default) | removed | bypass\n"
        "  --float          writes the output as 32-bit float\n"
        "The output is aligned with the input (latency compensated): both files\n"
        "can be compared in an audio editor.\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    std::string inPath = argv[1], outPath = argv[2];
    ChainParams p = defaultChainParams();
    bool asFloat = false, riaa = false, rumble = true;
    double phonoGainDb = 0.0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(1); } return argv[++i]; };
        if (a == "--sens") { const int v = std::atoi(next().c_str()); p.declick.intensity = declickIntensityForSens(v); p.decrackle.intensity = decrackleIntensityForSens(v); }
        else if (a == "--declick-sens") p.declick.intensity = declickIntensityForSens(std::atoi(next().c_str()));
        else if (a == "--decrackle-sens") p.decrackle.intensity = decrackleIntensityForSens(std::atoi(next().c_str()));
        else if (a == "--declick") p.declick.intensity = std::strtof(next().c_str(), nullptr);
        else if (a == "--decrackle") p.decrackle.intensity = std::strtof(next().c_str(), nullptr);
        else if (a == "--maxclick") p.declick.maxClickMs = std::strtof(next().c_str(), nullptr);
        else if (a == "--pitch") { const bool on = (next() != "off"); p.declick.pitchProtect = p.decrackle.pitchProtect = on; }
        else if (a == "--stereo") { const bool on = (next() != "off"); p.declick.stereo = p.decrackle.stereo = on; }
        else if (a == "--multiband") { const bool on = (next() != "off"); p.declick.multiband = p.decrackle.multiband = on; }
        else if (a == "--protect") { const int v = std::atoi(next().c_str()); p.declick.protect = p.decrackle.protect = v; }
        else if (a == "--mode") p.mode = modeFromName(next().c_str());
        else if (a == "--float") asFloat = true;
        else if (a == "--riaa") riaa = true;
        else if (a == "--phono-gain") phonoGainDb = std::atof(next().c_str());
        else if (a == "--rumble") rumble = (next() != "off");
        else if (a == "--antidist") p.dist.enabled = (next() != "off");
        else if (a == "--arm-length") p.dist.arm.effLenMm = std::fmin(400.0, std::fmax(150.0, std::atof(next().c_str())));
        else if (a == "--align") { const std::string v = next(); if (!alignFromName(v, p.dist.arm.align)) { std::printf("unknown alignment: %s\n", v.c_str()); return 1; } }
        else if (a == "--nulls") { const std::string v = next(); double a1 = 0, a2 = 0; if (std::sscanf(v.c_str(), "%lf,%lf", &a1, &a2) != 2) { std::printf("--nulls expects r1,r2 in mm\n"); return 1; } p.dist.arm.align = Alignment::Custom; p.dist.arm.null1 = a1; p.dist.arm.null2 = a2; }
        else if (a == "--stylus") { const std::string v = next(); if (!stylusFromName(v, p.dist.stylus, p.dist.stylusUm)) { std::printf("unknown stylus: %s\n", v.c_str()); return 1; } }
        else if (a == "--disc") { const std::string v = next(); if (!discFromName(v, p.dist.disc.fmt)) { std::printf("unknown format: %s\n", v.c_str()); return 1; } }
        else if (a == "--side-min") p.dist.sideMin = std::fmin(40.0, std::fmax(2.0, std::atof(next().c_str())));
        else if (a == "--radius") { p.dist.autoPos = false; p.dist.posMm = std::fmin(150.0, std::fmax(50.0, std::atof(next().c_str()))); }
        else if (a == "--dist-int") p.dist.intensity = std::fmin(1.5, std::fmax(0.0, std::atof(next().c_str())));
        else { std::printf("unknown option: %s\n", a.c_str()); usage(); return 1; }
    }
    p.declick.intensity = std::fmax(0.f, std::fmin(10.f, p.declick.intensity));
    p.decrackle.intensity = std::fmax(0.f, std::fmin(10.f, p.decrackle.intensity));
    p.declick.maxClickMs = std::fmax(0.2f, std::fmin(5.f, p.declick.maxClickMs));

    WavData w;
    std::string err;
    if (!readWav(inPath, w, err)) { std::printf("Error: %s\n", err.c_str()); return 1; }
    Chain ch;
    if (!ch.prepare(w.sampleRate, w.channels)) { std::printf("Error: unsupported sample rate or channel count\n"); return 1; }
    ch.setParams(p);
    if (p.dist.enabled && p.dist.autoPos) ch.distStartSide();   // the file starts at the beginning of a side

    const double dur = double(w.frames()) / w.sampleRate;
    std::printf("Input: %s — %d Hz, %d channel(s), %d bits%s, %.1f s\n", inPath.c_str(), w.sampleRate, w.channels,
                w.bits, w.isFloat ? " float" : "", dur);
    std::printf("Settings: declick %.1f (max click %.1f ms), decrackle %.1f, pitch protection %s, mode %s\n",
                p.declick.intensity, p.declick.maxClickMs, p.decrackle.intensity,
                p.declick.pitchProtect ? "on" : "off", modeName(p.mode));

    if (riaa) {
        const int nc = w.channels;
        const double g = std::pow(10.0, phonoGainDb / 20.0);
        std::vector<PhonoStage> ph(static_cast<size_t>(nc));
        for (auto& p2 : ph) { p2.prepare(w.sampleRate); p2.setMixNow(true, g); }
        float peak = 0.f;
        for (size_t i = 0; i < w.samples.size(); ++i) {
            float& v = w.samples[i];
            v = ph[i % size_t(nc)].tick(v, true, g, rumble);
            peak = std::fmax(peak, std::fabs(v));
        }
        std::printf("RIAA correction applied (gain %+.1f dB, rumble filter %s); peak after RIAA %.1f dBFS%s\n", phonoGainDb,
                    rumble ? "on" : "off", 20.0 * std::log10(std::fmax(peak, 1e-9f)), peak >= 1.f ? " — WARNING: above 0 dBFS, lower --phono-gain" : "");
    }
    // Input peak: warn if the recording clips (nothing can repair that).
    size_t clipped = 0;
    for (float v : w.samples) clipped += (std::fabs(v) >= 0.999f);

    const auto t0 = std::chrono::steady_clock::now();
    auto y = processOffline(ch, w.samples, w.channels, 512);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    WavData o = w;
    o.samples.swap(y);
    if (asFloat) { o.isFloat = true; o.bits = 32; }
    if (!writeWav(outPath, o, err)) { std::printf("Error: %s\n", err.c_str()); return 1; }

    const auto a = ch.declickStats(), b = ch.decrackleStats();
    std::printf("Output: %s\n", outPath.c_str());
    std::printf("Clicks repaired: %llu (%.1f/s); crackles repaired: %llu (%.1f/s)\n",
                (unsigned long long)a.repaired, a.repaired / std::fmax(dur, 1e-9),
                (unsigned long long)b.repaired, b.repaired / std::fmax(dur, 1e-9));
    std::printf("Protected (onsets / periodic sounds): %llu clicks, %llu crackles\n",
                (unsigned long long)a.protectedHits, (unsigned long long)b.protectedHits);
    if (p.dist.enabled) {
        const AntiDistState ds = ch.distState();
        ArmGeometry g = p.dist.arm; g.compute();
        std::printf("Anti-distortion: arm %.0f mm (offset %.2f°, overhang %.2f mm), stylus %.1f µm; end of file: radius %.1f mm, tracking error %+.2f°, groove %.0f cm/s%s\n",
                    g.effLenMm, g.offsetDeg, g.overhangMm, stylusRadiusUm(p.dist.stylus, p.dist.stylusUm), ds.radiusMm, ds.trackDeg, ds.grooveCms,
                    ds.levelReady ? "" : " (level not estimated yet: file too short)");
    }
    std::printf("Chain latency: %d samples (%.1f ms), compensated in the file\n", ch.latency(),
                1000.0 * ch.latency() / w.sampleRate);
    std::printf("Computing time: %.2f s, i.e. %.1f %% of one core for real time\n", secs, 100.0 * secs / std::fmax(dur, 1e-9));
    if (clipped > 0)
        std::printf("WARNING: %zu samples clipped at 0 dBFS in the input. Clipping cannot be repaired:\n"
                    "lower the input gain of your sound card.\n", clipped);
    return 0;
}
