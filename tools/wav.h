// vinylclean — reading / writing WAV files (PCM 16/24/32-bit, float 32).
// Small standalone code, no dependency. Original work.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

namespace vc {

struct WavData {
    int sampleRate = 48000;
    int channels = 2;
    int bits = 24;         // 16, 24, 32
    bool isFloat = false;  // true: 32-bit float
    std::vector<float> samples;  // interleaved, ±1.0
    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

namespace wavdetail {
inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline void wr32(FILE* f, uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; fwrite(b, 1, 4, f); }
inline void wr16(FILE* f, uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; fwrite(b, 1, 2, f); }
}

inline bool readWav(const std::string& path, WavData& out, std::string& err) {
    using namespace wavdetail;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> buf;
    {
        uint8_t tmp[65536];
        size_t r;
        while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + r);
        fclose(f);
    }
    if (buf.size() < 12 || memcmp(buf.data(), "RIFF", 4) || memcmp(buf.data() + 8, "WAVE", 4)) {
        err = "not a WAV file (RIFF/WAVE)";
        return false;
    }
    size_t pos = 12;
    int fmtTag = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    while (pos + 8 <= buf.size()) {
        const uint8_t* ck = buf.data() + pos;
        uint32_t len = rd32(ck + 4);
        const uint8_t* body = ck + 8;
        size_t avail = buf.size() - pos - 8;
        if (!memcmp(ck, "fmt ", 4) && len >= 16 && avail >= 16) {
            fmtTag = rd16(body);
            ch = rd16(body + 2);
            rate = static_cast<int>(rd32(body + 4));
            bits = rd16(body + 14);
            if (fmtTag == 0xFFFE && len >= 26 && avail >= 26) fmtTag = rd16(body + 24);
        } else if (!memcmp(ck, "data", 4)) {
            data = body;
            dataLen = std::min<size_t>(len, avail);
            break;
        }
        pos += 8 + len + (len & 1);
    }
    if (!data || ch <= 0 || rate <= 0) { err = "incomplete WAV (fmt or data missing)"; return false; }
    if (!((fmtTag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmtTag == 3 && bits == 32))) {
        err = "unsupported WAV format (PCM 16/24/32-bit or 32-bit float only)";
        return false;
    }
    const int bps = bits / 8;
    const size_t n = dataLen / bps;
    out.sampleRate = rate;
    out.channels = ch;
    out.bits = bits;
    out.isFloat = (fmtTag == 3);
    out.samples.resize(n - n % ch);
    for (size_t i = 0; i < out.samples.size(); ++i) {
        const uint8_t* p = data + i * bps;
        float v;
        if (out.isFloat) { float t; memcpy(&t, p, 4); v = t; }
        else if (bits == 16) v = int16_t(rd16(p)) / 32768.f;
        else if (bits == 24) { int32_t t = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8; v = t / 8388608.f; }
        else v = static_cast<float>(int32_t(rd32(p)) / 2147483648.0);
        out.samples[i] = v;
    }
    return true;
}

// Writes in the format given in w (bits / isFloat). No dither: unmodified
// samples stay bit-identical.
inline bool writeWav(const std::string& path, const WavData& w, std::string& err) {
    using namespace wavdetail;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot create " + path; return false; }
    const int bps = w.isFloat ? 4 : w.bits / 8;
    const uint32_t dataLen = static_cast<uint32_t>(w.samples.size() * bps);
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + dataLen); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16); wr16(f, w.isFloat ? 3 : 1); wr16(f, uint16_t(w.channels));
    wr32(f, uint32_t(w.sampleRate)); wr32(f, uint32_t(w.sampleRate * w.channels * bps));
    wr16(f, uint16_t(w.channels * bps)); wr16(f, uint16_t(bps * 8));
    fwrite("data", 1, 4, f); wr32(f, dataLen);
    std::vector<uint8_t> out(w.samples.size() * bps);
    for (size_t i = 0; i < w.samples.size(); ++i) {
        uint8_t* p = out.data() + i * bps;
        const double v = w.samples[i];
        if (w.isFloat) { float t = static_cast<float>(v); memcpy(p, &t, 4); }
        else if (bps == 2) { double d = std::nearbyint(v * 32768.0); d = std::fmin(std::fmax(d, -32768.0), 32767.0); int16_t t = int16_t(d); p[0] = uint8_t(t); p[1] = uint8_t(t >> 8); }
        else if (bps == 3) { double d = std::nearbyint(v * 8388608.0); d = std::fmin(std::fmax(d, -8388608.0), 8388607.0); int32_t t = int32_t(d); p[0] = uint8_t(t); p[1] = uint8_t(t >> 8); p[2] = uint8_t(t >> 16); }
        else { double d = std::nearbyint(v * 2147483648.0); d = std::fmin(std::fmax(d, -2147483648.0), 2147483647.0); int32_t t = int32_t(d); memcpy(p, &t, 4); }
    }
    fwrite(out.data(), 1, out.size(), f);
    const bool ok = !ferror(f);
    fclose(f);
    if (!ok) err = "write error " + path;
    return ok;
}

} // namespace vc
