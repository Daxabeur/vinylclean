// vinylclean — CPU load measurement of the processing chain.
// Gives, for each sample rate, the percentage of ONE core used in stereo,
// on average and in the worst block (what matters to avoid dropouts).
#include "presets.h"
#include "synth.h"
#include "wav.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <time.h>

using namespace vc;

static double cpuNow() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct Result { double avgPct, worstPct; unsigned long long clicks; };

static Result bench(double fs, const std::vector<float>& x, const ChainParams& p, int blockReq) {
    Chain d;
    d.prepare(fs, 2);
    d.setParams(p);
    const int block = blockReq > 0 ? blockReq : d.hopSize();
    const size_t F = x.size() / 2;
    std::vector<float> L(block), R(block), oL(block), oR(block);
    const float* ip[2] = {L.data(), R.data()};
    float* op[2] = {oL.data(), oR.data()};
    double worst = 0;
    const double c0 = cpuNow();
    for (size_t t = 0; t + block <= F; t += block) {
        for (int i = 0; i < block; ++i) { L[i] = x[2 * (t + i)]; R[i] = x[2 * (t + i) + 1]; }
        const auto a = std::chrono::steady_clock::now();
        d.process(ip, op, block);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        if (t > F / 10) worst = std::max(worst, dt);  // ignores the start
    }
    const double cpu = cpuNow() - c0;
    const double dur = double(F) / fs;
    return {100.0 * cpu / dur, 100.0 * worst / (block / fs), (unsigned long long)(d.declickStats().repaired + d.decrackleStats().repaired)};
}

int main(int argc, char** argv) {
    double seconds = 20;
    int block = 0;  // 0 = hop size (like vc-live)
    std::string wavPath;
    std::vector<double> rates = {44100, 48000, 88200, 96000};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--block" && i + 1 < argc) block = std::atoi(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) rates = {std::atof(argv[++i])};
        else if (a == "--wav" && i + 1 < argc) wavPath = argv[++i];
        else { std::printf("Usage: vc-bench [--seconds S] [--block N] [--rate HZ] [--wav file.wav]\n"); return 1; }
    }
    if (block > 0) std::printf("=== vinylclean: load of the declick + decrackle chain (stereo, blocks of %d) ===\n", block);
    else std::printf("=== vinylclean: load of the declick + decrackle chain (stereo, blocks = vc-live period) ===\n");
    std::printf("100 %% = one fully busy core. The Pi 3B+ has 4.\n\n");
    std::printf("  rate    declick / decrackle              | average | worst blk | repairs\n");

    std::vector<float> wavSamples;
    int wavRate = 0;
    if (!wavPath.empty()) {
        WavData w; std::string err;
        if (!readWav(wavPath, w, err) || w.channels != 2) { std::printf("WAV error (stereo required): %s\n", err.c_str()); return 1; }
        wavSamples = w.samples; wavRate = w.sampleRate;
        rates = {double(wavRate)};
    }
    for (double fs : rates) {
        std::vector<float> x;
        if (!wavSamples.empty()) x = wavSamples;
        else { x = vctest::synthStereo(fs, size_t(fs * seconds), vctest::kAll, 99); vctest::addClicks(x, 2, fs, 15, 2.0, 98); }
        struct { const char* name; int s; } cfgs[] = {
            {"all off (0 / 0)", 0}, {"sensitivity 1 / 1", 1}, {"sensitivity 3 / 3", 3}, {"sensitivity 5 / 5", 5}};
        for (auto& c : cfgs) {
            ChainParams p = defaultChainParams();
            applySensitivity(p, c.s, c.s);
            auto r = bench(fs, x, p, block);
            std::printf("  %5.1fk  %-32s |  %5.1f %% |  %6.1f %% | %llu\n", fs / 1000, c.name, r.avgPct, r.worstPct, r.clicks);
        }
    }
    std::printf("\nTarget: average < 30 %% of one core for the whole chain.\n");
    std::printf("The \"worst block\" must stay well below 100 %% to avoid any dropout.\n");
    return 0;
}
