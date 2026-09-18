#ifndef ES_CURSOR_HPP
#define ES_CURSOR_HPP

#include "json.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace es {

using json = nlohmann::json;

/**
 * 游标负载（解码后的内容）
 *
 * 游标是发给调用方的不透明字符串，内部承载：
 *  - PIT id            ：Elasticsearch Point in Time 快照标识
 *  - searchAfter 值    ：上一页最后一条记录的排序值，用于 search_after 前进
 *  - 参数指纹          ：索引 + 查询 + 排序 + 页大小的哈希，防止游标被
 *                        拿去配合另一组检索条件使用
 *  - 过期时间          ：客户端侧过期点，与 PIT keep_alive 对齐
 */
struct CursorPayload {
    std::string pitId;
    json searchAfter = json::array();
    std::string fingerprint;
    int64_t expiresAt = 0;  // Unix 时间戳（秒）
};

/**
 * 游标编解码器
 *
 * 游标格式： esc1.<base64url(payload json)>.<base64url(hmac-sha256)>
 * 签名密钥由客户端实例持有，不随游标外发；任何对游标内容的篡改都会
 * 导致签名校验失败。
 */
class CursorCodec {
public:
    explicit CursorCodec(std::string secret);

    /**
     * 生成随机密钥（用于构造 CursorCodec）
     */
    static std::string generateSecret();

    /**
     * 计算检索参数指纹。同一游标只允许配合首次请求时的
     * 索引、查询、排序和页大小继续使用。
     */
    static std::string fingerprint(const std::string& indexName,
                                   const json& query,
                                   const json& sort,
                                   int pageSize);

    /**
     * 编码并签名
     */
    std::string encode(const CursorPayload& payload) const;

    /**
     * 解码并验签。
     * @return 负载；格式非法或签名不符时返回 std::nullopt
     */
    std::optional<CursorPayload> decode(const std::string& cursor) const;

private:
    std::string secret_;
};

} // namespace es

#endif // ES_CURSOR_HPP
