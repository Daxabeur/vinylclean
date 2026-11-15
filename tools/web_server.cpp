// vinylclean — minimal HTTP + WebSocket server. Original work.
#include "web_server.h"
#include "sha1.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace vc {

static constexpr size_t kMaxClients = 12;
static constexpr size_t kMaxIn = 64 * 1024;
static constexpr size_t kMaxOut = 1024 * 1024;

bool WebServer::start(int port, const std::string& pageFile, const char* builtinPage, Callbacks cb, std::string& err) {
    pageFile_ = pageFile;
    builtin_ = builtinPage;
    cb_ = std::move(cb);
    listenFd_ = socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = listenFd_ >= 0;
    if (!v6) listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) { err = std::strerror(errno); return false; }
    int one = 1, zero = 0;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int rc;
    if (v6) {
        setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);  // IPv4 + IPv6
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(uint16_t(port));
        rc = bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(uint16_t(port));
        rc = bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (rc < 0 || listen(listenFd_, 8) < 0) {
        err = std::string("port ") + std::to_string(port) + " : " + std::strerror(errno);
        close(listenFd_); listenFd_ = -1;
        return false;
    }
    fcntl(listenFd_, F_SETFL, O_NONBLOCK);
    if (pipe(wakePipe_) < 0) { err = "pipe"; return false; }
    fcntl(wakePipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wakePipe_[1], F_SETFL, O_NONBLOCK);
    run_ = true;
    th_ = std::thread([this] {
        // Low-priority thread: it must never disturb the audio.
        setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 15);
        loop();
    });
    return true;
}

void WebServer::stop() {
    if (!run_.exchange(false)) return;
    if (wakePipe_[1] >= 0) { char c = 0; (void)!write(wakePipe_[1], &c, 1); }
    if (th_.joinable()) th_.join();
    for (auto& c : cl_) if (c.fd >= 0) close(c.fd);
    cl_.clear();
    if (listenFd_ >= 0) close(listenFd_);
    for (int& f : wakePipe_) if (f >= 0) { close(f); f = -1; }
    listenFd_ = -1;
}

void WebServer::broadcast(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        if (queue_.size() < 64) queue_.push_back(text);
    }
    char c = 1;
    (void)!write(wakePipe_[1], &c, 1);
}

void WebServer::sendFrame(Client& c, const std::string& p, uint8_t opcode) {
    if (c.out.size() > kMaxOut) return;  // client too slow: skip
    std::string h;
    h += char(0x80 | opcode);
    if (p.size() < 126) h += char(p.size());
    else if (p.size() < 65536) { h += char(126); h += char(p.size() >> 8); h += char(p.size() & 255); }
    else { h += char(127); for (int i = 7; i >= 0; --i) h += char((uint64_t(p.size()) >> (8 * i)) & 255); }
    c.out += h;
    c.out += p;
}

static std::string httpResponse(const char* status, const char* type, const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 " << status << "\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" << body;
    return o.str();
}


static std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}
static std::string urlEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

// Sends a large file in a separate thread (blocking, inherited low
// priority): the page and the sound are never slowed by a download.
static void sendFileDetached(int fd, const std::string& path, const std::string& name) {
    std::thread([fd, path, name] {
        const int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        timeval tv{30, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        const int in = open(path.c_str(), O_RDONLY);
        struct stat st;
        if (in < 0 || fstat(in, &st) != 0) {
            const char* r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            if (send(fd, r, std::strlen(r), MSG_NOSIGNAL) < 0) {}
            if (in >= 0) close(in);
            close(fd);
            return;
        }
        std::string ascii;
        for (unsigned char c : name) ascii += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? char(c) : '_';
        std::ostringstream h;
        h << "HTTP/1.1 200 OK\r\nContent-Type: audio/flac\r\nContent-Length: " << st.st_size
          << "\r\nContent-Disposition: attachment; filename=\"" << ascii << "\"; filename*=UTF-8''" << urlEncode(name)
          << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        const std::string hs = h.str();
        bool ok = send(fd, hs.data(), hs.size(), MSG_NOSIGNAL) == ssize_t(hs.size());
        off_t off = 0;
        while (ok && off < st.st_size) {
            const ssize_t w = sendfile(fd, in, &off, size_t(std::min<off_t>(st.st_size - off, 1 << 20)));
            if (w <= 0) ok = false;
        }
        close(in);
        shutdown(fd, SHUT_WR);
        close(fd);
    }).detach();
}

void WebServer::handleHttp(Client& c) {
    const size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (c.in.size() > 16384) { close(c.fd); c.fd = -1; }
        return;
    }
    std::string req = c.in.substr(0, end);
    c.in.erase(0, end + 4);
    std::istringstream is(req);
    std::string method, path;
    is >> method >> path;
    std::string key;
    bool upgrade = false;
    std::string line;
    std::getline(is, line);
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t col = line.find(':');
        if (col == std::string::npos) continue;
        std::string name = line.substr(0, col), val = line.substr(col + 1);
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);
        for (auto& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (name == "sec-websocket-key") key = val;
        if (name == "upgrade" && (val == "websocket" || val == "WebSocket")) upgrade = true;
    }
    const std::string base = path.substr(0, path.find('?'));
    if (base == "/ws" && upgrade && !key.empty()) {
        const std::string acc = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t dg[20];
        sha1(reinterpret_cast<const uint8_t*>(acc.data()), acc.size(), dg);
        c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                 base64(dg, 20) + "\r\n\r\n";
        c.ws = true;
        if (cb_.onConnect) sendFrame(c, cb_.onConnect());
        return;
    }
    static const std::string kDl = "/download/";
    if (method == "GET" && base.compare(0, kDl.size(), kDl) == 0 && cb_.downloadPath) {
        const std::string name = urlDecode(base.substr(kDl.size()));
        const std::string full = cb_.downloadPath(name);
        if (full.empty()) { c.out += httpResponse("404 Not Found", "text/plain; charset=utf-8", "not found\n"); return; }
        // The connection is handed to the sending thread; it is removed from the list without closing it.
        sendFileDetached(c.fd, full, name);
        c.fd = -1;
        return;
    }
    if (method == "GET" && (base == "/" || base == "/index.html")) {
        std::string page;
        std::ifstream f(pageFile_);
        if (!pageFile_.empty() && f) { std::stringstream ss; ss << f.rdbuf(); page = ss.str(); }
        else page = builtin_ ? builtin_ : "";
        c.out += httpResponse("200 OK", "text/html; charset=utf-8", page);
    } else {
        c.out += httpResponse("404 Not Found", "text/plain; charset=utf-8", "not found\n");
    }
}

void WebServer::handleWs(Client& c) {
    while (c.in.size() >= 2) {
        const uint8_t b0 = uint8_t(c.in[0]), b1 = uint8_t(c.in[1]);
        const uint8_t op = b0 & 0x0F;
        const bool masked = b1 & 0x80;
        uint64_t len = b1 & 0x7F;
        size_t pos = 2;
        if (len == 126) { if (c.in.size() < 4) return; len = (uint8_t(c.in[2]) << 8) | uint8_t(c.in[3]); pos = 4; }
        else if (len == 127) {
            if (c.in.size() < 10) return;
            len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(c.in[2 + i]);
            pos = 10;
        }
        if (len > kMaxIn) { close(c.fd); c.fd = -1; return; }
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) { if (c.in.size() < pos + 4) return; for (int i = 0; i < 4; ++i) mask[i] = uint8_t(c.in[pos + i]); pos += 4; }
        if (c.in.size() < pos + len) return;
        std::string payload = c.in.substr(pos, size_t(len));
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i & 3]);
        c.in.erase(0, pos + size_t(len));
        if (op == 0x8) { sendFrame(c, "", 0x8); c.ws = false; return; }      // close
        if (op == 0x9) { sendFrame(c, payload, 0xA); continue; }             // ping → pong
        if (op == 0x1 && cb_.onMessage) {
            const std::string r = cb_.onMessage(payload);
            if (!r.empty()) sendFrame(c, r);
        }
    }
}

void WebServer::loop() {
    std::vector<pollfd> pf;
    while (run_) {
        pf.clear();
        pf.push_back({listenFd_, POLLIN, 0});
        pf.push_back({wakePipe_[0], POLLIN, 0});
        for (auto& c : cl_) pf.push_back({c.fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        if (poll(pf.data(), pf.size(), 500) < 0 && errno != EINTR) break;
        if (pf[1].revents & POLLIN) { char buf[64]; while (read(wakePipe_[0], buf, sizeof buf) > 0) {} }
        // Messages to broadcast.
        std::vector<std::string> q;
        { std::lock_guard<std::mutex> lk(qMutex_); q.swap(queue_); }
        for (auto& m : q) for (auto& c : cl_) if (c.ws) sendFrame(c, m);
        // New clients.
        if (pf[0].revents & POLLIN) {
            for (;;) {
                int fd = accept(listenFd_, nullptr, nullptr);
                if (fd < 0) break;
                if (cl_.size() >= kMaxClients) { close(fd); continue; }
                fcntl(fd, F_SETFL, O_NONBLOCK);
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                Client c; c.fd = fd;
                cl_.push_back(std::move(c));
            }
        }
        // Read / write.
        for (size_t i = 0; i < cl_.size(); ++i) {
            Client& c = cl_[i];
            const short re = (i + 2 < pf.size() && pf[i + 2].fd == c.fd) ? pf[i + 2].revents : short(POLLIN | POLLOUT);
            if (re & (POLLIN | POLLHUP | POLLERR)) {
                char buf[8192];
                for (;;) {
                    ssize_t r = recv(c.fd, buf, sizeof buf, 0);
                    if (r > 0) { c.in.append(buf, size_t(r)); if (c.in.size() > kMaxIn * 2) { r = 0; } else continue; }
                    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close(c.fd); c.fd = -1; }
                    break;
                }
                if (c.fd >= 0) { if (c.ws) handleWs(c); else handleHttp(c); }
            }
            if (c.fd >= 0 && !c.out.empty()) {
                ssize_t w = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                if (w > 0) c.out.erase(0, size_t(w));
                else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { close(c.fd); c.fd = -1; }
                // HTTP response sent: close (Connection: close).
                if (c.fd >= 0 && !c.ws && c.out.empty() && c.in.empty()) { shutdown(c.fd, SHUT_WR); close(c.fd); c.fd = -1; }
            }
        }
        size_t w = 0;
        for (size_t i = 0; i < cl_.size(); ++i) if (cl_[i].fd >= 0) { if (w != i) cl_[w] = std::move(cl_[i]); ++w; }
        cl_.resize(w);
        int n = 0; for (auto& c : cl_) n += c.ws;
        nClients_ = n;
    }
}

// ---------------------------------------------------------------- flat JSON
bool JsonFlat::parse(const std::string& s) {
    kv.clear();
    size_t i = 0;
    auto ws = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
    auto str = [&](std::string& out) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                const char e = s[i];
                if (e == 'u' && i + 4 < s.size()) {
                    // \uXXXX (and surrogate pairs) → UTF-8
                    auto hex4 = [&](size_t at) { return unsigned(std::strtoul(s.substr(at, 4).c_str(), nullptr, 16)); };
                    unsigned cp = hex4(i + 1);
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                        const unsigned lo = hex4(i + 3);
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                } else out += (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : (e == 'b') ? '\b' : (e == 'f') ? '\f' : e;
            } else out += s[i];
            ++i;
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    for (;;) {
        ws();
        if (i < s.size() && s[i] == '}') return true;
        std::string k, v;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i; ws();
        if (i < s.size() && s[i] == '"') { if (!str(v)) return false; }
        else { while (i < s.size() && s[i] != ',' && s[i] != '}') v += s[i++]; while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back(); }
        kv.emplace_back(k, v);
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return true;
        return false;
    }
}
bool JsonFlat::has(const char* k) const { for (auto& p : kv) if (p.first == k) return true; return false; }
std::string JsonFlat::str(const char* k, const std::string& def) const { for (auto& p : kv) if (p.first == k) return p.second; return def; }
double JsonFlat::num(const char* k, double def) const {
    for (auto& p : kv) if (p.first == k) {
        if (p.second == "true") return 1;
        if (p.second == "false") return 0;
        char* e = nullptr; const double v = std::strtod(p.second.c_str(), &e); return (e && e != p.second.c_str()) ? v : def;
    }
    return def;
}
std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

} // namespace vc
