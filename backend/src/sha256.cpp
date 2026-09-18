#include "sha256.hpp"
#include <cstring>

namespace es {

// ==================== SHA-256 ====================

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

} // namespace

Sha256::Sha256()
    : bitLen_(0), bufferLen_(0) {
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
}

void Sha256::transform(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    bitLen_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        size_t take = 64 - bufferLen_;
        if (take > len) take = len;
        std::memcpy(buffer_ + bufferLen_, data, take);
        bufferLen_ += take;
        data += take;
        len -= take;
        if (bufferLen_ == 64) {
            transform(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Sha256::update(const std::string& s) {
    update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::array<uint8_t, 32> Sha256::final() {
    // 追加 0x80 与长度填充
    uint8_t pad = 0x80;
    uint64_t savedBitLen = bitLen_;
    update(&pad, 1);
    bitLen_ = savedBitLen;  // 填充不计入消息长度
    uint8_t zero = 0;
    while (bufferLen_ != 56) {
        update(&zero, 1);
        bitLen_ = savedBitLen;
    }
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<uint8_t>(savedBitLen >> (56 - i * 8));
    }
    update(lenBytes, 8);

    std::array<uint8_t, 32> digest;
    for (int i = 0; i < 8; ++i) {
        digest[i * 4]     = static_cast<uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    return digest;
}

namespace {

std::string toHex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0f];
    }
    return out;
}

} // namespace

std::string Sha256::hex(const std::string& s) {
    Sha256 h;
    h.update(s);
    auto digest = h.final();
    return toHex(digest.data(), digest.size());
}

// ==================== HMAC-SHA256 ====================

std::string hmacSha256Hex(const std::string& key, const std::string& message) {
    constexpr size_t kBlockSize = 64;

    std::string keyBlock(key);
    if (keyBlock.size() > kBlockSize) {
        Sha256 h;
        h.update(keyBlock);
        auto digest = h.final();
        keyBlock.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    }
    keyBlock.resize(kBlockSize, '\0');

    std::string inner(keyBlock), outer(keyBlock);
    for (size_t i = 0; i < kBlockSize; ++i) {
        inner[i] = static_cast<char>(inner[i] ^ 0x36);
        outer[i] = static_cast<char>(outer[i] ^ 0x5c);
    }

    Sha256 hInner;
    hInner.update(inner);
    hInner.update(message);
    auto innerDigest = hInner.final();

    Sha256 hOuter;
    hOuter.update(outer);
    hOuter.update(innerDigest.data(), innerDigest.size());
    auto result = hOuter.final();
    return toHex(result.data(), result.size());
}

// ==================== base64url ====================

namespace {

const char kB64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64UrlValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

} // namespace

std::string base64UrlEncode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out += kB64UrlAlphabet[(n >> 18) & 63];
        out += kB64UrlAlphabet[(n >> 12) & 63];
        if (i + 1 < len) out += kB64UrlAlphabet[(n >> 6) & 63];
        if (i + 2 < len) out += kB64UrlAlphabet[n & 63];
    }
    return out;
}

std::string base64UrlEncode(const std::string& s) {
    return base64UrlEncode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool base64UrlDecode(const std::string& s, std::string& out) {
    if (s.size() % 4 == 1) return false;  // 不可能是合法的 base64 长度
    out.clear();
    out.reserve(s.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v = b64UrlValue(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xff);
        }
    }
    return true;
}

// ==================== 常量时间比较 ====================

bool constantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace es
