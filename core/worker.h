// vinylclean — helper thread that processes the second channel in parallel
// ("one core per channel").
//
// The audio thread posts a small job (a phase of a hop for channel 1), does the
// same phase for channel 0 itself, then waits for the helper. Both channels of a
// phase only READ what the other produced in the previous phases, so the result
// is identical, bit for bit, to processing them one after the other.
//
// Real time: no allocation, no lock in the normal path. The helper spins for a
// short while waiting for the next job (the phases of a hop follow each other
// within microseconds), then sleeps on a condition variable between periods so
// that it never burns a core for nothing.
// Original work.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#elif defined(_WIN32)
// The two Win32 functions needed, declared here: <windows.h> would bring its
// macros (near, far, min, max…) into the DSP code.
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void* thread, int priority);
#endif

namespace vc {

class ChannelWorker {
public:
    using Fn = void (*)(void*);
    ~ChannelWorker() { stop(); }

    // rtPriority > 0: SCHED_FIFO priority (Linux) if allowed, else normal priority.
    bool start(int rtPriority = 0) {
        stop();
        quit_ = false; seq_ = 0; done_ = 0; sleeping_ = false;
        try { th_ = std::thread([this, rtPriority] { loop(rtPriority); }); } catch (...) { return false; }
        return true;
    }
    void stop() {
        if (!th_.joinable()) return;
        { std::lock_guard<std::mutex> lk(m_); quit_ = true; }
        cv_.notify_one();
        th_.join();
    }
    bool running() const { return th_.joinable(); }

    // Audio thread: start fn(arg) on the helper.
    void post(Fn fn, void* arg) {
        fn_ = fn; arg_ = arg;
        const uint32_t s = seq_.load(std::memory_order_relaxed) + 1;
        seq_.store(s);                       // seq_cst: pairs with the helper going to sleep (no lost wake-up)
        if (sleeping_.load()) { std::lock_guard<std::mutex> lk(m_); cv_.notify_one(); }
    }
    // Audio thread: wait for the end of the job posted last.
    void wait() {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        for (unsigned i = 0; done_.load(std::memory_order_acquire) != s; ++i)
            if (i > 64) std::this_thread::yield();
    }

private:
    void loop(int rtPriority) {
#if defined(__linux__)
        if (rtPriority > 0) { sched_param sp{}; sp.sched_priority = rtPriority; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }
#elif defined(_WIN32)
        if (rtPriority > 0) SetThreadPriority(GetCurrentThread(), 15);   // THREAD_PRIORITY_TIME_CRITICAL
#endif
        uint32_t seen = 0;
        for (;;) {
            // Short spin: the next phase of the hop comes right away.
            unsigned spins = 0;
            while (seq_.load(std::memory_order_acquire) == seen && spins < 20000) { ++spins; if (spins > 200) std::this_thread::yield(); }
            if (seq_.load(std::memory_order_acquire) == seen) {
                std::unique_lock<std::mutex> lk(m_);
                sleeping_.store(true);
                cv_.wait(lk, [&] { return quit_ || seq_.load() != seen; });
                sleeping_.store(false);
                if (quit_) return;
            }
            seen = seq_.load(std::memory_order_acquire);
            fn_(arg_);
            done_.store(seen, std::memory_order_release);
        }
    }

    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::atomic<uint32_t> seq_{0}, done_{0};
    std::atomic<bool> sleeping_{false};
    Fn fn_ = nullptr;
    void* arg_ = nullptr;
};

} // namespace vc
