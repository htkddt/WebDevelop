# Tutorial: Chat L1 memory (in-process)

This tutorial explains how **in-process chat L1** works in the C library: per-session rings used to build prompts, how that differs from Redis/Mongo, and how to configure **ring size** and **idle eviction**.

**Design reference (normative detail):** [`.cursor/chat_l1_memory.md`](../.cursor/chat_l1_memory.md)

**Public API overview:** [`api.md`](api.md)

---

## 1. What “L1” means here

| Layer | Role |
|--------|------|
| **This L1** | Per `(tenant, user)` **in RAM**: last *N* turns (roles, text, source char, timestamps) for **prompt assembly** and UI helpers. |
| **Redis** | Optional **history cache** and **vector RAG** keys — separate TTLs; see [`redis.md`](../.cursor/redis.md) if present in your tree. |
| **MongoDB** | Durable **`bot.records`** turns; `api_load_chat_history` refills L1 from Mongo/Redis cache when connected. |

L1 eviction frees **only process memory**; it does not delete Mongo or Redis data.

---

## 2. Session key and ring size

Each logical session is stored under:

```text
{tenant_id}:{user_slot}
```

- **Normal chat** (`api_chat`, `api_chat_stream`): `user_slot` is the same string you pass as `user_id` (after the API’s defaulting: NULL/empty → `API_DEFAULT_USER_ID`, i.e. `"default"`).
- **Tenant-wide history load**: `api_load_chat_history(ctx, tenant_id, NULL)` uses the sentinel slot documented in `.cursor/chat_l1_memory.md` (internal `__tenant_wide__`), so L1 does not collide with the `"default"` user id.

The **ring capacity** for every session equals **`context_batch_size`** passed in `api_options_t` at `api_create`, capped by the library maximum (`API_CTX_CAPACITY_MAX` in `api.c`). Set **`context_batch_size`** to control how many **cycles** (user + assistant messages) fit in the sliding window.

---

## 3. Configure at `api_create`

```c
#include "api.h"

api_options_t opts = {0};
opts.mode = M4ENGINE_MODE_ONLY_MEMORY;   /* or any mode; L1 is always per-context */
opts.context_batch_size = 16;            /* 0 = API_CONTEXT_BATCH_SIZE_DEFAULT (30) */

/* Idle eviction: drop a session’s ring after no activity for N seconds. */
opts.session_idle_seconds = 0;           /* 0 = use env M4_SESSION_IDLE_SECONDS or default 300 s */
/* opts.session_idle_seconds = 600; */   /* example: force 10 minutes */

api_context_t *ctx = api_create(&opts);
```

**Environment:**

- **`M4_SESSION_IDLE_SECONDS`**: integer seconds; **`0`** disables eviction for contexts that use env/default (when `opts.session_idle_seconds == 0`).
- Default idle TTL when neither opts nor env disable it: **`API_SESSION_IDLE_DEFAULT_SEC`** (300) — see `api.h`.

**Activity** refreshes `last_activity` on history load pushes, user/assistant pushes in chat/stream paths, and session touches inside the implementation (see `.cursor/chat_l1_memory.md`).

---

## 4. Keep tenant/user consistent

Use the **same** `tenant_id` and `user_id` (or both NULL/empty defaults) for:

- `api_load_chat_history(ctx, tenant_id, user_id)`
- `api_chat(ctx, tenant_id, user_id, …)`
- `api_chat_stream(ctx, tenant_id, user_id, …)`

Otherwise the prompt may be built from one session’s ring while Mongo/RAG use another scope.

---

## 5. Typical flows

### 5.1 Cold start (no history load)

1. `api_create` → empty map of sessions.
2. First `api_chat` for `(t1, u1)` creates the session `t1:u1`, pushes the user line, builds the prompt from that ring, then pushes the assistant line.
3. A second user `u2` gets a **separate** ring under `t1:u2`.

### 5.2 Reload from storage

1. `api_load_chat_history(ctx, tenant_id, user_id)` clears **that** session’s ring and appends up to `context_batch_size` turns from Redis cache or Mongo (when connected).
2. The next `api_chat` for the same pair extends that ring.

### 5.3 Reading the “current” session for the UI

After a successful `api_load_chat_history`, `api_chat`, or `api_chat_stream`, the library remembers the **last active** session key.

- `api_get_history_count(ctx)`
- `api_get_history_message(ctx, index, …)` (role, content, optional `source_out`, timestamp)
- `api_get_last_reply_source(ctx)` → `API_SOURCE_MEMORY`, `API_SOURCE_OLLAMA`, `API_SOURCE_REDIS`, or `API_SOURCE_MONGODB`

If that session was **evicted** by idle TTL, getters behave like an **empty** session until the next chat/load recreates it.

---

## 6. Threading

The session map is **not** safe for concurrent `api_*` calls on the **same** `api_context_t` without external locking. Serialize access from one thread or use a queue, same as other shared `ctx` usage.

---

## 7. Sanity check with stats

`api_get_stats(ctx, &out)` includes a **rough** `memory_bytes` estimate for L1: number of live sessions × ring capacity × approximate per-slot buffers. Use it for coarse monitoring, not exact accounting.

---

## 8. Related docs

| Topic | Doc |
|--------|-----|
| Full L1 spec | [`.cursor/chat_l1_memory.md`](../.cursor/chat_l1_memory.md) |
| `api_*` table and options | [`api.md`](api.md) |
| FFI / other languages | [`TUTORIAL_BINDINGS.md`](TUTORIAL_BINDINGS.md) |
