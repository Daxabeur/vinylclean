// vinylclean — small HTTP + WebSocket server (RFC 6455), no dependency.
// One thread, low priority, non-blocking sockets and poll(). It never touches
// the sound: it exchanges text messages (JSON) with vc-live.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vc {

class WebServer {
public:
    // onMessage: text message received from a browser (optionally returns
    // a reply for that client only). onConnect: full state to send to a new
    // client. pageFile: HTML file read on EVERY request if it exists
    // (editable on the fly); otherwise the embedded page.
    struct Callbacks {
        std::function<std::string(const std::string&)> onMessage;
        std::function<std::string()> onConnect;
        // GET /download/<name>: full path of the file to send, or "" (refused).
        std::function<std::string(const std::string&)> downloadPath;
    };

    bool start(int port, const std::string& pageFile, const char* builtinPage, Callbacks cb, std::string& err);
    // Other page (e.g. "/simple.html"), same rule: file on disk if present, else embedded. Before start().
    void addPage(const std::string& path, const std::string& file, const char* builtin) { extra_.push_back({path, file, builtin}); }
    void stop();
    void broadcast(const std::string& text);  // queued, sent by the server thread
    int clients() const { return nClients_.load(); }
    ~WebServer() { stop(); }

private:
    struct Client {
        int fd = -1;
        bool ws = false;
        std::string in, out;
    };
    void loop();
    void handleHttp(Client& c);
    void handleWs(Client& c);
    void sendFrame(Client& c, const std::string& payload, uint8_t opcode = 0x1);

    int listenFd_ = -1;
    int wakePipe_[2] = {-1, -1};
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<int> nClients_{0};
    std::vector<Client> cl_;
    std::mutex qMutex_;            // server thread ↔ main thread (never the audio thread)
    std::vector<std::string> queue_;
    std::string pageFile_;
    const char* builtin_ = nullptr;
    struct Page { std::string path, file; const char* builtin; };
    std::vector<Page> extra_;
    Callbacks cb_;
};

// Minimal flat JSON: {"key": number | "text" | true | false, ...}
struct JsonFlat {
    std::vector<std::pair<std::string, std::string>> kv;  // raw values (text without quotes)
    bool parse(const std::string& s);
    bool has(const char* k) const;
    std::string str(const char* k, const std::string& def = "") const;
    double num(const char* k, double def = 0) const;
};
std::string jsonEscape(const std::string& s);

} // namespace vc
