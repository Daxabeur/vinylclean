// vinylclean desktop — portable HTTP + WebSocket server. Original work.
#include "http_server.h"
#include "sha1.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
using sock_t = SOCKET;
static void closeSock(intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
static int sockErrno() { return WSAGetLastError(); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#  include <cerrno>
using sock_t = int;
static void closeSock(intptr_t s) { ::close(static_cast<int>(s)); }
static int sockErrno() { return errno; }
#endif

namespace vcd {

namespace {

// UTF-8 path → FILE* (Windows: wide API, so any character works).
FILE* openFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(n > 0 ? n : 1), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    };
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool sendAll(intptr_t s, const char* p, size_t n) {
    while (n > 0) {
        const int k = ::send(static_cast<sock_t>(s), p, int(std::min<size_t>(n, 1 << 20)), 0);
        if (k <= 0) return false;
        p += k; n -= size_t(k);
    }
    return true;
}
bool sendStr(intptr_t s, const std::string& t) { return sendAll(s, t.data(), t.size()); }
int recvSome(intptr_t s, char* p, size_t n) { return ::recv(static_cast<sock_t>(s), p, int(n), 0); }
bool recvAll(intptr_t s, char* p, size_t n) {
    while (n > 0) {
        const int k = recvSome(s, p, n);
        if (k <= 0) return false;
        p += k; n -= size_t(k);
    }
    return true;
}

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

std::string response(const std::string& status, const std::string& type, const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 " << status << "\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" << body;
    return o.str();
}

std::string urlEncode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}

std::string queryParam(const std::string& q, const std::string& key) {
    size_t i = 0;
    while (i < q.size()) {
        size_t e = q.find('&', i); if (e == std::string::npos) e = q.size();
        const std::string kv = q.substr(i, e - i);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? "" : urlDecode(kv.substr(eq + 1));
        i = e + 1;
    }
    return "";
}

}  // namespace

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) { o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
        else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

std::string httpGetLocal(int port, const std::string& path, int timeoutMs) {
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return "";
    DWORD tv = DWORD(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#else
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return "";
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string r;
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
        sendStr(intptr_t(s), "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")) {
        char b[1024];
        for (int k; (k = recvSome(intptr_t(s), b, sizeof b)) > 0 && r.size() < 65536;) r.append(b, size_t(k));
    }
    closeSock(intptr_t(s));
    const size_t he = r.find("\r\n\r\n");
    return (r.rfind("HTTP/1.1 200", 0) == 0 && he != std::string::npos) ? r.substr(he + 4) : "";
}

struct HttpServer::Client {
    intptr_t sock;
    std::mutex sendMu;
    std::atomic<bool> alive{true};
    bool sendText(const std::string& t) {
        std::lock_guard<std::mutex> lk(sendMu);
        if (!alive) return false;
        std::string h;
        h += char(0x81);
        if (t.size() < 126) h += char(t.size());
        else if (t.size() < 65536) { h += char(126); h += char((t.size() >> 8) & 255); h += char(t.size() & 255); }
        else { h += char(127); for (int i = 7; i >= 0; --i) h += char((uint64_t(t.size()) >> (8 * i)) & 255); }
        alive = sendStr(sock, h) && sendStr(sock, t);
        return alive;
    }
    void sendControl(unsigned char op, const std::string& payload) {
        std::lock_guard<std::mutex> lk(sendMu);
        std::string h; h += char(0x80 | op); h += char(payload.size());
        alive = alive && sendStr(sock, h + payload);
    }
};

bool HttpServer::start(int port, bool lan, Callbacks cb, std::string& err) {
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    cb_ = std::move(cb);
    for (int p = port; p < port + 20; ++p) {
        const sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
        if (s == INVALID_SOCKET) { err = "socket"; return false; }
        BOOL excl = TRUE; setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#else
        if (s < 0) { err = "socket"; return false; }
        int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(p));
        a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(s, 16) == 0) {
            listen_ = intptr_t(s);
            port_ = p;
            run_ = true;
            acceptTh_ = std::thread([this] { acceptLoop(); });
            return true;
        }
        closeSock(intptr_t(s));
    }
    err = "no free port from " + std::to_string(port) + " (error " + std::to_string(sockErrno()) + ")";
    return false;
}

void HttpServer::stop() {
    if (!run_.exchange(false)) return;
    // Linux: close() alone does not wake a thread blocked in accept(); shutdown() does.
#ifdef _WIN32
    shutdown(static_cast<SOCKET>(listen_), SD_BOTH);
#else
    shutdown(int(listen_), SHUT_RDWR);
#endif
    closeSock(listen_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : ws_) {
#ifdef _WIN32
            shutdown(static_cast<SOCKET>(c->sock), SD_BOTH);
#else
            shutdown(int(c->sock), SHUT_RDWR);
#endif
        }
    }
    if (acceptTh_.joinable()) acceptTh_.join();
    for (int i = 0; i < 100 && active_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void HttpServer::acceptLoop() {
    while (run_) {
        const sock_t c = ::accept(static_cast<sock_t>(listen_), nullptr, nullptr);
#ifdef _WIN32
        if (c == INVALID_SOCKET) { if (!run_) break; std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
#else
        if (c < 0) { if (!run_) break; std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
#endif
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        ++active_;
        std::thread([this, c] { serve(intptr_t(c)); --active_; }).detach();
    }
}

void HttpServer::broadcast(const std::string& text) {
    std::vector<std::shared_ptr<Client>> list;
    { std::lock_guard<std::mutex> lk(mu_); list = ws_; }
    for (auto& c : list) c->sendText(text);
}

void HttpServer::serve(intptr_t s) {
    // Read the request head (≤ 16 KB).
    std::string head;
    char buf[4096];
    std::string extra;   // bytes after the head (start of a body or of WebSocket frames)
    while (head.find("\r\n\r\n") == std::string::npos) {
        const int k = recvSome(s, buf, sizeof buf);
        if (k <= 0 || head.size() > 16384) { closeSock(s); return; }
        head.append(buf, size_t(k));
    }
    const size_t he = head.find("\r\n\r\n");
    extra = head.substr(he + 4);
    head.resize(he);
    std::istringstream hs(head);
    std::string method, target, version, line;
    hs >> method >> target >> version;
    std::getline(hs, line);
    std::map<std::string, std::string> hdr;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string v = line.substr(c + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        hdr[lower(line.substr(0, c))] = v;
    }
    const size_t q = target.find('?');
    const std::string path = urlDecode(target.substr(0, q));
    const std::string query = q == std::string::npos ? "" : target.substr(q + 1);

    // ---- WebSocket ----
    if (method == "GET" && path == "/ws" && lower(hdr["upgrade"]) == "websocket" && !hdr["sec-websocket-key"].empty()) {
        const std::string acc = hdr["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t dg[20];
        vc::sha1(reinterpret_cast<const uint8_t*>(acc.data()), acc.size(), dg);
        if (!sendStr(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                            vc::base64(dg, 20) + "\r\n\r\n")) { closeSock(s); return; }
        auto cl = std::make_shared<Client>();
        cl->sock = s;
        if (cb_.onConnect) for (auto& m : cb_.onConnect()) cl->sendText(m);
        { std::lock_guard<std::mutex> lk(mu_); ws_.push_back(cl); nClients_ = int(ws_.size()); }
        // Frames: everything already received comes first.
        size_t xi = 0;
        auto rd = [&](char* p, size_t n) -> bool {
            while (n > 0 && xi < extra.size()) { *p++ = extra[xi++]; --n; }
            return n == 0 || recvAll(s, p, n);
        };
        std::string msg;
        while (run_ && cl->alive) {
            unsigned char h2[2];
            if (!rd(reinterpret_cast<char*>(h2), 2)) break;
            const bool fin = h2[0] & 0x80;
            const unsigned op = h2[0] & 0x0F;
            uint64_t len = h2[1] & 0x7F;
            if (len == 126) { unsigned char b[2]; if (!rd(reinterpret_cast<char*>(b), 2)) break; len = (uint64_t(b[0]) << 8) | b[1]; }
            else if (len == 127) { unsigned char b[8]; if (!rd(reinterpret_cast<char*>(b), 8)) break; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | b[i]; }
            if (len > (16u << 20)) break;
            unsigned char mask[4] = {0, 0, 0, 0};
            if (h2[1] & 0x80) { if (!rd(reinterpret_cast<char*>(mask), 4)) break; }
            std::string payload(size_t(len), '\0');
            if (len && !rd(&payload[0], size_t(len))) break;
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i & 3]);
            if (op == 0x8) { cl->sendControl(0x8, ""); break; }
            if (op == 0x9) { cl->sendControl(0xA, payload.substr(0, 125)); continue; }
            if (op == 0xA) continue;
            msg += payload;
            if (!fin) continue;
            if (cb_.onMessage) { const std::string r = cb_.onMessage(msg); if (!r.empty()) cl->sendText(r); }
            msg.clear();
        }
        { std::lock_guard<std::mutex> lk(mu_);
          ws_.erase(std::remove(ws_.begin(), ws_.end(), cl), ws_.end()); nClients_ = int(ws_.size()); }
        { std::lock_guard<std::mutex> lk(cl->sendMu); cl->alive = false; }
        closeSock(s);
        return;
    }

    // ---- Page ----
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        sendStr(s, response("200 OK", "text/html; charset=utf-8", cb_.page ? cb_.page() : ""));
        closeSock(s);
        return;
    }
    if (method == "GET" && path == "/simple.html" && cb_.simplePage) {
        sendStr(s, response("200 OK", "text/html; charset=utf-8", cb_.simplePage()));
        closeSock(s);
        return;
    }
    if (method == "GET" && path == "/ping") { sendStr(s, response("200 OK", "text/plain", "vinylclean\n")); closeSock(s); return; }

    // ---- Upload (file cleaning) ----
    if (method == "POST" && path == "/upload" && cb_.upload) {
        const uint64_t len = std::strtoull(hdr["content-length"].c_str(), nullptr, 10);
        uint64_t left = len;
        size_t xi = 0;
        auto read = [&](char* p, size_t n) -> size_t {
            if (left == 0) return 0;
            n = size_t(std::min<uint64_t>(n, left));
            size_t got = 0;
            while (got < n && xi < extra.size()) p[got++] = extra[xi++];
            if (got == 0) { const int k = recvSome(s, p, n); if (k <= 0) return 0; got = size_t(k); }
            left -= got;
            return got;
        };
        const std::string r = cb_.upload(queryParam(query, "name"), queryParam(query, "fmt"), len, read);
        sendStr(s, r.empty() ? response("200 OK", "text/plain", "ok\n") : response("400 Bad Request", "text/plain; charset=utf-8", r + "\n"));
        closeSock(s);
        return;
    }

    // ---- Files (recordings, file cleaning), with Range support for the audio player ----
    if (method == "GET" && cb_.file) {
        const ServedFile f = cb_.file(path);
        FILE* fp = f.path.empty() ? nullptr : openFile(f.path, "rb");
        if (!fp) { sendStr(s, response("404 Not Found", "text/plain; charset=utf-8", "not found\n")); closeSock(s); return; }
#ifdef _WIN32
        _fseeki64(fp, 0, SEEK_END); const uint64_t size = uint64_t(_ftelli64(fp));
#else
        fseeko(fp, 0, SEEK_END); const uint64_t size = uint64_t(ftello(fp));
#endif
        uint64_t a = 0, b = size ? size - 1 : 0;
        bool partial = false;
        const std::string rg = hdr["range"];
        if (rg.rfind("bytes=", 0) == 0 && size > 0) {
            const std::string r = rg.substr(6);
            const size_t d = r.find('-');
            if (d != std::string::npos) {
                if (d > 0) a = std::strtoull(r.substr(0, d).c_str(), nullptr, 10);
                if (d + 1 < r.size()) b = std::strtoull(r.substr(d + 1).c_str(), nullptr, 10);
                if (d == 0) { a = size - std::min<uint64_t>(size, b); b = size - 1; }
                if (b >= size) b = size - 1;
                partial = a <= b;
                if (!partial) { a = 0; b = size - 1; }
            }
        }
        const uint64_t n = size ? b - a + 1 : 0;
        std::ostringstream o;
        o << "HTTP/1.1 " << (partial ? "206 Partial Content" : "200 OK") << "\r\nContent-Type: " << f.type
          << "\r\nContent-Length: " << n << "\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: close\r\n";
        if (partial) o << "Content-Range: bytes " << a << "-" << b << "/" << size << "\r\n";
        if (!f.downloadName.empty()) {
            std::string ascii;
            for (unsigned char c : f.downloadName) ascii += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? char(c) : '_';
            o << "Content-Disposition: attachment; filename=\"" << ascii << "\"; filename*=UTF-8''" << urlEncode(f.downloadName) << "\r\n";
        }
        o << "\r\n";
        bool ok = sendStr(s, o.str());
#ifdef _WIN32
        _fseeki64(fp, int64_t(a), SEEK_SET);
#else
        fseeko(fp, off_t(a), SEEK_SET);
#endif
        std::vector<char> fb(1 << 16);
        uint64_t left = n;
        while (ok && left > 0) {
            const size_t k = std::fread(fb.data(), 1, size_t(std::min<uint64_t>(left, fb.size())), fp);
            if (k == 0) break;
            ok = sendAll(s, fb.data(), k);
            left -= k;
        }
        std::fclose(fp);
        closeSock(s);
        return;
    }
    sendStr(s, response("404 Not Found", "text/plain; charset=utf-8", "not found\n"));
    closeSock(s);
}

} // namespace vcd
