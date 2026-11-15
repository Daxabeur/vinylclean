// vinylclean — vc-live: REAL-TIME cleaning while listening.
// Sound card input → declick → decrackle → output (direct ALSA),
// with a control page (phone or touchscreen) served by the Pi.
//
// "Audiophile" choices:
//  - hw: device (no hidden ALSA conversion) and the card's best native
//    format: outside the repaired clicks, the signal comes out
//    bit-identical;
//  - no resampling: the card runs at the requested rate;
//  - audio thread at real-time priority (SCHED_FIFO) and locked memory;
//  - ALSA period = internal rhythm of the declicker: even load, no peaks.
//
// Threads: audio (real time); main (keyboard, status, reconfiguration,
// saving); web (low priority). The audio thread only reads atomics:
// no lock, no allocation, no I/O other than ALSA.
#include "presets.h"
#include "web_server.h"
#include "index_html.h"
#include <alsa/asoundlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

using namespace vc;

static std::atomic<bool> gRun{true};
static void onSignal(int) { gRun = false; }

static const unsigned kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};

#include "loudness.h"
#include "levels.h"
#include "riaa.h"
#ifdef VC_RECORD
#include "recorder.h"
#endif

// ---- Shared parameters (web / keyboard → audio), lock-free ----
struct Shared {
    std::atomic<float> declick{6.f};
    std::atomic<float> crackle{4.f};
    std::atomic<int>   protect{1};
    std::atomic<int>   mode{2};
    std::atomic<float> maxClick{2.5f};
    std::atomic<bool>  multiband{true};
    std::atomic<bool>  stereo{true};
    std::atomic<bool>  riaa{false};          // built-in phono preamp (turntable connected directly)
    std::atomic<float> phonoGain{1.f};       // linear gain after the RIAA
    std::atomic<bool>  rumble{true};         // rumble filter
    std::atomic<unsigned> satPhono{0};       // the phono gain makes the signal clip (after RIAA)
    // Playback anti-distortion (turntable and cartridge)
    std::atomic<bool>  adOn{false}, adAutoPos{true};
    std::atomic<float> adArmLen{222.f}, adNull1{60.325f}, adNull2{117.42f}, adStylusUm{7.6f};
    std::atomic<float> adPosMm{100.f}, adSideMin{20.f}, adInt{1.f};
    std::atomic<int>   adAlign{2}, adStylus{2}, adDisc{0};
    std::atomic<unsigned> adNewSide{0};      // "start of side" button
    std::atomic<float> adRadius{0.f}, adTrack{0.f}, adGroove{0.f}, adSide{-1.f};
    std::atomic<bool>  adReady{false};
    // Measurements (audio → display)
    std::atomic<unsigned long long> repaired{0}, crackles{0};
    std::atomic<unsigned> xruns{0};
    std::atomic<float> load{0.f}, loadMax{0.f};
    std::atomic<float> peakIn[2], peakOut[2];
    std::atomic<unsigned> satCount{0};      // input samples at 0 dBFS (clipping)
    std::atomic<bool> rtOk{false};
    std::atomic<bool> failed{false};
    LoudRing loud;
    LevelRing lv;                  // balance / stereo / noise sums (after the phono preamp)
    std::atomic<bool> recClean{false};   // the recording in progress takes the cleaned sound
#ifdef VC_RECORD
    Recorder* rec = nullptr;      // RAW turntable sound → file (before any processing)
#endif
    Shared() { for (int c = 0; c < 2; ++c) { peakIn[c] = 0.f; peakOut[c] = 0.f; } }
};

// ---- Persistent settings ----
struct Settings {
    float declick = declickIntensityForSens(kDefaultDeclickSens);
    float decrackle = decrackleIntensityForSens(kDefaultDecrackleSens);
    float maxClick = kDefaultMaxClickMs;
    int protect = kDefaultProtect, mode = 2;
    bool multiband = kDefaultMultiband;
    bool stereo = kDefaultStereo;
    bool riaa = false;
    float phonoGainDb = 12.f;
    bool rumble = true;
    bool recClean = false;         // recording: false = raw turntable sound, true = cleaned sound
    // Anti-distortion
    bool adOn = false, adAutoPos = true;
    float armLen = 222.f, null1 = 60.325f, null2 = 117.42f, stylusUm = 7.6f;
    float posMm = 100.f, sideMin = 20.f, adInt = 1.f;
    int align = 2, stylus = 2, disc = 0;
    std::string inDev = "hw:0,0", outDev = "hw:0,0";
    unsigned rate = 48000;
    int periods = 3;
};

// Common sensitivity (single slider): -1 if declick and decrackle differ ("custom").
static int commonSens(const Settings& s) {
    const int a = declickSensForIntensity(s.declick), b = decrackleSensForIntensity(s.decrackle);
    return (a >= 0 && a == b) ? a : -1;
}
static std::string settingsToJson(const Settings& s) {
    char b[2048];
    std::snprintf(b, sizeof b,
                  "{\"scale\":3,\"ad\":%d,\"armLen\":%g,\"align\":%d,\"null1\":%g,\"null2\":%g,\"stylus\":%d,\"stylusUm\":%g,\"disc\":%d,\"autoPos\":%d,\"posMm\":%g,\"sideMin\":%g,\"adInt\":%g,"
                  "\"sens\":%d,\"sensDeclick\":%d,\"sensDecrackle\":%d,\"declick\":%g,\"decrackle\":%g,\"protect\":%d,\"maxclick\":%g,\"multiband\":%d,\"stereo\":%d,\"riaa\":%d,\"phonoGain\":%g,\"rumble\":%d,\"recClean\":%d,\"mode\":%d,"
                  "\"in\":\"%s\",\"out\":\"%s\",\"rate\":%u,\"periods\":%d}",
                  s.adOn ? 1 : 0, s.armLen, s.align, s.null1, s.null2, s.stylus, s.stylusUm, s.disc, s.adAutoPos ? 1 : 0, s.posMm, s.sideMin, s.adInt,
                  commonSens(s), declickSensForIntensity(s.declick), decrackleSensForIntensity(s.decrackle), s.declick, s.decrackle, s.protect, s.maxClick, s.multiband ? 1 : 0, s.stereo ? 1 : 0, s.riaa ? 1 : 0, s.phonoGainDb, s.rumble ? 1 : 0, s.recClean ? 1 : 0, s.mode,
                  jsonEscape(s.inDev).c_str(), jsonEscape(s.outDev).c_str(), s.rate, s.periods);
    return b;
}
static void settingsFromJson(const JsonFlat& j, Settings& s) {
    if (j.has("sens")) { const int v = int(j.num("sens")); s.declick = declickIntensityForSens(v); s.decrackle = decrackleIntensityForSens(v); }
    if (j.has("sensDeclick")) s.declick = declickIntensityForSens(int(j.num("sensDeclick")));
    if (j.has("sensDecrackle")) s.decrackle = decrackleIntensityForSens(int(j.num("sensDecrackle")));
    if (j.has("declick")) s.declick = float(std::fmin(10, std::fmax(0, j.num("declick"))));
    if (j.has("decrackle")) s.decrackle = float(std::fmin(10, std::fmax(0, j.num("decrackle"))));
    if (j.has("protect")) s.protect = int(std::fmin(2, std::fmax(0, j.num("protect"))));
    if (j.has("maxclick")) s.maxClick = float(std::fmin(5, std::fmax(0.5, j.num("maxclick"))));
    if (j.has("multiband")) s.multiband = j.num("multiband") != 0;
    if (j.has("stereo")) s.stereo = j.num("stereo") != 0;
    if (j.has("riaa")) s.riaa = j.num("riaa") != 0;
    if (j.has("phonoGain")) s.phonoGainDb = float(std::fmin(40, std::fmax(0, j.num("phonoGain"))));
    if (j.has("rumble")) s.rumble = j.num("rumble") != 0;
    if (j.has("recClean")) s.recClean = j.num("recClean") != 0;
    auto clampf = [](double v, double lo, double hi) { return float(std::fmin(hi, std::fmax(lo, v))); };
    if (j.has("ad")) s.adOn = j.num("ad") != 0;
    if (j.has("armLen")) s.armLen = clampf(j.num("armLen"), 150, 400);
    if (j.has("align")) s.align = int(clampf(j.num("align"), 0, 3));
    if (j.has("null1")) s.null1 = clampf(j.num("null1"), 50, 140);
    if (j.has("null2")) s.null2 = clampf(j.num("null2"), 50, 146);
    if (j.has("stylus")) s.stylus = int(clampf(j.num("stylus"), 0, 5));
    if (j.has("stylusUm")) s.stylusUm = clampf(j.num("stylusUm"), 1, 25);
    if (j.has("disc")) s.disc = int(clampf(j.num("disc"), 0, 2));
    if (j.has("autoPos")) s.adAutoPos = j.num("autoPos") != 0;
    if (j.has("posMm")) s.posMm = clampf(j.num("posMm"), 50, 150);
    if (j.has("sideMin")) s.sideMin = clampf(j.num("sideMin"), 2, 40);
    if (j.has("adInt")) s.adInt = clampf(j.num("adInt"), 0, 1.5);
    if (j.has("mode")) s.mode = int(std::fmin(2, std::fmax(0, j.num("mode"))));
    if (j.has("in")) s.inDev = j.str("in");
    if (j.has("out")) s.outDev = j.str("out");
    if (j.has("rate")) s.rate = unsigned(j.num("rate"));
    if (j.has("periods")) s.periods = int(std::fmin(8, std::fmax(2, j.num("periods"))));
}
// ---- Conversion between the card format and ±1 floats ----
struct Format { snd_pcm_format_t f; int bytes; const char* name; };
static const Format kFormats[] = {
    {SND_PCM_FORMAT_S32_LE, 4, "32 bits"},
    {SND_PCM_FORMAT_S24_3LE, 3, "24 bits"},
    {SND_PCM_FORMAT_S24_LE, 4, "24 bits (in 32)"},
    {SND_PCM_FORMAT_S16_LE, 2, "16 bits"},
};

static inline float toFloat(const uint8_t* p, snd_pcm_format_t f) {
    switch (f) {
        case SND_PCM_FORMAT_S32_LE: { int32_t v; std::memcpy(&v, p, 4); return v * (1.f / 2147483648.f); }
        case SND_PCM_FORMAT_S24_3LE: { int32_t v = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8; return v * (1.f / 8388608.f); }
        case SND_PCM_FORMAT_S24_LE: { int32_t v; std::memcpy(&v, p, 4); v = int32_t(uint32_t(v) << 8) >> 8; return v * (1.f / 8388608.f); }
        default: { int16_t v; std::memcpy(&v, p, 2); return v * (1.f / 32768.f); }
    }
}
static inline void fromFloat(float x, uint8_t* p, snd_pcm_format_t f) {
    // Multiplications by powers of 2: exact. An unmodified sample therefore
    // gets back exactly its original value.
    if (!(x == x)) x = 0.f;  // NaN → silence
    switch (f) {
        case SND_PCM_FORMAT_S32_LE: {
            int32_t v = (x >= 1.f) ? INT32_MAX : (x <= -1.f) ? INT32_MIN : int32_t(std::lrintf(x * 2147483648.f));
            std::memcpy(p, &v, 4); break;
        }
        case SND_PCM_FORMAT_S24_3LE: case SND_PCM_FORMAT_S24_LE: {
            float y = std::fmin(std::fmax(x * 8388608.f, -8388608.f), 8388607.f);
            int32_t v = int32_t(std::lrintf(y));
            if (f == SND_PCM_FORMAT_S24_LE) std::memcpy(p, &v, 4);
            else { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); }
            break;
        }
        default: {
            float y = std::fmin(std::fmax(x * 32768.f, -32768.f), 32767.f);
            int16_t v = int16_t(std::lrintf(y));
            std::memcpy(p, &v, 2);
        }
    }
}

// ---- ALSA configuration ----
static bool setupPcm(snd_pcm_t* h, const Format& fm, unsigned rate, int ch, snd_pcm_uframes_t& period,
                     snd_pcm_uframes_t& buffer, int nperiods, bool playback) {
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(h, hw) < 0) return false;
    snd_pcm_hw_params_set_rate_resample(h, hw, 0);
    if (snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) return false;
    if (snd_pcm_hw_params_set_format(h, hw, fm.f) < 0) return false;
    if (snd_pcm_hw_params_set_channels(h, hw, ch) < 0) return false;
    if (snd_pcm_hw_params_set_rate(h, hw, rate, 0) < 0) return false;
    snd_pcm_uframes_t p = period;
    int dir = 0;
    if (snd_pcm_hw_params_set_period_size_near(h, hw, &p, &dir) < 0) return false;
    snd_pcm_uframes_t b = p * nperiods;
    if (snd_pcm_hw_params_set_buffer_size_near(h, hw, &b) < 0) return false;
    if (snd_pcm_hw_params(h, hw) < 0) return false;
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(h, sw);
    snd_pcm_sw_params_set_avail_min(h, sw, period);
    snd_pcm_sw_params_set_start_threshold(h, sw, playback ? buffer * 2 : 1);  // manual start
    return snd_pcm_sw_params(h, sw) >= 0;
}

// Capabilities of a device: rates and best format. If `h` is already open
// (device in use), it is queried.
struct Caps { std::vector<unsigned> rates; std::string bestFormat; bool ok = false; };
static Caps probeHandle(snd_pcm_t* h, int ch) {
    Caps c;
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(h, hw) < 0) return c;
    snd_pcm_hw_params_set_rate_resample(h, hw, 0);
    if (snd_pcm_hw_params_test_channels(h, hw, ch) < 0) return c;
    for (unsigned r : kRates) if (snd_pcm_hw_params_test_rate(h, hw, r, 0) == 0) c.rates.push_back(r);
    for (const auto& f : kFormats) if (snd_pcm_hw_params_test_format(h, hw, f.f) == 0) { c.bestFormat = f.name; break; }
    c.ok = !c.rates.empty();
    return c;
}
static Caps probeDevice(const std::string& dev, snd_pcm_stream_t st, int ch) {
    snd_pcm_t* h = nullptr;
    if (snd_pcm_open(&h, dev.c_str(), st, SND_PCM_NONBLOCK) < 0) return Caps{};
    Caps c = probeHandle(h, ch);
    snd_pcm_close(h);
    return c;
}
// ---- Audio engine ----
struct Engine {
    Settings cfg;
    int channels = 2;
    int periodReq = 0;
    Shared* sh = nullptr;
    snd_pcm_t* cap = nullptr;
    snd_pcm_t* play = nullptr;
    Format fmt{};
    snd_pcm_uframes_t period = 0, buffer = 0;
    bool linked = false;
    unsigned seenSide = 0;   // last press seen on "start of side"
    Chain dec;
    Caps capsIn, capsOut;
    Biquad kShelf[2], kHp[2];
    PhonoStage phono[2];
    std::thread th;
    std::atomic<bool> stopReq{false};

    void close() {
        if (cap) { snd_pcm_drop(cap); snd_pcm_close(cap); cap = nullptr; }
        if (play) { snd_pcm_drop(play); snd_pcm_close(play); play = nullptr; }
        linked = false;
    }

    bool open(std::string& err) {
        if (!dec.prepare(cfg.rate, channels)) { err = "invalid sample rate or channel count"; return false; }
        for (auto& ph : phono) { ph.prepare(cfg.rate); ph.setMixNow(sh->riaa.load(), sh->phonoGain.load()); }
        seenSide = sh->adNewSide.load();
        period = periodReq > 0 ? periodReq : dec.hopSize();
        int e;
        if ((e = snd_pcm_open(&cap, cfg.inDev.c_str(), SND_PCM_STREAM_CAPTURE, 0)) < 0) {
            err = "input " + cfg.inDev + ": " + snd_strerror(e);
            if (e == -EBUSY) err += " (already in use: is another audio program or the service still running?)";
            cap = nullptr; return false;
        }
        if ((e = snd_pcm_open(&play, cfg.outDev.c_str(), SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
            err = "output " + cfg.outDev + ": " + snd_strerror(e);
            play = nullptr; close(); return false;
        }
        capsIn = probeHandle(cap, channels);
        capsOut = probeHandle(play, channels);
        bool ok = false;
        for (const auto& f : kFormats) {
            snd_pcm_uframes_t pc = period, bc = 0, pp = period, bp = 0;
            if (setupPcm(cap, f, cfg.rate, channels, pc, bc, cfg.periods, false) &&
                setupPcm(play, f, cfg.rate, channels, pp, bp, cfg.periods, true) && pc == pp) {
                fmt = f; period = pc; buffer = bp; ok = true;
                break;
            }
        }
        if (!ok) {
            err = "the card refuses " + std::to_string(cfg.rate) + " Hz / " + std::to_string(channels) + " channels";
            close(); return false;
        }
        for (int c = 0; c < 2; ++c) kWeighting(cfg.rate, kShelf[c], kHp[c]);
        linked = false;  // no input/output link: explicit start of both (see startStreams)
        return true;
    }

    bool startStreams() {
        snd_pcm_drop(play);
        snd_pcm_drop(cap);
        snd_pcm_prepare(play);
        snd_pcm_prepare(cap);
        std::vector<uint8_t> silence(size_t(period) * channels * fmt.bytes, 0);
        const int pre = std::max(1, int(buffer / period) - 1);
        for (int i = 0; i < pre; ++i)
            if (snd_pcm_writei(play, silence.data(), period) < 0) return false;
        // Start the output then the input. Even if both streams are linked
        // (same card), we check that the OUTPUT really runs: with some USB
        // cards, the link starts the input without the output.
        if (snd_pcm_state(play) != SND_PCM_STATE_RUNNING && snd_pcm_start(play) < 0) return false;
        if (snd_pcm_state(cap) != SND_PCM_STATE_RUNNING && snd_pcm_start(cap) < 0) return false;
        if (snd_pcm_state(play) != SND_PCM_STATE_RUNNING) return false;
        return true;
    }

    void run() {
        sched_param sp{};
        sp.sched_priority = 75;
        sh->rtOk = (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0);

        const int nch = channels;
        const size_t P = period;
        // Buffers allocated BEFORE the audio loop.
        std::vector<uint8_t> inRaw(P * nch * fmt.bytes), outRaw(P * nch * fmt.bytes);
        std::vector<float> planIn(P * nch), planOut(P * nch), planRest(P * nch);
        std::vector<const float*> ip(nch);
        std::vector<float*> op(nch);
        std::vector<float*> rp(nch);
        for (int c = 0; c < nch; ++c) { ip[c] = &planIn[c * P]; op[c] = &planOut[c * P]; rp[c] = &planRest[c * P]; }
        const double periodSec = double(P) / cfg.rate;
        float loadAvg = 0.f;
        int warm = 50;  // the first periods (start-up) do not count in the maximum

        if (!startStreams()) { sh->failed = true; return; }
        while (gRun && !stopReq.load(std::memory_order_relaxed)) {
            size_t got = 0;
            bool xrun = false;
            while (got < P && gRun) {
                snd_pcm_sframes_t r = snd_pcm_readi(cap, inRaw.data() + got * nch * fmt.bytes, P - got);
                if (r < 0) { xrun = true; break; }
                got += size_t(r);
            }
            if (!gRun) break;
            if (xrun) { sh->xruns++; if (!startStreams()) { sh->failed = true; return; } continue; }

            float pin[2] = {0.f, 0.f}, pout[2] = {0.f, 0.f};
            for (size_t i = 0; i < P; ++i)
                for (int c = 0; c < nch; ++c) {
                    const float v = toFloat(&inRaw[(i * nch + c) * fmt.bytes], fmt.f);
                    planIn[c * P + i] = v;
                    if (c < 2) pin[c] = std::fmax(pin[c], std::fabs(v));
                }

            // Built-in phono preamp: RIAA + rumble filter + gain (fades, no jumps).
            {
                const bool on = sh->riaa.load(std::memory_order_relaxed);
                const double g = sh->phonoGain.load(std::memory_order_relaxed);
                const bool rb = sh->rumble.load(std::memory_order_relaxed);
                if (on || !phono[0].settled(false)) {
                    bool over = false;
                    for (int c = 0; c < nch && c < 2; ++c) {
                        float* x = &planIn[c * P];
                        for (size_t i = 0; i < P; ++i) { x[i] = phono[c].tick(x[i], on, g, rb); over = over || std::fabs(x[i]) >= 0.999f; }
                    }
                    if (over) sh->satPhono.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // Off: the filters keep following the signal (re-enable without a jump).
                    for (int c = 0; c < nch && c < 2; ++c) { float* x = &planIn[c * P]; for (size_t i = 0; i < P; ++i) phono[c].tick(x[i], false, g, rb); }
                }
            }
            // Balance / stereo / noise of the turntable signal (after the phono preamp).
            sh->lv.push(levelSums(ip[0], ip[nch >= 2 ? 1 : 0], P));
            const bool recClean = sh->recClean.load(std::memory_order_relaxed);
#ifdef VC_RECORD
            if (sh->rec && !recClean) sh->rec->push(ip.data(), nch, P);   // raw sound: lock-free copy, never blocking (after the RIAA)
#endif
            ChainParams prm;
            prm.declick.intensity = sh->declick.load(std::memory_order_relaxed);
            prm.declick.maxClickMs = sh->maxClick.load(std::memory_order_relaxed);
            prm.declick.protect = sh->protect.load(std::memory_order_relaxed);
            prm.decrackle.intensity = sh->crackle.load(std::memory_order_relaxed);
            prm.decrackle.protect = prm.declick.protect;
            prm.declick.multiband = prm.decrackle.multiband = sh->multiband.load(std::memory_order_relaxed);
            prm.declick.stereo = prm.decrackle.stereo = sh->stereo.load(std::memory_order_relaxed);
            prm.mode = modeFromInt(sh->mode.load(std::memory_order_relaxed));
            {
                auto& d = prm.dist;
                const auto r = std::memory_order_relaxed;
                d.enabled = sh->adOn.load(r);
                d.arm.effLenMm = sh->adArmLen.load(r);
                d.arm.align = static_cast<Alignment>(sh->adAlign.load(r));
                d.arm.null1 = sh->adNull1.load(r); d.arm.null2 = sh->adNull2.load(r);
                d.stylus = static_cast<StylusType>(sh->adStylus.load(r));
                d.stylusUm = sh->adStylusUm.load(r);
                d.disc.fmt = static_cast<DiscFormat>(sh->adDisc.load(r));
                d.autoPos = sh->adAutoPos.load(r);
                d.posMm = sh->adPosMm.load(r); d.sideMin = sh->adSideMin.load(r); d.intensity = sh->adInt.load(r);
            }
            dec.setParams(prm);
            {
                const unsigned ns = sh->adNewSide.load(std::memory_order_relaxed);
                if (ns != seenSide) { seenSide = ns; dec.distStartSide(); }
            }

            const auto t0 = std::chrono::steady_clock::now();
            dec.process(ip.data(), op.data(), int(P), recClean ? rp.data() : nullptr);
#ifdef VC_RECORD
            if (sh->rec && recClean) sh->rec->push(rp.data(), nch, P);   // cleaned sound (whatever the listening mode)
#endif
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            {
                const AntiDistState ds = dec.distState();
                const auto r = std::memory_order_relaxed;
                sh->adRadius.store(float(ds.radiusMm), r); sh->adTrack.store(float(ds.trackDeg), r);
                sh->adGroove.store(float(ds.grooveCms), r); sh->adSide.store(float(ds.sideSec), r);
                sh->adReady.store(ds.levelReady, r);
            }

            float ms = 0.f;
            for (size_t i = 0; i < P; ++i)
                for (int c = 0; c < nch; ++c) {
                    const float v = planOut[c * P + i];
                    if (c < 2) {
                        pout[c] = std::fmax(pout[c], std::fabs(v));
                        const float k = kHp[c].tick(kShelf[c].tick(v));   // K-weighting
                        ms += k * k;
                    }
                    fromFloat(v, &outRaw[(i * nch + c) * fmt.bytes], fmt.f);
                }
            sh->loud.push({ms / float(P), std::fmax(pout[0], pout[1]), uint32_t(P)});

            size_t put = 0;
            while (put < P && gRun) {
                snd_pcm_sframes_t w = snd_pcm_writei(play, outRaw.data() + put * nch * fmt.bytes, P - put);
                if (w < 0) { xrun = true; break; }
                put += size_t(w);
            }
            if (xrun) { sh->xruns++; if (!startStreams()) { sh->failed = true; return; } }

            const float ld = float(100.0 * dt / periodSec);
            loadAvg = 0.98f * loadAvg + 0.02f * ld;
            sh->load.store(loadAvg, std::memory_order_relaxed);
            if (warm > 0) { --warm; loadAvg = ld; }
            else if (ld > sh->loadMax.load(std::memory_order_relaxed)) sh->loadMax.store(ld, std::memory_order_relaxed);
            sh->repaired.store(dec.declickStats().repaired, std::memory_order_relaxed);
            sh->crackles.store(dec.decrackleStats().repaired, std::memory_order_relaxed);
            for (int c = 0; c < 2; ++c) {
                const int cc = std::min(c, nch - 1);
                sh->peakIn[c].store(std::fmax(pin[cc], sh->peakIn[c].load(std::memory_order_relaxed)), std::memory_order_relaxed);
                sh->peakOut[c].store(std::fmax(pout[cc], sh->peakOut[c].load(std::memory_order_relaxed)), std::memory_order_relaxed);
            }
            if (std::fmax(pin[0], pin[1]) >= 0.999f) sh->satCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void startThread() { stopReq = false; sh->failed = false; th = std::thread([this] { run(); }); }
    void stopThread() { stopReq = true; if (th.joinable()) th.join(); }
};

// ---- List of sound cards ----
static std::string cardsJson(const Engine& eng, bool engineOpen) {
    std::ostringstream o;
    o << "{\"type\":\"cards\",\"cards\":[";
    int card = -1;
    bool first = true;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char* name = nullptr; char* id = nullptr;
        snd_card_get_name(card, &name);
        snd_ctl_t* ctl = nullptr;
        std::string cid;
        char hwn[16]; std::snprintf(hwn, sizeof hwn, "hw:%d", card);
        if (snd_ctl_open(&ctl, hwn, 0) == 0) {
            snd_ctl_card_info_t* info; snd_ctl_card_info_alloca(&info);
            if (snd_ctl_card_info(ctl, info) == 0) cid = snd_ctl_card_info_get_id(info);
            snd_ctl_close(ctl);
        }
        (void)id;
        const std::string dev = "hw:" + std::to_string(card) + ",0";
        Caps ci, co;
        const bool inUseIn = engineOpen && (eng.cfg.inDev == dev), inUseOut = engineOpen && (eng.cfg.outDev == dev);
        ci = inUseIn ? eng.capsIn : probeDevice(dev, SND_PCM_STREAM_CAPTURE, eng.channels);
        co = inUseOut ? eng.capsOut : probeDevice(dev, SND_PCM_STREAM_PLAYBACK, eng.channels);
        if (!ci.ok && !co.ok) { if (name) free(name); continue; }
        if (!first) o << ",";
        first = false;
        o << "{\"dev\":\"" << dev << "\",\"name\":\"" << jsonEscape(name ? name : cid) << "\",\"in\":[";
        for (size_t i = 0; i < ci.rates.size(); ++i) o << (i ? "," : "") << ci.rates[i];
        o << "],\"out\":[";
        for (size_t i = 0; i < co.rates.size(); ++i) o << (i ? "," : "") << co.rates[i];
        o << "],\"format\":\"" << jsonEscape(ci.ok ? ci.bestFormat : co.bestFormat) << "\"}";
        if (name) free(name);
    }
    o << "]}";
    return o.str();
}

static void usage() {
    std::printf(
        "vc-live — real-time declick + decrackle, with a control page\n\n"
        "Usage: vc-live [options]\n"
        "  -d, --device DEV    sound card for input AND output (default hw:0,0)\n"
        "  --in DEV / --out DEV  input and output on different cards\n"
        "  -r, --rate HZ       44100, 48000 (default), 88200, 96000 (176400, 192000 if the card and the Pi keep up)\n"
        "  --sens N            sensitivity 0..5, declick and decrackle together (default 3, 0 = off)\n"
        "  --declick-sens N    sensitivity of the declicker only 0..5\n"
        "  --decrackle-sens N  sensitivity of the decrackler only 0..5\n"
        "  --declick X / --decrackle X   (advanced) internal intensities 0..10\n"
        "  --protect N         music protection: 0 none, 1 normal, 2 maximum\n"
        "  --mode M            repair (default) | removed | bypass\n"
        "  --maxclick MS       maximum click length (0.5 to 5 ms)\n"
        "  --multiband on|off  multiband repair (default on)\n"
        "  --stereo on|off     both channels help each other (default on)\n"
        "  --riaa on|off       built-in phono preamp: turntable connected WITHOUT a preamp (default off)\n"
        "  --phono-gain DB     gain after the RIAA, 0 to 40 dB (default 12)\n"
        "  --rumble on|off     15 Hz rumble filter with the RIAA (default on)\n"
        "  --antidist on|off   playback anti-distortion (default off)\n"
        "  --arm-length MM     effective tonearm length (pivot → stylus), default 222\n"
        "  --align A           baerwald | lofgren | stevenson (default)   --nulls R1,R2  custom null points (mm)\n"
        "  --stylus T          conical | elliptical (0.3×0.7) | fine-elliptical (0.2×0.7) | shibata | microline | radius in µm\n"
        "  --disc F            33 | 45 | maxi   --side-min MIN  length of a side (default 20)\n"
        "  --radius MM         fixed stylus position (otherwise: estimated from the stylus drop)\n"
        "  --dist-int X        correction intensity 0..1.5 (default 1)\n"
        "  --http PORT         port of the control page (default 8080, 0 = no page)\n"
        "  --web FILE          HTML page read from disk on every load\n"
        "                      (default /usr/local/share/vinylclean/index.html if present)\n"
        "  --settings FILE     saved settings (default ~/.config/vinylclean/settings.json)\n"
        "  --no-save           do not save the settings\n"
        "  --rec-dir FOLDER    recordings folder (default ~/recordings)\n"
        "  --period N / --periods N   ALSA period size and count (default auto / 3)\n"
        "  --quiet             no status display\n\n"
        "Keys while listening: + / -  sensitivity (both)   c / v  decrackle sensitivity\n"
        "  1 Bypass (original)  2 Removed (what is removed)  3 Repair (cleaned)\n"
        "  p protection (none / normal / maximum)   [ / ]  max click   m multiband   s stereo   r RIAA\n"
        "  d anti-distortion   n start of side (stylus dropped)   q quit\n");
}

static std::string defaultSettingsPath() {
    const char* h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.config/vinylclean/settings.json";
}
// Recordings folder: ~/recordings. Installations made before 1.2 used another
// folder name ("enregistrements"): it is kept if it exists and ~/recordings does not.
static std::string defaultRecDir() {
    const char* h = std::getenv("HOME");
    const std::string home = h ? h : ".", now = home + "/recordings", before = home + "/enregistrements";
    struct stat sb {};
    return (::stat(now.c_str(), &sb) != 0 && ::stat(before.c_str(), &sb) == 0) ? before : now;
}
static void mkdirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

int main(int argc, char** argv) {
    Shared sh;
    Settings st;
    std::string settingsPath = defaultSettingsPath();
    std::string webFile = "/usr/local/share/vinylclean/index.html";
    int httpPort = 8080, period = 0;
    std::string recDir = defaultRecDir();
    bool quiet = false, save = true;

    // 1) Settings file (if it exists), 2) command-line options.
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--settings")) settingsPath = argv[i + 1];
    {
        std::ifstream f(settingsPath);
        if (f) {
            std::stringstream ss; ss << f.rdbuf(); JsonFlat j;
            if (j.parse(ss.str())) {
                settingsFromJson(j, st);
                // File from 0.5: its intensities followed the old scale.
                // Start again from the default sensitivities (card, rate and
                // protection are kept).
                if (!j.has("scale")) {
                    st.declick = declickIntensityForSens(kDefaultDeclickSens);
                    st.decrackle = decrackleIntensityForSens(kDefaultDecrackleSens);
                }
                // Before 1.1 (separate sliders), the default was declick 3 / decrackle 2:
                // it is brought back to the single slider (3 / 3). A deliberate setting is kept.
                else if (j.num("scale") < 3 && declickSensForIntensity(st.declick) == 3 && decrackleSensForIntensity(st.decrackle) == 2)
                    st.decrackle = decrackleIntensityForSens(3);
            }
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(1); } return argv[++i]; };
        if (a == "-d" || a == "--device") { st.inDev = st.outDev = next(); }
        else if (a == "--in") st.inDev = next();
        else if (a == "--out") st.outDev = next();
        else if (a == "-r" || a == "--rate") st.rate = unsigned(std::atoi(next().c_str()));
        else if (a == "--sens") { const int v = std::atoi(next().c_str()); st.declick = declickIntensityForSens(v); st.decrackle = decrackleIntensityForSens(v); }
        else if (a == "--declick-sens") st.declick = declickIntensityForSens(std::atoi(next().c_str()));
        else if (a == "--decrackle-sens") st.decrackle = decrackleIntensityForSens(std::atoi(next().c_str()));
        else if (a == "--declick") st.declick = std::strtof(next().c_str(), nullptr);
        else if (a == "--decrackle") st.decrackle = std::strtof(next().c_str(), nullptr);
        else if (a == "--protect") st.protect = std::atoi(next().c_str());
        else if (a == "--maxclick") st.maxClick = std::strtof(next().c_str(), nullptr);
        else if (a == "--multiband") st.multiband = (next() != "off");
        else if (a == "--stereo") st.stereo = (next() != "off");
        else if (a == "--riaa") st.riaa = (next() != "off");
        else if (a == "--phono-gain") st.phonoGainDb = float(std::fmin(40.0, std::fmax(0.0, std::atof(next().c_str()))));
        else if (a == "--rumble") st.rumble = (next() != "off");
        else if (a == "--antidist") st.adOn = (next() != "off");
        else if (a == "--arm-length") st.armLen = float(std::fmin(400.0, std::fmax(150.0, std::atof(next().c_str()))));
        else if (a == "--align") { Alignment al; const std::string v = next(); if (!alignFromName(v, al)) { std::printf("unknown alignment: %s\n", v.c_str()); return 1; } st.align = int(al); }
        else if (a == "--nulls") { const std::string v = next(); float a1 = 0, a2 = 0; if (std::sscanf(v.c_str(), "%f,%f", &a1, &a2) != 2) { std::printf("--nulls expects r1,r2 in mm\n"); return 1; } st.align = int(Alignment::Custom); st.null1 = a1; st.null2 = a2; }
        else if (a == "--stylus") { StylusType t; double um = st.stylusUm; const std::string v = next(); if (!stylusFromName(v, t, um)) { std::printf("unknown stylus: %s\n", v.c_str()); return 1; } st.stylus = int(t); st.stylusUm = float(um); }
        else if (a == "--disc") { DiscFormat d; const std::string v = next(); if (!discFromName(v, d)) { std::printf("unknown format: %s\n", v.c_str()); return 1; } st.disc = int(d); }
        else if (a == "--side-min") st.sideMin = float(std::fmin(40.0, std::fmax(2.0, std::atof(next().c_str()))));
        else if (a == "--radius") { st.adAutoPos = false; st.posMm = float(std::fmin(150.0, std::fmax(50.0, std::atof(next().c_str())))); }
        else if (a == "--dist-int") st.adInt = float(std::fmin(1.5, std::fmax(0.0, std::atof(next().c_str()))));
        else if (a == "--mode") st.mode = int(modeFromName(next().c_str()));
        else if (a == "--http") httpPort = std::atoi(next().c_str());
        else if (a == "--web") webFile = next();
        else if (a == "--settings") next();
        else if (a == "--no-save") save = false;
        else if (a == "--rec-dir") recDir = next();
        else if (a == "--period") period = std::atoi(next().c_str());
        else if (a == "--periods") st.periods = std::max(2, std::atoi(next().c_str()));
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::printf("unknown option: %s\n\n", a.c_str()); usage(); return 1; }
    }
    st.declick = std::fmin(10.f, std::fmax(0.f, st.declick));
    st.decrackle = std::fmin(10.f, std::fmax(0.f, st.decrackle));
    st.protect = std::min(2, std::max(0, st.protect));
    st.maxClick = std::fmin(5.f, std::fmax(0.5f, st.maxClick));

    auto pushToShared = [&](const Settings& s) {
        sh.declick = s.declick; sh.crackle = s.decrackle; sh.protect = s.protect;
        sh.maxClick = s.maxClick; sh.mode = s.mode; sh.multiband = s.multiband; sh.stereo = s.stereo;
        sh.riaa = s.riaa; sh.phonoGain = float(std::pow(10.0, s.phonoGainDb / 20.0)); sh.rumble = s.rumble;
        sh.adArmLen = s.armLen; sh.adAlign = s.align; sh.adNull1 = s.null1; sh.adNull2 = s.null2;
        sh.adStylus = s.stylus; sh.adStylusUm = s.stylusUm; sh.adDisc = s.disc; sh.adAutoPos = s.adAutoPos;
        sh.adPosMm = s.posMm; sh.adSideMin = s.sideMin; sh.adInt = s.adInt; sh.adOn = s.adOn;
    };
    pushToShared(st);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0 && !quiet)
        std::printf("Note: memory not locked (memlock rights) — usually harmless.\n");

    // State shared with the web thread (lock: never in the audio thread).
    std::mutex stMutex;
    Settings pending;
    std::atomic<bool> reconfig{false}, dirty{false}, wantCards{false}, changed{false};
    std::string lastError;
    Engine eng;
    eng.sh = &sh;
    eng.periodReq = period;
    std::atomic<bool> engineOpen{false};

    // ---- Recording ----
#ifdef VC_RECORD
    Recorder rec;
    sh.rec = &rec;
    ::mkdir(recDir.c_str(), 0755);
#endif
    struct RecCmd { int kind; std::string arg; };   // 1 start, 2 stop, 3 delete
    std::vector<RecCmd> recCmds;                    // queue: web thread → main thread (stMutex)
    std::string recMsg;                             // last message (error, automatic stop…)
    std::string recName;                            // displayed name of the current file
    std::atomic<bool> wantRecs{true};

    // ---- Web page ----
    WebServer web;
    auto stateJson = [&]() {
        std::lock_guard<std::mutex> lk(stMutex);
        std::string s = settingsToJson(st);
        s.insert(1, "\"type\":\"settings\",");
        return s;
    };
    WebServer::Callbacks cb;
    cb.onConnect = [&]() { wantCards = true; wantRecs = true; return stateJson(); };
#ifdef VC_RECORD
    cb.downloadPath = [&](const std::string& name) -> std::string {
        std::string full;
        return validRecFile(recDir, name, &full) ? full : std::string();
    };
#endif
    cb.onMessage = [&](const std::string& m) -> std::string {
        JsonFlat j;
        if (!j.parse(m)) return "";
        const std::string cmd = j.str("cmd");
        if (cmd == "set") {
            std::lock_guard<std::mutex> lk(stMutex);
            Settings s = st;
            settingsFromJson(j, s);
            const bool audioChange = s.inDev != st.inDev || s.outDev != st.outDev || s.rate != st.rate || s.periods != st.periods;
            if (audioChange) { pending = s; reconfig = true; s.inDev = st.inDev; s.outDev = st.outDev; s.rate = st.rate; s.periods = st.periods; }
            st = s;
            pushToShared(st);
            dirty = true;
            changed = true;   // the main thread will send the state back to ALL screens
        } else if (cmd == "cards") {
            wantCards = true;
        } else if (cmd == "ad_side") {
            sh.adNewSide.fetch_add(1);   // "stylus dropped at the start of the side"
        } else if (cmd == "rec_start" || cmd == "rec_stop" || cmd == "rec_delete") {
            std::lock_guard<std::mutex> lk(stMutex);
            recCmds.push_back({cmd == "rec_start" ? 1 : cmd == "rec_stop" ? 2 : 3, j.str(cmd == "rec_delete" ? "file" : "name")});
        }
        return "";
    };
    if (httpPort > 0) {
        std::string err;
        if (!web.start(httpPort, webFile, kIndexHtml, cb, err)) std::printf("Control page unavailable: %s\n", err.c_str());
        else if (!quiet) std::printf("Control page: http://%s.local:%d  (or http://<Pi address>:%d)\n",
                                     [] { static char h[64]; gethostname(h, sizeof h); return h; }(), httpPort, httpPort);
    }

    // ---- Keyboard ----
    termios oldt{};
    const bool tty = isatty(0) && !quiet;
    if (tty) {
        tcgetattr(0, &oldt);
        termios t = oldt;
        t.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &t);
    }

    auto openEngine = [&](const Settings& want) -> bool {
        eng.cfg = want;
        std::string err;
        if (!eng.open(err)) {
            std::lock_guard<std::mutex> lk(stMutex);
            lastError = err;
            return false;
        }
        { std::lock_guard<std::mutex> lk(stMutex); lastError.clear(); }
        sh.xruns = 0; sh.loadMax = 0.f;
        eng.startThread();
        engineOpen = true;
        if (!quiet) {
            const double ioMs = 1000.0 * eng.buffer / want.rate, dspMs = 1000.0 * eng.dec.latency() / want.rate;
            std::printf("\nvc-live: input %s, output %s, %u Hz, %d channels, %s; latency ≈ %.0f ms\n",
                        want.inDev.c_str(), want.outDev.c_str(), want.rate, eng.channels, eng.fmt.name, dspMs + ioMs);
        }
        return true;
    };

    LoudnessMeter meter;
    meter.reset(st.rate);
    LevelStats levels;
    levels.reset(st.rate);
    double lST = -120, lInt = -120, lLRA = 0, lPLR = 0, lSec = 0;
    auto lastLoud = std::chrono::steady_clock::now();
    Settings good = st;  // last audio configuration that worked
    if (!openEngine(st)) {
        std::printf("Error: %s\n", lastError.c_str());
        if (httpPort <= 0) return 1;
        std::printf("The control page stays available to choose another card.\n");
    } else good = st;

    if (tty) std::printf("Keys: 1/2/3 listening, +/- sensitivity, c/v decrackle only, p protection, [ ] max click, m multiband, s stereo, r RIAA, q quit\n");

    auto lastPrint = std::chrono::steady_clock::now();
    auto lastBcast = lastPrint, lastSave = lastPrint, lastRetry = lastPrint;
    bool rtReported = false;
    while (gRun) {
        // Keyboard (or simply a 50 ms wait).
        pollfd pfd{0, POLLIN, 0};
        if (tty && poll(&pfd, 1, 50) > 0) {
            char c = 0;
            if (read(0, &c, 1) == 1) {
                std::lock_guard<std::mutex> lk(stMutex);
                Settings s = st;
                // Next / previous sensitivity (from a "custom" value, the closest one).
                auto step = [](float v, int dir, float (*f)(int)) {
                    int cur = 0;
                    for (int k = 0; k <= kMaxSens; ++k) if (f(k) <= v + 1e-3f) cur = k;
                    if (dir < 0 && f(cur) < v - 1e-3f) ++cur;
                    return f(std::clamp(cur + dir, 0, kMaxSens));
                };
                switch (c) {
                    case '+': case '=': s.declick = step(s.declick, +1, declickIntensityForSens); s.decrackle = step(s.decrackle, +1, decrackleIntensityForSens); break;
                    case '-': case '_': s.declick = step(s.declick, -1, declickIntensityForSens); s.decrackle = step(s.decrackle, -1, decrackleIntensityForSens); break;
                    case 'c': case 'C': s.decrackle = step(s.decrackle, +1, decrackleIntensityForSens); break;
                    case 'v': case 'V': s.decrackle = step(s.decrackle, -1, decrackleIntensityForSens); break;
                    case 'p': case 'P': s.protect = (s.protect + 1) % 3; break;
                    case '[': s.maxClick = std::fmax(0.5f, s.maxClick - 0.5f); break;
                    case ']': s.maxClick = std::fmin(5.f, s.maxClick + 0.5f); break;
                    case 'm': case 'M': s.multiband = !s.multiband; break;
                    case 's': case 'S': s.stereo = !s.stereo; break;
                    case 'r': case 'R': s.riaa = !s.riaa; break;
                    case 'd': case 'D': s.adOn = !s.adOn; break;
                    case 'n': case 'N': sh.adNewSide.fetch_add(1); break;
                    case '1': s.mode = 0; break;
                    case '2': s.mode = 1; break;
                    case '3': s.mode = 2; break;
                    case 'q': case 'Q': gRun = false; break;
                    default: break;
                }
                st = s;
                pushToShared(st);
                dirty = true;
                changed = true;
                lastPrint -= std::chrono::seconds(5);
            }
        } else if (!tty) {
            usleep(50000);
        }
        const auto now = std::chrono::steady_clock::now();

#ifdef VC_RECORD
        // Recording: page commands, automatic stops.
        // recMsg is sent to the page as "code|detail|duration|lost": the page shows it
        // in its own language (see web/index.html, recMessage()).
        auto recStop = [&](const std::string& code) {
            if (!rec.recording()) return;
            const std::string e = rec.error();
            const double sec = rec.seconds();
            const uint64_t lost = rec.droppedBlocks();
            rec.stop();
            char d[32];
            std::snprintf(d, sizeof d, "%d:%02d", int(sec) / 60, int(sec) % 60);
            const std::string msg = code + "|" + (e.empty() ? recName : e) + "|" + d + "|" + (lost ? "1" : "0");
            { std::lock_guard<std::mutex> lk(stMutex); recMsg = msg; }
            if (!quiet) std::printf("\nRecording finished (%s): %s, %s%s%s\n", code.c_str(), recName.c_str(), d,
                                    e.empty() ? "" : ", ", e.c_str());
            wantRecs = true;
        };
        {
            std::vector<RecCmd> cmds;
            { std::lock_guard<std::mutex> lk(stMutex); cmds.swap(recCmds); }
            for (auto& c : cmds) {
                if (c.kind == 1 && !rec.recording()) {
                    std::string err;
                    if (!engineOpen) err = "card_not_ready";
                    else if (freeBytes(recDir) < (uint64_t(300) << 20)) err = "no_space";
                    else {
                        const std::string base = safeRecName(c.arg);
                        const std::string path = uniqueRecPath(recDir, base);
                        bool clean; { std::lock_guard<std::mutex> lk(stMutex); clean = st.recClean; }
                        const int bits = (eng.fmt.bytes == 2 && !clean) ? 16 : 24;
                        sh.recClean = clean;   // before start(): the audio thread then pushes the right signal
                        if (rec.start(path, eng.cfg.rate, std::min(eng.channels, 2), bits, base, err,
                                      clean ? "Cleaned recording — vinylclean" : "Raw recording (no processing) — vinylclean")) {
                            recName = path.substr(recDir.size() + 1);
                            std::lock_guard<std::mutex> lk(stMutex); recMsg.clear();
                            if (!quiet) std::printf("\nRecording: %s (%u Hz, %d bits)\n", path.c_str(), eng.cfg.rate, bits);
                        }
                    }
                    if (!err.empty()) { std::lock_guard<std::mutex> lk(stMutex); recMsg = "start_fail|" + err; }
                    wantRecs = true;
                } else if (c.kind == 2) {
                    recStop("saved");
                } else if (c.kind == 3) {
                    std::string full;
                    std::lock_guard<std::mutex> lk(stMutex);
                    if (rec.recording() && c.arg == recName) recMsg = "del_current|" + c.arg;
                    else if (!validRecFile(recDir, c.arg, &full) || ::unlink(full.c_str()) != 0) recMsg = "del_fail|" + c.arg;
                    else recMsg = "del_ok|" + c.arg;
                    wantRecs = true;
                }
            }
            if (rec.failed()) recStop("stop_error");
            static auto lastSpace = now;
            if (rec.recording() && now - lastSpace > std::chrono::seconds(2)) {
                lastSpace = now;
                if (freeBytes(recDir) < (uint64_t(200) << 20)) recStop("stop_disk");
            }
            if (rec.recording() && (!engineOpen || sh.failed)) recStop("stop_card");
        }
#endif
        // Audio reconfiguration requested by the page (card, rate).
        if (reconfig.exchange(false)) {
#ifdef VC_RECORD
            if (rec.recording()) recStop("stop_reconfig");
#endif
            Settings want;
            { std::lock_guard<std::mutex> lk(stMutex); want = pending; }
            if (engineOpen) { eng.stopThread(); eng.close(); engineOpen = false; }
            meter.reset(want.rate);
            levels.reset(want.rate);
            if (openEngine(want)) {
                good = want;
                std::lock_guard<std::mutex> lk(stMutex);
                st.inDev = want.inDev; st.outDev = want.outDev; st.rate = want.rate; st.periods = want.periods;
                dirty = true;
            } else {
                std::string e;
                { std::lock_guard<std::mutex> lk(stMutex); e = lastError; }
                std::printf("\nNew configuration failed (%s): back to the previous one.\n", e.c_str());
                if (openEngine(good)) { std::lock_guard<std::mutex> lk(stMutex); lastError = e + " — previous configuration restored"; }
            }
            web.broadcast(stateJson());
            wantCards = true;
        }
        // Card error (unplugged…): new attempt every 3 s.
        if (engineOpen && sh.failed) { eng.stopThread(); eng.close(); engineOpen = false; { std::lock_guard<std::mutex> lk(stMutex); lastError = "sound card error"; } }
        if (!engineOpen && now - lastRetry > std::chrono::seconds(3)) {
            lastRetry = now;
            Settings want; { std::lock_guard<std::mutex> lk(stMutex); want = st; }
            if (openEngine(want)) good = want;
        }
        // Loudness and dynamic range (computed once per second).
        { LoudItem it; while (sh.loud.pop(it)) meter.add(it); }
        { LevelItem it; while (sh.lv.pop(it)) levels.add(it); }
        if (now - lastLoud >= std::chrono::seconds(1)) { lastLoud = now; meter.compute(lST, lInt, lLRA, lPLR, lSec); levels.compute(); }
        if (changed.exchange(false) && httpPort > 0) web.broadcast(stateJson());
        if (wantCards.exchange(false) && httpPort > 0) web.broadcast(cardsJson(eng, engineOpen));
#ifdef VC_RECORD
        {
            static auto lastRecs = now;
            if (rec.recording() && now - lastRecs > std::chrono::seconds(5)) wantRecs = true;
            if (wantRecs.exchange(false) && httpPort > 0) {
                lastRecs = now;
                std::string js = "{\"type\":\"recs\",\"free\":" + std::to_string(freeBytes(recDir)) + ",\"items\":[";
                bool first = true;
                for (const auto& e : listRecordings(recDir)) {
                    char d[32]; std::strftime(d, sizeof d, "%d/%m/%Y %H:%M", std::localtime(&e.mtime));
                    const bool cur = rec.recording() && e.name == recName;
                    char b[160];
                    std::snprintf(b, sizeof b, "\"bytes\":%llu,\"sec\":%.1f,\"date\":\"%s\",\"rec\":%s}",
                                  (unsigned long long)e.bytes, cur ? rec.seconds() : e.seconds, d, cur ? "true" : "false");
                    js += (first ? "" : ",") + std::string("{\"name\":\"") + jsonEscape(e.name) + "\"," + b;
                    first = false;
                }
                js += "]}";
                web.broadcast(js);
            }
        }
#endif

        // Saving the settings (at most every 2 s).
        if (save && dirty && now - lastSave > std::chrono::seconds(2)) {
            lastSave = now;
            dirty = false;
            std::string js; { std::lock_guard<std::mutex> lk(stMutex); js = settingsToJson(st); }
            mkdirs(settingsPath);
            const std::string tmp = settingsPath + ".tmp";
            std::ofstream f(tmp);
            if (f) { f << js << "\n"; f.close(); std::rename(tmp.c_str(), settingsPath.c_str()); }
        }

        // Recent clipping (less than 3 s).
        static unsigned lastSat = 0;
        static auto lastSatTime = now - std::chrono::seconds(10);
        if (sh.satCount.load() != lastSat) { lastSat = sh.satCount.load(); lastSatTime = now; }
        const bool satRecent = now - lastSatTime < std::chrono::seconds(3);
        static unsigned lastSatP = 0;
        static auto lastSatPTime = now - std::chrono::seconds(10);
        if (sh.satPhono.load() != lastSatP) { lastSatP = sh.satPhono.load(); lastSatPTime = now; }
        const bool satPhonoRecent = now - lastSatPTime < std::chrono::seconds(3);
        // State to the page (10 times per second).
        auto db = [](float v) { return v > 1e-9f ? 20.f * std::log10(v) : -120.f; };
        if (httpPort > 0 && now - lastBcast >= std::chrono::milliseconds(100)) {
            lastBcast = now;
            float pi[2], po[2];
            for (int c = 0; c < 2; ++c) { pi[c] = sh.peakIn[c].exchange(0.f); po[c] = sh.peakOut[c].exchange(0.f); }
            std::string err; { std::lock_guard<std::mutex> lk(stMutex); err = lastError; }
            std::string recJs = std::string(",\"satPhono\":") + (satPhonoRecent ? "true" : "false") + ",\"recOn\":false";
#ifdef VC_RECORD
            {
                std::string msg; { std::lock_guard<std::mutex> lk(stMutex); msg = recMsg; }
                char rb[256];
                std::snprintf(rb, sizeof rb, ",\"recOn\":%s,\"recSec\":%.1f,\"recLost\":%llu,\"recAvail\":true,\"recName\":\"",
                              rec.recording() ? "true" : "false", rec.seconds(), (unsigned long long)rec.droppedBlocks());
                recJs = std::string(",\"satPhono\":") + (satPhonoRecent ? "true" : "false") + rb + jsonEscape(rec.recording() ? recName : std::string()) + "\",\"recMsg\":\"" + jsonEscape(msg) + "\"";
            }
#endif
            char b[1600];
            std::snprintf(b, sizeof b,
                          "{\"type\":\"status\",\"running\":%s,\"inL\":%.1f,\"inR\":%.1f,\"outL\":%.1f,\"outR\":%.1f,"
                          "\"sat\":%s,\"clicks\":%llu,\"crackles\":%llu,\"load\":%.1f,\"loadMax\":%.1f,\"xruns\":%u,"
                          "\"lufsS\":%.1f,\"lufsI\":%.1f,\"lra\":%.1f,\"plr\":%.1f,\"loudSec\":%.0f,"
                          "\"rate\":%u,\"outState\":\"%s\",\"format\":\"%s\",\"latency\":%.0f,\"rt\":%s,\"error\":\"%s\",\"clients\":%d}",
                          engineOpen ? "true" : "false", db(pi[0]), db(pi[1]), db(po[0]), db(po[1]),
                          satRecent ? "true" : "false", sh.repaired.load(), sh.crackles.load(),
                          (double)sh.load.load(), (double)sh.loadMax.load(), sh.xruns.load(),
                          lST, lInt, lLRA, lPLR, lSec,
                          engineOpen ? eng.cfg.rate : 0u, (engineOpen && eng.play) ? snd_pcm_state_name(snd_pcm_state(eng.play)) : "",
                          engineOpen ? eng.fmt.name : "",
                          engineOpen ? 1000.0 * (eng.dec.latency() + eng.buffer) / eng.cfg.rate : 0.0,
                          sh.rtOk ? "true" : "false", jsonEscape(err).c_str(), web.clients());
            std::string full = b;
            full.insert(full.size() - 1, recJs);
            {
                char ab[200];
                std::snprintf(ab, sizeof ab, ",\"adR\":%.1f,\"adErr\":%.2f,\"adV\":%.1f,\"adSide\":%.0f,\"adReady\":%s",
                              (double)sh.adRadius.load(), (double)sh.adTrack.load(), (double)sh.adGroove.load(),
                              (double)sh.adSide.load(), sh.adReady.load() ? "true" : "false");
                full.insert(full.size() - 1, ab);
                auto num = [](double v, const char* f) { char t[32]; if (std::isfinite(v)) std::snprintf(t, sizeof t, f, v); else std::snprintf(t, sizeof t, "null"); return std::string(t); };
                full.insert(full.size() - 1, ",\"bal\":" + num(levels.balanceDb, "%.1f") + ",\"corr\":" + num(levels.correlation, "%.2f") +
                                             ",\"noise\":" + num(levels.noiseDb, "%.0f") + ",\"recClean\":" + (sh.recClean ? "true" : "false"));
            }
            web.broadcast(full);
        }

        // Terminal display.
        if (!quiet && now - lastPrint >= std::chrono::seconds(tty ? 1 : 10)) {
            lastPrint = now;
            if (!rtReported && engineOpen) {
                rtReported = true;
                if (!sh.rtOk) std::printf("Warning: real-time priority refused (check the audio group and limits.d).\n");
            }
            static const char* modes[] = {"Bypass ", "Removed", "Repair "};
            static const char* prots[] = {"none", "normal", "max"};
            Settings s; { std::lock_guard<std::mutex> lk(stMutex); s = st; }
            auto sensTxt = [](int v) { static char b[2][8]; static int k = 0; k ^= 1; if (v < 0) std::snprintf(b[k], 8, "custom"); else std::snprintf(b[k], 8, "%d", v); return b[k]; };
            std::printf("%s[%s] declick %s | decrackle %s | protection %s | clicks %llu | crackles %llu | load %4.1f %% (max %4.1f %%) | xruns %u%s%s%s%s",
                        tty ? "\r" : "", modes[s.mode], sensTxt(declickSensForIntensity(s.declick)), sensTxt(decrackleSensForIntensity(s.decrackle)), prots[s.protect],
                        sh.repaired.load(), sh.crackles.load(), (double)sh.load.load(), (double)sh.loadMax.load(),
                        sh.xruns.load(), satRecent ? " | CLIPPING!" : "",
                        s.adOn ? " | anti-dist." : "",
#ifdef VC_RECORD
                        rec.recording() ? " | ● RECORDING" : "",
#else
                        "",
#endif
                        tty ? "   " : "\n");
            std::fflush(stdout);
        }
    }
    gRun = false;
#ifdef VC_RECORD
    if (rec.recording()) { rec.stop(); if (!quiet) std::printf("\nRecording finished and closed cleanly.\n"); }
#endif
    if (engineOpen) { eng.stopThread(); eng.close(); }
    web.stop();
    if (tty) { tcsetattr(0, TCSANOW, &oldt); std::printf("\n"); }
    if (save) {
        mkdirs(settingsPath);
        std::ofstream f(settingsPath);
        if (f) f << settingsToJson(st) << "\n";
    }
    std::printf("Done. %llu clicks and %llu crackles repaired, %u xruns.\n", sh.repaired.load(), sh.crackles.load(), sh.xruns.load());
    return 0;
}
