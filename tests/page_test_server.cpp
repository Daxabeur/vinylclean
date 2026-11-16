// vinylclean — test server for the web pages (no sound card needed).
// Serves web/index.html and web/simple.html with the real web server of vc-live
// and sends the same messages as vc-live, with a synthetic signal that goes
// through the real level code (levels.h):
//   {"cmd":"test","sig":"sine18"}   sine whose peak is at −18 dBFS (must read 0 VU)
//   {"cmd":"test","sig":"burst"}    0.5 s of sine at −18 dBFS every 2 s (needle ballistics)
//   {"cmd":"test","sig":"silence"}  silence
//   {"cmd":"test","sig":"music"}    moving level, a few clicks, peaks above −1 dBFS
// Settings sent with {"cmd":"set",...} are kept and sent back to all pages.
//   page-test-server <port> <web folder>
// Original work.
#include "web_server.h"
#include "levels.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vc;

static std::atomic<bool> gRun{true};

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8099;
    const std::string dir = argc > 2 ? argv[2] : "web";
    std::signal(SIGINT, [](int) { gRun = false; });
    std::signal(SIGTERM, [](int) { gRun = false; });

    std::mutex mu;
    std::map<std::string, std::string> st = {
        {"sens", "3"}, {"sensDeclick", "3"}, {"sensDecrackle", "3"}, {"declick", "3"}, {"decrackle", "3"}, {"protect", "1"},
        {"maxclick", "2"}, {"multiband", "1"}, {"stereo", "1"}, {"riaa", "0"}, {"phonoGain", "12"}, {"rumble", "1"},
        {"recClean", "0"}, {"vuRef", "-18"}, {"mode", "2"}, {"ad", "1"}, {"armLen", "222"}, {"align", "2"}, {"null1", "60.325"},
        {"null2", "117.42"}, {"stylus", "2"}, {"stylusUm", "7.6"}, {"disc", "0"}, {"autoPos", "1"}, {"posMm", "100"},
        {"sideMin", "20"}, {"adInt", "1"}, {"in", "\"hw:0,0\""}, {"out", "\"hw:0,0\""}, {"rate", "48000"}, {"periods", "3"}};
    std::string sig = "music";
    std::atomic<bool> changed{false};
    auto stateJson = [&] {
        std::lock_guard<std::mutex> lk(mu);
        std::string s = "{\"type\":\"settings\"";
        for (auto& kv : st) s += ",\"" + kv.first + "\":" + kv.second;
        return s + "}";
    };
    WebServer web;
    WebServer::Callbacks cb;
    cb.onConnect = stateJson;
    cb.onMessage = [&](const std::string& m) -> std::string {
        JsonFlat j;
        if (!j.parse(m)) return "";
        std::lock_guard<std::mutex> lk(mu);
        if (j.str("cmd") == "set") {
            for (auto& kv : j.kv) if (kv.first != "cmd") {
                const bool text = kv.first == "in" || kv.first == "out";
                st[kv.first] = text ? "\"" + jsonEscape(kv.second) + "\"" : kv.second;
            }
            if (j.has("sens")) st["sensDeclick"] = st["sensDecrackle"] = st["sens"];
            if (j.has("sensDeclick") || j.has("sensDecrackle")) st["sens"] = st["sensDeclick"] == st["sensDecrackle"] ? st["sensDeclick"] : "-1";
            changed = true;
        } else if (j.str("cmd") == "test") sig = j.str("sig");
        return "";
    };
    web.addPage("/simple.html", dir + "/simple.html", "");
    std::string err;
    if (!web.start(port, dir + "/index.html", "", cb, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("http://127.0.0.1:%d/simple.html\n", port);
    std::fflush(stdout);

    // Synthetic signal, 48 kHz, 512-sample periods, through LevelRing / LevelStats like vc-live.
    const unsigned rate = 48000, P = 512;
    LevelStats levels; levels.reset(rate);
    std::vector<float> l(P), r(P);
    double ph = 0, t = 0;
    unsigned long long clicks = 0;
    auto next = std::chrono::steady_clock::now();
    auto lastB = next;
    float pk[2] = {0, 0};
    uint32_t rnd = 1;
    while (gRun) {
        std::string s; { std::lock_guard<std::mutex> lk(mu); s = sig; }
        for (unsigned i = 0; i < P; ++i, t += 1.0 / rate) {
            double a = 0, b = 0;
            const double sine = std::sin(ph); ph += 2 * M_PI * 1000.0 / rate;
            if (s == "sine18") a = b = std::pow(10.0, -18 / 20.0) * sine;
            else if (s == "burst") { const double k = std::fmod(t, 2.0) < 0.5 ? 1 : 0; a = b = k * std::pow(10.0, -18 / 20.0) * sine; }
            else if (s == "music") {
                const double env = std::pow(10.0, (-23 + 5 * std::sin(t * 1.3) + 3 * std::sin(t * 5.1)) / 20.0);
                a = env * sine; b = env * 0.7 * std::sin(ph * 1.01);
                if (std::fmod(t, 6.0) < 0.01) a = 0.95;   // a peak above −1 dBFS every 6 s
            }
            l[i] = float(a); r[i] = float(b);
            pk[0] = std::fmax(pk[0], std::fabs(l[i])); pk[1] = std::fmax(pk[1], std::fabs(r[i]));
        }
        if (s == "music") { rnd = rnd * 1664525u + 1013904223u; if ((rnd >> 24) < 6) ++clicks; }
        levels.add(levelSums(l.data(), r.data(), P));
        next += std::chrono::microseconds(1000000ULL * P / rate);
        std::this_thread::sleep_until(next);
        const auto now = std::chrono::steady_clock::now();
        if (changed.exchange(false)) web.broadcast(stateJson());
        static auto lastList = now - std::chrono::seconds(10);
        if (now - lastList >= std::chrono::seconds(3)) {   // recordings and sound cards, like vc-live
            lastList = now;
            std::string r = "{\"type\":\"recs\",\"free\":21500000000,\"items\":[";
            const char* names[] = {"Pink Floyd - Animals - Face A", "Pink Floyd - Animals - Face B", "Miles Davis - Kind of Blue - Face A",
                                   "Miles Davis - Kind of Blue - Face B", "Daft Punk - Discovery - Face A", "Daft Punk - Discovery - Face B",
                                   "Daft Punk - Discovery - Face C", "Bach - Suites - Face A", "Test 45 tours"};
            for (int i = 0; i < 9; ++i)
                r += std::string(i ? "," : "") + "{\"name\":\"" + names[i] + ".flac\",\"sec\":" + std::to_string(1100 + 37 * i) +
                     ",\"bytes\":" + std::to_string(180000000 + 7000000 * i) + ",\"date\":\"2026-11-1" + std::to_string(i % 7) + " 21:0" + std::to_string(i) + "\",\"rec\":false}";
            web.broadcast(r + "]}");
            web.broadcast("{\"type\":\"cards\",\"cards\":[{\"name\":\"Scarlett 2i2 USB\",\"dev\":\"hw:0,0\",\"in\":[44100,48000,88200,96000],"
                          "\"out\":[44100,48000,88200,96000],\"format\":\"24 bits\"},{\"name\":\"bcm2835 Headphones\",\"dev\":\"hw:1,0\",\"in\":[],\"out\":[44100,48000],\"format\":\"16 bits\"}]}");
        }
        if (now - lastB >= std::chrono::milliseconds(100)) {
            lastB = now;
            double vl, vr; levels.takeVu(vl, vr);
            auto db = [](float v) { return v > 1e-9f ? 20.f * std::log10(v) : -120.f; };
            char b[700];
            std::snprintf(b, sizeof b,
                "{\"type\":\"status\",\"running\":true,\"inL\":%.1f,\"inR\":%.1f,\"outL\":%.1f,\"outR\":%.1f,\"sat\":false,"
                "\"clicks\":%llu,\"crackles\":%llu,\"load\":23.0,\"loadMax\":31.0,\"xruns\":0,\"lufsS\":-20,\"lufsI\":-20,\"lra\":6,"
                "\"plr\":12,\"loudSec\":30,\"rate\":48000,\"outState\":\"RUNNING\",\"format\":\"24 bits\",\"latency\":46,\"rt\":true,"
                "\"error\":\"\",\"clients\":%d,\"satPhono\":false,\"recOn\":false,\"recSec\":0,\"recLost\":0,\"recAvail\":true,\"recName\":\"\","
                "\"recMsg\":\"\",\"adR\":120.0,\"adErr\":0.80,\"adV\":9.0,\"adSide\":95,\"adReady\":true,\"bal\":0.3,\"corr\":0.62,\"noise\":-61,"
                "\"recClean\":false,\"vuL\":%.2f,\"vuR\":%.2f}",
                db(pk[0]), db(pk[1]), db(pk[0]), db(pk[1]), clicks, clicks * 7, web.clients(), vl, vr);
            pk[0] = pk[1] = 0;
            web.broadcast(b);
        }
    }
    web.stop();
    return 0;
}
