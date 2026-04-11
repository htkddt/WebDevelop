# Mini AI Switch — Smart topic & model switching

Design for a **smart topic** engine option that can switch between lightweight models (Tiny, B2, …) and full models (Ollama, etc.), with behavior that depends on execution mode (memory / Redis / Mongo / Mongo+Redis) and optional pre-query / connection pooling. Includes a logging template for connection errors and crashes.

**Main chat model** (when not using mini path): resolved by **`model_switch`** + **`OLLAMA_*`** / **`OLLAMA_DEFAULT_MODEL`** — **`.cursor/default_models.md`**, **`.cursor/model_switch.md`**. **Mini model** names: **`smart_topic_options_t.model_tiny` / `model_b2`** or env (see **`M4ENGINE_SMART_TOPIC_MODEL_TINY`** in `python_ai`); if unset, **`smart_topic.c`** uses Ollama discovery / defaults — align ops docs with actual **`smart_topic.c`** when changing pulls.

---

## 1. Initial engine option: Smart topic

Use the **same config type** as the main engine (`engine_config_t` or extended `api_options_t`). Add a nested struct for smart-topic / mini-AI switch.

### 1.1 Config shape (conceptual)

```c
/* Library / model size tier for mini AI. */
typedef enum {
    MINI_AI_TYPE_TINY,   /* Smallest, fastest (e.g. tinyllama, 1B). */
    MINI_AI_TYPE_B2,     /* Small (e.g. ~2–3B). */
    MINI_AI_TYPE_SMALL,  /* Optional: small but stronger. */
    MINI_AI_TYPE_COUNT
} mini_ai_type_t;

/* Default Mongo collection when mini_ai_collection is not set. */
#define SMART_TOPIC_COLLECTION_DEFAULT "smart_topic"

typedef struct smart_topic_options {
    bool enable;                    /* Master switch: use mini AI for topic routing / caching. */
    mini_ai_type_t library_type;   /* Tiny | B2 | ... which model tier to use. */
    /* Collection for mini_ai: if set (validated 1..63, [a-zA-Z0-9_]), use it; else use smart_topic. */
    const char *mini_ai_collection;  /* NULL or "" => use SMART_TOPIC_COLLECTION_DEFAULT ("smart_topic"). */
    /* Reuse same base: execution_mode, mongo_uri, redis_host, etc. from engine_config_t. */
    /* Optional: model name override per type, e.g. "tinyllama", "phi2". */
    const char *model_tiny;
    const char *model_b2;
} smart_topic_options_t;
```

- **enable:** `true` = smart switch active (queue, cache, pre-query as below); `false` = no mini AI path.
- **library_type:** Which tier (Tiny, B2, …) to use when the switch selects the “mini” path.
- **mini_ai_collection:** When **passed** (non-NULL, non-empty, and valid): use this MongoDB collection for mini_ai persistence (topic cache, routing decisions, audit). When **not passed** (NULL or empty): use **smart_topic** as the collection name (i.e. `SMART_TOPIC_COLLECTION_DEFAULT` = `"smart_topic"`). Validation: same as log collection — length 1..63, chars `[a-zA-Z0-9_]` only.
- Config (Mongo URI, Redis host/port, execution_mode, etc.) is the **same** as the main engine so storage and cache behave consistently.

### 1.2 Collection: mini_ai vs smart_topic

| Caller provides              | Mongo collection used for mini_ai / smart_topic |
|-----------------------------|--------------------------------------------------|
| **Passes** `mini_ai_collection` (valid) | That collection (e.g. `mini_ai`, `my_mini_cache`). |
| **Does not pass** (NULL or `""`)       | **smart_topic** (`SMART_TOPIC_COLLECTION_DEFAULT`). |

So: **pass a collection for mini_ai when you want a dedicated collection; otherwise the library uses the default collection name `smart_topic`.**

Validation for `mini_ai_collection` when provided: same as `log_coll` (length 1..63, `[a-zA-Z0-9_]` only). Invalid or empty => fallback to `smart_topic`.

---

### 1.3 Behavior per execution mode (when `enable == true`)

#### a) MODE_ONLY_MEMORY (no Mongo)

- **Once enable:** Use an **in-memory queue** for incoming “smart topic” requests.
- **Mem keeps:** Keep the queue and recent results in RAM (e.g. circular buffer / fixed-size deque).
- No Redis/Mongo; all state is process-local. On crash, queue and cache are lost.

#### b) MODE_ONLY_MONGO (or when Redis not used)

- **If have MongoDB only:** Persist topic/routing decisions and optionally query results in Mongo using the **mini_ai collection** (if passed) or **smart_topic** (default).
- Cache reuse: can “save query from cache” only in the sense of reading back from Mongo by (tenant_id, query_hash, etc.) — slower than Redis.
- Optional: short TTL or “last N results” in memory, with Mongo as history/audit.

#### c) MODE_MONGO_REDIS (Redis + Mongo)

- **Redis ⇒ memory path:** Use Redis as the **query cache** for smart-topic / mini-AI responses.
  - Key pattern e.g. `m4:smart:{tenant_id}:{query_hash}`.
  - On cache hit: return from Redis (no Ollama/mini model call).
- **Save query from cache:** On cache miss, call mini model (Ollama or other); then **save** (query → response) into Redis with TTL (e.g. 300s).
- Mongo: async or batch write for audit / analytics into the **mini_ai** collection (if set) or **smart_topic** (same as main engine rules).

#### d) MODE_MONGO_REDIS_ELK

- Same as (c), plus:
  - ELK for auto-language and search if smart-topic metadata is indexed.
  - Logging (see §4) can go to Mongo/ELK when allowed by rule (no Mongo/ELK logs in MEMORY mode).

---

## 2. Public API (smart topic)

The **only** two functions exposed for smart topic / mini AI switch:

### 2.1 initial_smart_topic

Initialize the smart-topic subsystem (queue, cache, optional pre-query to Ollama/other models). Call once after engine init, or when enabling smart topic.

```c
/**
 * Initialize smart topic (mini AI switch). Uses opts for collection (else smart_topic), mode, and model tier.
 * Returns 0 on success, -1 on error (e.g. invalid opts, storage unavailable when mode requires it).
 */
int initial_smart_topic(const smart_topic_options_t *opts, struct storage_ctx *storage);
```

- **opts:** Config (enable, library_type, mini_ai_collection, model overrides). If `opts->enable == false`, may no-op and return 0.
- **storage:** Storage context from engine (for Mongo/Redis when execution mode uses them). Can be NULL in MODE_ONLY_MEMORY.
- **Return:** `0` = success; `-1` = error (invalid opts, init failed).

### 2.2 get_smart_topic

Get the current smart-topic result or state (e.g. resolved topic label, cache hit, or which model was selected). Use after a query or to inspect last outcome.

```c
/**
 * Get current smart topic state / result (e.g. topic label, cache hit, selected model).
 * out: buffer for result string; out_size: buffer size. Result is null-terminated.
 * Returns 0 on success, -1 if not initialized or error.
 */
int get_smart_topic(char *out, size_t out_size);
```

- **out:** Caller-provided buffer for the result (e.g. topic name, JSON snippet, or "cache_hit").
- **out_size:** Size of `out`; output is always null-terminated.
- **Return:** `0` = success; `-1` = not initialized, disabled, or error.

No other smart-topic functions are part of the public API; all other behavior (pre-query, logging, mode-specific cache) is internal.

---

## 3. When smart switch is enabled: pre-query / open models

- **Pre-query for “open” backends:** When `smart_topic_options.enable == true`, the library can **pre-open** or **pre-warm** connections to:
  - **Ollama** (127.0.0.1:11434): e.g. one health check or lightweight `/api/tags` at init.
  - **Other models:** Same idea for any other “open” type (e.g. another local endpoint or remote inference URL).
- **Purpose:** Detect connection failures early (at init or first use) and optionally **switch** to a fallback (e.g. mini model only, or “no AI” mode) instead of failing at first user query.
- **Implementation notes:**
  - Optional background thread or init step: `ollama_query(..., "Hi", out, size)` or a dedicated `ollama_ping()`.
  - If pre-query fails: set internal state “Ollama unavailable”; on chat, either retry once or use mini/cache-only path and log (see §3).

---

## 4. Logging template (warnings, errors, connection, crash)

Use a **single template** so all branches (engine, storage, Ollama, Redis, Mongo) log in a consistent way. Rule: in MEMORY mode do **not** write logs to Mongo/ELK (counters only); in other modes logs can go to Mongo/ELK per rule.

### 4.1 Log levels and when to use

| Level   | When |
|--------|------|
| **ERROR** | Connection failure (Ollama, Redis, Mongo, ES), invalid response, crash path, unrecoverable state. |
| **WARN**  | Timeout, retry exhausted, fallback taken (e.g. cache-only), pre-query failed. |
| **INFO**  | Smart switch enabled, pre-query success, model selected (Tiny/B2/Ollama). |
| **DEBUG** | Cache hit/miss, queue depth, internal state (only if `debug_mode`). |

### 4.2 Template (format)

```
[LEVEL] component context message (key=value ...)
```

- **LEVEL:** `ERROR` | `WARN` | `INFO` | `DEBUG`
- **component:** `engine` | `ollama` | `redis` | `mongo` | `storage` | `smart_topic` | `dispatcher`
- **context:** Short context (e.g. `init`, `chat`, `batch`, `pre_query`)
- **message:** Human-readable one line
- **key=value:** Optional: `errno`, `host`, `port`, `tenant_id`, `file`, `line`

### 4.3 Example lines

```
[ERROR] ollama connection_failed host=127.0.0.1 port=11434 errno=61 (__FILE__:__LINE__)
[WARN]  smart_topic pre_query Ollama unreachable; using cache-only (__FILE__:__LINE__)
[ERROR] redis connection_failed host=127.0.0.1 port=6379 (__FILE__:__LINE__)
[WARN]  storage mongo_timeout tenant_id=xyz (__FILE__:__LINE__)
[ERROR] engine crash_path signal=11 (__FILE__:__LINE__)
[INFO]  smart_topic enabled library_type=Tiny model=tinyllama
```

### 4.4 Where logs go (per execution mode)

| Mode                | stderr / file | Mongo/ELK (ai_logs etc.) |
|---------------------|---------------|---------------------------|
| MODE_ONLY_MEMORY    | Yes           | No (counters only)        |
| MODE_ONLY_MONGO     | Yes           | Yes (if opts set)         |
| MODE_MONGO_REDIS    | Yes           | Yes                       |
| MODE_MONGO_REDIS_ELK| Yes           | Yes                       |

Traceability: include `__FILE__` and `__LINE__` in log payload or in a structured field for segment-fault debugging (rule §9).

---

## 5. Enhance (future ideas — to be filled)

- **Model routing:** Route by topic complexity (e.g. simple → Tiny, complex → Ollama/B2).
- **Cost / latency SLA:** Prefer mini model when latency &gt; threshold or when tenant is “low tier”.
- **A/B testing:** Log which model served each request for later analysis.
- **Circuit breaker:** After N consecutive Ollama failures, stop calling Ollama for T seconds and use cache/mini only.
- **Metrics:** Expose smart_topic cache hit rate, queue depth, pre-query success in `api_get_stats` / debug monitor.
- *(Add more as you go.)*

---

## 6. Summary

| Item | Description |
|------|-------------|
| **Smart topic config** | `enable`, `library_type` (Tiny \| B2 \| …), same base config as engine. |
| **Collection** | Pass `mini_ai_collection` to use a dedicated Mongo collection; **otherwise use `smart_topic`** as default. Validated 1..63 chars, `[a-zA-Z0-9_]`. |
| **Memory mode** | In-memory queue + keep results in RAM; no Redis/Mongo. |
| **Redis** | Use Redis as query cache; “save query from cache” = write result to Redis on miss. |
| **Mongo** | Persist/audit; with Redis, cache in Redis and optionally batch to Mongo. |
| **Mongo+Redis** | Cache in Redis, persist to Mongo, ELK when available. |
| **Pre-query** | When smart switch enabled, pre-open/pre-check Ollama and other open models at init. |
| **Public API** | Only `initial_smart_topic(opts, storage)` and `get_smart_topic(out, out_size)`. |
| **Logging** | Template: `[LEVEL] component context message (key=value)`; ERROR/WARN for connection/crash; no Mongo/ELK logs in MEMORY mode. |

This file lives under `.cursor` as a design reference; implement in `include/` and `src/` when ready.
