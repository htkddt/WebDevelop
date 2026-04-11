# Mongo module — rules

Do not modify without updating **include/mongo.h** if API or constants change.  
Rule reference is injected at the top of `include/mongo.h`.

## 0. Data record shape (Mongo) — do not modify without review

**Rule (Cursor):** **`.cursor/rules/embed-vector-metadata.mdc`** — any logic that **uses** a stored **`vector`** must strictly use the same document’s **`metadata`** embedding fields (**`embed_schema`**, **`vector_dim`**, **`embed_family`**, **`model_id`**). Do not interpret **`vector`** without that bundle (legacy rows: migrate or infer only in migration code).

Canonical document shape for the **records** (chat) collection: `MONGO_CHAT_DB` / `MONGO_CHAT_COLLECTION` (default `bot` / `records`). Written by `storage_append_turn`. Rendered via L1 memory circular buffer when in-memory.

```json
{
    "tenant_id": "string",
    "user": "string",
    "vector": [],
    "turn": {
        "input": "string",
        "assistant": "string"
    },
    "timestamp": "1731234567890",
    "metadata": {
        "ver": 1,
        "encrypted": false,
        "model_id": "string",
        "embed_schema": 1,
        "vector_dim": 384,
        "embed_family": "custom",
        "lang": "string",
        "score": 0.0
    },
    "createdAt": "<BSON date_time>"
}
```

- **tenant_id:** Data tenant; default `"default"`.
- **user:** User id; default `"default"`.
- **vector:** Float array from embedding model; empty if not set. Used for vector search (RAG).
- **turn:** One chat turn: `input` = user message, `assistant` = bot reply.
- **timestamp:** String, epoch milliseconds (e.g. `"1731234567890"`) for ordering/display. Used when loading history (fallback when `createdAt` not read).
- **metadata.ver:** Integer, default 1 (migration).
- **metadata.encrypted:** Boolean, default false (payload/turn not encrypted at rest).
- **metadata.model_id:** Resolved tag used for that turn’s embedding (**`m4_embed_*`** / **`VECTOR_GEN_MODEL_ID`** for custom). When you change defaults or env, query stale `model_id` and re-embed. Checklist: **`.cursor/default_models.md`**.
- **metadata.embed_schema:** Integer (**1** = current); bump only if you redefine provenance semantics. See **`.cursor/embed_migration.md`**.
- **metadata.vector_dim:** Length of **`vector`** at insert (int32). Use with **`embed_family`** to detect mixed spaces after model/backend changes.
- **metadata.embed_family:** **`custom`** | **`ollama`** | **`legacy`** (vector without `model_id`). Drives migration filters alongside **`vector_dim`**.
- **metadata.lang:** Language code from lang_detect; `"mixed"` when score < 0.5.
- **metadata.score:** Double 0..1 (lang confidence).
- **createdAt:** BSON date_time (epoch ms). Used for sort/limit in chat history; index key.

**Indexes (both created in code)**

When **both** are true — (1) library built with **`USE_MONGOC=1`** (`make USE_MONGOC=1`), (2) **`storage_connect()`** is called with a non-empty Mongo URI — `storage_ensure_records_index()` runs and creates **both** of these on `bot.records`:

1. `{ "tenant_id": 1, "createdAt": 1 }` — for chat history (filter tenant_id, sort by createdAt asc, limit).
2. `{ "tenant_id": 1, "user": 1, "timestamp": -1 }` — for tenant/user time-ordered queries.

Check logs for `[STORAGE] index created: bot.records` (one line per index). **If you see no indexes in MongoDB:** build with `USE_MONGOC=1`, link that build into your app, and ensure the app opens a Mongo connection at startup.

**Async (optional):**
- Full playbook: **`.cursor/embed_migration.md`** (provenance backfill, re-embed from **`turn.input`**, Redis index consistency, throttle).
- Migrate once when **`model_id`** / **`embed_family`** / **`vector_dim`** drift from target policy.

## 1. Scope

Mongo module provides: **initial** (connect), **set** (insert/upsert/bulk), **search** (vector search, find with sort/limit). Used for chat persistence, ai_logs, batch records, and RAG when execution mode includes Mongo.

## 2. Connection and lifecycle

- Use **libmongoc** v2.x. Optional at build: `USE_MONGOC=1`.
- **Initial:** `mongoc_init()` once; create client (or pool) from URI. Prefer `mongoc_client_pool_t` over new client per request when under load.
- **Destroy:** Every `mongoc_cursor_t` MUST be followed by `mongoc_cursor_destroy`. Destroy client/pool; `mongoc_cleanup()` on process teardown.
- **Null safety:** Check every pointer from `mongoc_*` and `malloc` immediately.

## 3. Databases and collections

- **Records (main):** `MONGO_DB_NAME` / `MONGO_COLLECTION` in `include/mongo.h` (default `m4_ai` / `records`). Document shape §0. Every document MUST include `tenant_id` for multi-tenant isolation.
- **Chat:** `MONGO_CHAT_DB` / `MONGO_CHAT_COLLECTION` (default `bot` / `records`). Document shape §0 (tenant_id, user, turn { input, assistant }, timestamp, createdAt, metadata). Legacy per-message shape had role, content, ts (still supported when reading).
- **AI logs:** `MONGO_AI_LOGS_DB` / `MONGO_AI_LOGS_COLLECTION` (default `bot` / `ai_logs`). Overridable via validated setter (db/coll name 1..63 chars, `[a-zA-Z0-9_]` only).

## 4. Set (write) rules

- **Batch (records):** Use `mongoc_bulk_operation_t` for any data load **> 100** records. Batch size 100–500 for Atlas. Document shape per §0 (tenant_id, user, vector, turn, timestamp, metadata).
- **Chat / ai_logs:** Single-doc insert; include `createdAt` (date_time) for ordering.
- **Indexing:** Chat (bot.records): both indexes are created in code when built with **USE_MONGOC=1** and when `storage_connect()` runs with a non-empty mongo_uri (see `storage_ensure_records_index` in storage.c). If no indexes appear in MongoDB, build with `make USE_MONGOC=1` and use that lib. Refuse queries without `tenant_id` in filter. All strings UTF-8; set locale in app.

## 5. Search rules

- **Vector search:** When implemented, use Atlas Vector Search with **tenant_id** in filter; expose via storage/API as needed.
- **Chat history:** Find with filter `tenant_id`, sort by `createdAt` ascending, limit = batchContextSize. Iterate cursor; destroy cursor when done.
- No query without tenant_id (or equivalent) filter.

## 6. Reference

- Data record shape: §0 above; see also `.cursor/storage.md` for L1 memory struct.
- Execution modes: main `.cursor/rule.md` §2.
- Module API and constants: `include/mongo.h`.
