/*
 * Redis module — M4 storage.
 * Module rules: .cursor/redis.md (update this header if rules change API or constants).
 */
#ifndef M4_REDIS_H
#define M4_REDIS_H

#include <stddef.h>
#include <stdint.h>

#define REDIS_DEFAULT_HOST "127.0.0.1"
#define REDIS_DEFAULT_PORT 6379
/** Default TTL in seconds for L1 cache (e.g. chat history). Used when calling redis_set_value for cache keys. */
#define REDIS_CACHE_TTL_SECONDS 300
/** TTL in seconds for L2 reply cache (semantic/RAG). Same query within this window can return cached reply; after 1 min go to Ollama. */
#define REDIS_REPLY_CACHE_TTL_SECONDS 60

typedef struct redis_ctx redis_ctx_t;

redis_ctx_t *redis_create(const char *host, int port);
void redis_destroy(redis_ctx_t *ctx);

int redis_initial(redis_ctx_t *ctx);
void redis_disconnect(redis_ctx_t *ctx);
int redis_connected(redis_ctx_t *ctx);

/* L1 (Simple): exact match — counters and key/value cache. */
int redis_set_counter(redis_ctx_t *ctx, const char *tenant_id, const char *key, int64_t delta);
int redis_set_value(redis_ctx_t *ctx, const char *key, const char *value, int ttl_seconds);

int redis_search_counter(redis_ctx_t *ctx, const char *tenant_id, const char *key, int64_t *value);
/** Get value by key. Returns 1 if key exists and value was written to out (null-term); 0 if key missing or error. */
int redis_search_value(redis_ctx_t *ctx, const char *key, char *out, size_t out_size);

/* L2 (Semantic): RediSearch FT.SEARCH vector KNN. Use multilingual embeddings; fallback when L1 miss.
 * When implementing: apply REDIS_REPLY_CACHE_TTL_SECONDS (60) to the stored doc key (e.g. EXPIRE after HSET)
 * so same query within 1 minute returns cached reply; after 1 min go to Ollama. */
int redis_set_vector(redis_ctx_t *ctx, const char *tenant_id, const char *doc_id,
                    const float *vector, size_t dim, const char *payload, size_t payload_len);
/* redis_set_vector uses chat TTL; use redis_set_vector_ttl for custom TTL. */

/** Callback for each RAG hit: payload (snippet), score, userdata. Used by storage_rag_search. */
typedef void (*redis_semantic_hit_cb)(const char *payload, size_t payload_len, double score, void *userdata);
/** Default min cosine similarity for chat RAG semantic hits (in-memory stub). */
#define REDIS_SEMANTIC_MIN_SCORE_DEFAULT 0.85

/** L2 semantic search: best hit with cosine >= min_score invokes callback once (payload + score). Returns 0. */
int redis_search_semantic(redis_ctx_t *ctx, const char *tenant_id, const float *query_vector,
                          size_t dim, size_t k, double min_score,
                          redis_semantic_hit_cb callback, void *userdata);

/**
 * Store vector+payload. ttl_seconds: 0 → REDIS_REPLY_CACHE_TTL_SECONDS; <0 → no expiry (e.g. geo landmark index).
 */
int redis_set_vector_ttl(redis_ctx_t *ctx, const char *tenant_id, const char *doc_id,
                         const float *vector, size_t dim, const char *payload, size_t payload_len,
                         int ttl_seconds);

#endif /* M4_REDIS_H */
