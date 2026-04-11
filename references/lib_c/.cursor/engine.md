# Engine (`engine_t`, `include/engine.h`)

The **C library engine** owns **storage handles**, **optional background workers**, and **turn persistence**. It does **not** embed a model or set Ollama `num_ctx` — those live in **Ollama** (Modelfile / CLI) and **`OLLAMA_*` env** (see `ollama.h`, `api.c`).

**Version:** `ENGINE_VERSION` in `engine.h` (e.g. `1.0.0`).

---

## 1. `engine_config_t` (what you pass to `engine_create`)

| Field | Role |
|-------|------|
| `mode` | `run_mode_t`: `MODE_M4_NPU` / `MODE_CUDA_REMOTE` / `MODE_HYBRID` — used by **dispatcher** for batch routing. |
| `execution_mode` | `MODE_ONLY_MEMORY` … `MODE_MONGO_REDIS_ELK` — **A/B/C/D** per `.cursor/rule.md` §7; controls whether Mongo/Redis/ELK URIs are wired and **ONLY_MEMORY** short-circuits DB writes. |
| `mongo_uri`, `redis_host`, `redis_port`, `es_host`, `es_port` | Passed to `storage_create`; empty / NULL disables pieces per mode branch in `api.c` → `fill_default_config`. |
| `batch_size` | Default **100** when filled from `api_create` (batch processing hint). |
| `vector_search_enabled` | **On** for `MONGO_REDIS` and `MONGO_REDIS_ELK` (chat RAG path in `api.c`). |
| `debug_mode` | Reserved / future use. |
| `smart_topic_opts` | If non-NULL and `enable`, `engine_init` runs **`initial_smart_topic()`**. |
| `geo_learning_enabled` | **On** for `MONGO_REDIS` / `MONGO_REDIS_ELK`: **`geo_learning_init`** + **`geo_learning_enqueue_turn`** after successful `engine_append_turn`. See `.cursor/geo_leanring.md`. |
| `geo_authority_enabled` | From **`api_options_t.geo_authority`**: **`geo_authority_init`** / shutdown; L1 cache, prompt hint, audit, **`conflict_detector`** hook via `api.c`. See `.cursor/auth_geo.md`, `.cursor/conflict_detector.md`. |
| `model_switch_opts` | From **`api_options_t.model_switch_opts`**: per-lane model + endpoint + inject; **`api_options_t.default_model_lane`**. See `.cursor/model_switch.md`. |
| `vector_gen_backend` | **0** = custom hash embed for RAG/turn **`vector`** (**`vector_generate.h`**); **1** = **`ollama_embeddings`**. Mirrors **`api_options_t`**. See **`.cursor/vector_generate.md`**. |
| `vector_ollama_model` | When backend **1**: preferred embed model string; NULL/empty = default embed resolution. |

**Typical wiring:** `api_create` builds `engine_config_t` via `fill_default_config()` — apps using the engine without `api.h` must set the same flags explicitly.

---

## 2. Lifecycle

```
engine_create(config)
  → storage_create(mongo, redis, es)

engine_init(engine)
  → storage_connect
  → [optional] initial_smart_topic
  → [optional] geo_authority_init
  → [optional] geo_learning_init(storage)

engine_destroy(engine)
  → geo_authority_shutdown   (if enabled)
  → geo_learning_shutdown    (if enabled)
  → storage_destroy
```

Failures starting **geo_authority** or **geo_learning** are **non-fatal**: engine continues; see stderr logs in `engine.c`.

---

## 3. Important APIs

| API | Notes |
|-----|--------|
| `engine_append_turn(..., temp_message_id, has_logic_conflict)` | Persists one user+assistant turn (Mongo `bot.records` when connected). **`has_logic_conflict`** → `metadata.has_logic_conflict` (`.cursor/conflict_detector.md`). **ONLY_MEMORY** → no-op return 0. On success + `geo_learning_enabled` → enqueue for worker. |
| `engine_append_chat` | Legacy single-role message append; not the main chat path for `api_chat`. |
| `engine_get_storage` | Internal storage for logs / advanced use. |
| `engine_vector_search_enabled` | Mirrors `config.vector_search_enabled`. |
| `engine_process_batch` | Dispatcher + `storage_upsert_batch`. |

---

## 4. Cross-references

| Topic | Doc |
|-------|-----|
| Chat turns, streaming, prompt tags | `docs/api.md`, `.cursor/streaming.md`, `.cursor/ptomp.md` |
| Mongo turn shape | `.cursor/mongo.md` |
| Geo learning / `geo_atlas` | `.cursor/geo_leanring.md` |
| Execution modes A–D | `.cursor/rule.md` §7 |

---

## 5. Ollama model & context (not in `engine_config`)

**When the app does not pass a model** (e.g. `ollama_query(..., NULL, ...)` / stream paths that use `NULL`), c-lib resolves the chat model in this order (**`src/ollama.c`**): **first model name from `GET /api/tags`** → **`OLLAMA_MODEL`** → compile-time **`OLLAMA_DEFAULT_MODEL`** in **`include/ollama.h`**. **`model_switch`** and **`M4_MODEL_*`** apply when **`api_apply_model_switch`** runs (non-NULL resolved model passed into Ollama). There is **no** `model` field on **`engine_config_t`**.

**Single checklist** for changing default tags and Python mirrors: **`.cursor/default_models.md`**. **Config not set** → use the fallback chain there (lane empty string, NULL model, etc.).

**Context length** (`num_ctx`, KV cache size) is **only** configured on the **Ollama** side (Modelfile `PARAMETER num_ctx …`, or server defaults). The library does not send `num_ctx` in JSON today.

**Example (ops):** `ollama pull <tag>` for whatever matches **`OLLAMA_DEFAULT_MODEL`** (or set **`OLLAMA_MODEL`**); tune **`num_ctx`** in the model’s Modelfile according to **GPU/RAM**.

## 6. Data flow verification

> Merged from `FLOW_DATA_UPDATE_CHECK.md`.

### `api_chat` (api.c)
- Data **is updated** correctly (Mongo + buffer). Prompt is built from **memory (buffer)** and the buffer **already contains the new message** before build → query memory is correct.

### Prompt assembly order (`ctx_build_prompt`)
| Order | Block | Source |
|-------|-------|--------|
| 1 | RAG context (optional) | Redis L2 `storage_rag_search` → `rag.buf` |
| 2 | Topic | `ctx_compose_topic(msg)` → `"Topic: ...\n\n"` |
| 3 | System time | `[SYSTEM_TIME]` from tag or wall clock |
| 4 | Persona | From `api_options_t.default_persona` or compiled-in default |
| 5 | Instructions | From `api_options_t.default_instructions` (optional) |
| 6 | Earlier (user only) | If count > 5: user inputs joined by ` | ` |
| 7 | Last 5 messages | Full `User:`/`Assistant:` from circular buffer |

### `MODE_ONLY_MEMORY`
No Mongo write; only in-memory buffer holds data. Matches design.
