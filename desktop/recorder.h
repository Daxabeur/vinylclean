// vinylclean desktop — recording of the RAW turntable sound to FLAC while
// listening. Same design as the Raspberry Pi recorder (tools/recorder.h):
// lock-free ring filled by the audio thread, low-priority writer thread.
// Portable version (Windows + Linux): std::filesystem, UTF-8 paths.
#pragma once

#include "platform.h"
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcd {

class Recorder {
public:
    // capacityFrames: memory reserve (≈ 10 s at 192 kHz by default, 15 MB).
    explicit Recorder(size_t capacityFrames = 192000 * 10) : cap_(capacityFrames * 2), buf_(capacityFrames * 2, 0) {}
    ~Recorder() { stop(); }

    // ---- Main thread ----
    // Starts recording into `path` (FLAC file). bits = 16 or 24.
    bool start(const std::string& path, unsigned rate, int channels, int bits, const std::string& title, std::string& err) {
        stop();
        if (channels < 1 || channels > 2 || (bits != 16 && bits != 24) || rate < 8000) { err = "unsupported format"; return false; }
        enc_ = FLAC__stream_encoder_new();
        if (!enc_) { err = "out of memory"; return false; }
        FLAC__stream_encoder_set_channels(enc_, unsigned(channels));
        FLAC__stream_encoder_set_bits_per_sample(enc_, unsigned(bits));
        FLAC__stream_encoder_set_sample_rate(enc_, rate);
        FLAC__stream_encoder_set_compression_level(enc_, 5);
        FLAC__stream_encoder_set_verify(enc_, false);
        // Tags (title, date) + 8 KB of padding to add more later without rewriting the file.
        meta_[0] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
        meta_[1] = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
        if (meta_[0] && meta_[1]) {
            auto add = [&](const char* k, const std::string& v) {
                FLAC__StreamMetadata_VorbisComment_Entry e;
                if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&e, k, v.c_str()))
                    FLAC__metadata_object_vorbiscomment_append_comment(meta_[0], e, false);
            };
            add("TITLE", title);
            char d[32]; const std::time_t t = std::time(nullptr); std::strftime(d, sizeof d, "%Y-%m-%d", std::localtime(&t));
            add("DATE", d);
            add("COMMENT", "Raw recording (no processing) — vinylclean");
            meta_[1]->length = 8192;
            FLAC__stream_encoder_set_metadata(enc_, meta_, 2);
        }
        fp_ = openUtf8(path, "w+b");
        if (!fp_ || FLAC__stream_encoder_init_FILE(enc_, fp_, nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            if (fp_) { std::fclose(fp_); fp_ = nullptr; }
            err = "cannot create the file";
            freeEncoder();
            return false;
        }
        ch_ = channels; bits_ = bits; rate_ = rate; path_ = path;
        scale_ = float(1 << (bits - 1));
        maxv_ = (1 << (bits - 1)) - 1;
        frames_ = 0; dropped_ = 0;
        { std::lock_guard<std::mutex> lk(m_); error_.clear(); }
        r_.store(w_.load());           // empty ring
        stopReq_ = false;
        writerDone_ = false;   // before the thread: otherwise the state of the previous file would look like a failure
        active_.store(true, std::memory_order_release);
        th_ = std::thread([this] { writerLoop(); });
        return true;
    }

    // Stops, flushes the reserve, finalizes the file (FLAC header updated). Returns the path.
    std::string stop() {
        active_.store(false, std::memory_order_release);
        if (th_.joinable()) { stopReq_ = true; th_.join(); }
        freeEncoder();
        std::string p; p.swap(path_);
        return p;
    }

    bool recording() const { return active_.load(std::memory_order_acquire) || th_.joinable(); }
    const std::string& path() const { return path_; }
    double seconds() const { return rate_ ? double(frames_.load()) / rate_ : 0.0; }
    uint64_t droppedBlocks() const { return dropped_.load(); }
    std::string error() const { std::lock_guard<std::mutex> lk(m_); return error_; }
    // The writer thread stopped by itself (disk full, error): to be called by the main thread.
    bool failed() const { return th_.joinable() && writerDone_.load(); }

    // ---- Audio thread (real time) ----
    // planar[c][i], c < nch. Only the first ch_ channels are kept.
    void push(const float* const* planar, int nch, size_t frames) {
        if (!active_.load(std::memory_order_acquire)) return;
        const int ch = ch_;
        if (nch < ch) return;
        const size_t need = frames * size_t(ch);
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const uint64_t r = r_.load(std::memory_order_acquire);
        if (cap_ - size_t(w - r) < need) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        size_t pos = size_t(w % cap_);
        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < ch; ++c) {
                long v = std::lrint(double(planar[c][i]) * scale_);
                v = std::min<long>(maxv_, std::max<long>(-maxv_ - 1, v));
                buf_[pos] = int32_t(v);
                if (++pos == cap_) pos = 0;
            }
        w_.store(w + need, std::memory_order_release);
    }

private:
    void writerLoop() {
        lowerThreadPriority();   // never ahead of the sound
        std::vector<int32_t> chunk(size_t(65536) * 2);
        for (;;) {
            const uint64_t w = w_.load(std::memory_order_acquire);
            uint64_t r = r_.load(std::memory_order_relaxed);
            size_t avail = size_t(w - r);
            avail -= avail % size_t(ch_);
            if (avail == 0) {
                if (stopReq_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const size_t n = std::min(avail, chunk.size() - chunk.size() % size_t(ch_));
            size_t pos = size_t(r % cap_);
            for (size_t i = 0; i < n; ++i) { chunk[i] = buf_[pos]; if (++pos == cap_) pos = 0; }
            r_.store(r + n, std::memory_order_release);
            if (!FLAC__stream_encoder_process_interleaved(enc_, chunk.data(), unsigned(n / size_t(ch_)))) {
                std::lock_guard<std::mutex> lk(m_);
                error_ = "write error (disk full?)";
                active_.store(false, std::memory_order_release);
                break;
            }
            frames_ += n / size_t(ch_);
        }
        writerDone_ = true;
    }
    void freeEncoder() {
        if (enc_) {
            if (FLAC__stream_encoder_get_state(enc_) == FLAC__STREAM_ENCODER_OK) FLAC__stream_encoder_finish(enc_);
            FLAC__stream_encoder_delete(enc_);
            enc_ = nullptr;
            fp_ = nullptr;   // closed by libFLAC
        }
        for (auto& m : meta_) if (m) { FLAC__metadata_object_delete(m); m = nullptr; }
    }

    const size_t cap_;
    std::vector<int32_t> buf_;
    std::atomic<uint64_t> w_{0}, r_{0};
    std::atomic<bool> active_{false}, stopReq_{false}, writerDone_{false};
    std::atomic<uint64_t> frames_{0}, dropped_{0};
    int ch_ = 2, bits_ = 24;
    unsigned rate_ = 0;
    float scale_ = 8388608.f;
    long maxv_ = 8388607;
    std::string path_;
    mutable std::mutex m_;
    std::string error_;
    FLAC__StreamEncoder* enc_ = nullptr;
    FILE* fp_ = nullptr;
    FLAC__StreamMetadata* meta_[2] = {nullptr, nullptr};
    std::thread th_;
};

// ---- Files ----

// Safe file name: no / \ : * ? " < > | nor control characters, no leading
// dot, ≤ 150 bytes (without cutting a UTF-8 character).
inline std::string safeRecName(const std::string& in) {
    std::string s;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F || std::strchr("/\\:*?\"<>|", c)) s += '-';
        else s += char(c);
    }
    auto trim = [](std::string& t) {
        while (!t.empty() && (t.back() == ' ' || t.back() == '.')) t.pop_back();
        size_t i = 0; while (i < t.size() && (t[i] == ' ' || t[i] == '.')) ++i;
        t.erase(0, i);
    };
    trim(s);
    if (s.size() > 4) {   // ".flac" typed by the user: removed
        std::string e = s.substr(s.size() - 5);
        for (auto& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (e == ".flac") { s.erase(s.size() - 5); trim(s); }
    }
    if (s.size() > 150) {
        size_t n = 150;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s.resize(n);
        trim(s);
    }
    if (s.empty()) {
        char d[64]; const std::time_t t = std::time(nullptr);
        std::strftime(d, sizeof d, "Recording %Y-%m-%d %Hh%M", std::localtime(&t));
        s = d;
    }
    return s;
}

// Free path in dir: "name.flac", otherwise "name (2).flac", "name (3).flac"…
inline std::string uniqueRecPath(const std::string& dir, const std::string& base) {
    std::string p = dir + "/" + base + ".flac";
    for (int i = 2; fileExists(p) && i < 10000; ++i) p = dir + "/" + base + " (" + std::to_string(i) + ").flac";
    return p;
}

// Does a name received from the page really designate a recording in dir?
inline bool validRecFile(const std::string& dir, const std::string& name, std::string* full = nullptr) {
    if (name.empty() || name.size() > 255 || name[0] == '.' || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos || name.find('\0') != std::string::npos)
        return false;
    if (name.size() < 6) return false;
    std::string e = name.substr(name.size() - 5);
    for (auto& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (e != ".flac") return false;
    const std::string p = dir + "/" + name;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(u8(p), ec)) return false;
    if (full) *full = p;
    return true;
}

struct RecEntry { std::string name; uint64_t bytes = 0; double seconds = -1; std::time_t mtime = 0; };

inline std::vector<RecEntry> listRecordings(const std::string& dir) {
    std::vector<RecEntry> v;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(u8(dir), ec)) {
        const std::string n = fromPath(de.path().filename());
        if (!validRecFile(dir, n)) continue;
        const std::string p = dir + "/" + n;
        RecEntry r;
        r.name = n;
        r.bytes = uint64_t(de.file_size(ec));
        r.mtime = fileTime(p);
        FILE* f = openUtf8(p, "rb");
        if (f) {   // STREAMINFO: total samples and rate (bytes 18..)
            unsigned char h[42];
            if (std::fread(h, 1, 42, f) == 42 && !std::memcmp(h, "fLaC", 4)) {
                const unsigned char* si = h + 8;
                const unsigned rate = (unsigned(si[10]) << 12) | (unsigned(si[11]) << 4) | (si[12] >> 4);
                const uint64_t total = (uint64_t(si[13] & 15) << 32) | (uint64_t(si[14]) << 24) | (uint64_t(si[15]) << 16) | (uint64_t(si[16]) << 8) | si[17];
                if (rate && total) r.seconds = double(total) / rate;
            }
            std::fclose(f);
        }
        v.push_back(r);
    }
    std::sort(v.begin(), v.end(), [](const RecEntry& a, const RecEntry& b) { return a.mtime > b.mtime; });
    return v;
}

inline uint64_t freeBytes(const std::string& dir) {
    std::error_code ec;
    const auto s = std::filesystem::space(u8(dir), ec);
    return ec ? 0 : uint64_t(s.available);
}

} // namespace vcd
