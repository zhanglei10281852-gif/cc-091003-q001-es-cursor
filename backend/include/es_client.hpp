#ifndef ES_CLIENT_HPP
#define ES_CLIENT_HPP

#include "http_client.hpp"
#include "es_cursor.hpp"
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
    json sort;  // 排序值（search_after 游标使用；未排序时为空）
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

// ==================== 游标分页异常（可区分的失败类型） ====================

/**
 * 游标分页相关异常基类
 */
class CursorException : public ESException {
public:
    explicit CursorException(const std::string& message)
        : ESException(message) {}
};

/**
 * 游标被篡改或无效：格式非法、签名校验失败，
 * 或游标被拿去配合另一组索引 / 查询 / 排序 / 页大小使用
 */
class CursorTamperedException : public CursorException {
public:
    explicit CursorTamperedException(const std::string& message)
        : CursorException(message) {}
};

/**
 * 游标已过期：超过 keep_alive 保活时间未继续翻页
 */
class CursorExpiredException : public CursorException {
public:
    explicit CursorExpiredException(const std::string& message)
        : CursorException(message) {}
};

/**
 * PIT 已被 Elasticsearch 释放：快照上下文在服务端已不存在
 * （保活超时被回收，或已被关闭）
 */
class PitGoneException : public CursorException {
public:
    explicit PitGoneException(const std::string& message)
        : CursorException(message) {}
};

// ==================== 游标分页类型 ====================

/**
 * 游标分页请求参数
 *
 * 首次请求将 cursor 留空：客户端会建立 Point in Time 快照并返回第一页。
 * 后续请求把上一页返回的 nextCursor 填入 cursor，其余参数必须与首次
 * 请求保持一致，否则会被拒绝（CursorTamperedException）。
 */
struct CursorSearchOptions {
    std::string indexName;               // 目标索引
    json query = json::object();         // 查询条件（缺省为 match_all）
    json sort = json::array();           // 排序定义（缺省按 _score 降序）
    int pageSize = 10;                   // 每页大小
    std::string keepAlive = "2m";        // PIT 保活时间（建议保持较短，
                                         // 异常退出后快照可自行回收）
    std::string cursor;                  // 上一页返回的游标（首次请求留空）
};

/**
 * 一页游标分页结果
 */
struct CursorPage {
    SearchResult result;                 // 本页搜索结果
    std::string nextCursor;              // 下一页游标（不透明字符串）
    bool hasMore = false;                // 是否还有下一页；false 时 PIT 已自动关闭
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

    // ==================== 稳定游标分页（PIT + search_after） ====================

    /**
     * 建立 Point in Time 快照
     * @param indexName 索引名称
     * @param keepAlive 保活时间（如 "1m"、"30s"），每次搜索会续期
     * @return PIT id
     */
    std::string openPointInTime(const std::string& indexName,
                                const std::string& keepAlive);

    /**
     * 关闭 Point in Time 快照
     * @return 是否成功释放（PIT 不存在时返回 false，不抛异常）
     */
    bool closePointInTime(const std::string& pitId);

    /**
     * 稳定游标分页检索
     *
     * 首次请求（options.cursor 为空）建立 PIT 快照并返回第一页；
     * 后续请求传入上一页的 nextCursor，沿同一快照用 search_after 前进，
     * 期间索引发生的新增 / 删除不会打乱已经开始的浏览。
     *
     * 同分值记录通过自动追加的 _shard_doc 决胜排序保证确定次序。
     * 读到末页（hasMore == false）时 PIT 会被自动关闭。
     *
     * @throws CursorTamperedException 游标被篡改，或索引 / 查询 / 排序 /
     *         页大小与首次请求不一致
     * @throws CursorExpiredException 游标已超过保活时间
     * @throws PitGoneException PIT 已被 Elasticsearch 释放
     */
    CursorPage searchByCursor(const CursorSearchOptions& options);

    /**
     * 主动结束游标分页：关闭游标关联的 PIT 快照。
     * 之后该游标将无法再用于翻页。重复关闭是安全的（幂等）。
     */
    void closeCursor(const std::string& cursor);

    /**
     * 设置游标签名密钥（默认随机生成）。
     * 需要跨进程验证游标时，可在各实例上设置相同密钥。
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
    CursorCodec cursorCodec_;

    void log(const std::string& message);
    std::string buildUrl(const std::string& path);
    SearchResult parseSearchResponse(const json& response);
};

} // namespace es

#endif // ES_CLIENT_HPP
