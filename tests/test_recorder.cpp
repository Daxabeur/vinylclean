// vinylclean — tests of the FLAC recorder (raw sound).
#include "recorder.h"
#include <FLAC/stream_decoder.h>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

using namespace vc;

static int gPass = 0, gFail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    (ok ? gPass : gFail)++;
}

// Reads a whole FLAC file (interleaved) for comparison.
struct Dec { std::vector<int32_t> s; unsigned ch = 0, bits = 0, rate = 0; uint64_t total = 0; };
static FLAC__StreamDecoderWriteStatus wcb(const FLAC__StreamDecoder*, const FLAC__Frame* f, const FLAC__int32* const b[], void* u) {
    auto* d = static_cast<Dec*>(u);
    for (unsigned i = 0; i < f->header.blocksize; ++i)
        for (unsigned c = 0; c < f->header.channels; ++c) d->s.push_back(b[c][i]);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void mcb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* u) {
    auto* d = static_cast<Dec*>(u);
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
        d->ch = m->data.stream_info.channels; d->bits = m->data.stream_info.bits_per_sample;
        d->rate = m->data.stream_info.sample_rate; d->total = m->data.stream_info.total_samples;
    }
}
static void ecb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}
static bool readFlac(const std::string& p, Dec& d) {
    FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
    if (FLAC__stream_decoder_init_file(dec, p.c_str(), wcb, mcb, ecb, &d) != FLAC__STREAM_DECODER_INIT_STATUS_OK) { FLAC__stream_decoder_delete(dec); return false; }
    const bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec);
    FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    return ok;
}

// Simulates the audio thread: blocks of `period` frames, ≈ 20 times faster than real time.
static std::vector<int32_t> feed(Recorder& rec, int nch, int bits, unsigned rate, double sec, unsigned seed, int period = 256, bool fast = false) {
    std::mt19937 g(seed);
    const int full = 1 << (bits - 1);
    std::uniform_int_distribution<int> u(-full, full - 1);
    const size_t F = size_t(sec * rate);
    std::vector<int32_t> ref;
    std::vector<float> pl(size_t(period) * nch);
    std::vector<const float*> ptr(nch);
    for (size_t t = 0; t < F; t += period) {
        const int n = int(std::min<size_t>(period, F - t));
        for (int c = 0; c < nch; ++c) ptr[c] = &pl[size_t(c) * period];
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) {
                // random value, sometimes a peak at full scale (clipping)
                int v = (i % 997 == 0) ? (c ? -full : full - 1) : u(g) / 3;
                pl[size_t(c) * period + i] = float(v) / float(full);
                ref.push_back(v);
            }
        rec.push(ptr.data(), nch, size_t(n));
        if (!fast) std::this_thread::sleep_for(std::chrono::microseconds(int(1e6 * n / rate / 20)));
    }
    return ref;
}

int main() {
    std::printf("=== vinylclean: recorder tests ===\n");
    char tmpl[] = "/tmp/vcrecXXXXXX";
    const std::string dir = mkdtemp(tmpl);

    std::printf("\n1. Raw sound recorded bit-exact\n");
    for (int bits : {24, 16}) {
        Recorder rec;
        std::string err;
        const std::string p = dir + "/test" + std::to_string(bits) + ".flac";
        const bool st = rec.start(p, 96000, 2, bits, "Test é à ü", err);
        auto ref = feed(rec, 2, bits, 96000, 6.0, 7 + bits);
        rec.stop();
        Dec d;
        const bool rd = st && readFlac(p, d);
        char msg[160];
        std::snprintf(msg, sizeof msg, "%d bits, 96 kHz, 6 s: readable file, %u bits, %u Hz, %llu frames", bits, d.bits, d.rate, (unsigned long long)d.total);
        check(rd && d.bits == unsigned(bits) && d.rate == 96000 && d.ch == 2 && d.total == ref.size() / 2, msg);
        check(rd && d.s == ref, "bit-identical content (peaks at 0 dBFS included)");
        check(rec.droppedBlocks() == 0, "no block lost");
    }

    std::printf("\n1b. Several recordings in a row (same object)\n");
    {
        Recorder rec;
        std::string err;
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const std::string p = dir + "/series" + std::to_string(k) + ".flac";
            ok = ok && rec.start(p, 48000, 2, 24, "series", err);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ok = ok && !rec.failed();          // must not think it failed at start
            auto ref = feed(rec, 2, 24, 48000, 1.0, 40 + k);
            ok = ok && !rec.failed();
            rec.stop();
            Dec d;
            ok = ok && readFlac(p, d) && d.s == ref;
        }
        check(ok, "3 recordings in a row: each one complete and bit-identical");
    }

    std::printf("\n2. Disk too slow: the live sound never waits\n");
    {
        Recorder rec(4096);   // tiny reserve
        std::string err;
        const std::string p = dir + "/slow.flac";
        rec.start(p, 48000, 2, 24, "slow", err);
        const auto t0 = std::chrono::steady_clock::now();
        feed(rec, 2, 24, 48000, 20.0, 3, 256, true);   // as fast as possible
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        rec.stop();
        Dec d;
        char msg[160];
        std::snprintf(msg, sizeof msg, "push() does not block (20 s pushed in %.2f s); lost blocks counted: %llu", dt, (unsigned long long)rec.droppedBlocks());
        check(dt < 2.0 && rec.droppedBlocks() > 0, msg);
        check(readFlac(p, d) && d.total > 0, "the file stays valid");
    }

    std::printf("\n3. File names\n");
    check(safeRecName("Artist - Album - Side A") == "Artist - Album - Side A", "normal name kept (spaces, dashes)");
    check(safeRecName("AC/DC: Back in Black?") == "AC-DC- Back in Black-", "forbidden characters replaced");
    check(safeRecName("  ../../etc/passwd  ") == "-..-etc-passwd", "cannot leave the folder");
    check(safeRecName("Side B.flac") == "Side B", "extra \".flac\" typed by the user removed");
    check(safeRecName("").rfind("Recording ", 0) == 0, "empty name → \"Recording <date> <time>\"");
    check(safeRecName(std::string(300, 'e') + "é").size() <= 150, "too long name shortened");
    {
        std::string longName(149, 'a'); longName += "é";   // é cut in the middle at 150 bytes
        const std::string s = safeRecName(longName);
        check(s == std::string(149, 'a'), "no UTF-8 character cut in two");
    }
    {
        FILE* f = std::fopen((dir + "/Side A.flac").c_str(), "w"); std::fclose(f);
        f = std::fopen((dir + "/Side A (2).flac").c_str(), "w"); std::fclose(f);
        check(uniqueRecPath(dir, "Side A") == dir + "/Side A (3).flac", "name already taken → \"(3)\", never overwritten");
    }
    check(!validRecFile(dir, "../test24.flac") && !validRecFile(dir, ".cache.flac") && !validRecFile(dir, "a/b.flac") &&
          !validRecFile(dir, "test24.wav") && !validRecFile(dir, "absent.flac") && validRecFile(dir, "test24.flac"),
          "download / delete: only the .flac files of the folder");

    std::printf("\n4. Folder listing\n");
    {
        auto v = listRecordings(dir);
        bool found = false;
        for (auto& e : v) if (e.name == "test24.flac") found = std::fabs(e.seconds - 6.0) < 0.01 && e.bytes > 0;
        check(found, "duration and size read from the file (6.0 s)");
        check(freeBytes(dir) > 0, "free space measured");
    }

    std::string cmd = "rm -rf '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) {}
    std::printf("\nResult: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
