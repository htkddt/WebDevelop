# Smart topic (mini AI switch)

Smart topic adds **intent-based temperature routing** for Ollama queries: a **micro-query** to a small local model (e.g. TinyLlama) classifies the user message, then the main query uses a chosen temperature.

Design reference: [.cursor/smart_topic_ai_switch.md](../.cursor/smart_topic_ai_switch.md).

---

## 1. Overview

| Feature | Description |
|--------|-------------|
| **Init** | Optional at engine init: pass `smart_topic_opts` in `engine_config_t` or `api_options_t` with `enable == true`. `engine_init()` then calls `initial_smart_topic(opts, storage)`. |
| **Micro-query** | When smart_topic is enabled, before each main Ollama call the library sends a short prompt to the **TINY** model (e.g. TinyLlama) to classify intent. |
| **Intent → temperature** | **TECH** → `0.0` · **CHAT** → `0.8` · **EDUCATION** / **BUSINESS** → `0.35` · **DEFAULT** → `0.5`. |
| **Intent → model lane** | When **`model_switch`** merge flag is set, the same micro-reply selects the **lane key** (`EDUCATION`, `TECH`, …) for the dynamic `lanes[]` table (see [.cursor/model_switch.md](../.cursor/model_switch.md)). |
| **Collection** | Pass `mini_ai_collection` for a dedicated Mongo collection; otherwise the default collection name is **`smart_topic`**. |

---

## 2. Config (engine or API)

Use the same config type as the engine. Set `engine_config_t.smart_topic_opts` or `api_options_t.smart_topic_opts` to a pointer to `smart_topic_options_t` (from **smart_topic.h**).

| Field | Description |
|-------|-------------|
| `enable` | `true` = smart topic on (micro-query + temperature); `false` = off. |
| `library_type` | `MINI_AI_TYPE_TINY` (default), `MINI_AI_TYPE_B2`, or `MINI_AI_TYPE_SMALL`. Determines which model is used for the micro-query. |
| `execution_mode` | Same as engine (A/B/C/D); determines storage path for cache/persistence when implemented. |
| `mini_ai_collection` | Mongo collection name for mini_ai/smart_topic data. `NULL` or `""` ⇒ use **`smart_topic`**. Validated: 1..63 chars, `[a-zA-Z0-9_]`. |
| `model_tiny` / `model_b2` | Optional model name overrides (e.g. `"tinyllama"`, `"phi2"`). |

**Example (engine path, e.g. main.c):**

```c
#include "engine.h"
#include "smart_topic.h"

smart_topic_options_t smart_topic_opts = {
    .enable = true,
    .library_type = MINI_AI_TYPE_TINY,
    .execution_mode = MODE_MONGO_REDIS_ELK,
    .mini_ai_collection = NULL,   /* use "smart_topic" */
    .model_tiny = NULL,          /* default tinyllama */
    .model_b2 = NULL
};
engine_config_t config = {
    .mode = MODE_HYBRID,
    .execution_mode = MODE_MONGO_REDIS_ELK,
    /* ... mongo_uri, redis_host, etc. ... */
    .smart_topic_opts = &smart_topic_opts
};
engine_t *e = engine_create(&config);
engine_init(e);   /* calls initial_smart_topic when enable==true */
```

**Example (API path):**

```c
#include "api.h"
#include "smart_topic.h"

smart_topic_options_t smart_topic_opts = {
    .enable = true,
    .library_type = MINI_AI_TYPE_TINY,
    .execution_mode = MODE_MONGO_REDIS_ELK,
    .mini_ai_collection = NULL,
    .model_tiny = NULL,
    .model_b2 = NULL
};
api_options_t opts = { 0 };
opts.mode = M4ENGINE_MODE_MONGO_REDIS_ELK;
opts.smart_topic_opts = &smart_topic_opts;
api_context_t *ctx = api_create(&opts);
```

---

## 3. Public API (smart_topic.h)

| Function | Purpose |
|----------|--------|
| `initial_smart_topic(opts, storage)` | Initialize smart topic (collection, pre-query to Ollama). Called automatically from `engine_init()` when `config.smart_topic_opts` is set and enabled. |
| `get_smart_topic(out, out_size)` | Get current state: `"enabled"`, `"ollama_unavailable"`, or `"disabled"`. |
| `smart_topic_temperature_for_query(query, out_temperature)` | Run micro-query, set `*out_temperature` to 0.0 / 0.8 / 0.5. Used internally by the library when handling `api_chat` / `api_query` and by the terminal app. |

---

## 4. Intent routing (temperature)

When smart_topic is **enabled** and the app performs a query (e.g. `api_chat`, `api_query`, or terminal `get_bot_response`):

1. **Micro-query** to the TINY model with a classification prompt (TECH / CHAT / DEFAULT).
2. **Parse** the one-word response.
3. **Set temperature** for the main Ollama call:
   - **TECH** (C, code, Mongo, programming) → **0.0**
   - **CHAT** (greeting, general chat) → **0.8**
   - **DEFAULT** → **0.5**
4. Call **`ollama_query_with_options(host, port, model, prompt, temperature, out, size)`** (see **ollama.h**).

If the micro-query fails or smart_topic is disabled, the main query uses the default Ollama behaviour (no temperature option).

---

## 5. Ollama

- **ollama.h** exposes:
  - `ollama_query(host, port, model, prompt, out, out_size)` — no temperature.
  - `ollama_query_with_options(host, port, model, prompt, temperature, out, out_size)` — send `options: { temperature }` when `temperature >= 0`.
- Default TINY model for micro-query: **tinyllama** (overridable via `model_tiny`). Ensure Ollama has the model (e.g. `make ollama-tiny` or `ollama pull tinyllama`). See [Makefile](../Makefile) targets `ollama-tiny`, `ollama-serve`, `smart-topic-deps`.

---

## 6. Summary

| Item | Description |
|------|-------------|
| **Enable** | Set `config.smart_topic_opts` or `opts.smart_topic_opts` with `enable == true`; `engine_init` / `api_create` calls `initial_smart_topic`. |
| **Query path** | When not disabled, `api_chat` / `api_query` and terminal chat use micro-query → intent → `ollama_query_with_options` with TECH 0.0 / CHAT 0.8 / DEFAULT 0.5. |
| **Collection** | Pass `mini_ai_collection` or use default **`smart_topic`**. |
| **Docs** | [.cursor/smart_topic_ai_switch.md](../.cursor/smart_topic_ai_switch.md) (design), [api.md](api.md) (public API options), [FLOW_DIGRAM.md](FLOW_DIGRAM.md) (flows). |
