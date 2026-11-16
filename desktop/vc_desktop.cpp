// vinylclean desktop — the same cleaning as on the Raspberry Pi, for Windows
// and Linux computers:
//  - real time: sound card input → (RIAA) → declick → decrackle →
//    anti-distortion → output, with FLAC recording of the raw sound;
//  - file cleaning: import a WAV or FLAC file, clean it with the current
//    settings, listen before / after, download the result (WAV or FLAC).
// The control page is the Pi page (web/index.html) plus a "Clean a file" card
// (desktop/extra.html), shown in its own window (Edge / Chrome / Chromium in
// "app" mode) or in the default browser.
//
// The DSP is exactly the Raspberry Pi code (core/). Sound I/O: miniaudio
// (WASAPI on Windows, PulseAudio / PipeWire / ALSA on Linux).
#include "miniaudio.h"

#include "decrackle.h"
#include "presets.h"
#include "riaa.h"
#include "antidist.h"
#include "loudness.h"
#include "levels.h"

#include "http_server.h"
#include "json.h"
#include "platform.h"
#include "recorder.h"

#include "index_html.h"
#include "extra_html.h"
#include "simple_html.h"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace vc;
using vcd::JsonFlat;
using vcd::jsonEscape;

#ifndef VC_VERSION
#define VC_VERSION "dev"
#endif

static std::atomic<bool> gRun{true};
static void onSignal(int) { gRun = false; }

// ---- Shared parameters (web → audio), lock-free (same as vc-live) ----
struct Shared {
    std::atomic<float> declick{6.f}, crackle{6.5f}, maxClick{2.5f};
    std::atomic<int>   protect{1}, mode{2};
    std::atomic<bool>  multiband{true}, stereo{true};
    std::atomic<bool>  riaa{false}, rumble{true};
    std::atomic<float> phonoGain{1.f};
    std::atomic<unsigned> satPhono{0};
    std::atomic<bool>  adOn{false}, adAutoPos{true};
    std::atomic<float> adArmLen{222.f}, adNull1{60.325f}, adNull2{117.42f}, adStylusUm{7.6f};
    std::atomic<float> adPosMm{100.f}, adSideMin{20.f}, adInt{1.f};
    std::atomic<int>   adAlign{2}, adStylus{2}, adDisc{0};
    std::atomic<unsigned> adNewSide{0};
    std::atomic<float> adRadius{0.f}, adTrack{0.f}, adGroove{0.f}, adSide{-1.f};
    std::atomic<bool>  adReady{false};
    std::atomic<unsigned long long> repaired{0}, crackles{0};
    std::atomic<unsigned> xruns{0};
    std::atomic<float> load{0.f}, loadMax{0.f};
    std::atomic<float> peakIn[2], peakOut[2];
    std::atomic<unsigned> satCount{0};
    std::atomic<float> recPeak{0.f};      // peak of the recorded signal (silence auto-stop)
    LoudRing loud;
    LevelRing lv;
    std::atomic<bool> recClean{false};
    vcd::Recorder* rec = nullptr;
    Shared() { for (int c = 0; c < 2; ++c) { peakIn[c] = 0.f; peakOut[c] = 0.f; } }
};

// ---- Persistent settings (same keys as vc-live) ----
struct Settings {
    float declick = declickIntensityForSens(kDefaultDeclickSens);
    float decrackle = decrackleIntensityForSens(kDefaultDecrackleSens);
    float maxClick = kDefaultMaxClickMs;
    int protect = kDefaultProtect, mode = 2;
    bool multiband = kDefaultMultiband, stereo = kDefaultStereo;
    bool riaa = false;
    float phonoGainDb = 12.f;
    bool rumble = true;
    bool recClean = false;
    float vuRef = -18.f;           // VU meters of the touchscreen page: dBFS that reads 0 VU
    bool adOn = false, adAutoPos = true;
    float armLen = 222.f, null1 = 60.325f, null2 = 117.42f, stylusUm = 7.6f;
    float posMm = 100.f, sideMin = 20.f, adInt = 1.f;
    int align = 2, stylus = 2, disc = 0;
    std::string inDev = "default", outDev = "default";
    unsigned rate = 48000;
    int periods = 3;
};
static int commonSens(const Settings& s) {
    const int a = declickSensForIntensity(s.declick), b = decrackleSensForIntensity(s.decrackle);
    return (a >= 0 && a == b) ? a : -1;
}
static std::string settingsToJson(const Settings& s) {
    char b[2048];
    std::snprintf(b, sizeof b,
                  "{\"scale\":3,\"ad\":%d,\"armLen\":%g,\"align\":%d,\"null1\":%g,\"null2\":%g,\"stylus\":%d,\"stylusUm\":%g,\"disc\":%d,\"autoPos\":%d,\"posMm\":%g,\"sideMin\":%g,\"adInt\":%g,"
                  "\"sens\":%d,\"sensDeclick\":%d,\"sensDecrackle\":%d,\"declick\":%g,\"decrackle\":%g,\"protect\":%d,\"maxclick\":%g,\"multiband\":%d,\"stereo\":%d,\"riaa\":%d,\"phonoGain\":%g,\"rumble\":%d,\"recClean\":%d,\"vuRef\":%g,\"mode\":%d,"
                  "\"in\":\"%s\",\"out\":\"%s\",\"rate\":%u,\"periods\":%d}",
                  s.adOn ? 1 : 0, s.armLen, s.align, s.null1, s.null2, s.stylus, s.stylusUm, s.disc, s.adAutoPos ? 1 : 0, s.posMm, s.sideMin, s.adInt,
                  commonSens(s), declickSensForIntensity(s.declick), decrackleSensForIntensity(s.decrackle), s.declick, s.decrackle, s.protect, s.maxClick,
                  s.multiband ? 1 : 0, s.stereo ? 1 : 0, s.riaa ? 1 : 0, s.phonoGainDb, s.rumble ? 1 : 0, s.recClean ? 1 : 0, s.vuRef, s.mode,
                  jsonEscape(s.inDev).c_str(), jsonEscape(s.outDev).c_str(), s.rate, s.periods);
    return b;
}
static void settingsFromJson(const JsonFlat& j, Settings& s) {
    auto clampf = [](double v, double lo, double hi) { return float(std::fmin(hi, std::fmax(lo, v))); };
    if (j.has("sens")) { const int v = int(j.num("sens")); s.declick = declickIntensityForSens(v); s.decrackle = decrackleIntensityForSens(v); }
    if (j.has("sensDeclick")) s.declick = declickIntensityForSens(int(j.num("sensDeclick")));
    if (j.has("sensDecrackle")) s.decrackle = decrackleIntensityForSens(int(j.num("sensDecrackle")));
    if (j.has("declick")) s.declick = clampf(j.num("declick"), 0, 10);
    if (j.has("decrackle")) s.decrackle = clampf(j.num("decrackle"), 0, 10);
    if (j.has("protect")) s.protect = int(clampf(j.num("protect"), 0, 2));
    if (j.has("maxclick")) s.maxClick = clampf(j.num("maxclick"), 0.5, 5);
    if (j.has("multiband")) s.multiband = j.num("multiband") != 0;
    if (j.has("stereo")) s.stereo = j.num("stereo") != 0;
    if (j.has("riaa")) s.riaa = j.num("riaa") != 0;
    if (j.has("phonoGain")) s.phonoGainDb = clampf(j.num("phonoGain"), 0, 40);
    if (j.has("rumble")) s.rumble = j.num("rumble") != 0;
    if (j.has("recClean")) s.recClean = j.num("recClean") != 0;
    if (j.has("vuRef")) s.vuRef = float(std::fmin(-8, std::fmax(-24, j.num("vuRef"))));
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
    if (j.has("mode")) s.mode = int(clampf(j.num("mode"), 0, 2));
    if (j.has("in")) s.inDev = j.str("in");
    if (j.has("out")) s.outDev = j.str("out");
    if (j.has("rate")) s.rate = unsigned(clampf(j.num("rate"), 8000, 192000));
    if (j.has("periods")) s.periods = int(clampf(j.num("periods"), 2, 8));
}

// Chain parameters from the settings (file cleaning; always "Repair").
static ChainParams chainParams(const Settings& s) {
    ChainParams p;
    p.declick.intensity = s.declick; p.declick.maxClickMs = s.maxClick; p.declick.protect = s.protect;
    p.decrackle.intensity = s.decrackle; p.decrackle.protect = s.protect;
    p.declick.multiband = p.decrackle.multiband = s.multiband;
    p.declick.stereo = p.decrackle.stereo = s.stereo;
    p.mode = Mode::Repair;
    auto& d = p.dist;
    d.enabled = s.adOn; d.arm.effLenMm = s.armLen; d.arm.align = static_cast<Alignment>(s.align);
    d.arm.null1 = s.null1; d.arm.null2 = s.null2; d.stylus = static_cast<StylusType>(s.stylus); d.stylusUm = s.stylusUm;
    d.disc.fmt = static_cast<DiscFormat>(s.disc); d.autoPos = s.adAutoPos; d.posMm = s.posMm; d.sideMin = s.sideMin; d.intensity = s.adInt;
    return p;
}

// =====================================================================
// File cleaning (also used by the --clean command line)
// =====================================================================
struct CleanResult { unsigned rate = 0; int channels = 0, bits = 24; double seconds = 0; uint64_t clicks = 0, crackles = 0; };

// Streaming WAV writer (PCM 16/24 bits).
class WavWriter {
public:
    bool open(const std::string& path, unsigned rate, int ch, int bits) {
        f_ = vcd::openUtf8(path, "wb");
        if (!f_) return false;
        ch_ = ch; bps_ = bits / 8;
        const char hdr[44] = {};
        std::fwrite(hdr, 1, 44, f_);
        rate_ = rate;
        return true;
    }
    void write(const float* x, size_t n) {   // n interleaved samples
        buf_.resize(n * size_t(bps_));
        for (size_t i = 0; i < n; ++i) {
            uint8_t* p = buf_.data() + i * size_t(bps_);
            if (bps_ == 2) { double d = std::nearbyint(double(x[i]) * 32768.0); d = std::fmin(std::fmax(d, -32768.0), 32767.0); const int16_t t = int16_t(d); p[0] = uint8_t(t); p[1] = uint8_t(t >> 8); }
            else { double d = std::nearbyint(double(x[i]) * 8388608.0); d = std::fmin(std::fmax(d, -8388608.0), 8388607.0); const int32_t t = int32_t(d); p[0] = uint8_t(t); p[1] = uint8_t(t >> 8); p[2] = uint8_t(t >> 16); }
        }
        std::fwrite(buf_.data(), 1, buf_.size(), f_);
        data_ += buf_.size();
    }
    bool close() {
        if (!f_) return false;
        auto w32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f_); };
        auto w16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f_); };
        std::fseek(f_, 0, SEEK_SET);
        const uint32_t dl = uint32_t(std::min<uint64_t>(data_, 0xFFFFFFFFull - 36));
        std::fwrite("RIFF", 1, 4, f_); w32(36 + dl); std::fwrite("WAVE", 1, 4, f_);
        std::fwrite("fmt ", 1, 4, f_); w32(16); w16(1); w16(uint16_t(ch_)); w32(rate_); w32(rate_ * uint32_t(ch_ * bps_));
        w16(uint16_t(ch_ * bps_)); w16(uint16_t(bps_ * 8));
        std::fwrite("data", 1, 4, f_); w32(dl);
        const bool ok = !std::ferror(f_);
        std::fclose(f_); f_ = nullptr;
        return ok;
    }
private:
    FILE* f_ = nullptr;
    int ch_ = 2, bps_ = 3;
    unsigned rate_ = 48000;
    uint64_t data_ = 0;
    std::vector<uint8_t> buf_;
};

// Streaming FLAC writer.
class FlacWriter {
public:
    bool open(const std::string& path, unsigned rate, int ch, int bits) {
        enc_ = FLAC__stream_encoder_new();
        if (!enc_) return false;
        FLAC__stream_encoder_set_channels(enc_, unsigned(ch));
        FLAC__stream_encoder_set_bits_per_sample(enc_, unsigned(bits));
        FLAC__stream_encoder_set_sample_rate(enc_, rate);
        FLAC__stream_encoder_set_compression_level(enc_, 5);
        fp_ = vcd::openUtf8(path, "w+b");
        if (!fp_ || FLAC__stream_encoder_init_FILE(enc_, fp_, nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            if (fp_) std::fclose(fp_);
            FLAC__stream_encoder_delete(enc_); enc_ = nullptr; fp_ = nullptr;
            return false;
        }
        ch_ = ch; scale_ = double(1 << (bits - 1)); max_ = (1 << (bits - 1)) - 1;
        return true;
    }
    bool write(const float* x, size_t n) {
        buf_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            double d = std::nearbyint(double(x[i]) * scale_);
            d = std::fmin(std::fmax(d, -double(max_) - 1.0), double(max_));
            buf_[i] = int32_t(d);
        }
        return FLAC__stream_encoder_process_interleaved(enc_, buf_.data(), unsigned(n / size_t(ch_)));
    }
    bool close() {
        if (!enc_) return false;
        const bool ok = FLAC__stream_encoder_finish(enc_);   // also closes the FILE
        FLAC__stream_encoder_delete(enc_); enc_ = nullptr; fp_ = nullptr;
        return ok;
    }
private:
    FLAC__StreamEncoder* enc_ = nullptr;
    FILE* fp_ = nullptr;
    int ch_ = 2;
    double scale_ = 8388608.0;
    long max_ = 8388607;
    std::vector<int32_t> buf_;
};

static ma_result openDecoder(const std::string& path, const ma_decoder_config& cfg, ma_decoder& dec) {
#ifdef _WIN32
    return ma_decoder_init_file_w(vcd::widen(path).c_str(), &cfg, &dec);
#else
    return ma_decoder_init_file(path.c_str(), &cfg, &dec);
#endif
}

// Cleans `in` (WAV or FLAC) into `out` (fmt "wav" or "flac"), exactly like
// vc-file: optional RIAA, then the declick → decrackle → anti-distortion chain,
// latency compensated, the file being one side from its start.
static bool cleanFile(const std::string& in, const std::string& out, const std::string& fmt, const Settings& s,
                      const std::function<bool(double)>& progress, CleanResult& res, std::string& err) {
    // Native format (for the output bit depth).
    ma_decoder probe;
    ma_decoder_config pc = ma_decoder_config_init(ma_format_unknown, 0, 0);
    if (openDecoder(in, pc, probe) != MA_SUCCESS) { err = "unreadable file (WAV or FLAC expected)"; return false; }
    const ma_format nat = probe.outputFormat;
    ma_decoder_uninit(&probe);

    ma_decoder dec;
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (openDecoder(in, dc, dec) != MA_SUCCESS) { err = "unreadable file (WAV or FLAC expected)"; return false; }
    const int nch = int(dec.outputChannels);
    const unsigned rate = dec.outputSampleRate;
    ma_uint64 total = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &total);
    if (nch < 1 || nch > Chain::kMaxChannels || rate < 8000) { ma_decoder_uninit(&dec); err = "unsupported channel count or sample rate"; return false; }

    Chain ch;
    if (!ch.prepare(rate, nch)) { ma_decoder_uninit(&dec); err = "unsupported channel count or sample rate"; return false; }
    const ChainParams prm = chainParams(s);
    ch.setParams(prm);
    if (prm.dist.enabled && prm.dist.autoPos) ch.distStartSide();   // the file starts at the beginning of a side
    std::vector<PhonoStage> ph(static_cast<size_t>(nch));
    const double g = std::pow(10.0, s.phonoGainDb / 20.0);
    for (auto& p : ph) { p.prepare(rate); p.setMixNow(true, g); }

    const int bits = (nat == ma_format_s16 || nat == ma_format_u8) ? 16 : 24;
    WavWriter ww; FlacWriter fw;
    const bool flac = (fmt == "flac");
    if (!(flac ? fw.open(out, rate, nch, bits) : ww.open(out, rate, nch, bits))) { ma_decoder_uninit(&dec); err = "cannot create the output file"; return false; }

    const int B = 512;
    const size_t D = size_t(ch.latency());
    std::vector<float> inter(size_t(B) * nch), pin(size_t(B) * nch), pout(size_t(B) * nch), outI;
    outI.reserve(size_t(B) * nch);
    std::vector<const float*> ip(nch);
    std::vector<float*> op(nch);
    for (int c = 0; c < nch; ++c) { ip[c] = &pin[size_t(c) * B]; op[c] = &pout[size_t(c) * B]; }
    uint64_t readFrames = 0, produced = 0, written = 0;
    bool eof = false, ok = true;
    while (ok) {
        ma_uint64 got = 0;
        if (!eof) {
            ma_decoder_read_pcm_frames(&dec, inter.data(), ma_uint64(B), &got);
            if (got < ma_uint64(B)) eof = true;
            readFrames += got;
        }
        int m = int(got);
        // After the end: D frames of silence push the last samples out.
        if (eof && m < B) {
            const uint64_t need = readFrames + D;
            const int pad = int(std::min<uint64_t>(uint64_t(B - m), need > produced + uint64_t(m) ? need - produced - uint64_t(m) : 0));
            for (int i = m; i < m + pad; ++i) for (int c = 0; c < nch; ++c) inter[size_t(i) * nch + c] = 0.f;
            m += pad;
        }
        if (m == 0) break;
        for (int i = 0; i < int(got); ++i)
            for (int c = 0; c < nch; ++c) {
                float v = inter[size_t(i) * nch + c];
                if (s.riaa) v = ph[size_t(c)].tick(v, true, g, s.rumble);
                pin[size_t(c) * B + i] = v;
            }
        for (int i = int(got); i < m; ++i) for (int c = 0; c < nch; ++c) pin[size_t(c) * B + i] = 0.f;
        ch.process(ip.data(), op.data(), m);
        outI.clear();
        for (int i = 0; i < m; ++i) {
            const uint64_t o = produced + uint64_t(i);
            if (o < D || o - D >= readFrames) continue;
            for (int c = 0; c < nch; ++c) outI.push_back(pout[size_t(c) * B + i]);
        }
        produced += uint64_t(m);
        if (!outI.empty()) {
            if (flac) ok = fw.write(outI.data(), outI.size()); else ww.write(outI.data(), outI.size());
            written += outI.size() / size_t(nch);
        }
        if (progress && (produced % (uint64_t(B) * 64)) < uint64_t(B)) {
            const double denom = double(total ? total : readFrames + 1);
            if (!progress(std::min(0.999, double(readFrames) / denom))) { ok = false; err = "cancelled"; }
        }
        if (eof && written >= readFrames) break;
    }
    ma_decoder_uninit(&dec);
    const bool closed = flac ? fw.close() : ww.close();
    if (ok && !closed) { ok = false; err = "write error (disk full?)"; }
    res.rate = rate; res.channels = nch; res.bits = bits; res.seconds = double(readFrames) / rate;
    res.clicks = ch.declickStats().repaired; res.crackles = ch.decrackleStats().repaired;
    if (progress && ok) progress(1.0);
    return ok;
}

// =====================================================================
// Real-time engine (miniaudio duplex device)
// =====================================================================
struct DevInfo { std::string name; ma_device_id id; bool isDefault = false; };

struct Engine {
    Shared* sh = nullptr;
    ma_context ctx{};
    bool ctxOk = false;
    ma_device dev{};
    std::atomic<bool> open{false};
    std::atomic<bool> lost{false};   // the system stopped the device (unplugged, driver error…)
    Settings cfg;
    unsigned rate = 48000;
    int period = 512;
    Chain dec;
    PhonoStage phono[2];
    Biquad kShelf[2], kHp[2];
    std::vector<float> pin, pout, prest;
    std::vector<const float*> ip;
    std::vector<float*> op, rp;
    unsigned seenSide = 0;
    float loadAvg = 0.f;
    int warm = 50;
    static constexpr int kMax = 8192;   // largest block processed at once

    bool init(std::string& err) {
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) { err = "no audio system found"; return false; }
        ctxOk = true;
        return true;
    }
    void listDevices(std::vector<DevInfo>& caps, std::vector<DevInfo>& plays) {
        caps.clear(); plays.clear();
        if (!ctxOk) return;
        ma_device_info* pb = nullptr; ma_uint32 np = 0;
        ma_device_info* cp = nullptr; ma_uint32 nc = 0;
        if (ma_context_get_devices(&ctx, &pb, &np, &cp, &nc) != MA_SUCCESS) return;
        for (ma_uint32 i = 0; i < nc; ++i) caps.push_back({cp[i].name, cp[i].id, cp[i].isDefault != 0});
        for (ma_uint32 i = 0; i < np; ++i) plays.push_back({pb[i].name, pb[i].id, pb[i].isDefault != 0});
    }
    static void callback(ma_device* d, void* out, const void* in, ma_uint32 n) {
        auto* e = static_cast<Engine*>(d->pUserData);
        const float* x = static_cast<const float*>(in);
        float* y = static_cast<float*>(out);
        for (ma_uint32 off = 0; off < n;) {
            const int m = int(std::min<ma_uint32>(n - off, ma_uint32(kMax)));
            e->processBlock(x ? x + size_t(off) * 2 : nullptr, y + size_t(off) * 2, m);
            off += ma_uint32(m);
        }
    }
    void processBlock(const float* x, float* y, int P) {
        Shared* s = sh;
        const auto t0 = std::chrono::steady_clock::now();
        float pinPk[2] = {0.f, 0.f}, poutPk[2] = {0.f, 0.f};
        for (int i = 0; i < P; ++i)
            for (int c = 0; c < 2; ++c) {
                const float v = x ? x[size_t(i) * 2 + c] : 0.f;
                pin[size_t(c) * kMax + i] = v;
                pinPk[c] = std::fmax(pinPk[c], std::fabs(v));
            }
        // Built-in phono preamp (fades, no jumps).
        {
            const bool on = s->riaa.load(std::memory_order_relaxed);
            const double g = s->phonoGain.load(std::memory_order_relaxed);
            const bool rb = s->rumble.load(std::memory_order_relaxed);
            bool over = false;
            for (int c = 0; c < 2; ++c) {
                float* v = &pin[size_t(c) * kMax];
                if (on || !phono[0].settled(false)) { for (int i = 0; i < P; ++i) { v[i] = phono[c].tick(v[i], on, g, rb); over = over || std::fabs(v[i]) >= 0.999f; } }
                else for (int i = 0; i < P; ++i) phono[c].tick(v[i], false, g, rb);
            }
            if (over) s->satPhono.fetch_add(1, std::memory_order_relaxed);
        }
        s->lv.push(levelSums(ip[0], ip[1], size_t(P)));   // balance / stereo / noise
        const bool recClean = s->recClean.load(std::memory_order_relaxed);
        if (s->rec && !recClean) s->rec->push(ip.data(), 2, size_t(P));   // raw sound: lock-free copy (after the RIAA)
        {
            float pk = 0.f;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < P; ++i) pk = std::fmax(pk, std::fabs(pin[size_t(c) * kMax + i]));
            if (pk > s->recPeak.load(std::memory_order_relaxed)) s->recPeak.store(pk, std::memory_order_relaxed);
        }

        ChainParams prm;
        const auto r = std::memory_order_relaxed;
        prm.declick.intensity = s->declick.load(r);
        prm.declick.maxClickMs = s->maxClick.load(r);
        prm.declick.protect = s->protect.load(r);
        prm.decrackle.intensity = s->crackle.load(r);
        prm.decrackle.protect = prm.declick.protect;
        prm.declick.multiband = prm.decrackle.multiband = s->multiband.load(r);
        prm.declick.stereo = prm.decrackle.stereo = s->stereo.load(r);
        prm.mode = modeFromInt(s->mode.load(r));
        auto& d = prm.dist;
        d.enabled = s->adOn.load(r);
        d.arm.effLenMm = s->adArmLen.load(r);
        d.arm.align = static_cast<Alignment>(s->adAlign.load(r));
        d.arm.null1 = s->adNull1.load(r); d.arm.null2 = s->adNull2.load(r);
        d.stylus = static_cast<StylusType>(s->adStylus.load(r));
        d.stylusUm = s->adStylusUm.load(r);
        d.disc.fmt = static_cast<DiscFormat>(s->adDisc.load(r));
        d.autoPos = s->adAutoPos.load(r);
        d.posMm = s->adPosMm.load(r); d.sideMin = s->adSideMin.load(r); d.intensity = s->adInt.load(r);
        dec.setParams(prm);
        const unsigned ns = s->adNewSide.load(r);
        if (ns != seenSide) { seenSide = ns; dec.distStartSide(); }

        dec.process(ip.data(), op.data(), P, recClean ? rp.data() : nullptr);
        if (s->rec && recClean) s->rec->push(rp.data(), 2, size_t(P));    // cleaned sound (whatever the listening mode)
        const AntiDistState ds = dec.distState();
        s->adRadius.store(float(ds.radiusMm), r); s->adTrack.store(float(ds.trackDeg), r);
        s->adGroove.store(float(ds.grooveCms), r); s->adSide.store(float(ds.sideSec), r);
        s->adReady.store(ds.levelReady, r);

        float ms = 0.f;
        for (int i = 0; i < P; ++i)
            for (int c = 0; c < 2; ++c) {
                const float v = pout[size_t(c) * kMax + i];
                y[size_t(i) * 2 + c] = v;
                poutPk[c] = std::fmax(poutPk[c], std::fabs(v));
                const float k = kHp[c].tick(kShelf[c].tick(v));
                ms += k * k;
            }
        s->loud.push({ms / float(P), std::fmax(poutPk[0], poutPk[1]), uint32_t(P)});

        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const float ld = float(100.0 * dt * rate / P);
        loadAvg = 0.98f * loadAvg + 0.02f * ld;
        s->load.store(loadAvg, r);
        if (warm > 0) { --warm; loadAvg = ld; }
        else {
            if (ld > s->loadMax.load(r)) s->loadMax.store(ld, r);
            if (ld > 100.f) s->xruns.fetch_add(1, r);
        }
        s->repaired.store(dec.declickStats().repaired, r);
        s->crackles.store(dec.decrackleStats().repaired, r);
        for (int c = 0; c < 2; ++c) {
            s->peakIn[c].store(std::fmax(pinPk[c], s->peakIn[c].load(r)), r);
            s->peakOut[c].store(std::fmax(poutPk[c], s->peakOut[c].load(r)), r);
        }
        if (std::fmax(pinPk[0], pinPk[1]) >= 0.999f) s->satCount.fetch_add(1, r);
    }

    bool start(const Settings& want, std::string& err) {
        stop();
        cfg = want;
        rate = want.rate;
        if (!dec.prepare(rate, 2)) { err = "invalid sample rate"; return false; }
        for (auto& p : phono) { p.prepare(rate); p.setMixNow(sh->riaa.load(), sh->phonoGain.load()); }
        for (int c = 0; c < 2; ++c) kWeighting(rate, kShelf[c], kHp[c]);
        pin.assign(size_t(kMax) * 2, 0.f); pout.assign(size_t(kMax) * 2, 0.f); prest.assign(size_t(kMax) * 2, 0.f);
        ip = {&pin[0], &pin[kMax]}; op = {&pout[0], &pout[kMax]}; rp = {&prest[0], &prest[kMax]};
        seenSide = sh->adNewSide.load();
        loadAvg = 0.f; warm = 50;
        period = std::max(dec.hopSize(), int(std::lround(rate * 0.010 / dec.hopSize())) * dec.hopSize());   // ≈ 10 ms, multiple of the hop
        std::vector<DevInfo> caps, plays;
        listDevices(caps, plays);
        auto find = [](const std::vector<DevInfo>& v, const std::string& name) -> const DevInfo* {
            if (name.empty() || name == "default") return nullptr;
            for (auto& d : v) if (d.name == name) return &d;
            return nullptr;
        };
        const DevInfo* ci = find(caps, want.inDev);
        const DevInfo* co = find(plays, want.outDev);
        if (want.inDev != "default" && !ci) { err = "input not found: " + want.inDev; return false; }
        if (want.outDev != "default" && !co) { err = "output not found: " + want.outDev; return false; }
        ma_device_config dc = ma_device_config_init(ma_device_type_duplex);
        dc.capture.pDeviceID = ci ? &ci->id : nullptr;
        dc.capture.format = ma_format_f32;
        dc.capture.channels = 2;
        dc.playback.pDeviceID = co ? &co->id : nullptr;
        dc.playback.format = ma_format_f32;
        dc.playback.channels = 2;
        dc.sampleRate = rate;
        dc.periodSizeInFrames = ma_uint32(period);
        dc.periods = ma_uint32(std::max(2, want.periods));
        dc.performanceProfile = ma_performance_profile_low_latency;
        dc.noPreSilencedOutputBuffer = MA_TRUE;
        dc.noClip = MA_TRUE;
        dc.dataCallback = &Engine::callback;
        dc.pUserData = this;
        dc.notificationCallback = [](const ma_device_notification* n) {
            auto* e = static_cast<Engine*>(n->pDevice->pUserData);
            if (n->type == ma_device_notification_type_stopped && e->open) e->lost = true;
        };
        if (ma_device_init(&ctx, &dc, &dev) != MA_SUCCESS) { err = "cannot open the sound card at " + std::to_string(rate) + " Hz"; return false; }
        lost = false;
        open = true;   // before start(): a "stopped" notification is only meaningful once running
        if (ma_device_start(&dev) != MA_SUCCESS) { open = false; ma_device_uninit(&dev); err = "the sound card does not start"; return false; }
        return true;
    }
    void stop() {
        if (open) { open = false; ma_device_uninit(&dev); }
    }
    double latencyMs() const { return 1000.0 * (dec.latency() + period * cfg.periods) / rate; }
    ~Engine() { stop(); if (ctxOk) ma_context_uninit(&ctx); }
};

// ---- Command line ----
static void usage() {
    std::printf(
        "vinylclean " VC_VERSION " — real-time vinyl cleaning and file cleaning (desktop)\n\n"
        "Usage: vinylclean [options]\n"
        "  --port N          first port tried for the control page (default 8090)\n"
        "  --lan             allow other devices of the network (phone) to open the page\n"
        "  --browser         open the page in the default browser instead of an app window\n"
        "  --no-window       do not open any window (print the address)\n"
        "  --clean IN OUT    clean a WAV/FLAC file without the window (OUT: .wav or .flac),\n"
        "                    with the saved settings, then quit\n"
        "  --sens N          (with --clean) sensitivity 0..5\n");
}

int main(int argc, char** argv) {
    int port = 8090;
    bool lan = false, browserOnly = false, noWindow = false;
    std::string cleanIn, cleanOut;
    int cleanSens = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--lan") lan = true;
        else if (a == "--browser") browserOnly = true;
        else if (a == "--no-window") noWindow = true;
        else if (a == "--clean" && i + 2 < argc) { cleanIn = argv[++i]; cleanOut = argv[++i]; }
        else if (a == "--sens" && i + 1 < argc) cleanSens = std::atoi(argv[++i]);
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a.rfind("-psn", 0) == 0) continue;
        else { usage(); return 1; }
    }

    const std::string cfgDir = vcd::configDir();
    const std::string settingsPath = cfgDir + "/desktop-settings.json";
    Settings st;
    {
        std::ifstream f(vcd::u8(settingsPath));
        std::stringstream ss; ss << f.rdbuf();
        JsonFlat j;
        if (j.parse(ss.str())) settingsFromJson(j, st);
    }

    // ---- File cleaning without the window ----
    if (!cleanIn.empty()) {
        Settings s = st;
        if (cleanSens >= 0) { s.declick = declickIntensityForSens(cleanSens); s.decrackle = decrackleIntensityForSens(cleanSens); }
        std::string ext = cleanOut.size() > 5 ? cleanOut.substr(cleanOut.size() - 5) : "";
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        CleanResult r; std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!cleanFile(cleanIn, cleanOut, ext == ".flac" ? "flac" : "wav", s, nullptr, r, err)) { std::fprintf(stderr, "Error: %s\n", err.c_str()); return 1; }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s: %.1f s, %u Hz, %d channel(s), %d bits; %llu clicks, %llu crackles repaired; %.1f s of computing\n",
                    cleanOut.c_str(), r.seconds, r.rate, r.channels, r.bits, (unsigned long long)r.clicks, (unsigned long long)r.crackles, sec);
        return 0;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Shared sh;
    auto pushToShared = [&](const Settings& s) {
        sh.declick = s.declick; sh.crackle = s.decrackle; sh.protect = s.protect;
        sh.maxClick = s.maxClick; sh.mode = s.mode; sh.multiband = s.multiband; sh.stereo = s.stereo;
        sh.riaa = s.riaa; sh.phonoGain = float(std::pow(10.0, s.phonoGainDb / 20.0)); sh.rumble = s.rumble;
        sh.adArmLen = s.armLen; sh.adAlign = s.align; sh.adNull1 = s.null1; sh.adNull2 = s.null2;
        sh.adStylus = s.stylus; sh.adStylusUm = s.stylusUm; sh.adDisc = s.disc; sh.adAutoPos = s.adAutoPos;
        sh.adPosMm = s.posMm; sh.adSideMin = s.sideMin; sh.adInt = s.adInt; sh.adOn = s.adOn;
    };
    pushToShared(st);

    std::mutex stMutex;
    Settings pending;
    std::atomic<bool> reconfig{false}, dirty{false}, wantCards{true}, changed{false}, wantRecs{true}, wantFile{true};
    std::string lastError;

    Engine eng;
    eng.sh = &sh;
    std::string err;
    if (!eng.init(err)) lastError = err;

    // ---- Recording ----
    const std::string recDir = vcd::musicDir();
    vcd::Recorder rec;
    sh.rec = &rec;
    struct RecCmd { int kind; std::string arg; };
    std::vector<RecCmd> recCmds;
    std::string recMsg, recName;

    // ---- File cleaning (one job at a time) ----
    const std::string tmpDir = [] {
        std::error_code ec;
        auto p = std::filesystem::temp_directory_path(ec) / "vinylclean";
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p, ec);
        return vcd::fromPath(p);
    }();
    struct FileJob {
        std::mutex m;
        int state = 0;               // 0 nothing, 1 receiving, 2 cleaning, 3 done, 4 error
        double progress = 0, seconds = 0;
        std::string name, inPath, outPath, outName, fmt, error, inType;
        uint64_t clicks = 0, crackles = 0;
        std::thread th;
        std::atomic<bool> cancel{false};
    } job;
    auto fileJson = [&]() {
        std::lock_guard<std::mutex> lk(job.m);
        char b[256];
        std::snprintf(b, sizeof b, ",\"state\":%d,\"progress\":%.3f,\"sec\":%.1f,\"clicks\":%llu,\"crackles\":%llu,\"fmt\":\"%s\"}",
                      job.state, job.progress, job.seconds, (unsigned long long)job.clicks, (unsigned long long)job.crackles, job.fmt.c_str());
        return "{\"type\":\"file\",\"name\":\"" + jsonEscape(job.name) + "\",\"out\":\"" + jsonEscape(job.outName) +
               "\",\"error\":\"" + jsonEscape(job.error) + "\"" + b;
    };

    // ---- Web page ----
    vcd::HttpServer web;
    auto stateJson = [&]() {
        std::lock_guard<std::mutex> lk(stMutex);
        std::string s = settingsToJson(st);
        s.insert(1, "\"type\":\"settings\",");
        return s;
    };
    const std::string page = [] {
        std::string p = kIndexHtml;
        const size_t at = p.rfind("</body>");
        if (at != std::string::npos) p.insert(at, kExtraHtml); else p += kExtraHtml;
        return p;
    }();
    vcd::HttpServer::Callbacks cb;
    cb.page = [&] { return page; };
    cb.simplePage = [] { return std::string(kSimpleHtml); };
    cb.onConnect = [&]() { wantCards = true; wantRecs = true; wantFile = true; return std::vector<std::string>{stateJson(), fileJson()}; };
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
            changed = true;
        } else if (cmd == "cards") {
            wantCards = true;
        } else if (cmd == "ad_side") {
            sh.adNewSide.fetch_add(1);
        } else if (cmd == "rec_start" || cmd == "rec_stop" || cmd == "rec_delete") {
            std::lock_guard<std::mutex> lk(stMutex);
            recCmds.push_back({cmd == "rec_start" ? 1 : cmd == "rec_stop" ? 2 : 3, j.str(cmd == "rec_delete" ? "file" : "name")});
        } else if (cmd == "file_cancel") {
            job.cancel = true;
        } else if (cmd == "file_clear") {
            std::lock_guard<std::mutex> lk(job.m);
            if (job.state != 1 && job.state != 2) { job.state = 0; job.name.clear(); job.outName.clear(); job.error.clear(); }
            wantFile = true;
        }
        return "";
    };
    cb.file = [&](const std::string& path) -> vcd::ServedFile {
        static const std::string kDl = "/download/";
        if (path.compare(0, kDl.size(), kDl) == 0) {
            std::string full;
            if (vcd::validRecFile(recDir, path.substr(kDl.size()), &full)) return {full, "audio/flac", path.substr(kDl.size())};
            return {};
        }
        std::lock_guard<std::mutex> lk(job.m);
        if (path == "/file/original" && !job.inPath.empty() && job.state >= 2) return {job.inPath, job.inType, ""};
        if (job.state != 3) return {};
        if (path == "/file/cleaned") return {job.outPath, job.fmt == "flac" ? "audio/flac" : "audio/wav", ""};
        if (path == "/file/download") return {job.outPath, job.fmt == "flac" ? "audio/flac" : "audio/wav", job.outName};
        return {};
    };
    cb.upload = [&](const std::string& name, const std::string& fmtIn, uint64_t length,
                    const std::function<size_t(char*, size_t)>& read) -> std::string {
        const size_t dot = name.rfind('.');
        std::string ext = dot == std::string::npos ? "" : name.substr(dot);
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".wav" && ext != ".flac") return "WAV or FLAC file expected";
        {
            std::lock_guard<std::mutex> lk(job.m);
            if (job.state == 1 || job.state == 2) return "busy";
            if (job.th.joinable()) job.th.join();
            job.state = 1; job.progress = 0; job.name = name; job.error.clear(); job.outName.clear();
            job.fmt = (fmtIn == "flac") ? "flac" : "wav";
            job.inPath = tmpDir + "/input" + ext;
            job.inType = ext == ".flac" ? "audio/flac" : "audio/wav";
            job.cancel = false;
        }
        wantFile = true;
        if (vcd::freeBytes(tmpDir) < length * 3 + (uint64_t(200) << 20)) {
            std::lock_guard<std::mutex> lk(job.m); job.state = 4; job.error = "no_space"; wantFile = true; return "not enough disk space";
        }
        FILE* f = vcd::openUtf8(job.inPath, "wb");
        uint64_t got = 0;
        if (f) {
            std::vector<char> b(1 << 16);
            for (;;) { const size_t k = read(b.data(), b.size()); if (k == 0) break; std::fwrite(b.data(), 1, k, f); got += k; }
            std::fclose(f);
        }
        if (!f || got != length) {
            std::fprintf(stderr, "upload failed: %s, %llu of %llu bytes\n", f ? "short read" : "cannot create", (unsigned long long)got, (unsigned long long)length);
            std::lock_guard<std::mutex> lk(job.m); job.state = 4; job.error = "upload"; wantFile = true; return "upload failed";
        }
        Settings snap; { std::lock_guard<std::mutex> lk(stMutex); snap = st; }
        std::lock_guard<std::mutex> lk(job.m);
        job.state = 2;
        std::string base = name.substr(0, name.rfind('.'));
        job.outName = base + " (clean)." + job.fmt;
        job.outPath = tmpDir + "/output." + job.fmt;
        job.th = std::thread([&job, &wantFile, snap] {
            CleanResult r; std::string e;
            std::string in, out, fmt;
            { std::lock_guard<std::mutex> lk(job.m); in = job.inPath; out = job.outPath; fmt = job.fmt; }
            auto t0 = std::chrono::steady_clock::now();
            const bool ok = cleanFile(in, out, fmt, snap, [&](double p) {
                std::lock_guard<std::mutex> lk(job.m); job.progress = p;
                if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(200)) { t0 = std::chrono::steady_clock::now(); wantFile = true; }
                return !job.cancel.load();
            }, r, e);
            std::lock_guard<std::mutex> lk(job.m);
            job.state = ok ? 3 : 4; job.error = ok ? "" : e; job.seconds = r.seconds; job.clicks = r.clicks; job.crackles = r.crackles;
            job.progress = ok ? 1.0 : job.progress;
            wantFile = true;
        });
        return "";
    };
    // Already running (double-click twice…): open its window instead of starting a
    // second sound engine on the same card.
    for (int p = port; p < port + 20; ++p)
        if (vcd::httpGetLocal(p, "/ping", 300) == "vinylclean\n") {
            const std::string u = "http://127.0.0.1:" + std::to_string(p) + "/";
            if (!noWindow) vcd::openWindow(u, cfgDir + "/window", browserOnly);
            std::printf("vinylclean is already running: %s\n", u.c_str());
            return 0;
        }
    if (!web.start(port, lan, cb, err)) {
        std::fprintf(stderr, "Control page unavailable: %s\n", err.c_str());
#ifdef _WIN32
        MessageBoxW(nullptr, vcd::widen("vinylclean: control page unavailable (" + err + ")").c_str(), L"vinylclean", MB_ICONERROR);
#endif
        return 1;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(web.port()) + "/";
    std::printf("vinylclean %s — control page: %s\n", VC_VERSION, url.c_str());
    std::fflush(stdout);

    Settings good = st;
    bool engineOpen = false;
    if (eng.ctxOk) {
        if (eng.start(st, err)) { engineOpen = true; good = st; lastError.clear(); }
        else lastError = err;
    }
    bool windowOpened = false;
    if (!noWindow) windowOpened = vcd::openWindow(url, cfgDir + "/window", browserOnly);
    if (!noWindow && !windowOpened) std::printf("Open %s in a browser.\n", url.c_str());

    LoudnessMeter meter;
    meter.reset(st.rate);
    LevelStats levels;
    levels.reset(st.rate);
    double lST = -120, lInt = -120, lLRA = 0, lPLR = 0, lSec = 0;
    auto lastLoud = std::chrono::steady_clock::now(), lastBcast = lastLoud, lastSave = lastLoud, lastRecs = lastLoud;
    auto lastClient = lastLoud, lastSound = lastLoud, lastRetry = lastLoud;
    bool everConnected = false;
    unsigned lastSat = 0, lastSatP = 0;
    auto lastSatTime = lastLoud - std::chrono::seconds(10), lastSatPTime = lastSatTime;

    auto recStop = [&](const std::string& code) {
        if (!rec.recording()) return;
        const std::string e = rec.error();
        const double sec = rec.seconds();
        const uint64_t lost = rec.droppedBlocks();
        rec.stop();
        char d[32];
        std::snprintf(d, sizeof d, "%d:%02d", int(sec) / 60, int(sec) % 60);
        { std::lock_guard<std::mutex> lk(stMutex); recMsg = code + "|" + (e.empty() ? recName : e) + "|" + d + "|" + (lost ? "1" : "0"); }
        wantRecs = true;
    };

    while (gRun) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = std::chrono::steady_clock::now();

        // The window was closed: quit (a reload reconnects within 2 s).
        if (web.clients() > 0) { everConnected = true; lastClient = now; }
        else if (windowOpened && everConnected && now - lastClient > std::chrono::seconds(8)) break;   // a recording is stopped and saved below

        // ---- Recording commands ----
        {
            std::vector<RecCmd> cmds;
            { std::lock_guard<std::mutex> lk(stMutex); cmds.swap(recCmds); }
            for (auto& c : cmds) {
                if (c.kind == 1 && !rec.recording()) {
                    std::string e;
                    if (!engineOpen) e = "card_not_ready";
                    else if (vcd::freeBytes(recDir) < (uint64_t(300) << 20)) e = "no_space";
                    else {
                        const std::string base = vcd::safeRecName(c.arg);
                        const std::string path = vcd::uniqueRecPath(recDir, base);
                        bool clean; { std::lock_guard<std::mutex> lk(stMutex); clean = st.recClean; }
                        sh.recClean = clean;
                        if (rec.start(path, eng.rate, 2, 24, base, e, clean ? "Cleaned recording — vinylclean" : "Raw recording (no processing) — vinylclean")) {
                            recName = path.substr(recDir.size() + 1);
                            std::lock_guard<std::mutex> lk(stMutex); recMsg.clear();
                        }
                    }
                    if (!e.empty()) { std::lock_guard<std::mutex> lk(stMutex); recMsg = "start_fail|" + e; }
                    wantRecs = true;
                } else if (c.kind == 2) {
                    recStop("saved");
                } else if (c.kind == 3) {
                    std::string full;
                    std::lock_guard<std::mutex> lk(stMutex);
                    std::error_code ec;
                    if (rec.recording() && c.arg == recName) recMsg = "del_current|" + c.arg;
                    else if (!vcd::validRecFile(recDir, c.arg, &full) || !std::filesystem::remove(vcd::u8(full), ec)) recMsg = "del_fail|" + c.arg;
                    else recMsg = "del_ok|" + c.arg;
                    wantRecs = true;
                }
            }
            if (rec.failed()) recStop("stop_error");
            if (rec.recording() && now - lastRecs > std::chrono::seconds(2) && vcd::freeBytes(recDir) < (uint64_t(200) << 20)) recStop("stop_disk");
            if (rec.recording() && !engineOpen) recStop("stop_card");
            // No sound for 2 minutes (arm raised, side finished): stop and save.
            {
                const float pk = sh.recPeak.exchange(0.f);
                if (!rec.recording() || pk >= 3.2e-3f) lastSound = now;   // −50 dBFS
                else if (now - lastSound > std::chrono::seconds(120)) recStop("stop_silence");
            }
        }

        // ---- Sound card lost (unplugged…) or not started: new attempt every 3 s (like the Pi) ----
        if (engineOpen && eng.lost.load()) {
            eng.stop(); engineOpen = false;
            std::lock_guard<std::mutex> lk(stMutex); lastError = "sound card error";
        }
        if (!engineOpen && eng.ctxOk && !reconfig.load() && now - lastRetry > std::chrono::seconds(3)) {
            lastRetry = now;
            Settings want; { std::lock_guard<std::mutex> lk(stMutex); want = st; }
            std::string e;
            if (eng.start(want, e)) { engineOpen = true; good = want; meter.reset(eng.rate); levels.reset(eng.rate); std::lock_guard<std::mutex> lk(stMutex); lastError.clear(); }
        }
        // ---- Audio reconfiguration (card, rate) ----
        if (reconfig.exchange(false)) {
            if (rec.recording()) recStop("stop_reconfig");
            Settings want; { std::lock_guard<std::mutex> lk(stMutex); want = pending; }
            eng.stop(); engineOpen = false;
            std::string e;
            if (eng.start(want, e)) {
                engineOpen = true; good = want;
                std::lock_guard<std::mutex> lk(stMutex);
                st.inDev = want.inDev; st.outDev = want.outDev; st.rate = want.rate; st.periods = want.periods;
                lastError.clear();
            } else {
                std::string e2;
                if (eng.start(good, e2)) engineOpen = true;
                std::lock_guard<std::mutex> lk(stMutex);
                lastError = e + " — previous configuration restored";
            }
            meter.reset(eng.rate);
            levels.reset(eng.rate);
            dirty = true; changed = true;
        }

        // ---- Loudness ----
        { LoudItem it; while (sh.loud.pop(it)) meter.add(it); }
        { LevelItem it; while (sh.lv.pop(it)) levels.add(it); }
        if (now - lastLoud >= std::chrono::seconds(1)) { lastLoud = now; meter.compute(lST, lInt, lLRA, lPLR, lSec); levels.compute(); }

        // ---- Messages to the page ----
        if (changed.exchange(false)) web.broadcast(stateJson());
        if (wantCards.exchange(false)) {
            std::vector<DevInfo> caps, plays;
            eng.listDevices(caps, plays);
            struct Card { std::string name; bool in = false, out = false; };
            std::vector<Card> cards{{"default", true, true}};
            auto add = [&](const std::string& n, bool in) {
                for (auto& c : cards) if (c.name == n) { (in ? c.in : c.out) = true; return; }
                Card c; c.name = n; (in ? c.in : c.out) = true; cards.push_back(c);
            };
            for (auto& d : caps) add(d.name, true);
            for (auto& d : plays) add(d.name, false);
            std::string js = "{\"type\":\"cards\",\"cards\":[";
            const char* rates = "44100,48000,88200,96000";
            for (size_t i = 0; i < cards.size(); ++i) {
                js += (i ? "," : "");
                js += "{\"dev\":\"" + jsonEscape(cards[i].name) + "\",\"name\":\"" + jsonEscape(cards[i].name) + "\",\"in\":[" +
                      (cards[i].in ? rates : "") + "],\"out\":[" + (cards[i].out ? rates : "") + "],\"format\":\"32-bit float\"}";
            }
            js += "]}";
            web.broadcast(js);
        }
        if (rec.recording() && now - lastRecs > std::chrono::seconds(5)) wantRecs = true;
        if (wantRecs.exchange(false)) {
            lastRecs = now;
            std::string js = "{\"type\":\"recs\",\"free\":" + std::to_string(vcd::freeBytes(recDir)) + ",\"dir\":\"" + jsonEscape(recDir) + "\",\"items\":[";
            bool first = true;
            for (auto& e : vcd::listRecordings(recDir)) {
                char d[32]; std::strftime(d, sizeof d, "%d/%m/%Y %H:%M", std::localtime(&e.mtime));
                const bool cur = rec.recording() && e.name == recName;
                char b[256];
                std::snprintf(b, sizeof b, "\"bytes\":%llu,\"sec\":%.1f,\"date\":\"%s\",\"rec\":%s}",
                              (unsigned long long)e.bytes, cur ? rec.seconds() : e.seconds, d, cur ? "true" : "false");
                js += (first ? "" : ",") + std::string("{\"name\":\"") + jsonEscape(e.name) + "\"," + b;
                first = false;
            }
            js += "]}";
            web.broadcast(js);
        }
        if (wantFile.exchange(false)) web.broadcast(fileJson());

        // ---- Saving the settings (at most every 2 s) ----
        if (dirty && now - lastSave > std::chrono::seconds(2)) {
            dirty = false; lastSave = now;
            std::string js; { std::lock_guard<std::mutex> lk(stMutex); js = settingsToJson(st); }
            std::ofstream f(vcd::u8(settingsPath)); if (f) f << js << "\n";
        }

        if (sh.satCount.load() != lastSat) { lastSat = sh.satCount.load(); lastSatTime = now; }
        if (sh.satPhono.load() != lastSatP) { lastSatP = sh.satPhono.load(); lastSatPTime = now; }
        if (now - lastBcast >= std::chrono::milliseconds(100)) {
            lastBcast = now;
            auto db = [](float v) { return v > 1e-9f ? 20.f * std::log10(v) : -120.f; };
            float pi[2], po[2];
            for (int c = 0; c < 2; ++c) { pi[c] = sh.peakIn[c].exchange(0.f); po[c] = sh.peakOut[c].exchange(0.f); }
            std::string e, msg; { std::lock_guard<std::mutex> lk(stMutex); e = lastError; msg = recMsg; }
            char b[2400];
            std::snprintf(b, sizeof b,
                          "{\"type\":\"status\",\"running\":%s,\"inL\":%.1f,\"inR\":%.1f,\"outL\":%.1f,\"outR\":%.1f,"
                          "\"sat\":%s,\"clicks\":%llu,\"crackles\":%llu,\"load\":%.1f,\"loadMax\":%.1f,\"xruns\":%u,"
                          "\"lufsS\":%.1f,\"lufsI\":%.1f,\"lra\":%.1f,\"plr\":%.1f,\"loudSec\":%.0f,"
                          "\"rate\":%u,\"outState\":\"RUNNING\",\"format\":\"32-bit float\",\"latency\":%.0f,\"rt\":true,\"error\":\"%s\",\"clients\":%d,"
                          "\"satPhono\":%s,\"recOn\":%s,\"recSec\":%.1f,\"recLost\":%llu,\"recAvail\":true,"
                          "\"adR\":%.1f,\"adErr\":%.2f,\"adV\":%.1f,\"adSide\":%.0f,\"adReady\":%s,\"desktop\":true}",
                          engineOpen ? "true" : "false", db(pi[0]), db(pi[1]), db(po[0]), db(po[1]),
                          now - lastSatTime < std::chrono::seconds(3) ? "true" : "false", sh.repaired.load(), sh.crackles.load(),
                          (double)sh.load.load(), (double)sh.loadMax.load(), sh.xruns.load(),
                          lST, lInt, lLRA, lPLR, lSec,
                          engineOpen ? eng.rate : 0u, engineOpen ? eng.latencyMs() : 0.0, jsonEscape(e).c_str(), web.clients(),
                          now - lastSatPTime < std::chrono::seconds(3) ? "true" : "false",
                          rec.recording() ? "true" : "false", rec.seconds(), (unsigned long long)rec.droppedBlocks(),
                          (double)sh.adRadius.load(), (double)sh.adTrack.load(), (double)sh.adGroove.load(), (double)sh.adSide.load(),
                          sh.adReady.load() ? "true" : "false");
            std::string full = b;
            auto num = [](double v, const char* f) { char t[32]; if (std::isfinite(v)) std::snprintf(t, sizeof t, f, v); else std::snprintf(t, sizeof t, "null"); return std::string(t); };
            full.insert(full.size() - 1, ",\"bal\":" + num(levels.balanceDb, "%.1f") + ",\"corr\":" + num(levels.correlation, "%.2f") +
                                         ",\"noise\":" + num(levels.noiseDb, "%.0f") + ",\"recClean\":" + (sh.recClean ? "true" : "false"));
                double vl, vr; levels.takeVu(vl, vr);
                full.insert(full.size() - 1, ",\"vuL\":" + num(vl, "%.1f") + ",\"vuR\":" + num(vr, "%.1f"));
            full.insert(full.size() - 1, ",\"recName\":\"" + jsonEscape(rec.recording() ? recName : std::string()) + "\",\"recMsg\":\"" + jsonEscape(msg) + "\"");
            web.broadcast(full);
        }
    }

    // ---- Quit: a recording in progress is stopped and saved (FLAC finalized) ----
    if (rec.recording()) rec.stop();
    job.cancel = true;
    if (job.th.joinable()) job.th.join();
    eng.stop();
    web.stop();
    { std::string js; { std::lock_guard<std::mutex> lk(stMutex); js = settingsToJson(st); }
      std::ofstream f(vcd::u8(settingsPath)); if (f) f << js << "\n"; }
    std::error_code ec;
    std::filesystem::remove_all(vcd::u8(tmpDir), ec);
    return 0;
}
