# Public API (`api.h`)

The library exposes **8 public functions**. Everything else (prompt tags, model lanes, logging, embedding, learning) is configured via JSON options at create time or runs automatically inside c-lib.

## Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         Host Application                        │
│                    (Python / Node / Go / C)                      │
└──┬──────────────┬──────────────────┬─────────────┬──────────────┘
   │              │                  │             │
   ▼              ▼                  ▼             ▼
 api_create    api_chat           api_greet    api_geo_atlas_
 api_destroy   (sync/stream       (welcome     import_row
 (JSON opts)    + context_json)    message)    (data enrichment)
   │              │                  │
   ▼              ▼                  ▼
 api_load_chat_history    api_get_history_message    api_get_stats
```

---

## Prerequisites

| Requirement | Detail |
|-------------|--------|
| **Build** | `make lib` → `lib/libm4engine.dylib` (macOS) or `libm4engine.so` (Linux) |
| **Ollama** | Running on `127.0.0.1:11434` (default). Required for chat; optional for embedding (hash fallback). |
| **MongoDB** | Required for modes B/C/D. Default `mongodb://127.0.0.1:27017`. Build with `USE_MONGOC=1`. |
| **Redis** | Required for modes C/D. Default `127.0.0.1:6379`. |
| **Header** | `#include "api.h"` — single header for all 7 functions |

---

## Execution Modes

| Constant | Value | Mongo | Redis | ELK | Use case |
|----------|-------|-------|-------|-----|----------|
| `M4ENGINE_MODE_ONLY_MEMORY` | 0 | -- | -- | -- | Testing, offline, no persistence |
| `M4ENGINE_MODE_ONLY_MONGO` | 1 | yes | -- | -- | Simple persistence |
| `M4ENGINE_MODE_MONGO_REDIS` | 2 | yes | yes | -- | Production: RAG + caching |
| `M4ENGINE_MODE_MONGO_REDIS_ELK` | 3 | yes | yes | yes | Full: analytics + search |

---

## API 1: `api_create` / `api_destroy`

Create and destroy the engine context. Pass configuration as a JSON string. Any key not present uses the default value. NULL or `"{}"` gives full defaults.

```c
api_context_t *api_create(const char *json_opts);   // JSON options (recommended)
api_context_t *api_create_with_opts(const api_options_t *opts);  // C struct (legacy)
void api_destroy(api_context_t *ctx);
```

### Options (JSON)

```json
{
  "mode": 3,
  "mongo_uri": "mongodb://127.0.0.1:27017",
  "redis_host": "127.0.0.1",
  "redis_port": 6379,
  "es_host": "127.0.0.1",
  "es_port": 9200,
  "log_db": "my_logs_db",
  "log_coll": "my_logs",
  "context_batch_size": 30,
  "inject_geo_knowledge": 0,
  "disable_auto_system_time": 0,
  "geo_authority": 1,
  "geo_authority_csv_path": "/data/provinces.csv",
  "geo_migrate_legacy": 0,
  "vector_gen_backend": 0,
  "vector_ollama_model": null,
  "embed_migration_autostart": 0,
  "session_idle_seconds": 300,
  "default_persona": "You are a helpful assistant.",
  "default_instructions": "Always reply in Vietnamese.",
  "default_model_lane": 4,
  "learning_terms_path": "/data/nl_terms.json",
  "enable_learning_terms": 1,
  "defer_learning_terms_load": 1,
  "debug_modules": ["API", "ai_agent", "STORAGE"],
  "lanes": [
    {
      "key": "BUSINESS",
      "model": "finance-llm",
      "inject": "You are a financial advisor.",
      "api_url": "https://api.groq.com/openai/v1/chat/completions",
      "api_key": "gsk_..."
    },
    {
      "key": "TECH",
      "model": "codellama",
      "inject": "You are a code expert."
    }
  ]
}
```

### Options reference

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `mode` | int | `1` (ONLY_MONGO) | `0`=memory, `1`=mongo, `2`=mongo+redis, `3`=mongo+redis+elk |
| `mongo_uri` | string | `"mongodb://127.0.0.1:27017"` | MongoDB connection URI |
| `redis_host` | string | `"127.0.0.1"` | Redis host |
| `redis_port` | int | `6379` | Redis port |
| `es_host` | string | null | Elasticsearch host (null=disabled) |
| `es_port` | int | `9200` | Elasticsearch port |
| `log_db` / `log_coll` | string | null | Override ai_logs DB/collection |
| `context_batch_size` | int | `30` | History cycles for LLM context |
| `inject_geo_knowledge` | int | `0` | `1`=prepend [KNOWLEDGE_BASE] from geo_atlas |
| `disable_auto_system_time` | int | `0` | `1`=skip auto [SYSTEM_TIME] |
| `geo_authority` | int | `0` | `1`=enable L1 cache + conflict detector |
| `geo_authority_csv_path` | string | null | CSV file loaded at init |
| `geo_migrate_legacy` | int | `0` | `1`=auto-run geo_atlas backfill at init |
| `vector_gen_backend` | int | `0` | `0`=built-in hash 384-D, `1`=external embed model |
| `vector_ollama_model` | string | null | Override embed model when backend=1 |
| `embed_migration_autostart` | int | `0` | `1`=queue embed migration at init |
| `session_idle_seconds` | int | `300` | Idle eviction for session ring buffers |
| `default_persona` | string | null | Persona prompt (null=compiled-in default) |
| `default_instructions` | string | null | Extra instructions appended to prompt |
| `default_model_lane` | int | `0` | `0`=DEFAULT, `1`=EDUCATION, `2`=BUSINESS, `3`=TECH, `4`=CHAT |
| `learning_terms_path` | string | null | NL routing terms file (TSV/JSON) |
| `enable_learning_terms` | int | `0` | `1`=allow cue recording |
| `defer_learning_terms_load` | int | `0` | `1`=background thread load |
| `query_cache_path` | string | null | Path for query plan cache file. See query cache section. |
| `debug_modules` | string[] | null | Module keys for DEBUG logging. See debug section. |
| `lanes` | object[] | null | Model lane config with optional direct endpoints |

### Lane config (`lanes` array)

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `key` | string | yes | Lane name: `"BUSINESS"`, `"TECH"`, `"EDUCATION"`, etc. |
| `model` | string | no | Model ID (e.g. `"finance-llm"`, `"codellama"`) |
| `inject` | string | no | System inject text for this lane |
| `api_url` | string | no | Direct endpoint URL. null=use cloud pool routing |
| `api_key` | string | no | API key for the direct endpoint |

When `api_url` is set → direct call to that endpoint (no pool).
When `api_url` is null → ai_agent routes: cloud pool → Ollama fallback.

### Debug modules

Valid keys for `debug_modules`:

| Module | What it logs |
|--------|-------------|
| `API` | Request handling, session lifecycle, turn persistence |
| `ai_agent` | LLM calls (Ollama, cloud endpoints), prompt construction |
| `STORAGE` | MongoDB/Redis/ELK connection, queries, backfill |
| `GEO_LEARNING` | Geo place extraction, dedup, authority updates |
| `GEO_AUTH` | Geo authority cache lookups, prompt hints |
| `OLLAMA` | Ollama HTTP calls, model resolution, streaming |
| `ELK` | Elasticsearch indexing, search, bulk operations |
| `EMBED_MIGRATION` | Vector re-embedding batch jobs |
| `ENGINE` | Engine lifecycle, config, worker dispatch |
| `CHAT` | Chat context building, RAG prepend |
| `nl_learn_terms` | Term score recording, WAL append, save/load |
| `nl_learn_cues` | Phrase→intent cue detection, vocab scan |
| `LOGIC_CONFLICT` | Numbered list / merger conflict detection |
| `INTENT_ROUTE` | NL intent classify: scores, collection/field resolution |
| `SHARED_COLLECTION` | Registry load, term vocab build |
| `SMART_TOPIC` | Mini AI intent classify (TinyLlama/Phi2), temperature |

Also supported via env: `M4_DEBUG_MODULES=INTENT_ROUTE,SHARED_COLLECTION`

### ELK flow logging

ELK-related logs use the `[ELK flow]` prefix on stderr and are controlled by a separate env var (not `debug_modules`):

| Env var | Values | What it controls |
|---------|--------|-----------------|
| `M4_ELK_LOG` | unset/`1` = flow milestones, `0` = off, `2` = verbose (per-doc) | Cold backfill, pool start, index success/failure |
| `M4_ELK_DIAG` | `1` = one-shot snapshot at connect | Registry, pool, Mongo client status dump |

ELK flow log messages (always on stderr, level controlled by `M4_ELK_LOG`):

| Log | When |
|-----|------|
| `[ELK flow] init host=... port=...` | ELK module initialized |
| `[ELK flow] registry loaded path=... elk_allow_count=N` | SharedCollection registry parsed |
| `[ELK flow] pool started workers=N queue_cap=N` | Sync pool threads running |
| `[ELK flow] cold_backfill scanning db=...` | Full backfill started |
| `[ELK flow] incremental backfill collection=... since _id > ...` | Incremental sync (schedule_refresh) |
| `[ELK flow] pump enqueue collection=... docs=N mode=full/incremental` | Docs queued for indexing |
| `[ELK flow] change_stream: watching db=... (real-time sync)` | Change stream started successfully |
| `[ELK flow] change_stream: insert/update collection/id → index` | Real-time doc indexed (verbose, level 2) |
| `[ELK flow] change_stream: stopped` | Clean shutdown |
| `[ELK flow][ERROR] change_stream: ...` | Preflight failed — see message for fix instructions |
| `[ELK flow] sync_state: cannot write ...` | State file write failed |
| `[ELK] index ...: HTTP 400` | ES rejected a document (bad JSON, mapping conflict) |

### Example: minimal (memory only)

```c
api_context_t *ctx = api_create("{\"mode\": 0}");
if (!ctx) return 1;
// ... use ctx ...
api_destroy(ctx);
```

### Example: full defaults

```c
api_context_t *ctx = api_create(NULL);  // or api_create("{}")
```

### Example: production with lanes + debug

```c
api_context_t *ctx = api_create(
    "{"
    "  \"mode\": 2,"
    "  \"mongo_uri\": \"mongodb://db.example.com:27017\","
    "  \"default_persona\": \"You are Mắm, a Saigon local.\","
    "  \"debug_modules\": [\"ai_agent\", \"INTENT_ROUTE\", \"SHARED_COLLECTION\"],"
    "  \"lanes\": ["
    "    {\"key\": \"BUSINESS\", \"model\": \"llama-3.1-70b\","
    "     \"api_url\": \"https://api.groq.com/openai/v1/chat/completions\","
    "     \"api_key\": \"gsk_abc123\"}"
    "  ]"
    "}"
);
```

### Example: Python

```python
ctx = lib.api_create(b'{"mode": 3, "debug_modules": ["ai_agent"]}')
```

### Example: Node.js

```javascript
const ctx = lib.api_create(JSON.stringify({ mode: 3, debug_modules: ["ai_agent"] }));
```

### Data flow

```
api_create(json)
  ├── Parse JSON → options
  ├── Build lanes from "lanes" array
  ├── Validate URIs (mongo, shared_collection)
  ├── Init debug log modules
  ├── engine_create(config) + engine_init()
  │   ├── Connect Mongo/Redis/ELK based on mode
  │   ├── SharedCollection registry load → term vocab build
  │   ├── smart_topic warmup (if enabled)
  │   └── geo_learning worker start (if MONGO_REDIS+)
  ├── Load: nl_learn_terms (sync or deferred thread)
  │   ├── Load snapshot (JSON v2 or TSV v1)
  │   ├── Replay WAL deltas (.wal file)
  │   └── Open WAL for future appends
  ├── Apply: default_persona → prompt system
  ├── Apply: default_instructions → prompt system
  ├── Apply: default_model_lane → model_lane_key
  ├── Load: geo_authority_csv_path → L1 cache
  ├── Run: geo_migrate_legacy → backfill Mongo
  └── Return ctx (or NULL on failure)
```

### Intent routing (per chat turn)

When `learning_terms_path` is set and ELK mode is active, each chat turn runs through the intent routing pipeline before the LLM call:

```
api_chat / api_chat_stream
  │
  ├── Phase 1: CLASSIFY (intent_route_classify)
  │   ├── Tokenize user message
  │   ├── score_sum for ELK_ANALYTICS, ELK_SEARCH, RAG_VECTOR, CHAT
  │   ├── If best score > threshold(5) → route to ELK intent
  │   ├── Resolve collection via term vocab + synonym fallback
  │   └── If score < threshold → fall through to smart_topic LLM
  │
  ├── Phase 3: EXECUTE (intent_route_execute)
  │   ├── Resolve ELK index from SharedCollection registry
  │   ├── Build ELK query JSON (multi_match)
  │   └── Execute via storage_elk_search → parse result
  │
  ├── Phase 4: FORMAT (intent_route_format_data_result)
  │   ├── Build [DATA_RESULT] JSON block
  │   └── Prepend to LLM prompt context
  │
  └── LLM receives prompt with real data → answers with facts
```

Enable `debug_modules: ["INTENT_ROUTE"]` to see routing decisions on stderr.

### NL learning terms — persistence

When `learning_terms_path` is set and `enable_learning_terms` is `1`, the engine records NL cue scores to disk. Two files are used:

| File | Purpose | Created by |
|------|---------|------------|
| `{learning_terms_path}` | Snapshot: full state (JSON v2). Written at compaction or shutdown. | App layer (must exist) |
| `{learning_terms_path}.wal` | WAL: append-only delta log for O(1) per-turn writes. | Engine (auto-created) |

**Requirements:**
- The snapshot file (`learning_terms_path`) must exist before `api_create`. The app layer should create it with at minimum: `{"schema":"nl_learn_terms_v2","terms":{}}`.
- The directory must be **writable** — the engine creates a `.wal` file alongside the snapshot for performance (append-only, no full rewrite per turn).
- The `.wal` file is an implementation detail. The app does not need to read or manage it. It is automatically compacted into the snapshot at shutdown or when it exceeds 500 lines.

**Why WAL:** Without WAL, every chat turn rewrites the entire JSON file + fsync. With WAL, each turn appends ~40 bytes to the `.wal` file. As the learning file grows (hundreds of phrases), this avoids O(N) disk I/O per turn.

### Query plan cache (`query_cache_path`)

When `query_cache_path` is set, the engine persists LLM query plan extractions to disk. A background worker calls the LLM after each data-related chat turn to extract the correct collection, operation, and filters. These plans are cached so that subsequent similar questions can build precise ELK queries.

| File | Purpose | Created by |
|------|---------|------------|
| `{query_cache_path}` | LLM query plan cache. One JSON line per plan. | Engine (auto-created when path is set) |

**Format:** append-only JSON lines:
```
{"q":"how many products sold this year","c":"carts","o":"count","f":[{"field":"created_at","op":"gte","value":"2026-01-01"}]}
```

| Field | Description |
|-------|-------------|
| `q` | Normalized user question |
| `c` | Collection name (chosen by LLM) |
| `o` | Operation: `count`, `list`, `sum`, `avg`, `aggregate`, `group_by`, `find_one` |
| `f` | Filters array: `[{"field":"...","op":"eq/gte/lt/contains","value":"..."}]` |

**Requirements:**
- The directory must be **writable**.
- The file is created automatically if it doesn't exist. The app does not need to pre-create it.
- If `query_cache_path` is not set (null/empty), the background worker still runs and records collection scores to the learning terms store, but query plans are only cached in memory (lost on restart).

**How it works:**
1. User asks a data question → foreground responds immediately using current scores
2. Background worker sends the question to the LLM with collection schemas
3. LLM returns a query plan (collection + operation + filters)
4. Plan is appended to the cache file + stored in memory
5. Next similar question → Phase 3 looks up the cached plan and builds a precise ELK query with filters

**Relationship to learning terms:**

| File | What it stores | Who reads it |
|------|---------------|-------------|
| `learning_terms_path` + `.wal` | Phrase→intent scores (e.g. "sold"→SC:carts: 8) | Phase 1: routing decisions |
| `query_cache_path` | Full query plans (collection + operation + filters) | Phase 3: ELK query building |

Both are written by the background worker. They store different data and are not linked by a shared key (see intent_routing.md Q&A for design discussion).

### SharedCollection — term vocabulary

When `shared_collection_json_path` is set, the engine builds a **term vocabulary** at startup from the registry's `alias` and `metadata.field_hints` fields. This vocabulary maps user-facing words to collections and fields for NL intent routing.

Example registry entry:
```json
{
  "collection": "products",
  "alias": "Product catalog",
  "elk": { "allow": true },
  "metadata": {
    "field_hints": {
      "category": "computer, phone, tablet",
      "price": "Unit price in VND",
      "sold_date": "Date the item was sold"
    }
  }
}
```

Generated vocab entries: `products`, `product`, `catalog`, `category`, `computer`, `phone`, `tablet`, `price`, `sold`, `date`, etc. — each mapped back to the collection and field they came from.

The vocab is read-only in memory, rebuilt on every startup. No additional files.

### ELK indexing — data sync from MongoDB

When `mode=3` (MONGO_REDIS_ELK) and `shared_collection_json_path` is set, the engine indexes MongoDB documents to Elasticsearch for analytics and search queries.

#### Current behavior: cold backfill only

At startup, the engine walks every SharedCollection with `elk.allow: true` and indexes all existing Mongo documents to ELK. This is a one-time batch operation.

```
engine start → cold backfill → all Mongo docs → ELK indexes
engine running → new Mongo writes → NOT synced to ELK
engine restart → cold backfill again → fresh full sync
```

**Limitation:** ELK data becomes stale during the engine's lifetime. New documents added to Mongo after startup are not indexed until the next restart.

#### Live sync (change streams) — future

MongoDB change streams can push real-time updates to ELK automatically. This requires:

| Requirement | Details |
|-------------|---------|
| **MongoDB replica set** | Change streams require a replica set (or sharded cluster). Standalone `mongod` does not support them. Even a single-node replica set works: `mongod --replSet rs0` then `rs.initiate()`. |
| **MongoDB 3.6+** | Change streams were introduced in 3.6. Current versions (6.x, 7.x) fully support them. |
| **Resume tokens** | The engine must persist resume tokens to survive restarts without missing events. Stored in a dedicated Mongo collection (e.g. `_elk_sync_state`). |
| **Permissions** | The Mongo user needs `changeStream` and `find` privileges on the watched collections. |
| **Network stability** | Change streams are long-lived cursors. Network drops require reconnection + resume from last token. |

Env vars (reserved, not implemented):
- `M4_SHARED_COLLECTION_STREAM=1` — enable change stream sync
- `M4_SHARED_COLLECTION_FILE_POLL=1` — alternative: poll file mtime for changes

#### Workaround: periodic restart

Until live sync is implemented, the app layer can restart the engine periodically to trigger a fresh cold backfill:

```python
# In a cron job or admin endpoint:
api_destroy(ctx)
ctx = api_create(opts_json)  # cold backfill runs automatically
```

This re-indexes all data from Mongo to ELK. For small datasets (< 10K docs), this takes seconds and is practical for daily or hourly refresh.

#### Incremental sync (`schedule_refresh`)

**Problem:** Cold backfill re-indexes ALL docs from ALL collections on every restart. With 30 collections and millions of records, this is slow and wasteful.

**Solution:** Track last indexed `_id` per collection. On next sync, only query `_id > saved_id`. MongoDB ObjectIDs are time-ordered — no dependency on user-provided timestamp fields.

**Option:**

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `schedule_refresh` | boolean | `false` | Enable incremental ELK sync. When `true`, the engine tracks sync state and only indexes new docs since last sync. May be extended with additional options in the future. |

**Sync state file:**

Stored alongside the learning terms file (same data directory). Simple format — one `_id` per collection:

```
# .elk_sync_state (auto-created by engine)
products:69c73b68fc676cc67dd38322
carts:69c73daf6649be1729893602
product_categories:69c742356bf2e0a32c96ba88
```

**How it works:**

```
Engine start (schedule_refresh=true)
  │
  ├── Read .elk_sync_state file
  │
  ├── For each elk.allow collection:
  │   ├── If no saved _id → full backfill → save last _id
  │   └── If saved _id exists → query: { _id: { $gt: ObjectId("saved") } }
  │       ├── Batch 500 docs at a time → index to ELK
  │       └── Update saved _id to last doc processed
  │
  └── Engine running → next restart repeats (only new docs)

Engine start (schedule_refresh=false, default)
  │
  └── Full cold backfill every time (current behavior)
```

**Why `_id`:**
- Every Mongo doc has `_id` (guaranteed, no trust needed)
- ObjectIDs encode creation timestamp (time-ordered)
- `_id > last_saved_id` = all docs created after last sync
- No dependency on `createdAt`/`updatedAt` fields
- Works with any collection schema

**Scaling:**
- 30 collections × 1M records: full backfill = ~30M operations (slow, minutes)
- Same with incremental: 1K new docs since last sync = ~1K operations (fast, seconds)
- `batchSize=500` keeps memory flat

**Limitation:** `_id`-based sync only catches **new** docs (inserts). Updated existing docs (same `_id`, modified fields) are not re-indexed. For update-heavy workloads, a future `schedule_refresh` extension could add a periodic full re-sync (e.g. weekly) alongside the incremental daily sync.

#### BSON → JSON conversion

The engine converts MongoDB BSON documents to plain JSON for Elasticsearch. BSON extended types are unwrapped:

| BSON type | JSON output |
|-----------|-------------|
| `ObjectId` | Skipped (ES uses `_id` from URL) |
| `Int32` / `Int64` | Plain number: `42` |
| `Double` | Plain number: `3.14` |
| `String` | JSON string: `"hello"` |
| `Boolean` | `true` / `false` |
| `Date` | ISO 8601: `"2026-04-08T10:00:00.000Z"` |
| `Document` | Nested JSON object |
| `Array` | JSON array |
| `Null` | `null` |
| `Binary` / `Regex` / others | `null` (unsupported, skipped) |

The `_id` field is excluded from the JSON body — Elasticsearch receives the document ID via the URL path (`PUT /{index}/_doc/{id}`).

---

## API 2: `api_chat` (unified sync + stream)

One chat turn: push user message, optionally short-circuit via Redis RAG, call LLM, persist turn.

```c
int api_chat(api_context_t *ctx,
             const char *tenant_id,       // NULL → "default"
             const char *user_id,         // NULL → "default"
             const char *user_message,
             const char *context_json,    // NULL = no context. User/session info injected as [CONTEXT] in prompt.
             char *bot_reply_out,         // buffer for full reply
             size_t out_size,
             api_stream_token_cb stream_cb,  // NULL = sync, non-NULL = stream
             void *stream_userdata);
```

### Prerequisites
- `api_create` succeeded
- Ollama running (for LLM calls)
- `bot_reply_out` must be allocated when `stream_cb == NULL`

### Sync mode (`stream_cb == NULL`)

```c
char reply[32768];
int rc = api_chat(ctx, "tenant_a", "user_1", "What is Saigon?",
                  reply, sizeof(reply), NULL, NULL);
if (rc == 0)
    printf("Bot: %s\n", reply);
```

### Stream mode (`stream_cb != NULL`)

```c
void on_token(const char *token, const char *msg_id, int done, void *ud) {
    if (!done)
        printf("%s", token);  // partial token (UTF-8 safe)
    else
        printf("\n");         // stream finished
}

char reply[32768];
int rc = api_chat(ctx, "tenant_a", "user_1", "What is Saigon?",
                  reply, sizeof(reply), on_token, NULL);
// reply also contains the full assembled text after stream completes
```

### Data flow

```
api_chat(ctx, tenant, user, msg, context_json, buf, sz, cb, ud)
  │
  ├── Inject [CONTEXT] block from context_json (if set)
  ├── Normalize tenant/user → "default" if empty
  ├── Validate charset
  ├── Generate epoch_ms timestamp
  ├── Get/create session ring buffer
  ├── Push user message (source=MEMORY)
  │
  ├── [RAG path if vector_search enabled]
  │   ├── Embed user message (hash or Ollama)
  │   ├── Redis L2 search (5 results, score >= 0.85)
  │   └── If hit → return cached reply, skip LLM
  │
  ├── [LLM path]
  │   ├── ctx_build_prompt (topic, knowledge, persona, history)
  │   ├── model_switch (smart_topic → temperature + model)
  │   ├── stream_cb == NULL:
  │   │   └── ai_agent_complete_chat (Groq/Cerebras/Gemini/Ollama)
  │   └── stream_cb != NULL:
  │       └── Spawn pthread → ollama_query_stream → tokens via cb
  │           (UTF-8 accumulator ensures no broken multi-byte chars)
  │
  ├── geo_authority post-chat (conflict detection)
  ├── Push assistant to session
  ├── Embed turn + detect language
  ├── engine_append_turn (Mongo persist)
  └── Record learning cue (if enabled)
```

### Message sources (`api_stats_t.last_reply_source`)

| Code | Constant | Meaning |
|------|----------|---------|
| `'M'` | `API_SOURCE_MEMORY` | Current session |
| `'R'` | `API_SOURCE_REDIS` | Redis vector cache hit |
| `'G'` | `API_SOURCE_MONGODB` | Loaded from MongoDB |
| `'O'` | `API_SOURCE_OLLAMA` | Local Ollama |
| `'C'` | `API_SOURCE_CLOUD` | Hosted LLM (Groq/Cerebras/Gemini) |

---

## API 3: `api_load_chat_history`

Load persisted chat history from MongoDB into the in-memory session ring buffer.

```c
int api_load_chat_history(api_context_t *ctx,
                          const char *tenant_id,  // NULL → "default"
                          const char *user_id);   // NULL → tenant-wide (legacy)
```

### Prerequisites
- MongoDB configured and connected (ONLY_MEMORY returns 0 as no-op)

### Example

```c
// Load history for a specific user
int rc = api_load_chat_history(ctx, "tenant_a", "user_1");
if (rc == 0) {
    // History is now in the session ring buffer
    // Iterate with api_get_history_message
}
```

### Data flow

```
api_load_chat_history(ctx, tenant, user)
  ├── Normalize tenant → "default" if empty
  ├── Get/create session, clear existing ring buffer
  ├── Redis L1 cache check (fast path)
  ├── MongoDB query (sort by createdAt DESC, limit context_batch_size)
  └── Fill ring buffer oldest-first (source=MONGODB)
```

---

## API 4: `api_get_history_message`

Read a single message from the session ring buffer by index (0 = oldest).

```c
int api_get_history_message(api_context_t *ctx, int index,
                            char *role_buf, size_t role_size,
                            char *content_buf, size_t content_size,
                            char *source_out,
                            char *ts_buf, size_t ts_size,
                            char *llm_model_out, size_t llm_model_cap);
```

### Prerequisites
- Call `api_load_chat_history` or `api_chat` first (to populate session)

### Example: iterate all messages

```c
char role[32], content[4096], ts[24], model[160];
char source;

for (int i = 0; ; i++) {
    if (api_get_history_message(ctx, i,
            role, sizeof(role), content, sizeof(content),
            &source, ts, sizeof(ts), model, sizeof(model)) != 0)
        break;  // no more messages

    printf("[%c] %s %s: %s\n", source, ts, role, content);
    if (model[0])
        printf("    (model: %s)\n", model);
}
```

### Output fields

| Parameter | Content | Example |
|-----------|---------|---------|
| `role_buf` | `"user"` or `"assistant"` | `"assistant"` |
| `content_buf` | Message text (null-terminated) | `"Saigon is..."` |
| `source_out` | `API_SOURCE_*` char | `'O'` (Ollama) |
| `ts_buf` | Display timestamp | `"10:07:48.117"` |
| `llm_model_out` | Completion route | `"groq:llama-3.1-8b"` |

---

## API 5: `api_greet`

Generate a welcome message when the user opens the chat.

```c
int api_greet(api_context_t *ctx,
              const char *tenant_id, const char *user_id,
              const char *context_json,      // user info: {"name":"Ky","role":"ADMIN"}
              const char *greet_opts_json,   // options (NULL = defaults)
              char *reply_out, size_t out_size);
// Returns: 0 = greeting generated, 1 = no greeting needed, -1 = error
```

### Prerequisites
- `api_create` succeeded
- For `CHAT` response type: LLM available (cloud or Ollama)

### Options (`greet_opts_json`)

```json
{
  "condition": "TODAY",
  "response_type": "CHAT",
  "custom_prompt": null
}
```

| Key | Values | Default | Description |
|-----|--------|---------|-------------|
| `condition` | `"ALWAYS"`, `"TODAY"`, `"WEEK"`, `"HOUR"`, `"SESSION"` | `"TODAY"` | When to greet |
| `response_type` | `"CHAT"`, `"TEMPLATE"`, `"SILENT"` | `"CHAT"` | How to generate |
| `custom_prompt` | string or null | null | Override greeting prompt |

**Conditions:**

| Value | Greet if... |
|-------|-------------|
| `ALWAYS` | Always |
| `TODAY` | User hasn't chatted in the last 24 hours |
| `WEEK` | User hasn't chatted in the last 7 days |
| `HOUR` | Last chat was > 1 hour ago |
| `SESSION` | Session ring is empty (no history loaded) |

**Response types:**

| Value | How | Speed |
|-------|-----|-------|
| `CHAT` | LLM via ai_agent (Gemini/Groq/Ollama) with persona | ~1-3s |
| `TEMPLATE` | Instant: "Chào {name}! Hôm nay cần gì?" | Instant |
| `SILENT` | Empty reply — app handles display | Instant |

If `CHAT` fails (LLM down), auto-falls back to `TEMPLATE`.

### Example: LLM-powered greeting

```c
char greeting[4096];
int rc = api_greet(ctx, "default", "user_1",
    "{\"display_name\":\"Ky\",\"role\":\"Engineer\"}",
    "{\"condition\":\"TODAY\",\"response_type\":\"CHAT\"}",
    greeting, sizeof(greeting));

if (rc == 0)
    printf("Bot: %s\n", greeting);  // "Chào anh Ky! Hôm nay code gì?"
else if (rc == 1)
    printf("(already chatted today)\n");
```

### Example: instant template greeting

```c
int rc = api_greet(ctx, "default", "user_1",
    "{\"display_name\":\"Admin\"}",
    "{\"response_type\":\"TEMPLATE\"}",
    greeting, sizeof(greeting));
// → "Chào Admin! Hôm nay cần gì?"
```

### Example: Python

```python
rc = lib.api_greet(ctx, b"default", user_id,
    json.dumps({"display_name": "Admin", "role": "ADMIN"}).encode(),
    json.dumps({"condition": "TODAY", "response_type": "CHAT"}).encode(),
    reply, 4096)
if rc == 0:
    send_to_frontend(reply.value.decode())
```

### Data flow

```
api_greet(ctx, tenant, user, context, opts, reply, size)
  │
  ├── Parse condition + response_type from opts
  ├── Check condition (last_activity vs threshold)
  │   └── User chatted recently → return 1 (skip)
  │
  ├── response_type=TEMPLATE → "Chào {name}!" → return 0
  ├── response_type=SILENT → empty reply → return 0
  └── response_type=CHAT
      ├── Build prompt: [GREETING] + [CONTEXT] + persona
      ├── ai_agent_complete_chat (same routing as api_chat)
      │   ├── Cloud pool (Gemini → Groq → Cerebras)
      │   └── Ollama fallback
      ├── Push greeting to session as assistant message
      └── return 0
```

---

## API 6: `api_get_stats`

Fill a stats snapshot with connection health, counters, and last reply metadata.

```c
void api_get_stats(api_context_t *ctx, api_stats_t *out);
```

### Prerequisites
- `api_create` succeeded

### Example

```c
api_stats_t st;
api_get_stats(ctx, &st);

printf("Mongo: %s, Redis: %s, Ollama: %s\n",
       st.mongo_connected ? "up" : "down",
       st.redis_connected ? "up" : "down",
       st.ollama_connected ? "up" : "down");
printf("Processed: %llu, Errors: %llu\n", st.processed, st.errors);

if (st.last_reply_source)
    printf("Last reply: source=%c wire=%u model=%s\n",
           st.last_reply_source, st.last_chat_wire, st.last_llm_model);
```

### `api_stats_t` fields

| Field | Type | Description |
|-------|------|-------------|
| `memory_bytes` | `uint64_t` | Estimated session ring buffer footprint |
| `mongo_connected` | `int` | 1 if MongoDB connected |
| `redis_connected` | `int` | 1 if Redis connected |
| `elk_enabled` | `int` | 1 if ELK mode active |
| `elk_connected` | `int` | 1 if Elasticsearch reachable |
| `ollama_connected` | `int` | 1 if Ollama running (health check) |
| `error_count` | `uint64_t` | Total errors |
| `warning_count` | `uint64_t` | Total warnings |
| `processed` | `uint64_t` | Successfully processed turns |
| `errors` | `uint64_t` | Engine-level errors |
| `mongoc_linked` | `int` | 1 if compiled with USE_MONGOC |
| `last_reply_source` | `char` | `API_SOURCE_*` from last turn (0 if none) |
| `last_chat_wire` | `unsigned` | `API_CHAT_WIRE_*` from last turn |
| `last_llm_model` | `char[160]` | Completion route (e.g. `"groq:llama-3.1-8b"`) |

---

## API 8: `api_geo_atlas_import_row`

Insert one geo_atlas row for runtime data enrichment (e.g. from frontend CSV upload).

```c
int api_geo_atlas_import_row(api_context_t *ctx,
                             const char *tenant_id,
                             const char *name,             // required
                             const char *name_normalized,   // NULL → use name
                             const char *district,
                             const char *axis,
                             const char *category,
                             const char *city,
                             const float *vector,           // NULL → no Redis index
                             size_t vector_dim,             // 0 if no vector
                             const char *embed_model_id,
                             const char *source,            // NULL → "seed"
                             const char *verification_status, // NULL → "verified"
                             double trust_score);           // clamped 0..1
```

### Prerequisites
- MongoDB configured (for storage)
- Redis configured (optional, for geo vector index)

### Example: import a landmark

```c
int rc = api_geo_atlas_import_row(ctx,
    "default",           // tenant_id
    "Ben Thanh Market",  // name
    "ben_thanh_market",  // name_normalized
    "District 1",        // district
    NULL,                // axis
    "Landmark",          // category
    "Ho Chi Minh City",  // city
    NULL, 0,             // no vector
    NULL,                // embed_model_id
    "csv_import",        // source
    "verified",          // verification_status
    0.95);               // trust_score
```

### Data flow

```
api_geo_atlas_import_row(ctx, tenant, name, ...)
  ├── Build storage_geo_atlas_doc_t
  ├── storage_geo_atlas_insert_doc → MongoDB
  └── If vector provided + Redis connected:
      └── storage_geo_redis_index_landmark (persistent TTL)
```

---

## Complete Integration Example

```c
#include "api.h"
#include <stdio.h>
#include <string.h>

/* Stream callback */
void on_token(const char *token, const char *msg_id, int done, void *ud) {
    (void)msg_id; (void)ud;
    if (!done) printf("%s", token);
    else printf("\n");
}

int main(void) {
    /* 1. Create context with JSON */
    api_context_t *ctx = api_create(
        "{"
        "  \"mode\": 2,"
        "  \"default_persona\": \"You are a helpful assistant.\","
        "  \"context_batch_size\": 10,"
        "  \"debug_modules\": [\"ai_agent\"],"
        "  \"lanes\": ["
        "    {\"key\": \"TECH\", \"model\": \"codellama\", \"inject\": \"You are a code expert.\"}"
        "  ]"
        "}"
    );
    if (!ctx) return 1;

    const char *user_ctx = "{\"display_name\":\"Ky\",\"role\":\"Engineer\"}";

    /* 2. Greeting (first visit today) */
    char greeting[4096];
    int gr = api_greet(ctx, "default", "user_1", user_ctx,
                       "{\"condition\":\"TODAY\",\"response_type\":\"CHAT\"}",
                       greeting, sizeof(greeting));
    if (gr == 0)
        printf("Greeting: %s\n", greeting);

    /* 3. Load previous history */
    api_load_chat_history(ctx, "default", "user_1");

    /* 4. Print loaded history */
    char role[32], content[4096], ts[24];
    char src;
    for (int i = 0; ; i++) {
        if (api_get_history_message(ctx, i,
                role, sizeof(role), content, sizeof(content),
                &src, ts, sizeof(ts), NULL, 0) != 0)
            break;
        printf("[%c] %s %s: %.80s...\n", src, ts, role, content);
    }

    /* 5. Sync chat with user context */
    char reply[32768];
    if (api_chat(ctx, "default", "user_1", "Hello!",
                 user_ctx, reply, sizeof(reply), NULL, NULL) == 0)
        printf("Sync reply: %s\n", reply);

    /* 6. Stream chat with user context */
    printf("Stream reply: ");
    api_chat(ctx, "default", "user_1", "Tell me about Saigon.",
             user_ctx, reply, sizeof(reply), on_token, NULL);

    /* 6. Check stats */
    api_stats_t st;
    api_get_stats(ctx, &st);
    printf("Processed: %llu, Source: %c, Model: %s\n",
           st.processed, st.last_reply_source, st.last_llm_model);

    /* 7. Cleanup */
    api_destroy(ctx);
    return 0;
}
```

---

## Model Lane IDs

| Constant | Value | Use |
|----------|-------|-----|
| `M4_API_MODEL_LANE_DEFAULT` | 0 | General purpose |
| `M4_API_MODEL_LANE_EDUCATION` | 1 | Education queries |
| `M4_API_MODEL_LANE_BUSINESS` | 2 | Business queries |
| `M4_API_MODEL_LANE_TECH` | 3 | Technical / code queries |
| `M4_API_MODEL_LANE_CHAT` | 4 | Casual conversation |

Set via `api_options_t.default_model_lane` at create time. When `smart_topic_opts` is enabled, intent is auto-detected and merged with the lane.
