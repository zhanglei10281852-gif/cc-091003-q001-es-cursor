#include "es_client.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace es {

// ==================== 构造与析构 ====================

ESClient::ESClient(const std::string& host, int port)
    : cursorCodec_(CursorCodec::generateSecret()) {
    std::ostringstream oss;
    oss << "http://" << host << ":" << port;
    baseUrl_ = oss.str();
    httpClient_.setTimeout(30);
    httpClient_.setConnectTimeout(10);
}

ESClient::~ESClient() = default;

// ==================== 辅助方法 ====================

void ESClient::log(const std::string& message) {
    if (logCallback_) {
        logCallback_(message);
    }
}

std::string ESClient::buildUrl(const std::string& path) {
    return baseUrl_ + path;
}

void ESClient::setLogCallback(LogCallback callback) {
    logCallback_ = std::move(callback);
}

// ==================== 集群操作 ====================

bool ESClient::ping() {
    try {
        auto response = httpClient_.get(buildUrl("/"));
        return response.isSuccess();
    } catch (const HttpException&) {
        return false;
    }
}

json ESClient::clusterHealth() {
    auto response = httpClient_.get(buildUrl("/_cluster/health"));
    if (!response.isSuccess()) {
        throw ESException("Failed to get cluster health: " + response.body);
    }
    return json::parse(response.body);
}

json ESClient::clusterInfo() {
    auto response = httpClient_.get(buildUrl("/"));
    if (!response.isSuccess()) {
        throw ESException("Failed to get cluster info: " + response.body);
    }
    return json::parse(response.body);
}

// ==================== 索引操作 ====================

bool ESClient::createIndex(const std::string& indexName,
                           const json& mappings,
                           const json& settings) {
    json body;
    if (!mappings.empty()) {
        body["mappings"] = mappings;
    }
    if (!settings.empty()) {
        body["settings"] = settings;
    }
    
    log("Creating index: " + indexName);
    auto response = httpClient_.put(buildUrl("/" + indexName), body.dump());
    
    if (!response.isSuccess()) {
        auto error = json::parse(response.body);
        throw ESException("Failed to create index: " + 
                         error.value("error", json::object()).value("reason", response.body));
    }
    
    log("Index created successfully: " + indexName);
    return true;
}

bool ESClient::deleteIndex(const std::string& indexName) {
    log("Deleting index: " + indexName);
    auto response = httpClient_.del(buildUrl("/" + indexName));
    
    if (!response.isSuccess() && !response.isNotFound()) {
        throw ESException("Failed to delete index: " + response.body);
    }
    
    log("Index deleted: " + indexName);
    return true;
}

bool ESClient::indexExists(const std::string& indexName) {
    auto response = httpClient_.head(buildUrl("/" + indexName));
    return response.isSuccess();
}

json ESClient::getIndex(const std::string& indexName) {
    auto response = httpClient_.get(buildUrl("/" + indexName));
    if (!response.isSuccess()) {
        throw ESException("Failed to get index: " + response.body);
    }
    return json::parse(response.body);
}

bool ESClient::refreshIndex(const std::string& indexName) {
    auto response = httpClient_.post(buildUrl("/" + indexName + "/_refresh"), "");
    return response.isSuccess();
}

// ==================== 文档操作 ====================

DocResult ESClient::indexDocument(const std::string& indexName,
                                  const json& doc,
                                  const std::string& id) {
    std::string url = "/" + indexName + "/_doc";
    if (!id.empty()) {
        url += "/" + id;
    }
    
    auto response = httpClient_.post(buildUrl(url), doc.dump());
    
    DocResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.id = respJson.value("_id", "");
        result.index = respJson.value("_index", "");
        result.result = respJson.value("result", "");
        result.version = respJson.value("_version", 0);
        result.success = true;
        log("Document indexed: " + result.id);
    } else {
        result.success = false;
        throw ESException("Failed to index document: " + response.body);
    }
    
    return result;
}

std::optional<json> ESClient::getDocument(const std::string& indexName,
                                          const std::string& id) {
    auto response = httpClient_.get(buildUrl("/" + indexName + "/_doc/" + id));
    
    if (response.isNotFound()) {
        return std::nullopt;
    }
    
    if (!response.isSuccess()) {
        throw ESException("Failed to get document: " + response.body);
    }
    
    auto respJson = json::parse(response.body);
    if (respJson.value("found", false)) {
        return respJson["_source"];
    }
    return std::nullopt;
}

DocResult ESClient::updateDocument(const std::string& indexName,
                                   const std::string& id,
                                   const json& doc) {
    json body = {{"doc", doc}};
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_update/" + id), 
        body.dump()
    );
    
    DocResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.id = respJson.value("_id", "");
        result.index = respJson.value("_index", "");
        result.result = respJson.value("result", "");
        result.version = respJson.value("_version", 0);
        result.success = true;
        log("Document updated: " + result.id);
    } else {
        result.success = false;
        throw ESException("Failed to update document: " + response.body);
    }
    
    return result;
}

bool ESClient::deleteDocument(const std::string& indexName,
                              const std::string& id) {
    auto response = httpClient_.del(buildUrl("/" + indexName + "/_doc/" + id));
    
    if (response.isSuccess()) {
        log("Document deleted: " + id);
        return true;
    }
    
    if (response.isNotFound()) {
        return false;
    }
    
    throw ESException("Failed to delete document: " + response.body);
}

BulkResult ESClient::bulkIndex(const std::string& indexName,
                               const std::vector<json>& docs,
                               const std::vector<std::string>& ids) {
    std::ostringstream body;
    
    for (size_t i = 0; i < docs.size(); ++i) {
        json action = {{"index", {{"_index", indexName}}}};
        if (i < ids.size() && !ids[i].empty()) {
            action["index"]["_id"] = ids[i];
        }
        body << action.dump() << "\n";
        body << docs[i].dump() << "\n";
    }
    
    auto response = httpClient_.post(buildUrl("/_bulk"), body.str());
    
    BulkResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.took = respJson.value("took", 0);
        result.errors = respJson.value("errors", false);
        result.successCount = 0;
        result.failCount = 0;
        
        for (const auto& item : respJson["items"]) {
            DocResult docResult;
            const auto& indexResult = item["index"];
            docResult.id = indexResult.value("_id", "");
            docResult.index = indexResult.value("_index", "");
            docResult.result = indexResult.value("result", "");
            docResult.version = indexResult.value("_version", 0);
            docResult.success = indexResult.value("status", 500) < 300;
            
            if (docResult.success) {
                result.successCount++;
            } else {
                result.failCount++;
            }
            result.items.push_back(docResult);
        }
        
        log("Bulk indexed " + std::to_string(result.successCount) + " documents");
    } else {
        throw ESException("Bulk index failed: " + response.body);
    }
    
    return result;
}

// ==================== 搜索操作 ====================

SearchResult ESClient::parseSearchResponse(const json& response) {
    SearchResult result;
    result.took = response.value("took", 0);
    result.timedOut = response.value("timed_out", false);

    const auto& hits = response["hits"];
    const auto& total = hits["total"];
    result.total = total.is_object() ? total.value("value", 0) : total.get<int>();
    // 显式排序时 ES 可能返回 null 分数（未开启 track_scores）
    result.maxScore = hits.contains("max_score") && hits["max_score"].is_number()
                          ? hits["max_score"].get<double>()
                          : 0.0;

    for (const auto& hit : hits["hits"]) {
        SearchHit searchHit;
        searchHit.id = hit.value("_id", "");
        searchHit.index = hit.value("_index", "");
        searchHit.score = hit.contains("_score") && hit["_score"].is_number()
                              ? hit["_score"].get<double>()
                              : 0.0;
        searchHit.source = hit.value("_source", json::object());
        searchHit.highlight = hit.value("highlight", json::object());
        searchHit.sort = hit.value("sort", json::array());
        result.hits.push_back(searchHit);
    }

    return result;
}

SearchResult ESClient::matchSearch(const std::string& indexName,
                                   const std::string& field,
                                   const std::string& query,
                                   int from,
                                   int size) {
    json body = {
        {"query", {
            {"match", {{field, query}}}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::multiMatchSearch(const std::string& indexName,
                                        const std::vector<std::string>& fields,
                                        const std::string& query,
                                        int from,
                                        int size) {
    json body = {
        {"query", {
            {"multi_match", {
                {"query", query},
                {"fields", fields}
            }}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::termSearch(const std::string& indexName,
                                  const std::string& field,
                                  const std::string& value,
                                  int from,
                                  int size) {
    json body = {
        {"query", {
            {"term", {{field, value}}}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::boolSearch(const std::string& indexName,
                                  const json& must,
                                  const json& should,
                                  const json& mustNot,
                                  const json& filter,
                                  int from,
                                  int size) {
    json boolQuery;
    if (!must.empty()) boolQuery["must"] = must;
    if (!should.empty()) boolQuery["should"] = should;
    if (!mustNot.empty()) boolQuery["must_not"] = mustNot;
    if (!filter.empty()) boolQuery["filter"] = filter;
    
    json body = {
        {"query", {{"bool", boolQuery}}},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::searchWithHighlight(const std::string& indexName,
                                           const json& query,
                                           const std::vector<std::string>& highlightFields,
                                           int from,
                                           int size) {
    json fields;
    for (const auto& field : highlightFields) {
        fields[field] = json::object();
    }
    
    json body = {
        {"query", query},
        {"highlight", {
            {"pre_tags", {"<em>"}},
            {"post_tags", {"</em>"}},
            {"fields", fields}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::search(const std::string& indexName,
                              const json& queryBody) {
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_search"),
        queryBody.dump()
    );

    if (!response.isSuccess()) {
        throw ESException("Search failed: " + response.body);
    }

    return parseSearchResponse(json::parse(response.body));
}

// ==================== 稳定游标分页（PIT + search_after） ====================

namespace {

/**
 * 解析 ES 时间表达式为秒数（支持 "30s"、"2m"、"1h"、"1d"，纯数字按秒计）
 */
long parseKeepAliveSeconds(const std::string& keepAlive) {
    if (keepAlive.empty()) {
        return 60;
    }
    long multiplier = 1;
    std::string num = keepAlive;
    switch (keepAlive.back()) {
        case 's': multiplier = 1;     num.pop_back(); break;
        case 'm': multiplier = 60;    num.pop_back(); break;
        case 'h': multiplier = 3600;  num.pop_back(); break;
        case 'd': multiplier = 86400; num.pop_back(); break;
        default: break;  // 纯数字按秒处理
    }
    try {
        long value = std::stol(num);
        return value > 0 ? value * multiplier : 60;
    } catch (const std::exception&) {
        return 60;
    }
}

/**
 * 规范化查询：缺省为 match_all
 */
json normalizeQuery(const json& query) {
    if (query.is_null() || (query.is_object() && query.empty())) {
        return {{"match_all", json::object()}};
    }
    return query;
}

/**
 * 规范化排序：缺省按 _score 降序；确保以 _shard_doc 作为决胜排序，
 * 保证同分值记录有确定的次序（PIT 搜索专用，随快照保持稳定）。
 */
json normalizeSort(const json& sort) {
    json result;
    if (sort.is_null() || (sort.is_array() && sort.empty())) {
        result = json::array({{{"_score", "desc"}}});
    } else {
        result = sort;
    }

    bool hasTiebreaker = false;
    for (const auto& entry : result) {
        if ((entry.is_string() && entry.get<std::string>() == "_shard_doc") ||
            (entry.is_object() && entry.contains("_shard_doc"))) {
            hasTiebreaker = true;
            break;
        }
    }
    if (!hasTiebreaker) {
        result.push_back({{"_shard_doc", "asc"}});
    }
    return result;
}

} // namespace

std::string ESClient::openPointInTime(const std::string& indexName,
                                      const std::string& keepAlive) {
    log("Opening point in time on index: " + indexName);
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_pit?keep_alive=" + keepAlive), "");

    if (!response.isSuccess()) {
        throw ESException("Failed to open point in time: " + response.body);
    }

    std::string pitId = json::parse(response.body).value("id", "");
    if (pitId.empty()) {
        throw ESException("Failed to open point in time: empty pit id in response");
    }
    return pitId;
}

bool ESClient::closePointInTime(const std::string& pitId) {
    json body = {{"id", pitId}};

    HttpResponse response;
    try {
        response = httpClient_.del(buildUrl("/_pit"), body.dump());
    } catch (const HttpException& e) {
        // 关闭是尽力而为的清理动作，失败不抛出，遗留 PIT 由 keep_alive 回收
        log(std::string("Failed to close point in time: ") + e.what());
        return false;
    }

    if (response.isNotFound()) {
        // PIT 已不存在（已关闭或被回收），视为清理完成
        return false;
    }
    if (!response.isSuccess()) {
        log("Failed to close point in time: " + response.body);
        return false;
    }

    log("Point in time closed");
    return json::parse(response.body).value("succeeded", false);
}

CursorPage ESClient::searchByCursor(const CursorSearchOptions& options) {
    if (options.indexName.empty()) {
        throw ESException("Cursor search requires an index name");
    }
    if (options.pageSize <= 0) {
        throw ESException("Cursor search requires a positive page size");
    }

    // 规范化检索参数并计算指纹：游标与参数绑定，
    // 防止被拿去配合另一组索引 / 查询 / 排序 / 页大小使用
    json query = normalizeQuery(options.query);
    json sort = normalizeSort(options.sort);
    const std::string fp =
        CursorCodec::fingerprint(options.indexName, query, sort, options.pageSize);
    const long keepAliveSec = parseKeepAliveSeconds(options.keepAlive);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    std::string pitId;
    json searchAfter = json::array();

    if (options.cursor.empty()) {
        // 首次请求：建立 PIT 快照
        pitId = openPointInTime(options.indexName, options.keepAlive);
        log("Point in time opened for cursor pagination");
    } else {
        // 后续请求：解码并校验游标
        auto payload = cursorCodec_.decode(options.cursor);
        if (!payload) {
            throw CursorTamperedException(
                "Cursor is malformed or its signature does not verify");
        }
        if (payload->fingerprint != fp) {
            throw CursorTamperedException(
                "Cursor was issued for different search parameters "
                "(index / query / sort / page size do not match)");
        }
        if (now > payload->expiresAt) {
            throw CursorExpiredException(
                "Cursor has expired (keep_alive elapsed); start a new search");
        }
        pitId = payload->pitId;
        searchAfter = payload->searchAfter;
    }

    // PIT 搜索：URL 不带索引名，快照上下文在请求体中指定
    json body = {
        {"size", options.pageSize},
        {"query", query},
        {"sort", sort},
        {"pit", {{"id", pitId}, {"keep_alive", options.keepAlive}}},
        {"track_total_hits", true},
        {"track_scores", true}  // 显式排序时仍返回 _score
    };
    if (!searchAfter.empty()) {
        body["search_after"] = searchAfter;
    }

    HttpResponse response;
    try {
        response = httpClient_.post(buildUrl("/_search"), body.dump());
    } catch (...) {
        // 首次请求失败时调用方拿不到游标，立即清理刚建立的 PIT；
        // 后续请求失败则保留 PIT，调用方可持原游标重试
        if (options.cursor.empty()) {
            closePointInTime(pitId);
        }
        throw;
    }

    if (response.isNotFound()) {
        throw PitGoneException(
            "Point in time no longer exists on the server "
            "(already closed or reaped after keep_alive); start a new search");
    }
    if (!response.isSuccess()) {
        // 首次请求失败时清理刚建立的 PIT，避免泄漏
        if (options.cursor.empty()) {
            closePointInTime(pitId);
        }
        throw ESException("Cursor search failed: " + response.body);
    }

    json respJson = json::parse(response.body);
    CursorPage page;
    page.result = parseSearchResponse(respJson);

    // ES 可能轮换 PIT id，后续请求始终使用响应中最新的
    std::string latestPitId = respJson.value("pit_id", pitId);

    bool exhausted =
        page.result.hits.size() < static_cast<size_t>(options.pageSize);
    if (exhausted) {
        // 读到末页：自动关闭 PIT，本次会话结束
        closePointInTime(latestPitId);
        page.hasMore = false;
        page.nextCursor.clear();
    } else {
        CursorPayload next;
        next.pitId = latestPitId;
        next.searchAfter = page.result.hits.back().sort;
        next.fingerprint = fp;
        next.expiresAt = now + keepAliveSec;
        page.nextCursor = cursorCodec_.encode(next);
        page.hasMore = true;
    }

    return page;
}

void ESClient::closeCursor(const std::string& cursor) {
    auto payload = cursorCodec_.decode(cursor);
    if (!payload) {
        throw CursorTamperedException(
            "Cursor is malformed or its signature does not verify");
    }
    // 幂等：PIT 已不存在时 closePointInTime 返回 false，不视为错误
    closePointInTime(payload->pitId);
    log("Cursor closed by caller");
}

void ESClient::setCursorSecret(const std::string& secret) {
    cursorCodec_ = CursorCodec(secret);
}

} // namespace es
