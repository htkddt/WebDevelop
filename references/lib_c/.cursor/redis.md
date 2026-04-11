# Redis module — rules

Do not modify without updating **include/redis.h** if API or constants change.  
Rule reference is injected at the top of `include/redis.h`.

## Strategy (Dual-Layer Caching)

Yes — we can do it like this. The Redis module follows a **dual-layer** strategy so lookups can use exact match first, then semantic match, then upstream (Ollama/LLM).

**Purpose of the cache:** Check Redis **before** going to MongoDB (for history/session) and **before** calling Ollama (for reply). Cache hit → no Mongo read / no LLM call.

| Layer | Mechanism | Use case |
|-------|-----------|----------|
| **L1 (Simple)** | Standard `GET`/`SET`/`SETEX` with Hiredis | Exact string match: session, settings, **chat history** (`m4:cache:history:{tenant_id}`), query reply (`m4:cache:{tenant_id}:{hash}`). |
| **L2 (Semantic)** | RediSearch `FT.SEARCH` (vector KNN) | Meaning-based match: same intent in Thai/Viet/Eng maps to same vector space; also used for RAG context. |
| **Fallback** | L1 miss → L2 (for reply) → L2 miss → Ollama/LLM; for history: L1 miss → MongoDB | Cache hit at L1 or L2 avoids LLM call; L1 hit for history avoids Mongo read. |

- **Scale 3 support:** For L2 semantic cache, use **multilingual embeddings** so Thai/Viet/Eng queries live in one vector space in Redis.
- **Lookup order:** Always try L1 first (fastest); if miss, try L2 (for replies); if miss, call Ollama/LLM or read from Mongo. Optionally populate L1/L2 after miss with TTL (e.g. `REDIS_CACHE_TTL_SECONDS` = 300).

---

## 1. Scope

Redis module provides: **initial** (connect), **set** (L1: counter/value; L2: vector index), **search** (L1: get counter/value; L2: vector KNN). Used for real-time counters per tenant and, in MONGO_REDIS mode, dual-layer cache (L1 exact + L2 semantic) with fallback to LLM.

## 2. Connection and lifecycle

- Use **Hiredis**. Optional at build (stubbed if not linked).
- **Initial:** Connect to host:port. Use **127.0.0.1** for local Redis (never `localhost` per rule §3). Default port 6379.
- **Destroy:** Disconnect and free context. Null-check every pointer from `redisConnect` and commands.

## 3. Key naming and tenant isolation

- **L1 key pattern:** Tenant-scoped. Example `m4:{tenant_id}:{key}` or `m4:counter:{tenant_id}:{key}` or `m4:cache:{tenant_id}:{query_hash}` or `m4:cache:history:{tenant_id}`. No arbitrary user input in key names without validation.
- **L1 set:** INCRBY for counters; SET/SETEX for cache values. **TTL:** use `REDIS_CACHE_TTL_SECONDS` (300) for cache keys when implementing SETEX. Current stub ignores TTL; real impl should pass `ttl_seconds` to SETEX.
- **L1 search:** GET for counter or cached value. Return 0 or default when key missing.
- **L2 (Semantic):** Use a RediSearch index (e.g. `m4:semantic:{tenant_id}` or one index per tenant). Store embeddings + metadata; query with `FT.SEARCH` KNN. Embeddings MUST be multilingual (same model for Thai/Viet/Eng) so queries map to one vector space. **Lang detect:** Redis L2 vector search does **not** require lang_detect — search is by vector only; lang is optional for metadata or filtering. **Reply cache:** When L2 returns a high-score hit, c-lib returns that payload as the chat reply and skips the AI agent (Ollama). Use **REDIS_REPLY_CACHE_TTL_SECONDS** (60) for L2 reply cache so the same query within 1 minute hits cache; after 1 min TTL expires and we call Ollama again.

## 4. Latency and connectivity

- Target **&lt; 0.1 ms** for local Redis (rule §2 MONGO_REDIS: reads from Redis). Use pipeline only when batching; avoid blocking commands in hot path.
- On connection failure: log; optional fallback to Mongo or in-memory. Do not crash.

## 5. Key Patterns

> Merged from `REDIS_KEYS.md`. **Current L1/L2 implementation is a stub** — no real Hiredis connection.

### L1 (simple key/value)
| Key pattern | Purpose | TTL |
|-------------|---------|-----|
| `m4:cache:history:{tenant_id}` | Chat history blob (tenant-wide) | **300s** (`REDIS_CACHE_TTL_SECONDS`) |
| `m4:cache:history:{tenant_id}:{user_id}` | Same, scoped to Mongo `user` | **300s** |

### L2 (semantic / reply cache)
| Concept | Purpose | TTL | Status |
|---------|---------|-----|--------|
| Reply cache (RAG hit) | Same query returns cached assistant reply | **60s** (`REDIS_REPLY_CACHE_TTL_SECONDS`) | Stub: no persist/search yet |

### Constants (`include/redis.h`)
- `REDIS_CACHE_TTL_SECONDS` = 300
- `REDIS_REPLY_CACHE_TTL_SECONDS` = 60

## 6. Reference

- Execution modes: `.cursor/rule.md` §2.
- Module API: `include/redis.h` — L1: `redis_set_counter`, `redis_set_value`, `redis_search_counter`, `redis_search_value`; L2: `redis_set_vector`, `redis_search_semantic` (callback-based). Storage facade: `storage_rag_search()` uses L2 when Redis connected.
