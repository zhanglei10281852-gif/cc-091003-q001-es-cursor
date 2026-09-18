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

### 运行自动化验证

游标分页功能附带自动化验证程序（需要可访问的 Elasticsearch）：

```bash
# 本地编译后运行（默认连接 localhost:9200，可用 ES_HOST/ES_PORT 覆盖）
cd backend/build
./cursor_test

# 或通过 ctest 运行
ctest --output-on-failure

# 或使用 Docker（test profile）
docker-compose --profile test up --build cursor-test
```

验证内容：跨多页无重无漏、翻页期间增删文档快照内结果稳定、同分值记录次序确定、
主动结束/读到末页后旧游标被拒绝、篡改游标被拒绝、游标与检索参数绑定、过期游标被拒绝。

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

### 稳定游标分页（PIT + search_after）

面向长列表逐页浏览场景（如内容审核），解决 `from/size` 深分页在并发写入时重复/漏记录的问题：

- ✅ 首次请求自动建立 Point in Time 快照，返回结果与不透明的下一页游标
- ✅ 后续请求沿用同一查询、排序和快照，通过 `search_after` 前进；翻页期间的新增/删除不打乱已开始的浏览
- ✅ 同分值记录自动追加 `_shard_doc` 决胜排序，次序确定
- ✅ 游标经 HMAC-SHA256 签名并与索引/查询/排序/页大小绑定，篡改或换用参数即被拒绝
- ✅ 可区分的失败类型：`CursorTamperedException`（篡改/参数不匹配）、`CursorExpiredException`（超过保活时间）、`PitGoneException`（PIT 已被 ES 释放）
- ✅ 读到末页自动关闭 PIT，也可 `closeCursor` 主动结束；异常退出后遗留 PIT 由较短的 keep_alive 自动回收

```cpp
es::CursorSearchOptions options;
options.indexName = "articles";
options.query = {{"term", {{"category", "待审核"}}}};
options.pageSize = 20;
options.keepAlive = "2m";   // 每次翻页自动续期

while (true) {
    es::CursorPage page = client.searchByCursor(options);
    for (const auto& hit : page.result.hits) {
        // 处理本页记录 ...
    }
    if (!page.hasMore) break;          // 末页：PIT 已自动关闭
    options.cursor = page.nextCursor;  // 继续下一页
}
// 中途主动结束：client.closeCursor(page.nextCursor);
```

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
│   │   ├── es_client.hpp   # ES 客户端类
│   │   ├── es_cursor.hpp   # 游标编解码（签名、参数指纹）
│   │   ├── http_client.hpp # HTTP 客户端类
│   │   └── json.hpp        # nlohmann/json 库
│   ├── src/                # 源代码
│   │   ├── main.cpp        # 主程序入口
│   │   ├── es_client.cpp   # ES 客户端实现
│   │   ├── es_cursor.cpp   # 游标编解码实现
│   │   └── http_client.cpp # HTTP 客户端实现
│   ├── test/               # 自动化验证
│   │   └── cursor_test.cpp # 游标分页验证程序
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
5. **游标分页** - PIT + search_after 稳定翻页，含并发写入、主动结束、篡改检测演示
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
