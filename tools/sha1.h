// vinylclean — SHA-1 and base64, only for the WebSocket handshake
// (RFC 6455). Direct implementation of the FIPS 180-1 standard. Original work.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace vc {

inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    const uint64_t bitLen = uint64_t(len) * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; ++i) {
            const size_t k = off + i;
            uint8_t b;
            if (k < len) b = data[k];
            else if (k == len) b = 0x80;
            else if (k >= total - 8) b = uint8_t(bitLen >> (8 * (total - 1 - k)));
            else b = 0;
            blk[i] = b;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(blk[4 * i]) << 24) | (uint32_t(blk[4 * i + 1]) << 16) | (uint32_t(blk[4 * i + 2]) << 8) | blk[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
}

inline std::string base64(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(d[i]) << 16;
        if (i + 1 < n) v |= uint32_t(d[i + 1]) << 8;
        if (i + 2 < n) v |= d[i + 2];
        s += t[(v >> 18) & 63];
        s += t[(v >> 12) & 63];
        s += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        s += (i + 2 < n) ? t[v & 63] : '=';
    }
    return s;
}

} // namespace vc
