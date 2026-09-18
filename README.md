# Elasticsearch 全文检索 C++ 示例

基于 C++17 实现的 Elasticsearch 全文检索演示项目，展示如何使用 C++ 与 Elasticsearch 进行交互，实现索引管理、文档 CRUD 和全文检索功能。

## 运行方式

### 方式一：Docker Compose（推荐）

```bash
# 1. 启动所有服务
docker-compose up --build -d

# 2. 查看 C++ 演示程序输出
docker logs es-demo-cpp

# 3. 停止服务
docker-compose down

# 4. （可选）启用 Kibana 可视化界面
docker-compose --profile kibana up -d
```

### 方式二：本地编译运行

需要先安装依赖：libcurl-dev

```bash
# Ubuntu/Debian
sudo apt-get install libcurl4-openssl-dev

# macOS
brew install curl

# 1. 启动 Elasticsearch
docker-compose up -d elasticsearch

# 2. 编译 C++ 项目（CMake 会自动下载 nlohmann/json）
cd backend
mkdir build && cd build
cmake ..
make

# 3. 运行程序
./es_demo
```

## 服务说明

| 服务          | 端口 | 说明                       |
| ------------- | ---- | -------------------------- |
| Elasticsearch | 9200 | 搜索引擎服务               |
| Kibana        | 5601 | ES 可视化管理（可选）      |
| cpp-demo      | -    | C++ 演示程序（一次性运行） |

### 访问地址

- Elasticsearch: http://localhost:9200
- Kibana: http://localhost:5601 （需使用 `--profile kibana` 启动）

## 认证说明

本项目为开发演示环境，已禁用安全认证（`xpack.security.enabled=false`），无需用户名密码即可访问。

> ⚠️ 生产环境请务必启用安全认证。

## 功能特性

### 索引管理

- ✅ 创建索引（支持自定义 mapping）
- ✅ 删除索引
- ✅ 查看索引信息

### 文档操作

- ✅ 添加文档
- ✅ 批量添加文档
- ✅ 获取文档
- ✅ 更新文档
- ✅ 删除文档

### 全文检索

- ✅ Match 查询（分词匹配）
- ✅ Multi-Match 查询（多字段搜索）
- ✅ Term 查询（精确匹配）
- ✅ Bool 组合查询
- ✅ 高亮显示
- ✅ 分页查询（from/size）

### 游标分页（长列表稳定翻页）

面向内容审核等需要逐页遍历数万条记录的场景，基于 **Point in Time + search_after** 实现：

- ✅ 首次请求建立 PIT 快照，返回结果与不透明的下一页游标
- ✅ 翻页期间新增/删除文档不影响已开始的浏览（快照隔离）
- ✅ 自动追加 `_shard_doc` 决胜键，同分值记录次序确定
- ✅ 游标经 HMAC-SHA256 签名，与索引/查询/排序/页大小绑定，篡改或换用即被拒绝
- ✅ 可区分的失败类型：篡改 / 过期 / 条件不匹配 / PIT 已释放
- ✅ 读到末页自动关闭 PIT，支持主动结束；异常退出后 ES 按 keep_alive 自行回收

详见下方「游标分页」章节。

### 分词说明

本 Demo 使用 Elasticsearch 内置的 `standard` 分词器。`standard` 分词器对中文采用单字切分（Unigram），例如"人工智能"会被切分为"人"、"工"、"智"、"能"四个 token。

如需真正的中文词语切分（如将"人工智能"作为一个完整词语），需要：

1. 安装 [IK 分词器插件](https://github.com/medcl/elasticsearch-analysis-ik)
2. 修改索引 mapping 中的 `analyzer` 为 `ik_max_word`（最细粒度）或 `ik_smart`（智能切分）

示例配置见下方"扩展开发"章节。

## 技术栈

- **语言**: C++17
- **HTTP 客户端**: libcurl
- **JSON 处理**: nlohmann/json（CMake 自动下载）
- **搜索引擎**: Elasticsearch 8.11.0
- **构建工具**: CMake 3.16+
- **容器化**: Docker & Docker Compose

## 项目结构

```
.
├── backend/                 # C++ 后端代码
│   ├── CMakeLists.txt      # CMake 构建配置
│   ├── Dockerfile          # Docker 镜像构建
│   ├── include/            # 头文件
│   │   ├── es_client.hpp   # ES 客户端类（含游标分页 API）
│   │   ├── http_client.hpp # HTTP 客户端类
│   │   ├── sha256.hpp      # SHA-256 / HMAC / base64url
│   │   └── json.hpp        # nlohmann/json 库
│   ├── src/                # 源代码
│   │   ├── main.cpp        # 主程序入口
│   │   ├── es_client.cpp   # ES 客户端实现
│   │   ├── http_client.cpp # HTTP 客户端实现
│   │   └── sha256.cpp      # 签名算法实现
│   ├── tests/              # 自动化验证
│   │   ├── cursor_test.cpp # 游标分页集成测试（45 项检查）
│   │   └── mock_es.py      # 本地开发用 Mock ES（无 Docker 时使用）
│   └── data/               # 示例数据
│       └── sample_data.json
├── docs/                   # 文档
│   └── project_design.md   # 项目设计文档
├── docker-compose.yml      # Docker Compose 配置
├── .gitignore             # Git 忽略文件
└── README.md              # 项目说明
```

## 使用示例

程序运行后会自动执行以下演示：

1. **创建索引** - 创建名为 `articles` 的索引，配置中文分词
2. **批量导入** - 导入示例文章数据
3. **全文检索** - 演示各种搜索方式
4. **高亮显示** - 展示搜索结果高亮
5. **游标分页** - PIT + search_after 稳定遍历长列表（含审核期间增删、结束后旧游标被拒绝）
6. **清理资源** - 删除测试索引

### 输出示例

```
========================================
  Elasticsearch C++ 全文检索 DEMO
========================================

[1] 创建索引 'articles'...
✓ 索引创建成功

[2] 批量导入文档...
✓ 成功导入 5 篇文章

[3] 全文检索演示...

--- Match 查询: "人工智能" ---
命中 2 条结果:
  [1] 人工智能的发展历程 (score: 8.234)
  [2] 机器学习入门指南 (score: 5.123)

--- 高亮搜索: "深度学习" ---
  标题: 深度学习实战
  高亮: ...<em>深度学习</em>是机器学习的一个分支...

[4] 清理资源...
✓ 索引删除成功

========================================
  演示完成！
========================================
```

## 游标分页（长列表稳定翻页）

`from/size` 翻页在数据持续写入时会出现重复/遗漏，且深翻页性能差。游标分页基于
**Point in Time（PIT）快照 + search_after**，适合内容审核等需要完整遍历长列表的场景。

### 基本用法

```cpp
ESClient client("localhost", 9200);

// 首次与后续请求必须使用完全相同的参数
CursorSearchRequest req;
req.index = "articles";
req.query = {{"term", {{"category", "技术"}}}};   // 缺省为 match_all
req.sort = json::array({{{"created_at", "asc"}}}); // 自动追加 _shard_doc 决胜键
req.pageSize = 100;
req.keepAlive = "2m";          // PIT 保活时间（每次翻页自动续期）
req.cursorTtlSeconds = 600;    // 游标客户端侧有效期，应大于 keepAlive

CursorPage page = client.cursorSearchFirst(req);   // 建立 PIT 并取首页
while (page.hasMore) {
    // page.nextCursor 是不透明字符串，可持久化后稍候继续
    page = client.cursorSearchNext(req, page.nextCursor);
}
// 读到末页时 PIT 自动关闭；中途放弃可调用：
// client.closeCursor(page.nextCursor);
```

### 安全保障

- **不透明游标**：`v1.<base64url(payload)>.<hmac-sha256>`，payload 含 PIT id、
  `search_after` 值、查询/排序指纹、页大小与过期时间，整体签名；
- **绑定校验**：游标只能配合签发时的索引、查询、排序和页大小使用，
  换用即抛出 `CursorMismatchException`；
- **防篡改**：签名不匹配抛出 `CursorTamperedException`；
- **有效期**：超过 `cursorTtlSeconds` 抛出 `CursorExpiredException`；
  PIT 在 ES 侧被关闭/回收抛出 `PitNotFoundException`，四类失败可区分处理；
- **资源回收**：读到末页或 `closeCursor()` 主动结束时立即关闭 PIT；
  进程异常退出时，ES 会在 `keepAlive` 超时后自动回收遗留 PIT。
- **签名密钥**：默认随机构造（进程重启后旧游标失效）；多实例共享或
  跨重启续页时用 `setCursorSecret()` 设置稳定密钥。

### 运行自动化验证

```bash
# 方式一：Docker（真实 Elasticsearch 8.11.0）
docker-compose --profile test up --build cpp-test

# 方式二：本地编译 + 真实 ES
docker-compose up -d elasticsearch
cd backend && mkdir build && cd build && cmake .. && make
./es_cursor_test            # 或 ctest --output-on-failure

# 方式三：本地编译 + Mock ES（无 Docker 的开发机）
python3 ../tests/mock_es.py 9200 &
./es_cursor_test
```

验证覆盖：跨多页无重无漏、翻页期间增删文档快照稳定、同分值次序确定、
换条件/篡改/过期被拒绝、读到末页或主动结束后旧游标被拒绝（共 45 项检查）。

## 扩展开发

### 启用中文分词（IK 分词器）

如需真正的中文分词能力，可以使用带 IK 分词器的 Elasticsearch 镜像：

```yaml
# docker-compose.yml 中替换 elasticsearch 镜像
elasticsearch:
  image: elasticsearch-ik:8.11.0 # 需自行构建或使用社区镜像
```

然后修改索引 mapping：

```cpp
json mapping = {
    {"properties", {
        {"title", {{"type", "text"}, {"analyzer", "ik_max_word"}}},
        {"content", {{"type", "text"}, {"analyzer", "ik_smart"}}},
        {"tags", {{"type", "keyword"}}},
        {"created_at", {{"type", "date"}}}
    }}
};
client.createIndex("my_index", mapping);
```

### 添加新的搜索功能

```cpp
// 在 es_client.hpp 中添加新方法
SearchResult fuzzySearch(const std::string& index,
                         const std::string& field,
                         const std::string& value,
                         int fuzziness = 2);
```

## 许可证

MIT License
