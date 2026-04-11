# M4-Hardcore AI Engine — C library (this repo)

This repository is the **C library** for the M4-Hardcore AI Engine. **c-lib does not execute:** it only produces the library (`.a`, `.so`/`.dylib`, or `.o`). Execution is done by **c_ai**, **python_ai**, or the parent repo’s **ai_bot**, which use the prebuilt library (inject/link or load at runtime).

---start cycle - do not modify -
**Strict rules for AI-generated C Library code.** Optimized for Apple Silicon M4 & MongoDB v2.x.

## 1. Project Architecture (Mandatory)
AI must respect this folder structure for all code generation:
- `/include`: Header files (`.h`). Public APIs separate from Internal Logic.
- `/src`: Implementation modules (`storage.c`, `ai_ollama.c`, `cache_redis.c`, etc.).
- `/scripts`: Python/Shell scripts for Batch Ingestion & Watchdogs.
- `Makefile`: Optimized with `-mcpu=apple-m4 -O3` for ARM64 performance.

## 2. Storage & Execution Modes
AI must implement logic based on these 4 Configurable Modes:
- **MODE_ONLY_MEMORY**: Pure RAM/mmap. NO DB calls.
- **MODE_ONLY_MONGO**: Direct Cloud Atlas persistence. Use `batchSize` (100-500).
- **MODE_MONGO_REDIS**: Hybrid. Reads from Redis (0.1ms), Async Batch to Mongo.
- **MODE_MONGO_REDIS_ELK**: Full Stack. Use ELK for Fuzzy Search & Auto-Language.

## 3. High-Performance Connectivity (The 0.51s Rule)
- **DNS Bypass**: Use `127.0.0.1` for local services (Ollama/Redis). NEVER use `localhost`.
- **Socket Rules**: Set `CURLOPT_TCP_NODELAY = 1` and `CURLOPT_TIMEOUT = 10s`.
- **Latency Standard**: AI First Token MUST arrive in < 2s.

## 4. In-Memory Context (Circular Buffer Logic)
- **Sliding Window**: Store the last **batchContextSize messages** in a **Circular Array** in RAM.
- **Cycle Logic**: Use `head = (head + 1) % batchContextSize` to overwrite the oldest message.
- **Reconstruction**: When building a prompt, iterate from `(head - count)` to `head` to maintain chronological order.

## 5. Real-time Language Auto-detection
- **ELK Pipeline**: Every query MUST be pre-processed by `auto_lang_processor` in Elasticsearch.
- **Prompt Guard**: Prepend detected language (e.g., "Respond in [en]") to the System Prompt.
- **Fallback**: If ELK is down, default to English (en-US). NO Iraq/Thai hallucination.

## 6. Memory & Pointer Safety
- **Null Safety**: Check EVERY pointer from `malloc`, `mongoc`, or `hiredis` immediately.
- **Cursor Lifecycle**: Every `mongoc_cursor_t` MUST be followed by `mongoc_cursor_destroy`.
- **Buffer Management**: Use `OL_BUF_SIZE 32768` (32KB). `memset` before every reuse.

## 7. Batch Operations (1B Records Scaling)
- **Ingestion**: Use `mongoc_bulk_operation_t` for any data load > 100 records.
- **Indexing**: Refuse to generate queries without `tenant_id` or `status` index filters.

## 8. Hardware Optimization (M4/4070)
- **Unified Memory**: Minimize memory copies between CPU and Apple Silicon NPU.
- **Threading**: Isolate AI inference from the Main UI thread using `pthreads`.

## 9. Debugging & Monitoring
- **Analyze Terminal**: Send internal metrics (latency, hits) to a separate window via `/tmp` pipes.
- **Traceability**: Logs MUST include `__FILE__` and `__LINE__` for Segment Fault debugging.

## 10. Coding Style
- **Minimalism**: Restrict dependencies to `libmongoc`, `hiredis`, `libcurl`, and `ncurses`.
- **Documentation**: Use Doxygen style for public functions in `/include`.

## 11. Batch Context vs History Rule
- **Context Limit**: The `batchContextSize` (default: 30) defines the maximum number of recent messages sent to the LLM.
- **Priority Override**: When the context buffer is full, use a First-In-First-Out (FIFO) logic to discard the oldest message from the prompt, NOT from the database.
- **Deep Retrieval**: If the AI requires information beyond the `batchContextSize`, the library MUST trigger a targeted MongoDB query (RAG) instead of expanding the RAM buffer.
- **Init option**: Exposed as `api_options_t.context_batch_size` at create; 0 = default (30). Use when building context for the next chat turn.

## 12. Pre-Query Context Distillation
- **Anchor Strategy**: Before calling the LLM, the Library MUST compose a 5-line "Context Anchor" summarizing the Active Topic, Mode, and Last Status.
- **Top-Down Priority**: Prepend the Anchor to the prompt. This forces the AI to stay within the "Hardcore C" domain and prevents language drifting (Thai/Iraq).
- **History Clipping**: If the 30-message history contains "noisy" or "irrelevant" chat, prioritize the LAST 5 messages for the active reasoning block.
- **Strict Identity**: The Anchor MUST include a `[ROLE: Senior C Architect]` tag to prevent the AI from pretending to be human or a generic chatbot.
---end cycle

## Layout (this repo)

```
include/     # Public API headers (engine.h, ollama.h, storage.h, tenant.h, dispatcher.h, validate.h, …)
src/         # Library + app sources (app code is built only by the parent repo)
build/       # Object files (make lib / make lib-o)
lib/         # Prebuilt output: libm4engine.a, libm4engine.dylib | libm4engine.so (make lib)
dist/        # Package tarball (make package)
```

There is **no** `bin/` or executable built by c-lib’s Makefile.

## Tech stack

- **Core:** Pure C (Clang), C17. Apple Silicon: `-arch arm64` on macOS.
- **Database:** MongoDB v2.x C Driver (optional, `USE_MONGOC=1`).
- **Cache:** Redis (Hiredis) — stubbed.
- **Analytics:** Elasticsearch — stubbed / optional.
- **UI:** Ncurses (terminal_ui), libcurl (Ollama).

## Build (library only; no executable)

```bash
make validate
make lib          # lib/libm4engine.dylib or .so (default target)
make lib-static   # lib/libm4engine.a
make lib-o        # build .o files for inject/link into c_ai or other apps
make package      # dist/m4engine-<VERSION>-<OS>-<ARCH>.tar.gz
```

Optional: `make lib USE_MONGOC=1` (requires `brew install mongo-c-driver`). **c-lib never builds or runs an executable** — use c_ai / python_ai or the parent’s ai_bot to run.

## Per-mode capabilities (what each mode can do)

The library **does not trust all user input**. What is allowed depends on **execution_mode**:

| Mode | Mongo | Redis | ELK | Error/warning logs to Mongo/ELK |
|------|-------|-------|-----|----------------------------------|
| **MODE_ONLY_MEMORY** | No | No | No | **No** — counters only; never write to MongoDB or ELK. |
| **MODE_ONLY_MONGO** | Yes | No | No | Yes (ai_logs collection). |
| **MODE_MONGO_REDIS** | Yes | Yes | No | Yes. |
| **MODE_MONGO_REDIS_ELK** | Yes | Yes | Yes | Yes. |

- In **MEMORY** mode, stats only use in-memory counters (`stat_inc_error` / `stat_inc_warning`); no MongoDB or ELK (enforced in `stat.c`).
- Overriding the log collection is allowed **only** via **create context** with validated options: `opts.log_db` / `opts.log_coll` in `api_create(&opts)`. Names are validated (length 1..63, `[a-zA-Z0-9_]` only). Do **not** accept arbitrary user input for db/coll names.

## Conventions (when editing this repo)

1. **Headers:** All public API in `include/`. Implementation in `src/`. No `main()` in library-only code; `main.c` is the terminal app that uses the library.
2. **Execution modes:** Support the 4 types in `engine.h`: `MODE_ONLY_MEMORY`, `MODE_ONLY_MONGO`, `MODE_MONGO_REDIS`, `MODE_MONGO_REDIS_ELK`. Enforce per-mode capabilities above (e.g. no Mongo/ELK logs in MEMORY).
3. **Ollama:** Use `127.0.0.1` (not `localhost`). `CURLOPT_TIMEOUT` 10s, `CURLOPT_TCP_NODELAY` 1. Buffer size `OL_BUF_SIZE` 32768 (see `ollama.h`). **Default model tags** live only in `include/ollama.h` (`OLLAMA_DEFAULT_MODEL`, `OLLAMA_DEFAULT_EMBED_MODEL`); when changing them, follow **`.cursor/default_models.md`** so Python (`m4_default_models.py`) and docs stay aligned (see **`.cursor/rules/default-models.mdc`**).
4. **Locale:** Call `setlocale(LC_ALL, "")` in the application `main()` for UTF-8 / English.
5. **MongoDB:** Use `libmongoc` v2.x. Destroy every `mongoc_cursor_t`. Prefer `mongoc_client_pool_t` over a new client per request.
6. **Docs:** If you add or change public API or behaviour, update this rule or a README in this repo (and parent `docs/*` if part of a larger project).

## Public API (what to bind from other languages)

**Recommended surface: api.h only.** All other headers are internal/advanced.

- **Less API is better:** Add new **`api_*`** / public types only when unavoidable; prefer extending **`api_options_t`** or existing calls. Host-facing behavior must go through this documented surface — no extra “convenience” exports for FFI. See **`.cursor/rules/public-api-surface.mdc`**.
- **Approval:** Any change to the **public** surface (**`api.h`**, FFI-visible structs, or documented stable behavior) requires **explicit maintainer / author approval** — see **`.cursor/rules/public-api-surface.mdc`** (“Human approval required”).
- **api.h (public, 7 functions):** `api_create`, `api_destroy`, `api_chat` (unified sync+stream), `api_load_chat_history`, `api_get_history_message`, `api_get_stats`, `api_geo_atlas_import_row`; `api_options_t`, `api_stats_t`, `M4ENGINE_MODE_*`. Prompt tags, model lanes, and log collection are configured via `api_options_t` at create time. See `docs/api.md`.

Internal/advanced (for C apps that need finer control):

- **engine.h:** `engine_create`, `engine_destroy`, `engine_init`, `engine_append_chat`, `engine_get_stats`, `engine_get_storage`; `engine_config_t`, `execution_mode_t`.
- **ollama.h:** `ollama_query(host, port, model, prompt, out, out_size)`.
- **storage.h:** `storage_*`, `storage_set_ai_logs` (validated).
- **stat.h:** `stat_*`, `stat_snapshot_t`.
- **tenant.h:** `tenant_validate_id`.
- **validate.h:** `validate_environment`.

## Parent repo (if used as subproject)

When this repo is used inside a parent (e.g. `ai/`): the parent builds the **library** (from c-lib sources or by `make -C c-lib lib`) and puts it in `lib/`; the parent also builds **ai_bot** from c-lib’s app sources (main.c, terminal_ui, etc.). **c_ai** and **python_ai** consume the prebuilt library (link `.a`/`.so` or load `.so`/`.dylib`) to execute — they do not run c-lib by itself, they inject the library into their own process.

"@rules.md Implement the Hybrid Context Linking logic in src/engine.c. Use Redis for the last 30 messages and fallback to Mongo if Redis is empty. Ensure 0.51s latency."

## 13. Context Injection Logic add 
- ** check the current history
- **Reconstruction**: Always iterate the Circular Buffer from `(head - count)` to `head` to maintain chronological order in the prompt.
- **Formatting**: Use explicit headers (`User:`, `Assistant:`) for each historical message.
- **Memory Buffer**: Allocate a dedicated `context_buffer` of 64KB for the combined prompt before sending to AI.
- **System Guard**: Prepend a hidden System Message to every prompt: "Maintain the persona of a C Systems Engineer based on the provided history."

## 14. Storage module pattern

> Merged from `STORAGE_MODULES_DISCUSSION.md`.

Each storage backend (Mongo, Redis, ELK) follows the same pattern: **initial** (connect), **set** (write), **search** (read), **destroy** (cleanup). Config-driven via execution mode:

| Mode | Mongo | Redis | ELK |
|------|-------|-------|-----|
| `ONLY_MEMORY` | -- | -- | -- |
| `ONLY_MONGO` | init + set + search | -- | -- |
| `MONGO_REDIS` | init + set | init + search (fallback Mongo) | -- |
| `MONGO_REDIS_ELK` | init + set | init + search | init + ingest |

Each module has a header (`include/{module}.h`) and rule file (`.cursor/{module}.md`). `storage.c` composes them based on `engine_config_t.execution_mode`.
