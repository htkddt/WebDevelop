# Why you only see OLLAMA, never REDIS (and when you do see REDIS)

**Update:** The Redis module now includes an **in-memory L2 reply cache**. When you send the same (or very similar) question twice within 60 seconds, the second reply comes from the cache and the Bot line shows `[REDIS]`. No external Redis server is required for this.

If you still see only `[OLLAMA]`, it was because **no real Redis server** was used for storage or search in the original build; the code path used a stub.

## Trace: python_ai TUI → c-lib → Redis

| Layer | What happens |
|-------|----------------|
| **run_ai_tui.py** | `USE_MAX_OPTIONS=True` → `build_max_api_options()` → `mode=MONGO_REDIS_ELK`, `redis_host=127.0.0.1`, `redis_port=6379`. `api_create(ctypes.byref(opts))` passes these to the library. |
| **api.c** | `api_create` → engine with `vector_search_enabled=1` (mode MONGO_REDIS or MONGO_REDIS_ELK). `storage_connect` → `redis_create` + `redis_initial`; **stub** sets `ctx->connected = 1` so `storage_redis_connected()` is true. |
| **api_chat** | If `engine_vector_search_enabled` and `storage_redis_connected`: embed user message via Ollama, call `storage_rag_search(st, tenant, "default", embed_vec, dim, 5, 0.0, rag_accum_cb, &rag)`. |
| **storage.c** | `storage_rag_search` → `redis_search_semantic(ctx->redis, tenant_id, query_vector, dim, k, callback, userdata)`. |
| **redis.c (stub)** | `redis_search_semantic` does nothing: returns `0` and **never calls the callback**. So `rag.first_len == 0`, the "return Redis reply" branch in api_chat is never taken, and we always call Ollama. |
| **Result** | Bot line always shows `[OLLAMA]`. `redis_set_vector` (after each turn) is also a no-op, so nothing is ever stored for future hits. |

## What happens with Redis?

The Redis module in c-lib is a **stub** (`src/redis.c`):

| Step | What the code does | What actually happens |
|------|--------------------|------------------------|
| **After each turn** | `redis_set_vector(tenant, doc_id, vector, payload)` | **Stub:** no-op. Nothing is stored in Redis. |
| **Before Ollama** | `storage_rag_search()` → `redis_search_semantic(query_vector, k, callback)` | **Stub:** returns 0 hits. The callback is never called. |
| **Result** | If Redis had hits with score ≥ 0.85 we'd return that and show `[REDIS]`. | We never get hits → we always call Ollama → you always see `[OLLAMA]`. |

So no real Redis or RediSearch connection is used: the code path runs but `redis_set_vector` and `redis_search_semantic` do not persist or search anything.

## When do you see REDIS?

**In-memory L2 (current build):** `redis_set_vector` and `redis_search_semantic` in `src/redis.c` use a process-local cache (cosine similarity, TTL 60s). So:

- First time you ask "are you ready?" → Ollama runs → reply stored in cache → Bot shows `[OLLAMA]`.
- Second time you ask the same (or very similar) within 60s → cache hit → Bot shows `[REDIS]`.

No Redis server needed. For a **real Redis + RediSearch** backend (Hiredis, `USE_REDIS=1`):

1. **`redis_set_vector`** — Store the vector + payload in a RediSearch index with TTL 60.
2. **`redis_search_semantic`** — Run `FT.SEARCH` KNN and call the callback for each hit.

Then the same behaviour works across processes and restarts.

## Summary

- **Same input, only OLLAMA:** Expected until Redis is implemented. The stub never stores or finds anything.
- **What happens with Redis:** The stub runs but does nothing; every request goes to Ollama.
- **1-minute TTL:** When you implement Redis, use `REDIS_REPLY_CACHE_TTL_SECONDS` (60) for the reply cache so repeated "is it right?" within 1 minute can return from Redis.
