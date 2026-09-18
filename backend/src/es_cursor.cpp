#include "es_cursor.hpp"

#include <array>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

namespace es {

namespace {

// ==================== SHA-256（自包含实现，避免引入额外依赖） ====================

class Sha256 {
public:
    Sha256() : bitLen_(0), bufferLen_(0) {
        h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
        h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
    }

    void update(const uint8_t* data, size_t len) {
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

    std::array<uint8_t, 32> final() {
        buffer_[bufferLen_++] = 0x80;
        if (bufferLen_ > 56) {
            while (bufferLen_ < 64) buffer_[bufferLen_++] = 0;
            transform(buffer_);
            bufferLen_ = 0;
        }
        while (bufferLen_ < 56) buffer_[bufferLen_++] = 0;
        for (int i = 7; i >= 0; --i) {
            buffer_[bufferLen_++] = static_cast<uint8_t>(bitLen_ >> (i * 8));
        }
        transform(buffer_);

        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
        }
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void transform(const uint8_t* block) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

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

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];

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

        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    uint32_t h_[8];
    uint64_t bitLen_;
    uint8_t buffer_[64];
    size_t bufferLen_;
};

std::string sha256(const std::string& input) {
    Sha256 ctx;
    ctx.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    auto digest = ctx.final();
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string hmacSha256(const std::string& key, const std::string& message) {
    constexpr size_t kBlockSize = 64;
    std::string k = key;
    if (k.size() > kBlockSize) {
        k = sha256(k);
    }
    k.resize(kBlockSize, '\0');

    std::string inner(kBlockSize, '\0');
    std::string outer(kBlockSize, '\0');
    for (size_t i = 0; i < kBlockSize; ++i) {
        inner[i] = static_cast<char>(k[i] ^ 0x36);
        outer[i] = static_cast<char>(k[i] ^ 0x5c);
    }
    return sha256(outer + sha256(inner + message));
}

std::string toHex(const std::string& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

// ==================== Base64URL（无 padding） ====================

std::string base64UrlEncode(const std::string& input) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= input.size()) {
        uint32_t v = (static_cast<uint8_t>(input[i]) << 16) |
                     (static_cast<uint8_t>(input[i + 1]) << 8) |
                     static_cast<uint8_t>(input[i + 2]);
        out.push_back(alphabet[(v >> 18) & 0x3f]);
        out.push_back(alphabet[(v >> 12) & 0x3f]);
        out.push_back(alphabet[(v >> 6) & 0x3f]);
        out.push_back(alphabet[v & 0x3f]);
        i += 3;
    }
    size_t rem = input.size() - i;
    if (rem == 1) {
        uint32_t v = static_cast<uint8_t>(input[i]) << 16;
        out.push_back(alphabet[(v >> 18) & 0x3f]);
        out.push_back(alphabet[(v >> 12) & 0x3f]);
    } else if (rem == 2) {
        uint32_t v = (static_cast<uint8_t>(input[i]) << 16) |
                     (static_cast<uint8_t>(input[i + 1]) << 8);
        out.push_back(alphabet[(v >> 18) & 0x3f]);
        out.push_back(alphabet[(v >> 12) & 0x3f]);
        out.push_back(alphabet[(v >> 6) & 0x3f]);
    }
    return out;
}

bool base64UrlDecode(const std::string& input, std::string& output) {
    if (input.size() % 4 == 1) {
        return false;  // 非法的 base64 长度
    }
    auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    output.clear();
    output.reserve(input.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : input) {
        int v = valueOf(c);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return true;
}

// 常数时间比较，避免签名比对泄露时序信息
bool constantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

constexpr const char* kCursorPrefix = "esc1";

} // namespace

// ==================== CursorCodec ====================

CursorCodec::CursorCodec(std::string secret) : secret_(std::move(secret)) {
    if (secret_.empty()) {
        throw std::invalid_argument("CursorCodec secret must not be empty");
    }
}

std::string CursorCodec::generateSecret() {
    std::random_device rd;
    std::string raw(32, '\0');
    for (auto& c : raw) {
        c = static_cast<char>(rd());
    }
    return toHex(raw);
}

std::string CursorCodec::fingerprint(const std::string& indexName,
                                     const json& query,
                                     const json& sort,
                                     int pageSize) {
    // nlohmann::json 的 dump() 对 object 按键名排序输出，序列化结果是确定的
    std::string material = indexName + "\n" + query.dump() + "\n" +
                           sort.dump() + "\n" + std::to_string(pageSize);
    return toHex(sha256(material));
}

std::string CursorCodec::encode(const CursorPayload& payload) const {
    json body = {
        {"v", 1},
        {"pit", payload.pitId},
        {"sa", payload.searchAfter},
        {"fp", payload.fingerprint},
        {"exp", payload.expiresAt}
    };
    std::string encoded = base64UrlEncode(body.dump());
    std::string mac = base64UrlEncode(hmacSha256(secret_, encoded));
    return std::string(kCursorPrefix) + "." + encoded + "." + mac;
}

std::optional<CursorPayload> CursorCodec::decode(const std::string& cursor) const {
    // 格式：esc1.<payload>.<mac>
    const std::string prefix = std::string(kCursorPrefix) + ".";
    if (cursor.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    size_t dotPos = cursor.rfind('.');
    if (dotPos == std::string::npos || dotPos <= prefix.size() - 1 ||
        dotPos == cursor.size() - 1) {
        return std::nullopt;
    }

    std::string encoded = cursor.substr(prefix.size(), dotPos - prefix.size());
    std::string mac = cursor.substr(dotPos + 1);

    std::string expectedMac = base64UrlEncode(hmacSha256(secret_, encoded));
    if (!constantTimeEqual(mac, expectedMac)) {
        return std::nullopt;
    }

    std::string raw;
    if (!base64UrlDecode(encoded, raw)) {
        return std::nullopt;
    }

    json body = json::parse(raw, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::nullopt;
    }
    if (body.value("v", 0) != 1 || !body.contains("pit") || !body.contains("fp") ||
        !body.contains("exp") || !body.contains("sa")) {
        return std::nullopt;
    }
    if (!body["pit"].is_string() || !body["fp"].is_string() ||
        !body["exp"].is_number() || !body["sa"].is_array()) {
        return std::nullopt;
    }

    CursorPayload payload;
    payload.pitId = body["pit"].get<std::string>();
    payload.fingerprint = body["fp"].get<std::string>();
    payload.expiresAt = body["exp"].get<int64_t>();
    payload.searchAfter = body["sa"];
    if (payload.pitId.empty()) {
        return std::nullopt;
    }
    return payload;
}

} // namespace es
