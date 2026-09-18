/**
 * 稳定游标分页（PIT + search_after）自动化验证
 *
 * 需要可访问的 Elasticsearch（环境变量 ES_HOST / ES_PORT，默认 localhost:9200）。
 *
 * 验证内容：
 *   1. 跨多页无重无漏
 *   2. 翻页期间发生新增 / 删除，快照内结果保持稳定
 *   3. 同分值记录次序确定（两次完整翻页顺序一致）
 *   4. 主动结束后继续使用旧游标被拒绝（PitGoneException）
 *   5. 读到末页自动关闭后继续使用旧游标被拒绝（PitGoneException）
 *   6. 篡改游标被拒绝（CursorTamperedException）
 *   7. 游标配合另一组索引 / 查询 / 页大小被拒绝（CursorTamperedException）
 *   8. 过期游标被拒绝（CursorExpiredException）
 *   9. 游标编解码单元测试（往返、密钥隔离、参数指纹）
 *
 * 退出码：0 = 全部通过；1 = 存在失败用例。
 */

#include "es_client.hpp"
#include "es_cursor.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace es;
using json = nlohmann::json;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool condition, const std::string& name, const std::string& detail = "") {
    if (condition) {
        ++g_passed;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  [FAIL] " << name;
        if (!detail.empty()) {
            std::cout << " -- " << detail;
        }
        std::cout << "\n";
    }
}

void printCase(const std::string& title) {
    std::cout << "\n== " << title << " ==\n";
}

const std::string kIndex = "cursor_test_articles";
const std::string kCategory = "待审核";

json reviewQuery() {
    return {{"term", {{"category", kCategory}}}};
}

CursorSearchOptions makeOptions(int pageSize, const std::string& keepAlive = "2m") {
    CursorSearchOptions options;
    options.indexName = kIndex;
    options.query = reviewQuery();
    options.pageSize = pageSize;
    options.keepAlive = keepAlive;
    return options;
}

// 翻完整个游标会话，返回按顺序收集到的文档 id
std::vector<std::string> collectAll(ESClient& client, CursorSearchOptions options,
                                    int maxPages = 100) {
    std::vector<std::string> ids;
    for (int i = 0; i < maxPages; ++i) {
        auto page = client.searchByCursor(options);
        for (const auto& hit : page.result.hits) {
            ids.push_back(hit.id);
        }
        if (!page.hasMore) {
            return ids;
        }
        options.cursor = page.nextCursor;
    }
    throw ESException("collectAll: exceeded max pages");
}

void setupIndex(ESClient& client, int docCount) {
    if (client.indexExists(kIndex)) {
        client.deleteIndex(kIndex);
    }
    json mappings = {{"properties", {
        {"title", {{"type", "text"}}},
        {"category", {{"type", "keyword"}}},
        {"created_at", {{"type", "date"}, {"format", "yyyy-MM-dd"}}}
    }}};
    json settings = {{"number_of_shards", 1}, {"number_of_replicas", 0}};
    client.createIndex(kIndex, mappings, settings);

    std::vector<json> docs;
    std::vector<std::string> ids;
    for (int i = 1; i <= docCount; ++i) {
        docs.push_back({
            {"title", "待审核文章 " + std::to_string(i)},
            {"category", kCategory},
            {"created_at", "2023-01-01"}
        });
        ids.push_back("review-" + std::to_string(i));
    }
    auto result = client.bulkIndex(kIndex, docs, ids);
    if (result.failCount != 0) {
        throw ESException("setupIndex: bulk index had failures");
    }
    client.refreshIndex(kIndex);
}

std::set<std::string> expectedIds(int docCount) {
    std::set<std::string> ids;
    for (int i = 1; i <= docCount; ++i) {
        ids.insert("review-" + std::to_string(i));
    }
    return ids;
}

// ==================== 用例 ====================

// 1. 跨多页无重无漏
void testNoDuplicatesNoOmissions(ESClient& client) {
    printCase("用例 1：跨多页无重无漏（25 篇 / 每页 7）");
    setupIndex(client, 25);

    auto ids = collectAll(client, makeOptions(7));
    std::set<std::string> unique(ids.begin(), ids.end());

    check(ids.size() == 25, "总条数等于 25", "实际 " + std::to_string(ids.size()));
    check(unique.size() == ids.size(), "无重复记录");
    check(unique == expectedIds(25), "无遗漏记录");

    // 首页应报告准确总数（track_total_hits）
    auto first = client.searchByCursor(makeOptions(7));
    check(first.result.total == 25, "首页 total 为 25",
          "实际 " + std::to_string(first.result.total));
    client.closeCursor(first.nextCursor);
}

// 2. 翻页期间发生新增 / 删除，快照内结果稳定
void testSnapshotIsolation(ESClient& client) {
    printCase("用例 2：翻页期间新增 / 删除不影响已开始的浏览");
    setupIndex(client, 25);

    auto options = makeOptions(10);
    auto page = client.searchByCursor(options);  // 第 1 页，建立快照
    std::vector<std::string> ids;
    for (const auto& hit : page.result.hits) {
        ids.push_back(hit.id);
    }

    // 并发写入：新增 5 篇、删除 3 篇（含已读到的 review-1）
    std::vector<json> newDocs;
    std::vector<std::string> newIds;
    for (int i = 1; i <= 5; ++i) {
        newDocs.push_back({{"title", "新增文章 " + std::to_string(i)},
                           {"category", kCategory},
                           {"created_at", "2024-01-01"}});
        newIds.push_back("new-" + std::to_string(i));
    }
    client.bulkIndex(kIndex, newDocs, newIds);
    client.deleteDocument(kIndex, "review-1");
    client.deleteDocument(kIndex, "review-2");
    client.deleteDocument(kIndex, "review-3");
    client.refreshIndex(kIndex);

    // 继续翻完
    while (page.hasMore) {
        options.cursor = page.nextCursor;
        page = client.searchByCursor(options);
        for (const auto& hit : page.result.hits) {
            ids.push_back(hit.id);
        }
    }

    std::set<std::string> unique(ids.begin(), ids.end());
    check(ids.size() == 25, "快照内仍翻到 25 条", "实际 " + std::to_string(ids.size()));
    check(unique.size() == ids.size(), "无重复记录");
    check(unique == expectedIds(25), "结果集等于写入前的快照");
    check(unique.count("new-1") == 0, "新增文档在快照内不可见");
    check(unique.count("review-1") == 1, "已删除文档在快照内仍可见");
}

// 3. 同分值记录次序确定
void testDeterministicOrder(ESClient& client) {
    printCase("用例 3：同分值记录次序确定（两次翻页顺序一致）");
    setupIndex(client, 25);

    auto first = collectAll(client, makeOptions(7));
    auto second = collectAll(client, makeOptions(7));

    check(first == second, "两次完整翻页的 id 顺序完全一致");
    check(first.size() == 25, "翻页覆盖全部记录");
}

// 4. 主动结束后继续使用旧游标被拒绝
void testCursorRejectedAfterClose(ESClient& client) {
    printCase("用例 4：主动结束后旧游标被拒绝（PitGoneException）");
    setupIndex(client, 25);

    auto options = makeOptions(7);
    auto page = client.searchByCursor(options);
    std::string cursor = page.nextCursor;
    client.closeCursor(cursor);

    bool gotPitGone = false;
    try {
        options.cursor = cursor;
        client.searchByCursor(options);
    } catch (const PitGoneException&) {
        gotPitGone = true;
    } catch (const std::exception& e) {
        std::cout << "  （捕获到非预期异常: " << e.what() << "）\n";
    }
    check(gotPitGone, "关闭后继续使用游标抛出 PitGoneException");

    // 重复关闭是安全的（幂等）
    bool closeTwiceOk = true;
    try {
        client.closeCursor(cursor);
    } catch (...) {
        closeTwiceOk = false;
    }
    check(closeTwiceOk, "重复关闭同一游标不抛异常");
}

// 5. 读到末页自动关闭后，旧游标被拒绝
void testCursorRejectedAfterLastPage(ESClient& client) {
    printCase("用例 5：读到末页自动关闭后旧游标被拒绝（PitGoneException）");
    setupIndex(client, 25);

    auto options = makeOptions(7);
    std::string lastUsedCursor;
    CursorPage page;
    while (true) {
        lastUsedCursor = options.cursor;  // 本次请求使用的游标
        page = client.searchByCursor(options);
        if (!page.hasMore) {
            break;
        }
        options.cursor = page.nextCursor;
    }

    check(page.nextCursor.empty(), "末页不再返回游标");

    bool gotPitGone = false;
    try {
        auto reuse = makeOptions(7);
        reuse.cursor = lastUsedCursor;
        client.searchByCursor(reuse);
    } catch (const PitGoneException&) {
        gotPitGone = true;
    } catch (const std::exception& e) {
        std::cout << "  （捕获到非预期异常: " << e.what() << "）\n";
    }
    check(gotPitGone, "末页自动关闭后继续使用游标抛出 PitGoneException");
}

// 6. 篡改游标被拒绝
void testTamperedCursor(ESClient& client) {
    printCase("用例 6：篡改游标被拒绝（CursorTamperedException）");
    setupIndex(client, 25);

    auto options = makeOptions(7);
    auto page = client.searchByCursor(options);
    const std::string valid = page.nextCursor;

    auto expectTampered = [&](const std::string& cursor, const std::string& name) {
        bool gotTampered = false;
        try {
            auto o = makeOptions(7);
            o.cursor = cursor;
            client.searchByCursor(o);
        } catch (const CursorTamperedException&) {
            gotTampered = true;
        } catch (const std::exception& e) {
            std::cout << "  （" << name << " 捕获到非预期异常: " << e.what() << "）\n";
        }
        check(gotTampered, name);
    };

    // 改动负载区一个字符
    std::string tamperedPayload = valid;
    size_t pos = tamperedPayload.find('.') + 2;
    tamperedPayload[pos] = (tamperedPayload[pos] == 'A') ? 'B' : 'A';
    expectTampered(tamperedPayload, "改动负载内容的游标被拒绝");

    // 改动签名区一个字符
    std::string tamperedMac = valid;
    tamperedMac.back() = (tamperedMac.back() == 'A') ? 'B' : 'A';
    expectTampered(tamperedMac, "改动签名的游标被拒绝");

    // 完全非法的字符串
    expectTampered("not-a-cursor", "非法字符串被拒绝");
    expectTampered("esc1..", "残缺游标被拒绝");

    // 空游标表示首次请求（会建立新快照并返回第一页）
    bool emptyStartsNew = false;
    try {
        auto o = makeOptions(7);
        o.cursor.clear();
        auto p = client.searchByCursor(o);
        emptyStartsNew = !p.result.hits.empty();
        client.closeCursor(p.nextCursor);
    } catch (...) {
    }
    check(emptyStartsNew, "空游标视为首次请求");

    client.closeCursor(valid);
}

// 7. 游标配合另一组索引 / 查询 / 页大小被拒绝
void testParameterMismatch(ESClient& client) {
    printCase("用例 7：游标与检索参数绑定（CursorTamperedException）");
    setupIndex(client, 25);

    auto page = client.searchByCursor(makeOptions(7));
    const std::string cursor = page.nextCursor;

    auto expectTampered = [&](CursorSearchOptions o, const std::string& name) {
        o.cursor = cursor;
        bool gotTampered = false;
        try {
            client.searchByCursor(o);
        } catch (const CursorTamperedException&) {
            gotTampered = true;
        } catch (const std::exception& e) {
            std::cout << "  （" << name << " 捕获到非预期异常: " << e.what() << "）\n";
        }
        check(gotTampered, name);
    };

    auto otherPageSize = makeOptions(10);
    expectTampered(otherPageSize, "换用不同页大小被拒绝");

    auto otherQuery = makeOptions(7);
    otherQuery.query = {{"match_all", json::object()}};
    expectTampered(otherQuery, "换用不同查询被拒绝");

    auto otherIndex = makeOptions(7);
    otherIndex.indexName = "cursor_test_other";
    expectTampered(otherIndex, "换用不同索引被拒绝");

    auto otherSort = makeOptions(7);
    otherSort.sort = json::array({{{"created_at", "desc"}}});
    expectTampered(otherSort, "换用不同排序被拒绝");

    client.closeCursor(cursor);
}

// 8. 过期游标被拒绝
void testExpiredCursor(ESClient& client) {
    printCase("用例 8：过期游标被拒绝（CursorExpiredException）");
    setupIndex(client, 25);

    auto options = makeOptions(7, "1s");
    auto page = client.searchByCursor(options);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    bool gotExpired = false;
    try {
        options.cursor = page.nextCursor;
        client.searchByCursor(options);
    } catch (const CursorExpiredException&) {
        gotExpired = true;
    } catch (const std::exception& e) {
        std::cout << "  （捕获到非预期异常: " << e.what() << "）\n";
    }
    check(gotExpired, "超过 keep_alive 的游标抛出 CursorExpiredException");
}

// 9. 游标编解码单元测试（不依赖 ES）
void testCursorCodec() {
    printCase("用例 9：游标编解码单元测试");

    CursorCodec codec(CursorCodec::generateSecret());

    CursorPayload payload;
    payload.pitId = "fake-pit-id";
    payload.searchAfter = json::array({1.0, 42});
    payload.fingerprint = CursorCodec::fingerprint("idx", reviewQuery(),
                                                   json::array({{{"_score", "desc"}}}), 7);
    payload.expiresAt = 1893456000;

    // 往返一致
    auto decoded = codec.decode(codec.encode(payload));
    check(decoded.has_value(), "编码后可解码");
    if (decoded) {
        check(decoded->pitId == payload.pitId &&
              decoded->searchAfter == payload.searchAfter &&
              decoded->fingerprint == payload.fingerprint &&
              decoded->expiresAt == payload.expiresAt,
              "往返后负载内容一致");
    }

    // 其他密钥无法验签
    CursorCodec other(CursorCodec::generateSecret());
    check(!other.decode(codec.encode(payload)).has_value(),
          "不同密钥签发的游标无法通过校验");

    // 参数指纹：同参相同，异参不同
    auto fp1 = CursorCodec::fingerprint("idx", reviewQuery(), json::array(), 7);
    auto fp2 = CursorCodec::fingerprint("idx", reviewQuery(), json::array(), 7);
    auto fp3 = CursorCodec::fingerprint("idx", reviewQuery(), json::array(), 8);
    check(fp1 == fp2, "相同参数指纹一致");
    check(fp1 != fp3, "不同页大小指纹不同");

    // 密钥为空时构造失败
    bool emptyRejected = false;
    try {
        CursorCodec bad("");
    } catch (const std::invalid_argument&) {
        emptyRejected = true;
    }
    check(emptyRejected, "空密钥构造被拒绝");
}

} // namespace

int main() {
    const char* esHost = std::getenv("ES_HOST");
    const char* esPort = std::getenv("ES_PORT");
    std::string host = esHost ? esHost : "localhost";
    int port = esPort ? std::stoi(esPort) : 9200;

    std::cout << "游标分页自动化验证，连接 Elasticsearch " << host << ":" << port << "\n";

    ESClient client(host, port);

    int retries = 30;
    while (!client.ping() && retries-- > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (retries < 0) {
        std::cerr << "无法连接到 Elasticsearch\n";
        return 1;
    }

    int exitCode = 0;
    try {
        testCursorCodec();  // 不依赖 ES 的单元测试先行

        testNoDuplicatesNoOmissions(client);
        testSnapshotIsolation(client);
        testDeterministicOrder(client);
        testCursorRejectedAfterClose(client);
        testCursorRejectedAfterLastPage(client);
        testTamperedCursor(client);
        testParameterMismatch(client);
        testExpiredCursor(client);
    } catch (const std::exception& e) {
        std::cerr << "\n测试执行中断: " << e.what() << "\n";
        exitCode = 1;
    }

    // 清理测试索引
    try {
        if (client.indexExists(kIndex)) {
            client.deleteIndex(kIndex);
        }
    } catch (...) {
        // 尽力清理
    }

    std::cout << "\n========================================\n";
    std::cout << "通过 " << g_passed << " 项，失败 " << g_failed << " 项\n";
    std::cout << "========================================\n";

    return (exitCode == 0 && g_failed == 0) ? 0 : 1;
}
