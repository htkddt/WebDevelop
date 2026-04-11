/*
 * Redis module — L1 (exact) and L2 (vector/RAG) per .cursor/redis.md.
 * Stub implementation when USE_REDIS is not set; with USE_REDIS=1 and -lhiredis
 * this can be extended to use Hiredis + RediSearch FT.SEARCH KNN.
 *
 * In-memory L2 reply cache: when USE_REDIS is not set, set_vector stores and
 * search_semantic searches a process-local cache (cosine similarity, TTL 60s)
 * so repeated same/similar queries within 1 minute return [REDIS] instead of OLLAMA.
 */
#include "redis.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

#define L2_CACHE_MAX_DIM    2048
#define L2_CACHE_MAX_PAYLOAD 2048
#define L2_CACHE_MAX_ENTRIES 64

typedef struct {
    char tenant_id[32];
    float vector[L2_CACHE_MAX_DIM];
    size_t dim;
    char payload[L2_CACHE_MAX_PAYLOAD];
    size_t payload_len;
    time_t expiry;
} l2_entry_t;

static l2_entry_t s_l2_cache[L2_CACHE_MAX_ENTRIES];
static int s_l2_count = 0;
static int s_l2_next = 0;

static double cosine_similarity(const float *a, const float *b, size_t dim) {
    if (dim == 0) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    double n = (na > 0.0 && nb > 0.0) ? (dot / (sqrt(na) * sqrt(nb))) : 0.0;
    return n;
}

static void l2_prune_expired(void) {
    time_t now = time(NULL);
    int w = 0;
    for (int i = 0; i < s_l2_count; i++) {
        /* expiry == 0: permanent entry (geo index), never prune */
        if (s_l2_cache[i].expiry == 0 || s_l2_cache[i].expiry > now) {
            if (w != i) s_l2_cache[w] = s_l2_cache[i];
            w++;
        }
    }
    s_l2_count = w;
}

struct redis_ctx {
    char host[128];
    int port;
    int connected;
#ifdef USE_REDIS
    void *redis_context; /* redisContext* when Hiredis linked */
#endif
};

redis_ctx_t *redis_create(const char *host, int port) {
    redis_ctx_t *ctx = (redis_ctx_t *)malloc(sizeof(redis_ctx_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    if (host) {
        size_t n = strlen(host);
        if (n >= sizeof(ctx->host)) n = sizeof(ctx->host) - 1;
        memcpy(ctx->host, host, n);
        ctx->host[n] = '\0';
    }
    ctx->port = (port > 0) ? port : REDIS_DEFAULT_PORT;
    return ctx;
}

void redis_destroy(redis_ctx_t *ctx) {
    if (!ctx) return;
#ifdef USE_REDIS
    if (ctx->redis_context) {
        /* redisFree((redisContext*)ctx->redis_context); */
        ctx->redis_context = NULL;
    }
#endif
    free(ctx);
}

int redis_initial(redis_ctx_t *ctx) {
    if (!ctx) return -1;
#ifdef USE_REDIS
    /* TODO: redisConnect(ctx->host, ctx->port); set ctx->redis_context; return -1 on failure */
    (void)ctx;
    return 0;
#else
    ctx->connected = 1; /* stub: consider "connected" for API readiness */
    return 0;
#endif
}

void redis_disconnect(redis_ctx_t *ctx) {
    if (!ctx) return;
#ifdef USE_REDIS
    if (ctx->redis_context) {
        /* redisFree((redisContext*)ctx->redis_context); */
        ctx->redis_context = NULL;
    }
#endif
    ctx->connected = 0;
}

int redis_connected(redis_ctx_t *ctx) {
    return (ctx && ctx->connected) ? 1 : 0;
}

/* L1 */
int redis_set_counter(redis_ctx_t *ctx, const char *tenant_id, const char *key, int64_t delta) {
    (void)ctx;
    (void)tenant_id;
    (void)key;
    (void)delta;
    return 0;
}

int redis_set_value(redis_ctx_t *ctx, const char *key, const char *value, int ttl_seconds) {
    (void)ctx;
    (void)key;
    (void)value;
    (void)ttl_seconds;
    return 0;
}

int redis_search_counter(redis_ctx_t *ctx, const char *tenant_id, const char *key, int64_t *value) {
    if (value) *value = 0;
    (void)ctx;
    (void)tenant_id;
    (void)key;
    return 0;
}

int redis_search_value(redis_ctx_t *ctx, const char *key, char *out, size_t out_size) {
    if (out && out_size) out[0] = '\0';
    (void)ctx;
    (void)key;
    return 0;
}

/* L2 (Semantic / RAG). In-memory cache when USE_REDIS not set: store by set_vector, search by cosine similarity. */
int redis_set_vector_ttl(redis_ctx_t *ctx, const char *tenant_id, const char *doc_id,
                         const float *vector, size_t dim, const char *payload, size_t payload_len,
                         int ttl_seconds) {
    (void)doc_id;
    if (!ctx || !vector || dim == 0 || dim > L2_CACHE_MAX_DIM) return 0;
    if (!payload) payload_len = 0;
    if (payload_len > L2_CACHE_MAX_PAYLOAD - 1) payload_len = L2_CACHE_MAX_PAYLOAD - 1;

    l2_prune_expired();
    l2_entry_t *e;
    if (s_l2_count < L2_CACHE_MAX_ENTRIES) {
        e = &s_l2_cache[s_l2_count++];
    } else {
        e = &s_l2_cache[s_l2_next % L2_CACHE_MAX_ENTRIES];
        s_l2_next++;
    }
    size_t tlen = 0;
    if (tenant_id) {
        tlen = strlen(tenant_id);
        if (tlen >= sizeof(e->tenant_id)) tlen = sizeof(e->tenant_id) - 1;
        memcpy(e->tenant_id, tenant_id, tlen);
    }
    e->tenant_id[tlen] = '\0';
    memcpy(e->vector, vector, dim * sizeof(float));
    e->dim = dim;
    if (payload_len > 0) memcpy(e->payload, payload, payload_len);
    e->payload[payload_len] = '\0';
    e->payload_len = payload_len;
    {
        time_t now = time(NULL);
        if (ttl_seconds < 0)
            e->expiry = 0; /* permanent */
        else if (ttl_seconds == 0)
            e->expiry = now + REDIS_REPLY_CACHE_TTL_SECONDS;
        else
            e->expiry = now + (time_t)ttl_seconds;
    }
    return 0;
}

int redis_set_vector(redis_ctx_t *ctx, const char *tenant_id, const char *doc_id,
                    const float *vector, size_t dim, const char *payload, size_t payload_len) {
    return redis_set_vector_ttl(ctx, tenant_id, doc_id, vector, dim, payload, payload_len, 0);
}

int redis_search_semantic(redis_ctx_t *ctx, const char *tenant_id, const float *query_vector,
                          size_t dim, size_t k, double min_score,
                          redis_semantic_hit_cb callback, void *userdata) {
    if (!ctx || !callback || dim == 0 || dim > L2_CACHE_MAX_DIM) return 0;
    (void)k;
    l2_prune_expired();
    time_t now = time(NULL);
    const char *tid = tenant_id && tenant_id[0] ? tenant_id : "default";
    double best_score = 0.0;
    const char *best_payload = NULL;
    size_t best_len = 0;

    for (int i = 0; i < s_l2_count; i++) {
        l2_entry_t *e = &s_l2_cache[i];
        if (e->dim != dim) continue;
        if (e->expiry != 0 && e->expiry <= now) continue;
        if (strcmp(e->tenant_id, tid) != 0) continue;
        double score = cosine_similarity(query_vector, e->vector, dim);
        if (score >= min_score && score > best_score) {
            best_score = score;
            best_payload = e->payload;
            best_len = e->payload_len;
        }
    }
    if (best_payload && best_len > 0) {
        callback(best_payload, best_len, best_score, userdata);
        return 0;
    }
    return 0;
}
