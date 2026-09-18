#!/usr/bin/env python3
"""
开发/测试用 Mock Elasticsearch（8.x REST API 子集）。

仅用于在没有 Docker 的环境中运行 es_cursor_test / es_demo 做本地验证；
真实行为请以 docker-compose 中的 Elasticsearch 8.11.0 为准。

实现的端点：
  GET    /                          集群信息
  GET    /_cluster/health           集群健康
  PUT    /{index}                   创建索引
  DELETE /{index}                   删除索引
  HEAD   /{index}                   索引是否存在
  GET    /{index}                   索引信息
  POST   /{index}/_refresh          刷新
  POST   /{index}/_doc[/{id}]       写入文档
  GET    /{index}/_doc/{id}         读取文档
  POST   /{index}/_update/{id}      更新文档（{"doc": {...}}）
  DELETE /{index}/_doc/{id}         删除文档
  POST   /_bulk                     批量写入（index 动作）
  POST   /{index}/_pit?keep_alive=  建立 Point in Time（冻结快照）
  DELETE /_pit                      关闭 PIT（{"id": ...}）
  POST   /_search                   搜索（支持 pit / search_after / sort / track_total_hits）
  POST   /{index}/_search           搜索（支持 query / from / size / highlight）

PIT 语义与真实 ES 一致：
  * 快照在 _pit 建立时刻冻结，之后的新增/删除/修改不影响该 PIT 的搜索结果；
  * 每次携带 pit 的搜索会用请求体中的 keep_alive 续期；
  * 超过 keep_alive 未访问的 PIT 被回收，再次使用返回
    404 search_context_missing_exception；
  * _shard_doc 在快照内对每条文档固定，作为排序决胜键。
"""
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


def parse_keep_alive(s):
    m = re.fullmatch(r"(\d+)(ms|s|m|h|d)?", s or "")
    if not m:
        return 60.0
    value, unit = int(m.group(1)), m.group(2) or "s"
    return value * {"ms": 0.001, "s": 1, "m": 60, "h": 3600, "d": 86400}[unit]


class Store:
    def __init__(self):
        self.lock = threading.RLock()
        self.indices = {}   # name -> {"mappings":..., "settings":..., "docs": {id: doc}, "seq": n}
        self.pits = {}      # pid -> {"entries": [...], "expires": ts}
        self.pit_seq = 0

    # ---------- 文档 ----------
    def create_index(self, name, body):
        with self.lock:
            if name in self.indices:
                return 400, {"error": {"type": "resource_already_exists_exception",
                                       "reason": "index [%s] already exists" % name},
                             "status": 400}
            self.indices[name] = {"mappings": body.get("mappings", {}),
                                  "settings": body.get("settings", {}),
                                  "docs": {}, "seq": 0}
            return 200, {"acknowledged": True, "index": name}

    def delete_index(self, name):
        with self.lock:
            if name not in self.indices:
                return 404, {"error": {"type": "index_not_found_exception",
                                       "reason": "no such index [%s]" % name},
                             "status": 404}
            del self.indices[name]
            return 200, {"acknowledged": True}

    def index_doc(self, index, doc_id, doc):
        with self.lock:
            idx = self.indices.get(index)
            if idx is None:
                return 404, {"error": {"type": "index_not_found_exception",
                                       "reason": "no such index [%s]" % index},
                             "status": 404}
            if not doc_id:
                idx["seq"] += 1
                doc_id = "auto-%d" % idx["seq"]
            created = doc_id not in idx["docs"]
            idx["docs"][doc_id] = doc
            return (201 if created else 200), {
                "_index": index, "_id": doc_id,
                "result": "created" if created else "updated",
                "_version": 1}

    def get_doc(self, index, doc_id):
        with self.lock:
            idx = self.indices.get(index)
            if idx is None or doc_id not in idx["docs"]:
                return 404, {"_index": index, "_id": doc_id, "found": False}
            return 200, {"_index": index, "_id": doc_id, "found": True,
                         "_source": idx["docs"][doc_id]}

    def update_doc(self, index, doc_id, body):
        with self.lock:
            idx = self.indices.get(index)
            if idx is None or doc_id not in idx["docs"]:
                return 404, {"error": {"type": "document_missing_exception",
                                       "reason": "document missing"}, "status": 404}
            idx["docs"][doc_id].update(body.get("doc", {}))
            return 200, {"_index": index, "_id": doc_id, "result": "updated", "_version": 2}

    def delete_doc(self, index, doc_id):
        with self.lock:
            idx = self.indices.get(index)
            if idx is None:
                return 404, {"error": {"type": "index_not_found_exception",
                                       "reason": "no such index"}, "status": 404}
            if doc_id not in idx["docs"]:
                return 404, {"_index": index, "_id": doc_id, "result": "not_found"}
            del idx["docs"][doc_id]
            return 200, {"_index": index, "_id": doc_id, "result": "deleted", "_version": 2}

    # ---------- PIT ----------
    def open_pit(self, index, keep_alive):
        with self.lock:
            idx = self.indices.get(index)
            if idx is None:
                return 404, {"error": {"type": "index_not_found_exception",
                                       "reason": "no such index [%s]" % index},
                             "status": 404}
            self.pit_seq += 1
            pid = "pit-%d" % self.pit_seq
            # 冻结快照：复制文档引用并分配稳定的 _shard_doc
            entries = []
            for shard_doc, (doc_id, doc) in enumerate(idx["docs"].items()):
                entries.append({"shard_doc": shard_doc, "index": index,
                                "id": doc_id, "source": dict(doc)})
            self.pits[pid] = {"entries": entries,
                              "expires": time.time() + parse_keep_alive(keep_alive)}
            return 200, {"id": pid}

    def close_pit(self, pid):
        with self.lock:
            if pid not in self.pits:
                return 404, {"error": {"type": "search_context_missing_exception",
                                       "reason": "No search context found for id [%s]" % pid},
                             "status": 404}
            del self.pits[pid]
            return 200, {"succeeded": True, "num_freed": 1}

    def sweep_pits(self):
        with self.lock:
            now = time.time()
            expired = [p for p, v in self.pits.items() if v["expires"] < now]
            for p in expired:
                del self.pits[p]

    # ---------- 查询求值 ----------
    @staticmethod
    def _match_query(field, q, source):
        return str(q).lower() in str(source.get(field, "")).lower()

    @classmethod
    def eval_query(cls, query, source):
        if not query or "match_all" in query:
            return True
        if "term" in query:
            field, value = next(iter(query["term"].items()))
            if isinstance(value, dict):
                value = value.get("value")
            return source.get(field) == value
        if "match" in query:
            field, value = next(iter(query["match"].items()))
            if isinstance(value, dict):
                value = value.get("query", "")
            return cls._match_query(field, value, source)
        if "multi_match" in query:
            mm = query["multi_match"]
            return any(cls._match_query(f, mm.get("query", ""), source)
                       for f in mm.get("fields", []))
        if "bool" in query:
            b = query["bool"]
            for clause in b.get("must", []) + b.get("filter", []):
                if not cls.eval_query(clause, source):
                    return False
            for clause in b.get("must_not", []):
                if cls.eval_query(clause, source):
                    return False
            should = b.get("should", [])
            if should and not (b.get("must") or b.get("filter")):
                return any(cls.eval_query(c, source) for c in should)
            return True
        return True

    # ---------- 搜索 ----------
    def search(self, index, body):
        with self.lock:
            pit_id = None
            if "pit" in body:
                pit_id = body["pit"].get("id")
                pit = self.pits.get(pit_id)
                if pit is None:
                    return 404, {"error": {"type": "search_context_missing_exception",
                                           "reason": "No search context found for id [%s]" % pit_id},
                                 "status": 404}
                # 每次搜索用 keep_alive 续期
                pit["expires"] = time.time() + parse_keep_alive(
                    body["pit"].get("keep_alive", "1m"))
                entries = pit["entries"]
            else:
                idx = self.indices.get(index)
                if idx is None:
                    return 404, {"error": {"type": "index_not_found_exception",
                                           "reason": "no such index [%s]" % index},
                                 "status": 404}
                entries = [{"shard_doc": n, "index": index, "id": d, "source": doc}
                           for n, (d, doc) in enumerate(idx["docs"].items())]

            query = body.get("query") or {"match_all": {}}
            matched = [e for e in entries if self.eval_query(query, e["source"])]
            for e in matched:
                e["score"] = 1.0

            # total 是查询的全量匹配数，不随 search_after / from / size 变化
            total = len(matched)

            # 排序
            sort_spec = body.get("sort") or []
            norm_spec = []
            for item in sort_spec:
                if isinstance(item, str):
                    norm_spec.append((item, "asc"))
                else:
                    field, order = next(iter(item.items()))
                    if isinstance(order, dict):
                        order = order.get("order", "asc")
                    norm_spec.append((field, order))

            def sort_key(e, spec=norm_spec):
                key = []
                for field, order in spec:
                    if field == "_score":
                        v = e["score"]
                    elif field == "_shard_doc":
                        v = e["shard_doc"]
                    else:
                        v = e["source"].get(field)
                    key.append((v is None, v))
                return key

            if norm_spec:
                # 逐字段按方向稳定排序（最后字段优先排）
                for pos in reversed(range(len(norm_spec))):
                    field, order = norm_spec[pos]
                    reverse = (order == "desc")
                    matched.sort(key=lambda e, p=pos: sort_key(e)[p], reverse=reverse)

            # search_after：严格位于给定排序值之后
            search_after = body.get("search_after")
            if search_after is not None and norm_spec:
                def is_after(e):
                    key = sort_key(e)
                    for (field, order), (want_null, want), (have_null, have) in zip(
                            norm_spec, [(v is None, v) for v in search_after], key):
                        if have_null != want_null:
                            return have_null  # 缺失值排在最后
                        if have == want:
                            continue
                        return have < want if order == "desc" else have > want
                    return False
                matched = [e for e in matched if is_after(e)]

            offset = int(body.get("from", 0) or 0)
            size = body.get("size", 10)
            size = total if size is None else int(size)
            window = matched[offset:offset + size]

            has_score_sort = any(f == "_score" for f, _ in norm_spec)
            highlight = body.get("highlight")
            hits = []
            for e in window:
                hit = {"_index": e["index"], "_id": e["id"],
                       "_score": e["score"] if (has_score_sort or not norm_spec) else None,
                       "_source": e["source"]}
                if norm_spec:
                    hit["sort"] = [v for _, v in sort_key(e)]
                if highlight:
                    hit["highlight"] = self.build_highlight(query, e["source"], highlight)
                hits.append(hit)

            resp = {"took": 1, "timed_out": False,
                    "hits": {"total": {"value": total, "relation": "eq"},
                             "max_score": (max((e["score"] for e in matched), default=None)
                                           if (has_score_sort or not norm_spec) else None),
                             "hits": hits}}
            if pit_id is not None:
                resp["pit_id"] = pit_id
            return 200, resp

    @classmethod
    def build_highlight(cls, query, source, highlight):
        terms = []
        if "match" in query:
            _, v = next(iter(query["match"].items()))
            terms.append(v.get("query", "") if isinstance(v, dict) else v)
        if "multi_match" in query:
            terms.append(query["multi_match"].get("query", ""))
        pre = (highlight.get("pre_tags") or ["<em>"])[0]
        post = (highlight.get("post_tags") or ["</em>"])[0]
        out = {}
        for field in (highlight.get("fields") or {}):
            text = str(source.get(field, ""))
            for t in terms:
                if t and t in text:
                    text = text.replace(t, pre + t + post)
            if pre in text:
                out[field] = [text]
        return out


STORE = Store()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    # ---------- 工具 ----------
    def _send(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _json_body(self):
        raw = self._body()
        return json.loads(raw.decode("utf-8")) if raw else {}

    # ---------- 路由 ----------
    def do_GET(self):
        path = urlparse(self.path).path.strip("/")
        parts = path.split("/") if path else []
        if path == "":
            return self._send(200, {"name": "mock-es", "cluster_name": "es-demo-cluster",
                                    "version": {"number": "8.11.0-mock"},
                                    "tagline": "You Know, for Search"})
        if path == "_cluster/health":
            return self._send(200, {"status": "green", "number_of_nodes": 1})
        if len(parts) == 1:
            idx = STORE.indices.get(parts[0])
            if idx is None:
                return self._send(404, {"error": {"type": "index_not_found_exception",
                                                  "reason": "no such index"}, "status": 404})
            return self._send(200, {parts[0]: {"mappings": idx["mappings"],
                                               "settings": idx["settings"]}})
        if len(parts) == 3 and parts[1] == "_doc":
            status, resp = STORE.get_doc(parts[0], parts[2])
            return self._send(status, resp)
        return self._send(404, {"error": {"type": "not_found"}, "status": 404})

    def do_HEAD(self):
        path = urlparse(self.path).path.strip("/")
        exists = path in STORE.indices
        self.send_response(200 if exists else 404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_PUT(self):
        path = urlparse(self.path).path.strip("/")
        parts = path.split("/") if path else []
        if len(parts) == 1:
            status, resp = STORE.create_index(parts[0], self._json_body())
            return self._send(status, resp)
        return self._send(404, {"error": {"type": "not_found"}, "status": 404})

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path.strip("/")
        parts = path.split("/") if path else []
        query = parse_qs(parsed.query)

        if len(parts) == 2 and parts[1] == "_pit":
            keep_alive = (query.get("keep_alive") or ["1m"])[0]
            status, resp = STORE.open_pit(parts[0], keep_alive)
            return self._send(status, resp)
        if len(parts) == 2 and parts[1] == "_refresh":
            return self._send(200, {"_shards": {"total": 1, "successful": 1, "failed": 0}})
        if path == "_bulk":
            return self._bulk()
        if len(parts) >= 2 and parts[1] == "_doc":
            doc_id = parts[2] if len(parts) > 2 else None
            status, resp = STORE.index_doc(parts[0], doc_id, self._json_body())
            return self._send(status, resp)
        if len(parts) == 3 and parts[1] == "_update":
            status, resp = STORE.update_doc(parts[0], parts[2], self._json_body())
            return self._send(status, resp)
        if path == "_search" or (len(parts) == 2 and parts[1] == "_search"):
            index = parts[0] if len(parts) == 2 else None
            status, resp = STORE.search(index, self._json_body())
            return self._send(status, resp)
        return self._send(404, {"error": {"type": "not_found"}, "status": 404})

    def do_DELETE(self):
        path = urlparse(self.path).path.strip("/")
        parts = path.split("/") if path else []
        if path == "_pit":
            body = self._json_body()
            status, resp = STORE.close_pit(body.get("id", ""))
            return self._send(status, resp)
        if len(parts) == 1:
            status, resp = STORE.delete_index(parts[0])
            return self._send(status, resp)
        if len(parts) == 3 and parts[1] == "_doc":
            status, resp = STORE.delete_doc(parts[0], parts[2])
            return self._send(status, resp)
        return self._send(404, {"error": {"type": "not_found"}, "status": 404})

    def _bulk(self):
        lines = self._body().decode("utf-8").strip().split("\n")
        items = []
        i = 0
        while i < len(lines):
            action = json.loads(lines[i])
            meta = action.get("index", {})
            doc = json.loads(lines[i + 1]) if i + 1 < len(lines) else {}
            status, resp = STORE.index_doc(meta.get("_index"), meta.get("_id"), doc)
            items.append({"index": {"_index": resp.get("_index", meta.get("_index")),
                                    "_id": resp.get("_id", ""),
                                    "result": resp.get("result", "created"),
                                    "_version": 1,
                                    "status": status}})
            i += 2
        errors = any(it["index"]["status"] >= 300 for it in items)
        return self._send(200, {"took": 1, "errors": errors, "items": items})


def sweeper():
    while True:
        time.sleep(0.5)
        STORE.sweep_pits()


def main():
    port = 9200
    import sys
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    threading.Thread(target=sweeper, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("Mock Elasticsearch listening on http://127.0.0.1:%d" % port)
    server.serve_forever()


if __name__ == "__main__":
    main()
