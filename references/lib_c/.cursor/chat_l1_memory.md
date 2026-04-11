# Chat L1 memory (in-process, per API context)

## Where this is documented

- **This file** — canonical design for **chat** L1 (not Redis L1 keys, not vector SIMD L1 in `.cursor/vector_generate.md`).
- **Implementation:** `src/api.c` (`api_chat_session_t`, `m4_ht_t` map).
- **Related:** `.cursor/FLOW_DATA_UPDATE_CHECK.md` (prompt vs buffer), `.cursor/redis.md` / `.cursor/REDIS_KEYS.md` (Redis L1 history cache).

## Model

In-memory **L1** for chat is a **map**:

| Key | Value |
|-----|--------|
| Session key `"{tenant_id}:{user_slot}"` | Circular buffer (roles, messages, sources, timestamps) + **`last_activity`** (epoch seconds) |

- **`user_slot`**: Mongo/RAG `user` id for normal chat (`api_chat`), or the sentinel **`__tenant_wide__`** when `api_load_chat_history` is called with `user_id == NULL` (legacy tenant-wide history).
- **Ring capacity** = **`context_batch_size`** (capped by **`API_CTX_CAPACITY_MAX`**, same as before). Each session gets its own ring of that size.

## Activity and eviction

- **`last_activity`** is updated on: **load history** (each message pushed), **user/assistant push** (chat and stream paths).
- If a session is idle longer than **`session_idle_seconds`** (see below), it is **removed** from the map and its ring is freed.
- **Default idle TTL:** **300 seconds (5 minutes)**.
- **Override:** environment variable **`M4_SESSION_IDLE_SECONDS`** (integer seconds; **`0`** = disable eviction).
- **Override:** `api_options_t.session_idle_seconds` **> 0** wins over env/default when creating the context.

## API behavior

- **`api_get_history_message`** refers to the **last active** session: the most recent successful **`api_load_chat_history`** or **`api_chat`** that resolved a session key. Last reply source is available via `api_stats_t.last_reply_source`.
- If that session was evicted, getters behave like an **empty** session (count **0**, etc.) until the next load/chat.

## Threading

The map and sessions are **not** thread-safe across concurrent `api_*` on the same `api_context_t`. Host apps should **serialize** access (same as before).

## Redis L1 vs this L1

- **Redis** `m4:cache:history:…` reduces Mongo reads; TTL there is separate (**`REDIS_CACHE_TTL_SECONDS`**).
- **This** L1 holds the **prompt assembly** ring per logical session; eviction **`M4_SESSION_IDLE_SECONDS`** only frees **process RAM**, not Mongo/Redis.
