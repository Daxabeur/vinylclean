// vinylclean desktop — small platform helpers (Windows / Linux): UTF-8 paths,
// user folders, thread priority, opening the control window. Original work.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <shlobj.h>
#else
#  include <sys/resource.h>
#  include <sys/syscall.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <cstring>
extern char** environ;
#endif

namespace vcd {

inline std::filesystem::path u8(const std::string& s) { return std::filesystem::u8path(s); }
inline std::string fromPath(const std::filesystem::path& p) { return p.u8string(); }

#ifdef _WIN32
inline std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(w.size() - 1);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 1), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize(s.size() - 1);
    return s;
}
#endif

// fopen with a UTF-8 path (any character on Windows too).
inline FILE* openUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
inline bool fileExists(const std::string& p) { std::error_code ec; return std::filesystem::exists(u8(p), ec); }
inline std::time_t fileTime(const std::string& p) {
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(u8(p), ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sys);
}

inline void lowerThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 10);
#endif
}

inline std::string envStr(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }

#ifdef _WIN32
inline std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string r;
    if (SHGetKnownFolderPath(id, 0, nullptr, &p) == S_OK && p) r = narrow(p);
    if (p) CoTaskMemFree(p);
    return r;
}
#endif

// Folder for the settings (created if needed).
inline std::string configDir() {
#ifdef _WIN32
    std::string d = knownFolder(FOLDERID_RoamingAppData);
    if (d.empty()) d = ".";
    d += "\\vinylclean";
#else
    std::string d = envStr("XDG_CONFIG_HOME");
    if (d.empty()) d = (envStr("HOME").empty() ? std::string("/tmp") : envStr("HOME")) + "/.config";
    d += "/vinylclean";
#endif
    std::error_code ec; std::filesystem::create_directories(u8(d), ec);
    return d;
}

// Folder for the recordings: <Music>/vinylclean.
inline std::string musicDir() {
#ifdef _WIN32
    std::string d = knownFolder(FOLDERID_Music);
    if (d.empty()) d = knownFolder(FOLDERID_Profile);
    d += "\\vinylclean";
#else
    std::string d = envStr("XDG_MUSIC_DIR");
    if (d.empty()) {
        // ~/.config/user-dirs.dirs: XDG_MUSIC_DIR="$HOME/Music"
        const std::string home = envStr("HOME");
        d = home + "/Music";
        if (FILE* f = std::fopen((home + "/.config/user-dirs.dirs").c_str(), "r")) {
            char line[1024];
            while (std::fgets(line, sizeof line, f)) {
                std::string l = line;
                if (l.rfind("XDG_MUSIC_DIR=", 0) != 0) continue;
                const size_t a = l.find('"'), b = l.rfind('"');
                if (a == std::string::npos || b <= a) continue;
                std::string v = l.substr(a + 1, b - a - 1);
                if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
                if (!v.empty() && v != home && v != home + "/") d = v;
            }
            std::fclose(f);
        }
    }
    d += "/vinylclean";
#endif
    std::error_code ec; std::filesystem::create_directories(u8(d), ec);
    return d;
}

// Opens the control page in its own window: a Chromium-based browser in "app"
// mode (Edge on Windows, Chromium / Chrome on Linux), otherwise the default
// browser. Returns false if nothing could be started.
inline bool openWindow(const std::string& url, const std::string& profileDir, bool browserOnly) {
#ifdef _WIN32
    if (!browserOnly) {
        std::vector<std::string> cands;
        for (const char* base : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"}) {
            const std::string b = envStr(base);
            if (b.empty()) continue;
            cands.push_back(b + "\\Microsoft\\Edge\\Application\\msedge.exe");
            cands.push_back(b + "\\Google\\Chrome\\Application\\chrome.exe");
            cands.push_back(b + "\\Chromium\\Application\\chrome.exe");
        }
        for (const auto& exe : cands) {
            if (!fileExists(exe)) continue;
            const std::string args = "--app=" + url + " --user-data-dir=\"" + profileDir + "\" --window-size=1000,900 --no-first-run --no-default-browser-check";
            const auto r = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", widen(exe).c_str(), widen(args).c_str(), nullptr, SW_SHOWNORMAL));
            if (r > 32) return true;
        }
    }
    return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    auto inPath = [](const std::string& name) -> std::string {
        std::string p = envStr("PATH");
        size_t i = 0;
        while (i <= p.size()) {
            size_t e = p.find(':', i); if (e == std::string::npos) e = p.size();
            const std::string f = p.substr(i, e - i) + "/" + name;
            if (!p.substr(i, e - i).empty() && access(f.c_str(), X_OK) == 0) return f;
            i = e + 1;
        }
        return "";
    };
    auto spawn = [](const std::vector<std::string>& argv) -> bool {
        // Everything is prepared BEFORE fork(): the child of a multithreaded
        // program may only call async-signal-safe functions (setsid, fork, execve, _exit).
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        // The AppImage runtime sets these for its own files: the browser must not inherit them.
        std::vector<char*> env;
        for (char** e = environ; e && *e; ++e)
            if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) && std::strncmp(*e, "LD_PRELOAD=", 11)) env.push_back(*e);
        env.push_back(nullptr);
        const pid_t pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            setsid();
            if (fork() != 0) _exit(0);   // double fork: no zombie, survives on its own
            execve(a[0], a.data(), env.data());
            _exit(127);
        }
        int st = 0; waitpid(pid, &st, 0);
        return true;
    };
    if (!browserOnly) {
        for (const char* b : {"chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "microsoft-edge", "brave-browser", "vivaldi"}) {
            const std::string exe = inPath(b);
            if (exe.empty()) continue;
            if (spawn({exe, "--app=" + url, "--user-data-dir=" + profileDir, "--window-size=1000,900", "--no-first-run", "--no-default-browser-check"}))
                return true;
        }
    }
    const std::string xo = inPath("xdg-open");
    return !xo.empty() && spawn({xo, url});
#endif
}

} // namespace vcd

