// vinylclean desktop — small portable HTTP + WebSocket server (RFC 6455)
// for Windows and Linux. One thread per connection (a desktop app has only a
// few clients). It never touches the sound: it exchanges JSON text messages
// with the main program and serves files. Original work.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcd {

// A file served over HTTP (empty path = 404).
struct ServedFile {
    std::string path;          // full path on disk
    std::string type;          // Content-Type
    std::string downloadName;  // non-empty: sent as an attachment with this name
};

class HttpServer {
public:
    struct Callbacks {
        std::function<std::string()> page;                                   // GET /
        std::function<std::vector<std::string>()> onConnect;                 // messages for a new WebSocket client
        std::function<std::string(const std::string&)> onMessage;            // text message; optional reply
        std::function<ServedFile(const std::string& path)> file;             // any other GET (decoded path)
        // POST /upload?name=…&fmt=…: `read(buf, n)` returns the bytes read (0 = end/error).
        std::function<std::string(const std::string& name, const std::string& fmt, uint64_t length,
                                  const std::function<size_t(char*, size_t)>& read)> upload;
    };
    ~HttpServer() { stop(); }
    // Listens on 127.0.0.1 (or every interface if lan) on the first free port in [port, port + 20).
    bool start(int port, bool lan, Callbacks cb, std::string& err);
    void stop();
    int port() const { return port_; }
    void broadcast(const std::string& text);
    int clients() const { return nClients_.load(); }

private:
    struct Client;
    void acceptLoop();
    void serve(intptr_t sock);
    Callbacks cb_;
    intptr_t listen_ = -1;
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::thread acceptTh_;
    std::mutex mu_;
    std::vector<std::shared_ptr<Client>> ws_;
    std::atomic<int> nClients_{0}, active_{0};
};

std::string urlDecode(const std::string& s);
// Body of http://127.0.0.1:<port><path>, or "" (nothing there, timeout).
std::string httpGetLocal(int port, const std::string& path, int timeoutMs);

} // namespace vcd
