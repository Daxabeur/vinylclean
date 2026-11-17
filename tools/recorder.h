// vinylclean — recording of the turntable sound (raw or cleaned) to FLAC or
// WAV, while listening. FLAC 16 / 24 bits; WAV 16 / 24 bits or 32-bit float
// (keeps every value exactly, even above 0 dBFS). 16 bits: TPDF dither.
//
// Audio thread → push(): copy into a lock-free ring (no allocation, no
//                system call). If the ring is full (disk too slow), the
//                block is counted as lost: the live sound never slows down.
// Writer thread (low priority) → compresses to FLAC (libFLAC) and writes.
//
// The rest: safe file names, folder listing, deletion, free space.
#pragma once

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
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vc {

class Recorder {
public:
    // capacityFrames: memory reserve (≈ 10 s at 192 kHz by default, 15 MB).
    explicit Recorder(size_t capacityFrames = 192000 * 10) : cap_(capacityFrames * 2), buf_(capacityFrames * 2, 0) {}
    ~Recorder() { stop(); }

    // ---- Main thread ----
    // Starts recording into `path`. wav = false: FLAC, bits 16 or 24.
    // wav = true: WAV, bits 16, 24 or 32 (32 = 32-bit float).
    bool start(const std::string& path, unsigned rate, int channels, int bits, const std::string& title, std::string& err,
               const std::string& comment = "Raw recording (no processing) — vinylclean", bool wav = false) {
        stop();
        if (channels < 1 || channels > 2 || (bits != 16 && bits != 24 && !(wav && bits == 32)) || rate < 8000) { err = "unsupported format"; return false; }
        wav_ = wav; float_ = (bits == 32);
        if (wav) {
            if (!openWav(path, rate, channels, bits, title, comment)) { err = "cannot create the file"; return false; }
        } else {
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
            add("COMMENT", comment);
            meta_[1]->length = 8192;
            FLAC__stream_encoder_set_metadata(enc_, meta_, 2);
        }
        if (FLAC__stream_encoder_init_file(enc_, path.c_str(), nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            err = "cannot create the file";
            freeEncoder();
            return false;
        }
        }
        ch_ = channels; bits_ = bits; rate_ = rate; path_ = path;
        scale_ = float_ ? 1.f : float(1 << (bits - 1));
        maxv_ = float_ ? 0x7FFFFFFF : (1 << (bits - 1)) - 1;
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
                const float x = planar[c][i];
                int32_t out;
                if (float_) {
                    std::memcpy(&out, &x, 4);   // 32-bit float: the exact value
                } else {
                    double y = double(x) * scale_;
                    if (bits_ == 16 && y != std::nearbyint(y)) {
                        // TPDF dither (±1 LSB triangular): no distortion of quiet passages.
                        // A value already exact in 16 bits (16-bit card, raw sound) is kept as is.
                        rnd_ = rnd_ * 1664525u + 1013904223u; const double u1 = (rnd_ >> 8) * (1.0 / 16777216.0);
                        rnd_ = rnd_ * 1664525u + 1013904223u; const double u2 = (rnd_ >> 8) * (1.0 / 16777216.0);
                        y += u1 - u2;
                    }
                    long v = std::lrint(y);
                    out = int32_t(std::min<long>(maxv_, std::max<long>(-maxv_ - 1, v)));
                }
                buf_[pos] = out;
                if (++pos == cap_) pos = 0;
            }
        w_.store(w + need, std::memory_order_release);
    }

private:
    void writerLoop() {
        setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 10);   // never ahead of the sound
        std::vector<int32_t> chunk(size_t(65536) * 2);
        std::vector<unsigned char> bytes(chunk.size() * 4);
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
            if (wav_) {
                const char* e = writeWav(chunk.data(), n, bytes);
                if (e) {
                    std::lock_guard<std::mutex> lk(m_);
                    error_ = e;
                    active_.store(false, std::memory_order_release);
                    break;
                }
                frames_ += n / size_t(ch_);
                continue;
            }
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
        closeWav();
        if (enc_) {
            if (FLAC__stream_encoder_get_state(enc_) == FLAC__STREAM_ENCODER_OK) FLAC__stream_encoder_finish(enc_);
            FLAC__stream_encoder_delete(enc_);
            enc_ = nullptr;
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
    // WAV
    bool wav_ = false, float_ = false;
    uint32_t rnd_ = 22222;
    FILE* wf_ = nullptr;
    uint64_t wavData_ = 0;
    long wavRiffPos_ = 4, wavDataPos_ = 0, wavFactPos_ = 0;
    bool openWav(const std::string& path, unsigned rate, int channels, int bits, const std::string& title, const std::string& comment) {
        wf_ = std::fopen(path.c_str(), "wb");
        if (!wf_) return false;
        std::string h;
        auto u16 = [&](unsigned v) { h += char(v & 255); h += char((v >> 8) & 255); };
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) h += char((v >> (8 * i)) & 255); };
        const unsigned bps = unsigned(bits / 8), align = bps * unsigned(channels);
        h += "RIFF"; u32(0); h += "WAVE";
        h += "fmt "; u32(float_ ? 18 : 16); u16(float_ ? 3 : 1); u16(unsigned(channels)); u32(rate); u32(rate * align); u16(align); u16(unsigned(bits));
        if (float_) { u16(0); h += "fact"; u32(4); wavFactPos_ = long(h.size()); u32(0); }
        // Tags: title, date, comment (LIST INFO chunk).
        char d[32]; const std::time_t t = std::time(nullptr); std::strftime(d, sizeof d, "%Y-%m-%d", std::localtime(&t));
        std::string info = "INFO";
        auto tag = [&](const char* id, const std::string& v) {
            std::string z = v.substr(0, 1000); z += '\0'; if (z.size() & 1) z += '\0';
            info += id; for (int i = 0; i < 4; ++i) info += char((uint32_t(v.substr(0, 1000).size() + 1) >> (8 * i)) & 255); info += z;
        };
        tag("INAM", title); tag("ICRD", d); tag("ICMT", comment);
        h += "LIST"; u32(uint32_t(info.size())); h += info;
        h += "data"; wavDataPos_ = long(h.size()); u32(0);
        wavData_ = 0;
        return std::fwrite(h.data(), 1, h.size(), wf_) == h.size();
    }
    // Writer thread: packs and writes n samples. Returns an error message or nullptr.
    const char* writeWav(const int32_t* v, size_t n, std::vector<unsigned char>& b) {
        const unsigned bps = unsigned(bits_ / 8);
        const uint64_t add = uint64_t(n) * bps;
        if (wavData_ + add > 0xFFFFFFFFull - 65536 - 4096) return "the WAV file reached its 4 GB limit";
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t x = uint32_t(v[i]);
            for (unsigned j = 0; j < bps; ++j) b[k++] = (unsigned char)((x >> (8 * j)) & 255);   // little endian
        }
        if (std::fwrite(b.data(), 1, k, wf_) != k) return "write error (disk full?)";
        wavData_ += add;
        return nullptr;
    }
    void closeWav() {
        if (!wf_) return;
        auto put32 = [&](long pos, uint32_t v) {
            unsigned char q[4] = {(unsigned char)(v & 255), (unsigned char)((v >> 8) & 255), (unsigned char)((v >> 16) & 255), (unsigned char)(v >> 24)};
            std::fseek(wf_, pos, SEEK_SET); std::fwrite(q, 1, 4, wf_);
        };
        std::fflush(wf_);
        const long end = wavDataPos_ + 4 + long(wavData_);
        if (wavData_ & 1) { std::fseek(wf_, 0, SEEK_END); std::fputc(0, wf_); }   // chunks are word-aligned
        put32(wavDataPos_, uint32_t(wavData_));
        put32(wavRiffPos_, uint32_t(end + (wavData_ & 1) - 8));
        if (wavFactPos_) put32(wavFactPos_, uint32_t(wavData_ / (4u * unsigned(ch_))));
        std::fclose(wf_); wf_ = nullptr;
    }
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

// Free path in dir: "name.flac", otherwise "name (2).flac", "name (3).flac"… (ext ".flac" or ".wav")
inline std::string uniqueRecPath(const std::string& dir, const std::string& base, const std::string& ext = ".flac") {
    struct stat st;
    std::string p = dir + "/" + base + ext;
    for (int i = 2; ::stat(p.c_str(), &st) == 0 && i < 10000; ++i) p = dir + "/" + base + " (" + std::to_string(i) + ")" + ext;
    return p;
}

// Does a name received from the page really designate a recording in dir?
inline bool validRecFile(const std::string& dir, const std::string& name, std::string* full = nullptr) {
    if (name.empty() || name.size() > 255 || name[0] == '.' || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find('\0') != std::string::npos)
        return false;
    if (name.size() < 5) return false;
    std::string e = name.substr(name.size() - (name.size() >= 6 ? 5 : 4));
    for (auto& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (e != ".flac" && e.substr(e.size() - 4) != ".wav") return false;
    const std::string p = dir + "/" + name;
    struct stat st;
    if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (full) *full = p;
    return true;
}

// Duration of a WAV file from its header (fmt + data chunks). -1 if unknown.
inline double wavSeconds(FILE* f) {
    unsigned char h[12];
    if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) return -1;
    uint32_t rate = 0, align = 0;
    for (int guard = 0; guard < 64; ++guard) {
        unsigned char c[8];
        if (std::fread(c, 1, 8, f) != 8) return -1;
        const uint32_t len = uint32_t(c[4]) | (uint32_t(c[5]) << 8) | (uint32_t(c[6]) << 16) | (uint32_t(c[7]) << 24);
        if (!std::memcmp(c, "fmt ", 4)) {
            unsigned char fm[16];
            if (len < 16 || std::fread(fm, 1, 16, f) != 16) return -1;
            rate = uint32_t(fm[4]) | (uint32_t(fm[5]) << 8) | (uint32_t(fm[6]) << 16) | (uint32_t(fm[7]) << 24);
            align = uint32_t(fm[12]) | (uint32_t(fm[13]) << 8);
            if (std::fseek(f, long(len - 16 + (len & 1)), SEEK_CUR)) return -1;
        } else if (!std::memcmp(c, "data", 4)) {
            return (rate && align && len) ? double(len / align) / rate : -1;
        } else if (std::fseek(f, long(len + (len & 1)), SEEK_CUR)) return -1;
    }
    return -1;
}

struct RecEntry { std::string name; uint64_t bytes = 0; double seconds = -1; std::time_t mtime = 0; };

inline std::vector<RecEntry> listRecordings(const std::string& dir) {
    std::vector<RecEntry> v;
    DIR* d = opendir(dir.c_str());
    if (!d) return v;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (!validRecFile(dir, n)) continue;
        const std::string p = dir + "/" + n;
        struct stat st;
        if (::stat(p.c_str(), &st) != 0) continue;
        RecEntry r;
        r.name = n; r.bytes = uint64_t(st.st_size); r.mtime = st.st_mtime;
        FLAC__StreamMetadata si;
        if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".wav") == 0 || n.compare(n.size() - 4, 4, ".WAV") == 0)) {
            if (FILE* f = std::fopen(p.c_str(), "rb")) { r.seconds = wavSeconds(f); std::fclose(f); }
        } else if (FLAC__metadata_get_streaminfo(p.c_str(), &si) && si.data.stream_info.sample_rate && si.data.stream_info.total_samples)
            r.seconds = double(si.data.stream_info.total_samples) / si.data.stream_info.sample_rate;
        v.push_back(r);
    }
    closedir(d);
    std::sort(v.begin(), v.end(), [](const RecEntry& a, const RecEntry& b) { return a.mtime > b.mtime; });
    return v;
}

inline uint64_t freeBytes(const std::string& dir) {
    struct statvfs s;
    if (statvfs(dir.c_str(), &s) != 0) return 0;
    return uint64_t(s.f_bavail) * s.f_frsize;
}

} // namespace vc
