# Embed model change — sync, provenance, async migration

When **`OLLAMA_DEFAULT_MODEL`**, **`OLLAMA_EMBED_MODEL`**, **`vector_gen_backend`**, or **`M4_EMBED_FALLBACK_CUSTOM`** change, **stored vectors are not automatically rewritten**. Redis L2 and Mongo must stay **dimensionally consistent** per index. This doc ties **code** (**`embed.c`**, **`storage.c`**) to **ops**.

---

## 1. Single logical shape (provenance under `metadata` / geo top-level)

**New writes** (`USE_MONGOC=1`) include:

| Field | Location (`bot.records`) | Location (`geo_atlas`) | Meaning |
|--------|---------------------------|-------------------------|---------|
| **`embed_schema`** | `metadata.embed_schema` | top-level | Integer; **1** = current provenance block. Bump when you redefine fields or semantics. |
| **`vector_dim`** | `metadata.vector_dim` | top-level | Length of **`vector`** at write time (int32). |
| **`embed_family`** | `metadata.embed_family` | top-level | **`custom`** = hash (**`VECTOR_GEN_MODEL_ID`**), **`ollama`** = HTTP embed, **`legacy`** = vector present but **`model_id`** was empty (old rows). |
| **`model_id`** | `metadata.model_id` (existing) | **`embed_model_id`** | Human-readable embed model tag or **`m4-vector-hash-v1-384`**. |

**Rule:** treat **`(embed_family, vector_dim, model_id)`** as the identity of the embedding **space**. RAG search must only compare vectors that share the same triple (or you accept garbage similarity).

**Cursor rule:** **`.cursor/rules/embed-vector-metadata.mdc`** — production code must **not** use **`vector`** without the **`metadata`** record (same shape as turn objects); legacy exceptions only inside migration.

**Legacy documents** (before this change): may lack **`embed_schema`** / **`vector_dim`** / **`embed_family`**. Migration jobs should **infer** where possible:

- **`vector_dim`** ← `array length` of `vector`.
- **`embed_family`** ← `custom` if `metadata.model_id == VECTOR_GEN_MODEL_ID` else `ollama` if non-empty `model_id` else `legacy`.
- **`embed_schema`** ← set **1** on first touch.

---

## 2. When embed “type” or model changes

### 2a. Config-only (same backend, new Ollama tag)

Example: switch embed model from tag A → B, same dimension (rare) or different dimension (common).

1. **Flush or partition Redis** L2 keys that used the old dimension (in-memory stub: TTL expiry; real RediSearch: drop/recreate index or separate index name per **`embed_schema` + `vector_dim`**).
2. **Mongo:** query stale docs, e.g.  
   `{ "metadata.model_id": { "$ne": "<new_resolved>" } }`  
   plus filter on **`metadata.vector_dim`** if you mix dims.
3. **Re-embed** from canonical text: for **`bot.records`** use **`turn.input`**; for **geo** use **`name`** (and optional context policy).

### 2b. Backend switch (custom ↔ Ollama) or fallback **`M4_EMBED_FALLBACK_CUSTOM`**

- **Custom** is fixed **384**; **Ollama** dim is model-dependent. They **must not** share one Redis KNN index.
- After switch, run migration in **batches** (async): for each doc, compute new vector via **`m4_embed_text`** with the **target** policy, then **`$set`** vector + metadata fields + re-index Redis.

### 2c. Inject missing metadata on old rows

Async worker pseudo-steps:

1. **Find:** `{ $or: [ { "metadata.embed_schema": { $exists: false } }, { "metadata.vector_dim": { $exists: false } } ] }` (and `vector` exists).
2. **Patch:** set **`embed_schema`**, **`vector_dim`**, **`embed_family`**, and **`metadata.model_id`** if empty (best-effort from env / logs; else leave **`legacy`**).
3. **Optional second pass:** if policy says “everything must be Ollama dim D”, queue docs where **`vector_dim != D`** for full re-embed.

---

## 3. Recommended solution: **one pthread worker in C** (centralize, safe, less misleading)

**Why not “manage everything” in scattered scripts/handlers:** duplicate embed policy (Python vs C), ambiguous logs (“success” when only metadata changed), mixed Redis dims. **Better:** a **single background owner** — **one consumer thread + job queue** in c-lib, using **`m4_embed_*`** and Mongo updates only from that path.

### 3.1 Principles

| Goal | How |
|------|-----|
| **Centralize** | Migration / re-embed **only** inside the worker; **`api_chat`** keeps normal turn writes. Worker calls **`m4_embed_text`** (never a second ad-hoc resolver). |
| **Safe use** | `pthread_mutex_t` + `pthread_cond_t`; **one** Ollama embed in flight (or a semaphore of 1); **`engine_destroy`** sets **stop** flag and **`pthread_join`** before freeing **`storage_ctx`**. |
| **Less misleading** | Log tag **`[EMBED_MIGRATION]`** + **job_id** + **tenant** + **phase** (`scan` / `embed` / `mongo_patch` / `redis_skip` / `error`). Do not reuse chat debug lines for migration. |
| **Dynamic** | Bounded ring queue; coalesce jobs (e.g. multiple “tenant X” → one sweep); env **`M4_EMBED_MIGRATION_BATCH`**, **`M4_EMBED_MIGRATION_INTERVAL_MS`**, optional **`M4_EMBED_MIGRATION_ON_START`**. |

### 3.2 Triggers (two entry points, same queue)

1. **Engine init** — optional: if **`M4_EMBED_MIGRATION_ON_START=1`** or **`api_options_t.embed_migration_autostart`** / **`engine_config_t.embed_migration_autostart`**, enqueue provenance-only for tenant **`default`**. **Default off** so production is explicit.
2. **At init** — `api_options_t.embed_migration_autostart = 1` at `api_create` pushes the same jobs as (1). Internal function; no separate public API needed.

Both push identical **job structs** onto one FIFO; the worker is the **sole** consumer.

### 3.3 Worker loop (sketch)

- Wait on queue → pop **job** (`PROVENANCE_ONLY` | `REEMBED_TURNS` | `REEMBED_GEO` | …).
- **Discover** via Mongo cursor (batched).
- **Re-embed:** `turn.input` or geo **`name`** → **`m4_embed_text`** with **current** engine policy (hold **`engine_t*`** or snapshot **`m4_embed_options_t`** at enqueue time if you need frozen target).
- **`updateOne`:** `vector`, **`metadata.model_id`**, **`embed_family`**, **`vector_dim`**, **`embed_schema`**.
- **Redis:** patch or skip per **`.cursor/embed_migration.md` §2** (dim mismatch); log **`redis_skip`** when not safe.
- Throttle; on HTTP 429 sleep **`Retry-After`**.

**`USE_MONGOC=0`:** worker returns early / no-op with clear log — no fake “completed”.

### 3.4 What not to do

- Unbounded **pthread per request**.
- Synchronous full-tenant re-embed **inside** **`api_chat`**.
- Python re-embed for c-lib data **without** parity tests against **`m4_embed_*`**.

### 3.5 Alternative: external worker (Python / cron)

Same Mongo filters as §2; risk of **policy drift**. Prefer **C worker** when the app already links c-lib + mongoc.

**Idempotency:** optional **`metadata.embed_migrated_at`** or hash of **`turn.input`**.

---

## 4. Code map (do not fragment)

| Concern | Location |
|---------|-----------|
| Policy (Ollama vs custom, fallback) | **`src/embed.c`**, **`include/embed.h`** |
| Hash primitive | **`src/vector_generate.c`** |
| Persisted provenance on insert | **`storage_bson_append_embed_provenance`** in **`src/storage.c`** (`bot.records` **metadata**, **geo_atlas** top-level) |
| Turn text for re-embed | **`turn.input`** in **`bot.records`** |
| **Migration queue + pthread** | **`src/embed_worker.c`**, **`include/embed_worker.h`** — single consumer, **`engine_init`** (opt-in via `api_options_t.embed_migration_autostart`), see **§3** |

---

## 5. Cross-links

- **`.cursor/vector_generate.md`** — backends and env.
- **`.cursor/default_models.md`** — when **`OLLAMA_DEFAULT_MODEL`** / **`OLLAMA_EMBED_MODEL`** change.
- **`.cursor/mongo.md` §0** — full **`metadata`** shape.
