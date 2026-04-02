# Public API (`api.h`)

The library’s stable entry points live in **api.h**. Engine, storage, and ollama headers are internal for advanced use.

## Core APIs

| # | Function | Purpose |
|---|----------|---------|
| 1 | `api_create(opts)` | Create context from options. `opts` may be NULL for defaults (MONGO mode). |
| 2 | `api_destroy(ctx)` | Destroy context and all resources. |
| 2b | `api_set_prompt_tag` / `api_clear_prompt_tags` | Runtime prompt sections: whitelist keys `API_PROMPT_TAG_SYSTEM_TIME`, `API_PROMPT_TAG_PERSONA`, `API_PROMPT_TAG_INSTRUCTIONS` (see `.cursor/ptomp.md`). |
| 2c | `api_set_model_lane(ctx, lane)` / `api_set_model_lane_key(ctx, key)` | Session lane key for `model_switch` (`M4_API_MODEL_LANE_*` or arbitrary string). See [.cursor/model_switch.md](../.cursor/model_switch.md). |
| 3 | `api_chat(ctx, tenant_id, user_id, user_message, bot_reply_out, out_size)` | One chat turn: append user → Ollama → append bot; returns bot text. Both **tenant_id** and **user_id** scope Mongo/RAG (`user_id` NULL/"" → `API_DEFAULT_USER_ID`). Match **api_load_chat_history(ctx, tenant_id, user_id)** so the prompt is not mixed across users. |
| 3b | `api_chat_stream(ctx, tenant_id, user_id, user_message, temp_message_id, cb, userdata)` | Same turn logic as `api_chat`, but Ollama tokens are delivered via `api_stream_token_cb` from an internal pthread; optional `temp_message_id` for Mongo correlation (see `.cursor/streaming.md`). After the stream, the library calls `engine_append_turn` → Mongo `bot.records` (when not MEMORY mode and storage is connected). If libcurl errors after partial bytes, a non-empty assembled reply is still persisted. NDJSON lines are parsed for `"response":"…"` or, as a fallback, `"message"` / `"content":"…"` (some model streams). |
| 4 | `api_query(ctx, prompt, out, out_size)` | Raw Ollama query (no chat history). |
| 5 | `api_get_stats(ctx, api_stats_t *out)` | Fill stats (memory, connections, error/warning counts). |
| 6 | `api_set_log_collection(ctx, db, coll)` | Override ai_logs DB/collection (validated names). |

## Modes and defaults

Use `api_options_t.mode` with one of:

| Constant | Value | Behaviour |
|----------|-------|-----------|
| **M4ENGINE_MODE_ONLY_MEMORY** | 0 | No Mongo/Redis/ELK. Logs only in-memory. |
| **M4ENGINE_MODE_ONLY_MONGO** | 1 | Mongo only; default URI `mongodb://127.0.0.1:27017`. |
| **M4ENGINE_MODE_MONGO_REDIS** | 2 | Mongo + Redis; default host/port. |
| **M4ENGINE_MODE_MONGO_REDIS_ELK** | 3 | Mongo + Redis + ELK. |

Optional overrides (mongo_uri, redis_host, log_db, log_coll, **smart_topic_opts**, **model_switch_opts**, etc.) are passed in `api_options_t`. Log db/coll are **validated** (1..63 chars, `[a-zA-Z0-9_]` only).

### Smart topic (intent-based temperature)

When `api_options_t.smart_topic_opts` is set and `enable == true`, `api_create` passes it to the engine; `engine_init` calls `initial_smart_topic()`. **api_chat** / **api_chat_stream** run one micro-query (e.g. TinyLlama) to classify intent and set Ollama temperature: **TECH** → 0.0, **CHAT** → 0.8, **DEFAULT** → 0.5. **api_query** uses the same helper for temperature only. See [smart_topic.md](smart_topic.md).

### RAG / turn vectors (`vector_gen_backend`)

**`api_options_t.vector_gen_backend`** is mapped into **`m4_embed_options_from_engine`** (**`include/embed.h`**) and drives **`m4_embed_for_engine`** for Redis L2 RAG and Mongo **`bot.records`** user-message vectors (not the chat LLM). **geo_learning** uses the same **`m4_embed_text`** with **`m4_embed_options_geo_env`** (see **`.cursor/vector_generate.md`**).

| Value | Constant | Behaviour |
|-------|----------|-----------|
| **0** (default) | **`API_VECTOR_GEN_CUSTOM`** | Deterministic **384-D** hash (`vector_generate.h`); **`embed_model_id`** **`m4-vector-hash-v1-384`**. |
| **1** | **`API_VECTOR_GEN_OLLAMA`** | Ollama **`/api/embed`**; optional **`vector_ollama_model`**. |

**Env (shared):** **`M4_EMBED_FALLBACK_CUSTOM=1`** — if backend is Ollama and the embed call fails, fall back to the custom hash (watch Redis index dimension consistency). **Geo-only:** **`M4_EMBED_BACKEND=custom`** forces hash for landmark names.

**Migration:** Existing Redis/Mongo vectors from Ollama used a model-dependent dimension. After switching to **CUSTOM**, re-index or flush L2 so query and index dimensions match (**384**).

### Model switch (lane → model + inject)

Set **`api_options_t.model_switch_opts`** to a **`model_switch_options_t`** (`model_switch.h`): **`lanes[]`** rows `{ key, model, inject }` (e.g. `"EDUCATION"` → your Qwen tag), optional **`fallback_model`**, **`MODEL_SWITCH_FLAG_MERGE_SMART_TOPIC_INTENT`** so **smart_topic** intent (`EDUCATION`, `TECH`, …) selects the same keys when the user did not force a session lane. Missing model in table → **`getenv("M4_MODEL_<KEY>")`**. Full spec: [.cursor/model_switch.md](../.cursor/model_switch.md).

### System time (calendar year / “today”)

By default (`disable_auto_system_time == 0` in `api_options_t`), each chat prompt includes `[SYSTEM_TIME: YYYY-MM-DD HH:MM]` from the **library process** local clock when you do **not** set `API_PROMPT_TAG_SYSTEM_TIME`. Set `disable_auto_system_time` non-zero only if the host will always supply time via `api_set_prompt_tag` (or you accept no clock in the prompt). Use an explicit tag when the user’s timezone differs from the server.

### Dynamic prompt tags (persona / time / extra instructions)

Use `api_set_prompt_tag(ctx, key, value)` with keys from `api.h` (`API_PROMPT_TAG_*`). Values are copied into the context (max `API_PROMPT_TAG_VALUE_MAX` bytes). Unknown keys return `-1`. Empty or NULL `value` clears that tag. Tags apply to the next `api_chat` / `api_chat_stream` prompt build via `ctx_build_prompt`. See `.cursor/ptomp.md` for semantics and compose order.

### Context batch size (history size)

`api_options_t.context_batch_size` (**batchContextSize**) is the single control for how many **history cycles** (user+bot pairs) are kept and used: it defines both the in-memory history buffer size and how many of those are sent to the LLM when building the prompt. There is no separate hardcoded 30 — use this option. Set to **0** to use the default (`API_CONTEXT_BATCH_SIZE_DEFAULT`, 30). Set to e.g. **10** to keep and use only the last 10 cycles.

**Tutorial (per-tenant/user in-process rings, idle eviction, history getters):** [tutorial_chat_l1_memory.md](tutorial_chat_l1_memory.md). Design detail: [`.cursor/chat_l1_memory.md`](../.cursor/chat_l1_memory.md).

### Geo authority (L1 in-memory cache)

Set **`api_options_t.geo_authority`** non-zero to enable the **geo authority** module (`.cursor/auth_geo.md`): seeded provincial names, optional CSV hot-load via **`api_geo_authority_load_csv`** (second column = optional `merged_into` parent for merger checks), prompt hint **`[GEO_AUTHORITY_HINT]`**, stderr audit of unknown tokens, and the **conflict detector** (`.cursor/conflict_detector.md`): numbered-list count vs user-stated expectation, merger consistency, optional **`[LOGIC_CONFLICT]`** note appended to the reply, and **`metadata.has_logic_conflict`** on stored turns when Mongo is enabled. Does not require Mongo by itself; **`geo_learning`** still adds verified landmarks when that worker is active.

**Legacy `geo_atlas` rows** (missing `country`, or `merged_into` without `merged` status): **`api_geo_atlas_migrate_legacy`** or one-time **`GEO_ATLAS_MIGRATE_LEGACY=1`** on **`engine_init`** (see `.cursor/geo_leanring.md` §17).

## Usage

```c
#include "api.h"

api_options_t opts = { 0 };
opts.mode = M4ENGINE_MODE_ONLY_MEMORY;
api_context_t *ctx = api_create(&opts);
if (!ctx) return -1;

char reply[32768];
if (api_chat(ctx, "default", API_DEFAULT_USER_ID, "Hello", reply, sizeof(reply)) == 0)
    printf("Bot: %s\n", reply);

api_stats_t stats;
api_get_stats(ctx, &stats);

api_destroy(ctx);
```

With log collection override (at create or later):

```c
api_options_t opts = { 0 };
opts.mode = M4ENGINE_MODE_ONLY_MONGO;
opts.mongo_uri = "mongodb://myhost:27017";
opts.log_db   = "mydb";
opts.log_coll = "my_logs";
api_context_t *ctx = api_create(&opts);
// or later: api_set_log_collection(ctx, "mydb", "my_logs");
```

With smart topic (intent-based temperature):

```c
#include "smart_topic.h"

smart_topic_options_t smart_topic_opts = {
    .enable = true,
    .library_type = MINI_AI_TYPE_TINY,
    .execution_mode = MODE_MONGO_REDIS_ELK,  /* match opts.mode */
    .mini_ai_collection = NULL,
    .model_tiny = NULL,
    .model_b2 = NULL
};
api_options_t opts = { 0 };
opts.mode = M4ENGINE_MODE_MONGO_REDIS_ELK;
opts.smart_topic_opts = &smart_topic_opts;
api_context_t *ctx = api_create(&opts);
```

## Python HTTP server (reference consumer)

The **python_ai** repo includes **`server/app.py`** (Flask): a ctypes wrapper around this API for browsers and scripts. It is **not** shipped inside the c-lib binary; it is the canonical example of how a consumer wires **`api_options_t`** (see **`python_ai/training/full_options.py`**) and calls **`api_*`**.

**Layout hazard:** **`engine_ctypes.ApiOptions`** must match **`api_options_t`** byte-for-byte (field order). If C adds fields and Python lags, **`api_create`** reads past the struct → **crashes (e.g. segfault on `/api/chat/stream`)**. After changing **`api.h`**, update **`python_ai/engine_ctypes.py`** and all **`ApiOptions(...)`** call sites.

| C API | HTTP (default **5000**, falls back to **5001** if busy) |
|-------|--------------------------------------------------------|
| `api_chat` | `POST /api/chat` (JSON body: `message`, optional `tenant_id` / `user`) |
| `api_chat_stream` | `POST /api/chat/stream` (SSE; body includes `message`, optional `temp_message_id`) |
| `api_load_chat_history` | `GET /api/history` (query: `tenant_id` or `user`, optional `reload=1`) |
| `api_get_stats` | `GET /api/stats` |
| Bulk **`api_geo_atlas_import_row`** | `POST /api/geo/import` (multipart CSV or JSON `csv_text`; default **no** Ollama — use query **`embed=1`** to compute embeddings; matches **`ollama.c`** `/api/embed` + `input`) |

Library discovery: set **`M4ENGINE_LIB`** to the built **`libm4engine.{dylib,so}`** path (same as CLI **`run_ai.py`**). Chat model resolution follows c-lib: **`OLLAMA_MODEL`** env, else **`OLLAMA_DEFAULT_MODEL`** in **`ollama.h`**. Server options and Mongo/Redis env overrides are documented in **`python_ai/server/app.py`** (module docstring) and **`python_ai/training/README.md`**.

If your workspace also has the **Vite** frontend (**`fe/`**), see **`docs/COMBINE_TUTORIAL.md`** at the monorepo root (when present).

## Rules (see c-lib rule.md)

- **Per-mode:** In MEMORY mode the library never uses MongoDB or ELK for logs.
- **Trust:** Log collection override only with validated `log_db` / `log_coll`. Do not accept unvalidated user input.
- **Smart topic:** See [smart_topic.md](smart_topic.md) for config, intent routing (TECH/CHAT/DEFAULT → temperature), and Ollama TINY model (e.g. `make ollama-tiny`).
