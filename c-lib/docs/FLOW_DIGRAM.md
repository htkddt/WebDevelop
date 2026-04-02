# M4-Hardcore AI Engine — Flow Design

This document describes the control and data flows of the c-lib (C library) and the terminal app (ai_bot) that uses it.

---

## 1. Application boot flow

Boot sequence as implemented in `main.c`: validate environment → create engine → init storage → start debug monitor and terminal UI → enter main loop.

```mermaid
flowchart TD
    A[main] --> B[setlocale]
    B --> C[parse --mode]
    C --> D[validate_environment]
    D --> E{Validation OK?}
    E -->|No| F[exit 1]
    E -->|Yes| G[engine_create]
    G --> H[engine_init]
    H --> I{Init OK?}
    I -->|No| J[engine_destroy, exit 1]
    I -->|Yes| K[debug_monitor_create + start]
    K --> L[terminal_ui_init]
    L --> M[chat_history_add + redraw]
    M --> N[Main loop: terminal_ui_running]
```

**Steps:**

| Step | Action |
|------|--------|
| 1 | `validate_environment(&config)` — checks Redis, ELK; optional connectivity (rule.md). |
| 2 | `engine_create(&config)` — allocates engine, creates `storage_ctx_t` (Mongo/Redis/ES). |
| 3 | `engine_init(engine)` — `storage_connect()`; if `config.smart_topic_opts` is set and enabled, calls **initial_smart_topic()** (see [smart_topic.md](smart_topic.md)). |
| 4 | Debug monitor and ncurses terminal UI start; main loop runs until user quits. |

---

## 2. Chat flow (user message → Ollama → persistence)

When the user submits a line in the terminal, the app gets a bot reply via Ollama and persists both user and bot messages through the engine.

```mermaid
flowchart LR
    subgraph Input
        U[User types line]
    end
    subgraph App
        TS[chat_timestamp]
        ACH[engine_append_chat user]
        OQ[ollama_query]
        BCH[engine_append_chat bot]
    end
    subgraph Storage
        S[storage_append_chat]
    end
    U --> TS
    TS --> ACH
    ACH --> S
    ACH --> OQ
    OQ --> BCH
    BCH --> S
```

**Sequence:**

1. User submits line → `chat_timestamp()` for user message.
2. `engine_append_chat(engine, "default", "user", line, user_ts)` → in non–ONLY_MEMORY modes calls `storage_append_chat()` (MongoDB chat collection).
3. **If smart_topic enabled:** `get_smart_topic()` → `smart_topic_temperature_for_query(line, &temp)` (micro-query: TECH→0.0, CHAT→0.8, DEFAULT→0.5) → `ollama_query_with_options(..., temp, ...)`. **Else:** `ollama_query(...)`.
4. Bot response written to chat history and `engine_append_chat(engine, "default", "bot", buf, tmbuf)` → again `storage_append_chat()` when not ONLY_MEMORY.

Ollama is invoked from a worker thread; the UI shows “thinking” until the thread signals done. See [smart_topic.md](smart_topic.md).

---

## 3. Batch processing flow (engine_process_batch)

Used for bulk records (e.g. “simulate batch” with `s` in terminal, or when c_ai/python_ai send batches). Tenant is validated, then dispatcher selects backend and storage upserts.

```mermaid
flowchart TD
    A[engine_process_batch] --> B[tenant_validate_id]
    B --> C{Valid?}
    C -->|No| D[return -1]
    C -->|Yes| E[task_spec_t]
    E --> F[dispatcher_select_backend]
    F --> G{run_mode_t}
    G -->|MODE_M4_NPU| H[BACKEND_M4_NPU]
    G -->|MODE_CUDA_REMOTE| I[BACKEND_CUDA_REMOTE]
    G -->|MODE_HYBRID| J[hash tenant % 2]
    J --> K[M4 or CUDA]
    H --> L[dispatcher_dispatch]
    I --> L
    K --> L
    L --> M[storage_upsert_batch]
    M --> N[processed++ / errors++]
```

**Steps:**

| Step | Component | Action |
|------|-----------|--------|
| 1 | `engine.c` | Validate `tenant_id` via `tenant_validate_id()`. |
| 2 | `dispatcher` | Build `task_spec_t`; `dispatcher_select_backend(mode, &spec)` — M4 only, CUDA only, or hybrid (tenant hash % 2). |
| 3 | `dispatcher` | `dispatcher_dispatch(backend, &spec)` — currently stubbed (M4 enqueue / remote CUDA TODO). |
| 4 | `engine.c` | `storage_upsert_batch(storage, tenant_id, records, count)` — MongoDB bulk upsert. |
| 5 | `engine.c` | Update `processed` or `errors` counters. |

---

## 4. Execution modes (storage path)

Execution mode (`execution_mode_t`) decides whether chat and data hit external systems (rule §7).

```mermaid
flowchart TD
    M[execution_mode_t] --> A[MODE_ONLY_MEMORY]
    M --> B[MODE_ONLY_MONGO]
    M --> C[MODE_MONGO_REDIS]
    M --> D[MODE_MONGO_REDIS_ELK]
    A --> A1[RAM/mmap only\nno Mongo/Redis]
    B --> B1[MongoDB only\nbatch 100–500]
    C --> C1[Redis read\nasync Mongo write\nChange Streams]
    D --> D1[Mongo + Redis + ELK\nauto_lang_processor]
```

- **A — ONLY_MEMORY:** `engine_append_chat` is a no-op (no DB); batch path can still use in-memory handling.
- **B — ONLY_MONGO:** MongoDB only; batch via `mongoc_bulk_operation_t`.
- **C — MONGO_REDIS:** Redis for reads, async Mongo writes, Change Streams sync.
- **D — MONGO_REDIS_ELK:** Adds Elasticsearch with `pipeline=auto_lang_processor` for localization.

---

## 5. Run modes (compute backend)

Run mode (`run_mode_t`) selects where inference runs: M4 NPU, remote CUDA, or hybrid.

```mermaid
flowchart LR
    R[run_mode_t] --> M4[MODE_M4_NPU]
    R --> CUDA[MODE_CUDA_REMOTE]
    R --> HY[MODE_HYBRID]
    M4 --> D1[dispatcher → BACKEND_M4_NPU]
    CUDA --> D2[dispatcher → BACKEND_CUDA_REMOTE]
    HY --> D3[tenant hash % 2 → M4 or CUDA]
```

- **m4 / npu:** All work dispatched to M4 NPU (stub: TODO enqueue).
- **cuda / remote:** All work dispatched to remote CUDA (stub: TODO send).
- **hybrid:** Round-robin by tenant hash between M4 and CUDA.

---

## 6. Component dependency overview

High-level dependency between public API and internals.

```mermaid
flowchart TB
    subgraph App
        main[main.c]
    end
    subgraph Public API
        engine[engine.h]
        smart_topic[smart_topic.h]
        ollama[ollama.h]
        validate[validate.h]
        storage[storage.h]
        tenant[tenant.h]
        dispatcher[dispatcher.h]
    end
    subgraph Implementation
        engine_c[engine.c]
        smart_topic_c[smart_topic.c]
        ollama_c[ollama.c]
        validate_c[validate.c]
        storage_c[storage.c]
        tenant_c[tenant.c]
        dispatcher_c[dispatcher.c]
    end
    main --> engine
    main --> smart_topic
    main --> validate
    main --> ollama
    main --> terminal_ui
    engine --> smart_topic
    engine --> tenant
    engine --> dispatcher
    engine --> storage
    engine_c --> smart_topic_c
    engine_c --> tenant_c
    engine_c --> dispatcher_c
    engine_c --> storage_c
```

---

## 7. Cache before MongoDB (current behavior)

**Do we fetch/cache information before getting information from MongoDB?**

- **Yes, for chat history.** When Redis is connected, `api_load_chat_history(ctx, tenant_id, user_id)` uses **cache-first**: `storage_get_chat_history_cached()` tries L1 key `m4:cache:history:{tenant_id}` (tenant-wide) or `m4:cache:history:{tenant_id}:{user_id}` when `user_id` is set; on **miss**, Mongo is queried with **both** `tenant_id` and optional `user` filter, then Redis is populated (TTL **300** s, `REDIS_CACHE_TTL_SECONDS` in `include/redis.h`).
- **TTL:** L1 cache keys use **300 seconds** (5 minutes) when set. The constant `REDIS_CACHE_TTL_SECONDS` is in `redis.h`; the stub `redis_set_value` accepts `ttl_seconds` but does not apply it until a real Hiredis impl uses SETEX.
- **Per-message flow:** In `api_chat` we do **not** read from Mongo in the hot path. Context comes from in-memory buffer + optional RAG (Redis L2).
- **Reply caching:** The “L1 → L2 → Ollama” path for **reply** cache (avoid calling Ollama when L1/L2 has a hit) is **not implemented**: we always call Ollama. Cache is in place for **history** (check before Mongo); reply cache would be a separate step (check L1/L2 before `ollama_query`).

---

## 8. What is compiled into the prompt

The string sent to Ollama (`context_buf` in `api_chat`) is built in this **order** (see `api.c`: RAG prepend, then `ctx_build_prompt`):

| Order | Block | Content |
|-------|--------|--------|
| 1 | **RAG context** (optional) | Only when Redis L2 is connected and vector search enabled. Prepended as: `"Context from past turns:\n"` + up to 5 semantic-search snippets (from `storage_rag_search`) + `"\n\n"`. |
| 2 | **Topic** | `"Topic: "` + (current user message truncated to ~240 chars, or `"General"`) + `"\n\n"`. |
| 3 | **System guard** | `"Maintain the persona of a C Systems Engineer based on the provided history.\n\n"`. |
| 4 | **Last N messages** | Rule §12: last **5** messages from the in-memory circular buffer, each as `"User: "` or `"Assistant: "` + content + `"\n"`. |

So the **prompt** = `[RAG block if any]` + `Topic: ...` + system guard + last 5 user/assistant turns. Total size is bounded by `API_CONTEXT_BUFFER_SIZE` (64 KB); RAG prefix by `API_RAG_PREFIX_MAX` (4 KB).

---

## 9. SEQUENCE INPUT DATA

How **user text** enters the stack and how **assistant text** is **extracted** from Ollama for persistence (`engine_append_turn` → Mongo `turn.input` / `turn.assistant`). Two API shapes:

| Case | API | Ollama call | Where reply text is extracted |
|------|-----|-------------|------------------------------|
| **A — No stream** | `api_chat` | `POST /api/generate` with `"stream":false` | One HTTP body → `extract_generate_reply_body()` in `ollama.c` |
| **B — Stream** | `api_chat_stream` | `POST /api/generate` with `"stream":true` | Each NDJSON line → `extract_stream_token_json()`; fragments concatenated into `full` |

---

### 9.1 Case 1 — Without stream (`api_chat`)

**Input path (user text)**

1. Caller passes **`user_message`** and output buffer **`bot_reply_out`** / **`out_size`**.
2. **`epoch_ms_string`** → `user_ts` (epoch ms string for the turn).
3. **`ctx_push_message_with_source(ctx, "user", msg, API_SOURCE_MEMORY, user_ts)`** — user line enters the in-memory circular buffer (used for the next prompt’s “last N” turns).
4. Optional **Redis RAG** (when vector search enabled + Redis up): **`ollama_embeddings`** on `msg` → **`storage_rag_search`** → if top hit **`score ≥ API_RAG_REPLY_MIN_SCORE`**, reply text is taken from the hit payload (**assistant** = substring after the first `\n` in `input\nassistant` form), copied into **`bot_reply_out`**, then flow jumps to **`append_turn`** (skip Ollama).
5. Otherwise: **`ctx_build_prompt`** builds **`context_buf`** from history (+ optional RAG snippets prepended). **`api_apply_model_switch`** may change model / temperature / inject lane context into **`context_buf`**.

**Extract assistant text (non-stream)**

6. **`ollama_query`** or **`ollama_query_with_options`** sends **`context_buf`** to Ollama; the entire response body is buffered once.
7. **`extract_generate_reply_body(body, bot_reply_out, out_size)`** scans the JSON **in order** until a non-empty string is found:
   - `"response":"…"`
   - `"content":"…"` (chat-shaped or top-level)
   - after `"delta"`, nested `"content":"…"`
   - `"thinking":"…"`
   - `"text":"…"`
8. If nothing non-empty is found → **`ollama_query*` returns `-1`** → **`api_chat` returns `-1`** and **does not** call **`engine_append_turn`** (no partial turn with empty assistant from this path).

**After text is available**

9. **`run_geo_authority_post_chat`** (optional) may append a logic-conflict note to **`bot_reply_out`**.
10. **`ctx_push_message_with_source(ctx, "assistant", bot_reply_out, …)`**.
11. **`engine_append_turn(..., input=msg, assistant=bot_reply_out, …)`** → Mongo document `turn: { input, assistant }` (when not `MODE_ONLY_MEMORY` and storage connected).

```mermaid
sequenceDiagram
    participant App
    participant api as api_chat
    participant ctx as api_context buffer
    participant ol as ollama_query*
    participant parse as extract_generate_reply_body
    participant eng as engine_append_turn

    App->>api: user_message, bot_reply_out
    api->>ctx: push user (MEMORY)
    alt Redis RAG hit
        api->>api: bot_reply from payload
    else Ollama
        api->>ol: context_buf (prompt)
        ol->>parse: single JSON body
        parse->>api: bot_reply_out filled
    end
    api->>ctx: push assistant
    api->>eng: append_turn(input, assistant)
```

---

### 9.2 Case 2 — With stream (`api_chat_stream`)

**Input path (user text)**

1. Caller passes **`user_message`**, **`api_stream_token_cb cb`**, optional **`temp_message_id`**.
2. **`api_chat_prepare_for_stream`**:
   - **`epoch_ms_string`** → `user_ts`.
   - **`ctx_push_message_with_source(ctx, "user", msg, MEMORY, user_ts)`**.
   - Same **RAG short-circuit** as non-stream: on high-score hit, assistant text goes into **`redis_reply`** → function returns **`prep == 1`** (no worker thread, no Ollama stream).
3. If **`prep == 1`**: geo post-chat → push assistant → **`engine_append_turn(msg, redis_reply)`** → **`cb(redis_reply, msg_id, done=0)`** then **`cb("", msg_id, done=1)`**; **return**.

**Stream path (`prep == 0`)**

4. Worker thread runs **`ollama_query_stream`** with the prepared **`prompt`** (from **`context_buf`**) and resolved model / temperature.
5. libcurl delivers bytes; **`ol_stream_write_cb`** buffers until **`\\n`**, then **`ol_stream_flush_line`** runs on one NDJSON line.

**Extract text per line (stream)**

6. **`extract_stream_token_json(line, frag, …)`** tries the **same logical order** as non-stream (fragment must be non-empty to count):
   - `"response":"…"`
   - `"content":"…"`
   - after `"delta"`, `"content":"…"`
   - `"thinking":"…"`
   - `"text":"…"`
7. If a fragment is extracted: **`stream_forward_token`** → **`cb(token, msg_id, done=0)`** for the app UI, and the same fragment is appended to **`full`** (bounded by **`OL_BUF_SIZE`**).
8. After the HTTP stream ends, **`ol_stream_flush_line`** runs once more for any trailing line without a final newline.

**Persist**

9. If **`full` is empty** after a successful curl (or no parseable tokens): **stderr** log, **no** **`engine_append_turn`** (Mongo turn skipped — avoids `assistant: ""`).
10. If **`full`** is non-empty: geo post-chat on **`full`** → push assistant → **`engine_append_turn(..., input=user_msg, assistant=full, …)`** → **`cb("", msg_id, done=1)`**.

**App contract:** the library **does not** pass the assembled full reply in a separate “final chunk” with text; the app should **concatenate** tokens from **`cb`** while **`done == 0`**, or read from context after join. Mongo always gets the same assembled string as **`full`** when persistence runs.

```mermaid
sequenceDiagram
    participant App
    participant api as api_chat_stream
    participant prep as api_chat_prepare_for_stream
    participant ctx as api_context buffer
    participant w as stream worker
    participant ol as ollama_query_stream
    participant parse as extract_stream_token_json
    participant eng as engine_append_turn

    App->>api: user_message, cb
    api->>prep: build prompt / RAG
    prep->>ctx: push user
    alt prep==1 Redis hit
        prep->>api: redis_reply
        api->>eng: append_turn
        api->>App: cb(reply), cb(done)
    else prep==0
        api->>w: pthread + prompt
        loop Each NDJSON line
            ol->>parse: line
            parse->>App: cb(token, done=0)
            parse->>w: append to full
        end
        w->>eng: append_turn(user_msg, full)
        w->>App: cb("", done=1)
    end
```

---

## 10. Summary

| Flow | Entry | Exit / outcome |
|------|--------|-----------------|
| **Boot** | `main()` | Engine and UI ready; if `smart_topic_opts` set and enabled, **initial_smart_topic** runs in `engine_init`. |
| **Chat** | User input + Enter | User/bot messages in history and (if not ONLY_MEMORY) in MongoDB chat collection; bot reply via Ollama; if smart_topic enabled, micro-query sets temperature (TECH 0.0 / CHAT 0.8 / DEFAULT 0.5). RAG (Redis L2) prepends context when enabled. |
| **Batch** | `engine_process_batch(tenant_id, records, count)` | Tenant validated → dispatcher selects M4/CUDA → storage upsert; processed/errors updated. |
| **Execution mode** | `engine_config_t.execution_mode` | Chooses A/B/C/D storage path (memory-only up to Mongo+Redis+ELK). |
| **Run mode** | `engine_config_t.mode` / `--mode` | Chooses M4, CUDA, or hybrid for dispatcher backend selection. |
| **Smart topic** | `config.smart_topic_opts` / `opts.smart_topic_opts` | Init at `engine_init`; micro-query + temperature in chat/query path. See [smart_topic.md](smart_topic.md). |
| **Cache vs Mongo** | History load / chat | Chat history: Redis L1 first (key `m4:cache:history:{tenant_id}`), then Mongo on miss; TTL 300s. See §7. |
| **Prompt contents** | `api_chat` → Ollama | RAG block (if any) + Topic + system guard + last 5 messages. See §8. |
| **Input → extracted text** | `api_chat` / `api_chat_stream` | Non-stream: one-body `extract_generate_reply_body`. Stream: per-line `extract_stream_token_json` → `full`. See **§9 SEQUENCE INPUT DATA**. |

Integration notes for the **`python_ai`** Flask server and **`fe`** Vite app (ctypes layout, env, proxy) live in **[TUTORIAL_BINDINGS.md](TUTORIAL_BINDINGS.md)** §8 — not in this flow diagram.

For public API details, see `.cursor/rule.md`, [smart_topic.md](smart_topic.md), and `include/*.h`.
