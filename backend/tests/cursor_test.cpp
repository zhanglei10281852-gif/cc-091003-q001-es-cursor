/**
 * 游标分页（PIT + search_after）自动化验证
 *
 * 覆盖：
 *   1. SHA-256 / HMAC / base64url 基础算法正确性（标准测试向量）
 *   2. 跨多页翻页无重复、无遗漏
 *   3. 翻页期间新增/删除文档不影响已开始的浏览（快照稳定性）
 *   4. 同分值记录次序确定（多次遍历顺序完全一致）
 *   5. 游标与索引/查询/排序/页大小绑定，换用即被拒绝
 *   6. 篡改游标被拒绝
 *   7. 过期游标被拒绝
 *   8. 读到末页自动关闭 PIT，继续使用旧游标被拒绝
 *   9. 主动 closeCursor 后继续使用旧游标被拒绝
 *  10. 空结果集首页即末页
 *
 * 运行方式：
 *   ES_HOST=localhost ES_PORT=9200 ./es_cursor_test
 * 退出码为 0 表示全部通过。
 */
#include "es_client.hpp"
#include "sha256.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace es;
using json = nlohmann::json;

// ==================== 极简测试框架 ====================

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& name) {
    ++g_checks;
    if (cond) {
        std::cout << "  [ok] " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

template <typename Ex, typename Fn>
static bool throwsType(Fn&& fn) {
    try {
        fn();
    } catch (const Ex&) {
        return true;
    } catch (const std::exception& e) {
        std::cout << "    (unexpected exception type: " << e.what() << ")\n";
        return false;
    }
    return false;
}

static void section(const std::string& title) {
    std::cout << "\n== " << title << " ==\n";
}

// ==================== 基础算法测试 ====================

static void testCrypto() {
    section("SHA-256 / HMAC / base64url");

    check(Sha256::hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 empty string");
    check(Sha256::hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 \"abc\"");
    check(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 56-byte message");

    // RFC 4231 Test Case 1
    check(hmacSha256Hex(std::string(20, '\x0b'), "Hi There") ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "HMAC-SHA256 RFC4231 case 1");
    // RFC 4231 Test Case 2
    check(hmacSha256Hex("Jefe", "what do ya want for nothing?") ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "HMAC-SHA256 RFC4231 case 2");

    std::string bin;
    for (int i = 0; i < 256; ++i) bin += static_cast<char>(i);
    std::string decoded;
    check(base64UrlDecode(base64UrlEncode(bin), decoded) && decoded == bin,
          "base64url binary round-trip");
    check(base64UrlEncode("hello?") == "aGVsbG8_", "base64url alphabet (-_)");
    std::string tmp;
    check(!base64UrlDecode("a", tmp), "base64url rejects length % 4 == 1");
    check(!base64UrlDecode("aGVs$G8=", tmp), "base64url rejects invalid chars");
    check(constantTimeEqual("abc", "abc") && !constantTimeEqual("abc", "abd") &&
              !constantTimeEqual("abc", "abcd"),
          "constant-time compare");
}

// ==================== 集成测试数据 ====================

static const std::string kIndex = "cursor_test_articles";
static const int kDocCount = 37;
static const int kPageSize = 7;

static json testDoc(int i) {
    char day[8];
    std::snprintf(day, sizeof(day), "%02d", i % 28 + 1);
    return {
        {"title", "审核文章 " + std::to_string(i)},
        {"content", "内容审核团队的历史文章正文 " + std::to_string(i)},
        {"category", (i % 2 == 0) ? "技术" : "资讯"},
        {"views", i % 5},  // 大量相同分值，考验决胜键
        {"created_at", std::string("2024-01-") + day}
    };
}

static std::string docId(int i) {
    return "doc-" + std::to_string(i);
}

static void setupIndex(ESClient& client) {
    if (client.indexExists(kIndex)) {
        client.deleteIndex(kIndex);
    }
    json mappings = {
        {"properties", {
            {"title", {{"type", "text"}}},
            {"content", {{"type", "text"}}},
            {"category", {{"type", "keyword"}}},
            {"views", {{"type", "integer"}}},
            {"created_at", {{"type", "date"}, {"format", "yyyy-MM-dd"}}}
        }}
    };
    json settings = {{"number_of_shards", 1}, {"number_of_replicas", 0}};
    client.createIndex(kIndex, mappings, settings);

    std::vector<json> docs;
    std::vector<std::string> ids;
    for (int i = 1; i <= kDocCount; ++i) {
        docs.push_back(testDoc(i));
        ids.push_back(docId(i));
    }
    auto result = client.bulkIndex(kIndex, docs, ids);
    if (result.successCount != kDocCount) {
        throw std::runtime_error("bulk index failed");
    }
    client.refreshIndex(kIndex);
}

static CursorSearchRequest makeRequest() {
    CursorSearchRequest req;
    req.index = kIndex;
    req.sort = json::array({{{"views", "asc"}}});
    req.pageSize = kPageSize;
    req.keepAlive = "2m";
    req.cursorTtlSeconds = 600;
    return req;
}

/** 完整遍历一次游标分页，返回按顺序收集的文档 id。 */
static std::vector<std::string> walkAllPages(ESClient& client,
                                             const CursorSearchRequest& req,
                                             std::vector<CursorPage>* pages = nullptr) {
    std::vector<std::string> ids;
    CursorPage page = client.cursorSearchFirst(req);
    while (true) {
        for (const auto& hit : page.result.hits) {
            ids.push_back(hit.id);
        }
        if (pages) pages->push_back(page);
        if (!page.hasMore) break;
        page = client.cursorSearchNext(req, page.nextCursor);
    }
    return ids;
}

// ==================== 集成测试 ====================

static void testFullTraversal(ESClient& client) {
    section("跨多页翻页：无重复、无遗漏");

    auto req = makeRequest();
    std::vector<CursorPage> pages;
    auto ids = walkAllPages(client, req, &pages);

    check(ids.size() == static_cast<size_t>(kDocCount), "总条数等于文档数");
    check(std::set<std::string>(ids.begin(), ids.end()).size() == ids.size(),
          "无重复记录");
    std::set<std::string> expected;
    for (int i = 1; i <= kDocCount; ++i) expected.insert(docId(i));
    check(std::set<std::string>(ids.begin(), ids.end()) == expected, "无遗漏记录");

    // 页数与每页大小：5 页满页 + 1 页 2 条
    check(pages.size() == 6, "页数正确（37 / 7 -> 6 页）");
    bool sizesOk = true;
    for (size_t p = 0; p + 1 < pages.size(); ++p) {
        sizesOk = sizesOk && pages[p].result.hits.size() == static_cast<size_t>(kPageSize);
    }
    sizesOk = sizesOk && pages.back().result.hits.size() == 2;
    check(sizesOk, "每页大小符合预期");
    check(!pages.back().hasMore && pages.back().nextCursor.empty(),
          "末页 hasMore=false 且游标为空");
    check(pages.front().result.total == kDocCount, "total 统计准确");

    // 排序：views 单调不减（决胜键次序在确定性用例中验证）
    bool sorted = true;
    for (size_t p = 0; p < pages.size(); ++p) {
        for (size_t i = 1; i < pages[p].result.hits.size(); ++i) {
            int prev = pages[p].result.hits[i - 1].source["views"].get<int>();
            int cur = pages[p].result.hits[i].source["views"].get<int>();
            if (prev > cur) sorted = false;
        }
    }
    check(sorted, "按 views 升序");
}

static void testSnapshotStability(ESClient& client) {
    section("快照稳定性：翻页期间新增/删除不影响已开始的浏览");

    auto req = makeRequest();
    CursorPage page = client.cursorSearchFirst(req);
    check(page.result.total == kDocCount, "首页 total 为快照时刻的文档数");

    std::set<std::string> seen;
    for (const auto& hit : page.result.hits) seen.insert(hit.id);

    // 翻页期间：新增 5 篇、删除 2 篇（选未出现在首页的）
    std::vector<json> newDocs;
    std::vector<std::string> newIds;
    for (int i = 1; i <= 5; ++i) {
        newDocs.push_back(testDoc(100 + i));
        newIds.push_back("new-" + std::to_string(i));
    }
    client.bulkIndex(kIndex, newDocs, newIds);
    std::vector<std::string> deleted;
    for (int i = 1; i <= kDocCount && deleted.size() < 2; ++i) {
        if (seen.count(docId(i)) == 0) {
            client.deleteDocument(kIndex, docId(i));
            deleted.push_back(docId(i));
        }
    }
    client.refreshIndex(kIndex);

    // 继续遍历剩余页
    std::vector<std::string> rest;
    while (page.hasMore) {
        page = client.cursorSearchNext(req, page.nextCursor);
        for (const auto& hit : page.result.hits) rest.push_back(hit.id);
    }

    std::set<std::string> all(seen.begin(), seen.end());
    all.insert(rest.begin(), rest.end());
    check(all.size() == static_cast<size_t>(kDocCount), "快照内仍是无重无漏的 37 条");
    bool hasNew = false;
    for (const auto& id : all) hasNew = hasNew || id.rfind("new-", 0) == 0;
    check(!hasNew, "新增文档不出现在已开始的浏览中");
    check(all.count(deleted[0]) == 1 && all.count(deleted[1]) == 1,
          "翻页期间删除的文档在快照内仍可见");
}

static void testDeterministicOrder(ESClient& client) {
    section("同分值记录次序确定");

    auto req = makeRequest();
    auto run1 = walkAllPages(client, req);
    auto run2 = walkAllPages(client, req);
    check(run1 == run2, "两次完整遍历的 id 序列完全一致");
}

static void testCursorBinding(ESClient& client) {
    section("游标绑定：换索引/查询/排序/页大小即被拒绝");

    auto req = makeRequest();
    auto page = client.cursorSearchFirst(req);
    std::string cursor = page.nextCursor;
    check(!cursor.empty(), "获得有效游标");

    auto badSize = req;
    badSize.pageSize = kPageSize + 1;
    check(throwsType<CursorMismatchException>([&] { client.cursorSearchNext(badSize, cursor); }),
          "不同页大小被拒绝");

    auto badQuery = req;
    badQuery.query = {{"term", {{"category", "技术"}}}};
    check(throwsType<CursorMismatchException>([&] { client.cursorSearchNext(badQuery, cursor); }),
          "不同查询被拒绝");

    auto badSort = req;
    badSort.sort = json::array({{{"views", "desc"}}});
    check(throwsType<CursorMismatchException>([&] { client.cursorSearchNext(badSort, cursor); }),
          "不同排序被拒绝");

    auto badIndex = req;
    badIndex.index = "other_index";
    check(throwsType<CursorMismatchException>([&] { client.cursorSearchNext(badIndex, cursor); }),
          "不同索引被拒绝");

    // 同参数仍可正常翻页（对照组），随后主动结束
    bool sameParamsOk = true;
    try {
        auto p = client.cursorSearchNext(req, cursor);
        sameParamsOk = !p.result.hits.empty();
    } catch (const std::exception&) {
        sameParamsOk = false;
    }
    check(sameParamsOk, "相同参数可正常翻页（对照）");
    check(client.closeCursor(cursor), "用例结束主动关闭 PIT");
}

static void testCursorTampering(ESClient& client) {
    section("篡改游标被拒绝");

    auto req = makeRequest();
    auto page = client.cursorSearchFirst(req);
    std::string cursor = page.nextCursor;

    check(throwsType<CursorTamperedException>([&] { client.cursorSearchNext(req, "garbage"); }),
          "无格式字符串被拒绝");
    check(throwsType<CursorTamperedException>([&] { client.cursorSearchNext(req, ""); }),
          "空游标被拒绝");
    check(throwsType<CursorTamperedException>([&] { client.cursorSearchNext(req, "v1.!!!.abc"); }),
          "非法 base64 被拒绝");

    // 篡改负载区一个字符
    auto dot1 = cursor.find('.');
    auto dot2 = cursor.rfind('.');
    std::string badPayload = cursor;
    size_t mid = dot1 + 2;
    badPayload[mid] = (badPayload[mid] == 'A') ? 'B' : 'A';
    check(throwsType<CursorTamperedException>([&] { client.cursorSearchNext(req, badPayload); }),
          "篡改负载被拒绝");

    // 篡改签名区一个字符
    std::string badSig = cursor;
    badSig[dot2 + 1] = (badSig[dot2 + 1] == '0') ? '1' : '0';
    check(throwsType<CursorTamperedException>([&] { client.cursorSearchNext(req, badSig); }),
          "篡改签名被拒绝");

    // 篡改后的游标也不能用于关闭 PIT
    check(throwsType<CursorTamperedException>([&] { client.closeCursor(badPayload); }),
          "篡改游标不能用于 closeCursor");

    check(client.closeCursor(cursor), "原始游标仍可正常关闭 PIT");
}

static void testCursorExpiry(ESClient& client) {
    section("过期游标被拒绝");

    auto req = makeRequest();
    req.cursorTtlSeconds = 1;
    auto page = client.cursorSearchFirst(req);
    std::string cursor = page.nextCursor;

    std::this_thread::sleep_for(std::chrono::seconds(2));
    check(throwsType<CursorExpiredException>([&] { client.cursorSearchNext(req, cursor); }),
          "过期游标被拒绝");

    // 过期不等于泄漏：过期游标持有的 PIT 仍可被关闭
    check(client.closeCursor(cursor), "过期游标仍可关闭其 PIT");
}

static void testReuseAfterFinish(ESClient& client) {
    section("读到末页后继续使用旧游标被拒绝");

    auto req = makeRequest();
    CursorPage page = client.cursorSearchFirst(req);
    std::string lastCursor;
    while (page.hasMore) {
        lastCursor = page.nextCursor;
        page = client.cursorSearchNext(req, lastCursor);
    }
    check(!page.hasMore, "已读到末页（PIT 已自动关闭）");

    // 末页已由客户端自动关闭 PIT，旧游标在 ES 侧失效
    check(throwsType<PitNotFoundException>([&] { client.cursorSearchNext(req, lastCursor); }),
          "末页后重用旧游标被识别为 PIT 已释放");
}

static void testExplicitClose(ESClient& client) {
    section("主动结束后继续使用旧游标被拒绝");

    auto req = makeRequest();
    auto page = client.cursorSearchFirst(req);
    std::string cursor = page.nextCursor;

    check(client.closeCursor(cursor), "主动关闭返回 true");
    check(throwsType<PitNotFoundException>([&] { client.cursorSearchNext(req, cursor); }),
          "关闭后继续使用旧游标被拒绝");
    check(!client.closeCursor(cursor), "重复关闭返回 false（PIT 已不存在）");
}

static void testEmptyFirstPage(ESClient& client) {
    section("空结果集：首页即末页");

    auto req = makeRequest();
    req.query = {{"term", {{"category", "不存在的分类"}}}};
    auto page = client.cursorSearchFirst(req);
    check(page.result.hits.empty() && !page.hasMore && page.nextCursor.empty(),
          "空结果集首页即末页");
}

// ==================== 主函数 ====================

int main() {
    const char* esHost = std::getenv("ES_HOST");
    const char* esPort = std::getenv("ES_PORT");
    std::string host = esHost ? esHost : "localhost";
    int port = esPort ? std::stoi(esPort) : 9200;

    std::cout << "游标分页自动化验证 (ES: " << host << ":" << port << ")\n";

    testCrypto();

    ESClient client(host, port);
    client.setCursorSecret("cursor-test-secret");  // 固定密钥，保证用例可重复

    std::cout << "等待 Elasticsearch 就绪";
    int retries = 30;
    while (!client.ping() && retries > 0) {
        std::cout << "." << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        --retries;
    }
    std::cout << "\n";
    if (retries == 0) {
        std::cerr << "无法连接到 Elasticsearch\n";
        return 1;
    }

    try {
        setupIndex(client);
        testFullTraversal(client);
        testSnapshotStability(client);
        testDeterministicOrder(client);
        testCursorBinding(client);
        testCursorTampering(client);
        testCursorExpiry(client);
        testReuseAfterFinish(client);
        testExplicitClose(client);
        testEmptyFirstPage(client);
        client.deleteIndex(kIndex);
    } catch (const std::exception& e) {
        std::cerr << "测试过程异常: " << e.what() << "\n";
        ++g_failures;
    }

    std::cout << "\n========================================\n";
    std::cout << "共 " << g_checks << " 项检查，失败 " << g_failures << " 项\n";
    std::cout << (g_failures == 0 ? "全部通过 ✓" : "存在失败 ✗") << "\n";
    return g_failures == 0 ? 0 : 1;
}
