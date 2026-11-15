// vinylclean desktop — minimal flat JSON (same format as the Raspberry Pi
// version, tools/web_server.cpp), header-only so it builds on Windows too.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace vcd {

// Minimal flat JSON: {"key": number | "text" | true | false, ...}
struct JsonFlat {
    std::vector<std::pair<std::string, std::string>> kv;  // raw values (text without quotes)
    bool parse(const std::string& s);
    bool has(const char* k) const;
    std::string str(const char* k, const std::string& def = "") const;
    double num(const char* k, double def = 0) const;
};

inline bool JsonFlat::parse(const std::string& s) {
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
inline bool JsonFlat::has(const char* k) const { for (auto& p : kv) if (p.first == k) return true; return false; }
inline std::string JsonFlat::str(const char* k, const std::string& def) const { for (auto& p : kv) if (p.first == k) return p.second; return def; }
inline double JsonFlat::num(const char* k, double def) const {
    for (auto& p : kv) if (p.first == k) {
        if (p.second == "true") return 1;
        if (p.second == "false") return 0;
        char* e = nullptr; const double v = std::strtod(p.second.c_str(), &e); return (e && e != p.second.c_str()) ? v : def;
    }
    return def;
}
inline std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

} // namespace vcd
