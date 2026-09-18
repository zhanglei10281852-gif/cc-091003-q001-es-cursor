#ifndef ES_SHA256_HPP
#define ES_SHA256_HPP

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

namespace es {

/**
 * SHA-256 与 HMAC-SHA256 工具。
 *
 * 用于游标内容的完整性签名与查询条件指纹计算，
 * 不引入额外的第三方依赖（OpenSSL 等）。
 */
class Sha256 {
public:
    Sha256();

    void update(const uint8_t* data, size_t len);
    void update(const std::string& s);

    /** 结束计算并返回 32 字节摘要（调用后对象不可复用） */
    std::array<uint8_t, 32> final();

    /** 计算字符串的 SHA-256 十六进制摘要 */
    static std::string hex(const std::string& s);

private:
    uint32_t state_[8];
    uint64_t bitLen_;
    uint8_t buffer_[64];
    size_t bufferLen_;

    void transform(const uint8_t* block);
};

/**
 * HMAC-SHA256（RFC 2104），返回十六进制字符串。
 */
std::string hmacSha256Hex(const std::string& key, const std::string& message);

/**
 * base64url 编码（RFC 4648 §5，无填充字符 '='）。
 */
std::string base64UrlEncode(const uint8_t* data, size_t len);
std::string base64UrlEncode(const std::string& s);

/**
 * base64url 解码。遇到非法字符或长度不合法时返回 false。
 */
bool base64UrlDecode(const std::string& s, std::string& out);

/**
 * 常量时间字符串比较，避免签名比对泄露时序信息。
 */
bool constantTimeEqual(const std::string& a, const std::string& b);

} // namespace es

#endif // ES_SHA256_HPP
