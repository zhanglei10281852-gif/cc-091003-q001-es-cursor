#include "es_client.hpp"
#include "sha256.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <random>

namespace es {

// ==================== 构造与析构 ====================

ESClient::ESClient(const std::string& host, int port) {
    std::ostringstream oss;
    oss << "http://" << host << ":" << port;
    baseUrl_ = oss.str();
    httpClient_.setTimeout(30);
    httpClient_.setConnectTimeout(10);

    // 默认生成随机游标签名密钥：进程重启后旧游标自动失效。
    // 需要跨进程/重启使用游标时，可通过 setCursorSecret() 设置稳定密钥。
    std::random_device rd;
    std::ostringstream secret;
    for (int i = 0; i < 8; ++i) {
        secret << std::hex << std::setfill('0') << std::setw(8) << rd();
    }
    cursorSecret_ = secret.str();
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
    // 显式指定 sort 时 ES 会返回 "max_score": null
    result.maxScore = (hits.contains("max_score") && hits["max_score"].is_number())
        ? hits["max_score"].get<double>() : 0.0;

    for (const auto& hit : hits["hits"]) {
        SearchHit searchHit;
        searchHit.id = hit.value("_id", "");
        searchHit.index = hit.value("_index", "");
        // 显式指定 sort 且不含 _score 时 ES 会返回 "_score": null
        searchHit.score = (hit.contains("_score") && hit["_score"].is_number())
            ? hit["_score"].get<double>() : 0.0;
        searchHit.source = hit.value("_source", json::object());
        searchHit.highlight = hit.value("highlight", json::object());
        searchHit.sortValues = hit.value("sort", json::array());
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

// ==================== 游标分页（PIT + search_after） ====================

void ESClient::setCursorSecret(const std::string& secret) {
    if (secret.empty()) {
        throw ESException("Cursor secret must not be empty");
    }
    cursorSecret_ = secret;
}

std::string ESClient::openPointInTime(const std::string& indexName,
                                      const std::string& keepAlive) {
    log("Opening point in time on index: " + indexName);
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_pit?keep_alive=" + keepAlive), "");

    if (!response.isSuccess()) {
        throw ESException("Failed to open point in time: " + response.body);
    }

    auto respJson = json::parse(response.body);
    std::string pitId = respJson.value("id", "");
    if (pitId.empty()) {
        throw ESException("Failed to open point in time: missing id in response");
    }
    log("Point in time opened");
    return pitId;
}

bool ESClient::closePointInTime(const std::string& pitId) {
    log("Closing point in time");
    json body = {{"id", pitId}};
    auto response = httpClient_.del(buildUrl("/_pit"), body.dump());

    if (response.isNotFound()) {
        // PIT 已不存在（被关闭或因超过 keep_alive 被回收）
        return false;
    }
    if (!response.isSuccess()) {
        throw ESException("Failed to close point in time: " + response.body);
    }

    auto respJson = json::parse(response.body);
    return respJson.value("succeeded", false);
}

json ESClient::effectiveQuery(const CursorSearchRequest& request) const {
    if (request.query.empty()) {
        return {{"match_all", json::object()}};
    }
    return request.query;
}

json ESClient::effectiveSort(const CursorSearchRequest& request) const {
    json sort = request.sort.empty()
        ? json::array({{{"_score", "desc"}}})
        : request.sort;

    // 追加 _shard_doc 作为决胜键：PIT 生命周期内它对每条文档固定不变，
    // 保证同分值（同排序键）记录有确定且稳定的先后次序。
    bool hasTiebreaker = false;
    for (const auto& entry : sort) {
        if (entry.is_object() && entry.contains("_shard_doc")) {
            hasTiebreaker = true;
            break;
        }
    }
    if (!hasTiebreaker) {
        sort.push_back({{"_shard_doc", "asc"}});
    }
    return sort;
}

std::string ESClient::encodeCursor(const json& payload) const {
    std::string body = payload.dump();
    std::string sig = hmacSha256Hex(cursorSecret_, body);
    return "v1." + base64UrlEncode(body) + "." + sig;
}

json ESClient::decodeCursor(const std::string& cursor) const {
    // 格式：v1.<base64url(payload)>.<hex(hmac-sha256)>
    auto firstDot = cursor.find('.');
    auto lastDot = cursor.rfind('.');
    if (firstDot == std::string::npos || firstDot == lastDot ||
        cursor.substr(0, firstDot) != "v1") {
        throw CursorTamperedException("unexpected cursor format");
    }

    std::string payloadB64 = cursor.substr(firstDot + 1, lastDot - firstDot - 1);
    std::string sig = cursor.substr(lastDot + 1);

    std::string body;
    if (!base64UrlDecode(payloadB64, body)) {
        throw CursorTamperedException("payload is not valid base64url");
    }

    // 对解码后的原始字节重新计算签名并做常量时间比对
    std::string expected = hmacSha256Hex(cursorSecret_, body);
    if (!constantTimeEqual(expected, sig)) {
        throw CursorTamperedException("signature mismatch");
    }

    json payload;
    try {
        payload = json::parse(body);
    } catch (const json::exception&) {
        throw CursorTamperedException("payload is not valid JSON");
    }

    // 必要字段与类型检查
    if (!payload.is_object() ||
        payload.value("v", 0) != 1 ||
        !payload.contains("idx") || !payload["idx"].is_string() ||
        !payload.contains("qh") || !payload["qh"].is_string() ||
        !payload.contains("sh") || !payload["sh"].is_string() ||
        !payload.contains("sz") || !payload["sz"].is_number_integer() ||
        !payload.contains("pit") || !payload["pit"].is_string() ||
        !payload.contains("sa") ||
        !payload.contains("exp") || !payload["exp"].is_number_integer()) {
        throw CursorTamperedException("payload misses required fields");
    }
    return payload;
}

bool ESClient::isPitMissing(const HttpResponse& response) const {
    if (response.statusCode != 404) {
        return false;
    }
    try {
        auto err = json::parse(response.body);
        std::string type = err.value("error", json::object()).value("type", "");
        return type == "search_context_missing_exception";
    } catch (const json::exception&) {
        return false;
    }
}

CursorPage ESClient::runCursorSearch(const CursorSearchRequest& request,
                                     const std::string& pitId,
                                     const json& searchAfter) {
    // 注意：使用 PIT 时索引由 PIT 绑定，请求路径不能再带索引名
    json body = {
        {"size", request.pageSize},
        {"query", effectiveQuery(request)},
        {"sort", effectiveSort(request)},
        {"pit", {{"id", pitId}, {"keep_alive", request.keepAlive}}},
        {"track_total_hits", true}
    };
    if (!searchAfter.is_null()) {
        body["search_after"] = searchAfter;
    }

    auto response = httpClient_.post(buildUrl("/_search"), body.dump());

    if (isPitMissing(response)) {
        throw PitNotFoundException("PIT was closed or reclaimed by Elasticsearch");
    }
    if (!response.isSuccess()) {
        throw ESException("Cursor search failed: " + response.body);
    }

    auto respJson = json::parse(response.body);
    CursorPage page;
    page.result = parseSearchResponse(respJson);

    // ES 可能轮换 PIT id，后续请求应始终使用最新返回的 id
    std::string currentPit = pitId;
    if (respJson.contains("pit_id") && respJson["pit_id"].is_string()) {
        currentPit = respJson["pit_id"].get<std::string>();
    }

    bool lastPage = static_cast<int>(page.result.hits.size()) < request.pageSize;
    if (lastPage) {
        // 已读到末页：立即关闭 PIT，不再占用集群资源。
        // 关闭失败不影响本页数据返回：遗留 PIT 会在 keep_alive 超时后被 ES 回收。
        page.hasMore = false;
        page.nextCursor = "";
        try {
            closePointInTime(currentPit);
        } catch (const std::exception& e) {
            log(std::string("Failed to close point in time at last page: ") + e.what());
        }
        return page;
    }

    // 还有下一页：用本页最后一条命中的排序值构造新游标
    page.hasMore = true;
    json payload = {
        {"v", 1},
        {"idx", request.index},
        {"qh", Sha256::hex(effectiveQuery(request).dump())},
        {"sh", Sha256::hex(effectiveSort(request).dump())},
        {"sz", request.pageSize},
        {"pit", currentPit},
        {"sa", page.result.hits.back().sortValues},
        {"exp", std::time(nullptr) + request.cursorTtlSeconds}
    };
    page.nextCursor = encodeCursor(payload);
    return page;
}

CursorPage ESClient::cursorSearchFirst(const CursorSearchRequest& request) {
    if (request.index.empty()) {
        throw ESException("Cursor search requires an index name");
    }
    if (request.pageSize <= 0) {
        throw ESException("Cursor search requires pageSize > 0");
    }

    std::string pitId = openPointInTime(request.index, request.keepAlive);
    try {
        return runCursorSearch(request, pitId, json());
    } catch (...) {
        // 首页失败时 PIT 尚未交给调用方，尽力关闭避免泄漏；
        // 即使关闭失败，遗留 PIT 也会在 keep_alive 超时后被 ES 回收。
        try {
            closePointInTime(pitId);
        } catch (const std::exception&) {
            // 不掩盖原始异常
        }
        throw;
    }
}

CursorPage ESClient::cursorSearchNext(const CursorSearchRequest& request,
                                      const std::string& cursor) {
    json payload = decodeCursor(cursor);

    // 客户端侧有效期
    if (std::time(nullptr) > payload["exp"].get<long long>()) {
        throw CursorExpiredException(
            "cursor is older than " + std::to_string(request.cursorTtlSeconds) + "s");
    }

    // 绑定校验：游标只能配合签发时的索引、查询、排序和页大小使用
    if (payload["idx"].get<std::string>() != request.index) {
        throw CursorMismatchException("index differs from the one the cursor was issued for");
    }
    if (payload["qh"].get<std::string>() != Sha256::hex(effectiveQuery(request).dump())) {
        throw CursorMismatchException("query differs from the one the cursor was issued for");
    }
    if (payload["sh"].get<std::string>() != Sha256::hex(effectiveSort(request).dump())) {
        throw CursorMismatchException("sort differs from the one the cursor was issued for");
    }
    if (payload["sz"].get<int>() != request.pageSize) {
        throw CursorMismatchException("page size differs from the one the cursor was issued for");
    }

    return runCursorSearch(request, payload["pit"].get<std::string>(), payload["sa"]);
}

bool ESClient::closeCursor(const std::string& cursor) {
    // 只校验完整性（防篡改），刻意不校验有效期与绑定：
    // 过期游标持有的 PIT 同样需要被关闭。
    json payload = decodeCursor(cursor);
    return closePointInTime(payload["pit"].get<std::string>());
}

} // namespace es
