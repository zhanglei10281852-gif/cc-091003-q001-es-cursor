#ifndef ES_CLIENT_HPP
#define ES_CLIENT_HPP

#include "http_client.hpp"
#include "json.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace es {

using json = nlohmann::json;

/**
 * 搜索命中结果
 */
struct SearchHit {
    std::string id;
    std::string index;
    double score;
    json source;
    json highlight;
    json sortValues;  // 排序值（PIT/search_after 分页时由 ES 返回）
};

/**
 * 搜索结果
 */
struct SearchResult {
    int total;
    double maxScore;
    std::vector<SearchHit> hits;
    int took;  // 耗时（毫秒）
    bool timedOut;
};

/**
 * 文档操作结果
 */
struct DocResult {
    std::string id;
    std::string index;
    std::string result;  // created, updated, deleted
    int version;
    bool success;
};

/**
 * 批量操作结果
 */
struct BulkResult {
    int took;
    bool errors;
    std::vector<DocResult> items;
    int successCount;
    int failCount;
};

/**
 * Elasticsearch 客户端异常
 */
class ESException : public std::runtime_error {
public:
    explicit ESException(const std::string& message)
        : std::runtime_error(message) {}
};

// ==================== 游标分页异常体系 ====================

/**
 * 游标相关错误的基类。
 * 所有游标分页失败都可以通过捕获该类型统一处理，
 * 也可以按下列子类型区分具体原因。
 */
class CursorException : public ESException {
public:
    explicit CursorException(const std::string& message)
        : ESException(message) {}
};

/**
 * 游标被篡改或格式非法（签名不匹配、无法解码等）。
 */
class CursorTamperedException : public CursorException {
public:
    explicit CursorTamperedException(const std::string& message)
        : CursorException("cursor tampered or malformed: " + message) {}
};

/**
 * 游标已超过客户端侧有效期（cursorTtlSeconds）。
 */
class CursorExpiredException : public CursorException {
public:
    explicit CursorExpiredException(const std::string& message)
        : CursorException("cursor expired: " + message) {}
};

/**
 * 游标与当前请求不绑定：索引、查询条件、排序或页大小不一致。
 */
class CursorMismatchException : public CursorException {
public:
    explicit CursorMismatchException(const std::string& message)
        : CursorException("cursor does not match request: " + message) {}
};

/**
 * Elasticsearch 侧的 PIT 已不存在（被关闭或超过 keep_alive 被回收）。
 */
class PitNotFoundException : public CursorException {
public:
    explicit PitNotFoundException(const std::string& message)
        : CursorException("point in time no longer exists: " + message) {}
};

// ==================== 游标分页（PIT + search_after） ====================

/**
 * 游标分页请求参数。
 *
 * 首次请求（cursorSearchFirst）与后续请求（cursorSearchNext）必须传入
 * 完全相同的 index / query / sort / pageSize，否则游标校验失败并抛出
 * CursorMismatchException —— 游标不能被拿去配合另一组检索条件使用。
 */
struct CursorSearchRequest {
    std::string index;                 // 目标索引
    json query = json::object();       // 查询 DSL，缺省视为 match_all
    json sort = json::array();         // 业务排序；客户端会自动追加 _shard_doc 决胜键，
                                       // 保证同分值记录有确定次序
    int pageSize = 10;                 // 每页大小
    std::string keepAlive = "2m";      // PIT 保活时间（每次翻页自动续期；
                                       // 进程异常退出后 ES 会按该时长自行回收）
    long cursorTtlSeconds = 600;       // 游标客户端侧有效期（秒），应大于 keepAlive
};

/**
 * 一页游标分页结果。
 */
struct CursorPage {
    SearchResult result;      // 当前页数据
    std::string nextCursor;   // 不透明的下一页游标；空串表示没有下一页
    bool hasMore = false;     // 是否还有下一页
};

/**
 * Elasticsearch 客户端类
 */
class ESClient {
public:
    /**
     * 构造函数
     * @param host ES 主机地址
     * @param port ES 端口
     */
    explicit ESClient(const std::string& host = "localhost", int port = 9200);
    ~ESClient();
    
    // ==================== 集群操作 ====================
    
    /**
     * 检查 ES 连接是否正常
     */
    bool ping();
    
    /**
     * 获取集群健康状态
     */
    json clusterHealth();
    
    /**
     * 获取集群信息
     */
    json clusterInfo();
    
    // ==================== 索引操作 ====================
    
    /**
     * 创建索引
     * @param indexName 索引名称
     * @param mappings 映射配置（可选）
     * @param settings 索引设置（可选）
     */
    bool createIndex(const std::string& indexName,
                     const json& mappings = json::object(),
                     const json& settings = json::object());
    
    /**
     * 删除索引
     */
    bool deleteIndex(const std::string& indexName);
    
    /**
     * 检查索引是否存在
     */
    bool indexExists(const std::string& indexName);
    
    /**
     * 获取索引信息
     */
    json getIndex(const std::string& indexName);
    
    /**
     * 刷新索引（使文档可搜索）
     */
    bool refreshIndex(const std::string& indexName);
    
    // ==================== 文档操作 ====================
    
    /**
     * 索引文档（添加或更新）
     * @param indexName 索引名称
     * @param doc 文档内容
     * @param id 文档 ID（可选，不指定则自动生成）
     */
    DocResult indexDocument(const std::string& indexName,
                            const json& doc,
                            const std::string& id = "");
    
    /**
     * 获取文档
     */
    std::optional<json> getDocument(const std::string& indexName,
                                    const std::string& id);
    
    /**
     * 更新文档
     */
    DocResult updateDocument(const std::string& indexName,
                             const std::string& id,
                             const json& doc);
    
    /**
     * 删除文档
     */
    bool deleteDocument(const std::string& indexName,
                        const std::string& id);
    
    /**
     * 批量索引文档
     */
    BulkResult bulkIndex(const std::string& indexName,
                         const std::vector<json>& docs,
                         const std::vector<std::string>& ids = {});
    
    // ==================== 搜索操作 ====================
    
    /**
     * Match 查询（分词匹配）
     */
    SearchResult matchSearch(const std::string& indexName,
                             const std::string& field,
                             const std::string& query,
                             int from = 0,
                             int size = 10);
    
    /**
     * Multi-Match 查询（多字段匹配）
     */
    SearchResult multiMatchSearch(const std::string& indexName,
                                  const std::vector<std::string>& fields,
                                  const std::string& query,
                                  int from = 0,
                                  int size = 10);
    
    /**
     * Term 查询（精确匹配）
     */
    SearchResult termSearch(const std::string& indexName,
                            const std::string& field,
                            const std::string& value,
                            int from = 0,
                            int size = 10);
    
    /**
     * Bool 组合查询
     */
    SearchResult boolSearch(const std::string& indexName,
                            const json& must = json::array(),
                            const json& should = json::array(),
                            const json& mustNot = json::array(),
                            const json& filter = json::array(),
                            int from = 0,
                            int size = 10);
    
    /**
     * 带高亮的搜索
     */
    SearchResult searchWithHighlight(const std::string& indexName,
                                     const json& query,
                                     const std::vector<std::string>& highlightFields,
                                     int from = 0,
                                     int size = 10);
    
    /**
     * 通用搜索（自定义查询体）
     */
    SearchResult search(const std::string& indexName,
                        const json& queryBody);

    // ==================== 游标分页（PIT + search_after） ====================

    /**
     * 建立 Point in Time 快照。
     * @param indexName 目标索引
     * @param keepAlive 保活时间（如 "2m"），期间无请求则 ES 自动回收
     * @return PIT id
     */
    std::string openPointInTime(const std::string& indexName,
                                const std::string& keepAlive);

    /**
     * 关闭 Point in Time。
     * @return true 表示成功释放；false 表示 PIT 已不存在（重复关闭）
     */
    bool closePointInTime(const std::string& pitId);

    /**
     * 游标分页首页：建立 PIT 并返回第一页结果与不透明的下一页游标。
     * 结果不足一页时 PIT 会被立即关闭，nextCursor 为空。
     */
    CursorPage cursorSearchFirst(const CursorSearchRequest& request);

    /**
     * 游标分页后续页：沿用与首页完全相同的请求参数和上一页返回的游标。
     *
     * @throws CursorTamperedException  游标被篡改或格式非法
     * @throws CursorExpiredException   游标超过客户端侧有效期
     * @throws CursorMismatchException  游标与当前索引/查询/排序/页大小不绑定
     * @throws PitNotFoundException     ES 侧 PIT 已关闭或被回收
     */
    CursorPage cursorSearchNext(const CursorSearchRequest& request,
                                const std::string& cursor);

    /**
     * 主动结束游标分页：校验游标完整性后关闭其持有的 PIT。
     * 即使游标已过客户端有效期也会尝试关闭（避免服务端资源泄漏）。
     *
     * @return true 表示成功释放；false 表示 PIT 已不存在
     * @throws CursorTamperedException 游标被篡改或格式非法
     */
    bool closeCursor(const std::string& cursor);

    /**
     * 设置游标签名密钥。
     * 默认在客户端构造时随机生成（进程重启后旧游标即失效）；
     * 多实例共享游标或需要重启后继续翻页时，应设置稳定密钥。
     */
    void setCursorSecret(const std::string& secret);
    
    // ==================== 日志回调 ====================
    
    using LogCallback = std::function<void(const std::string&)>;
    
    /**
     * 设置日志回调
     */
    void setLogCallback(LogCallback callback);

private:
    std::string baseUrl_;
    HttpClient httpClient_;
    LogCallback logCallback_;
    std::string cursorSecret_;  // 游标 HMAC 签名密钥

    void log(const std::string& message);
    std::string buildUrl(const std::string& path);
    SearchResult parseSearchResponse(const json& response);

    // 游标分页内部实现
    json effectiveQuery(const CursorSearchRequest& request) const;
    json effectiveSort(const CursorSearchRequest& request) const;
    std::string encodeCursor(const json& payload) const;
    json decodeCursor(const std::string& cursor) const;  // 仅校验完整性，不校验有效期/绑定
    CursorPage runCursorSearch(const CursorSearchRequest& request,
                               const std::string& pitId,
                               const json& searchAfter);
    bool isPitMissing(const HttpResponse& response) const;
};

} // namespace es

#endif // ES_CLIENT_HPP
