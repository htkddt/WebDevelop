# Vector search & RAG — current support (Mongo, Redis, ELK)

Status of vector storage and RAG/semantic search in c-lib. **Redis (RediSearch) already supports Layer 2 vector/RAG search**; c-lib adds client support and wiring so AI flows can use it.

---

## Summary

| Backend | Vector **write** | Vector **search** / RAG |
|---------|------------------|--------------------------|
| **Mongo** | ✅ Yes | ❌ No (Atlas Vector Search not wired) |
| **Redis** | ✅ Yes (L2 when client enabled) | ✅ Yes (L2 RediSearch KNN — supported) |
| **ELK** | ❌ No (stub) | ❌ No |

---

## Redis (Layer 2 — primary RAG path)

- **Product support:** Redis Stack / RediSearch already supports **vector search** (FT.SEARCH KNN) and is the intended backend for RAG in MONGO_REDIS and MONGO_REDIS_ELK modes (`.cursor/redis.md`).
- **c-lib:** Client support added so that:
  - **Write:** When Redis is configured, each turn is also indexed in Redis L2 via `redis_set_vector` (tenant, doc_id, vector, payload). Storage calls this from `storage_append_turn` when a redis client is connected.
  - **Search:** `storage_rag_search(ctx, tenant_id, user_id, query_vector, dim, k, min_score, callback, userdata)` runs **Redis L2 semantic search** (RediSearch KNN) and invokes the callback for each hit (snippet, score). Used in the pre-query flow (step 4 → prompt context).
- **Flow:** User message → embed (Ollama) → RAG search (Redis L2) → if confident hits, prepend context to prompt → send to model. See `.cursor/PRE_QUERY_RAG_FLOW.md`.

---

## Mongo

- **Write:** Each turn is stored with a `vector` field (float array) via `storage_append_turn()`. Phase 1 generates the embedding with `ollama_embeddings()` and passes it in; with `USE_MONGOC=1`, Mongo documents in `bot.records` include `vector`, `turn`, `metadata.lang`, `metadata.score`.
- **Vector search / RAG:** Not implemented. There is no Atlas Vector Search (or similar) call in the codebase. The storage facade previously had `storage_vector_search`, which was removed as unused. To add RAG:
  - Implement a search path that takes `(tenant_id, user, vector, lang, k)` and returns top-k document IDs or snippets.
  - Use Atlas Vector Search index on the `vector` field with filter `tenant_id` (and optional `metadata.lang`), then wire that into the pre-query flow in `.cursor/PRE_QUERY_RAG_FLOW.md` (step 4 → prompt context).

---

## ELK

- **Write:** `storage_elk_ingest()` is a stub (TODO: POST to `{base_url}/ai_index/_doc?pipeline=auto_lang_processor`). No actual HTTP ingest in c-lib.
- **Vector search / RAG:** ELK is described as ingest + optional search in `.cursor/elk.md`. No vector or RAG search is implemented.

---

## What is in place for RAG

- **Phase 1 (done):** For each user message we compute `vector` and `lang` (`lang_detect`) and store them with the turn in Mongo (and in Redis L2 when Redis is configured). **`vector`** defaults to the built-in **384-D** hash embed (`API_VECTOR_GEN_CUSTOM`); set **`api_options_t.vector_gen_backend = API_VECTOR_GEN_OLLAMA`** to use **`ollama_embeddings`** (see **`docs/api.md`**).
- **RAG search:** `storage_rag_search()` uses **Redis L2** (RediSearch vector KNN) when the storage context has a connected Redis client. Used inside `api_chat` when `vector_search_enabled` and MONGO_REDIS / MONGO_REDIS_ELK: before building the prompt, we embed the user message, run RAG search, and prepend confident hits to the prompt context.
- **Public API:** RAG is internal to the library; callers use `api_chat` and get RAG-backed context automatically when Redis L2 is enabled.

---

## References

- `.cursor/mongo.md` — document shape (§0), vector search note (§5).
- `.cursor/redis.md` — L2 semantic (RediSearch vector KNN) strategy.
- `.cursor/elk.md` — ingest pipeline, optional search.
- `.cursor/PRE_QUERY_RAG_FLOW.md` — full RAG flow (steps 1–6).
- `.cursor/lang_vector_phase1.md` — VectorGen + LangDetector before storage.
