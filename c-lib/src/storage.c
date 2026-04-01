#include "storage.h"
#include "tenant.h"
#include "redis.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef USE_MONGOC
#include <limits.h>
#include <mongoc/mongoc.h>
#include "vector_generate.h"
#endif

#define AI_LOGS_NAME_MAX 63
#define RAG_PAYLOAD_MAX 2048
#define HISTORY_CACHE_KEY_PREFIX "m4:cache:history:"
#define HISTORY_CACHE_BUF_SIZE   (64 * 1024)

struct storage_ctx {
    char mongo_uri[256];
    char redis_host[128];
    int redis_port;
    char es_host[128];
    int es_port;
    int connected;
    char ai_logs_db[AI_LOGS_NAME_MAX + 1];   /* if [0] != '\0', use instead of STORAGE_AI_LOGS_DB */
    char ai_logs_coll[AI_LOGS_NAME_MAX + 1]; /* if [0] != '\0', use instead of STORAGE_AI_LOGS_COLLECTION */
    redis_ctx_t *redis; /* L2 RAG when configured */
#ifdef USE_MONGOC
    mongoc_client_t *mongo_client;
#endif
};

static int valid_log_name(const char *s) {
    if (!s || !*s) return 0;
    size_t n = 0;
    for (; *s && n <= AI_LOGS_NAME_MAX; s++, n++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
            (*s >= '0' && *s <= '9') || *s == '_')
            continue;
        return 0;
    }
    return (*s == '\0' && n >= 1 && n <= AI_LOGS_NAME_MAX) ? 1 : 0;
}

storage_ctx_t *storage_create(const char *mongo_uri, const char *redis_host, int redis_port,
                              const char *es_host, int es_port) {
    storage_ctx_t *ctx = (storage_ctx_t *)malloc(sizeof(storage_ctx_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    if (mongo_uri) strncpy(ctx->mongo_uri, mongo_uri, sizeof(ctx->mongo_uri) - 1);
    if (redis_host) strncpy(ctx->redis_host, redis_host, sizeof(ctx->redis_host) - 1);
    ctx->redis_port = redis_port;
    if (es_host) strncpy(ctx->es_host, es_host, sizeof(ctx->es_host) - 1);
    ctx->es_port = es_port;
    return ctx;
}

void storage_destroy(storage_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->redis) {
        redis_destroy(ctx->redis);
        ctx->redis = NULL;
    }
#ifdef USE_MONGOC
    if (ctx->mongo_client) {
        mongoc_client_destroy(ctx->mongo_client);
        ctx->mongo_client = NULL;
        mongoc_cleanup();
    }
#endif
    free(ctx);
}

/* Ensure indexes on records (chat) collection. Both created when USE_MONGOC=1 and storage_connect() runs. */
#ifdef USE_MONGOC
static void storage_ensure_records_index(storage_ctx_t *ctx) {
    if (!ctx || !ctx->mongo_client) return;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);
    bson_error_t err;

    /* 1. tenant_id + createdAt (for chat history: filter tenant_id, sort by createdAt asc, limit) */
    {
        bson_t keys;
        bson_init(&keys);
        BSON_APPEND_INT32(&keys, "tenant_id", 1);
        BSON_APPEND_INT32(&keys, "createdAt", 1);
        mongoc_index_model_t *model = mongoc_index_model_new(&keys, NULL);
        bson_destroy(&keys);
        if (model) {
            mongoc_index_model_t *models[] = { model };
            if (mongoc_collection_create_indexes_with_opts(coll, models, 1, NULL, NULL, &err))
                fprintf(stderr, "[STORAGE] index created: %s.%s { tenant_id: 1, createdAt: 1 }\n",
                        STORAGE_CHAT_DB, STORAGE_CHAT_COLLECTION);
            else if (err.code != 85 && err.code != 86)
                fprintf(stderr, "[STORAGE] index tenant_id+createdAt failed (code %u): %s\n", (unsigned)err.code, err.message);
            mongoc_index_model_destroy(model);
        }
    }

    /* 2. tenant_id + user + timestamp (for tenant/user time-ordered queries) */
    {
        bson_t keys;
        bson_init(&keys);
        BSON_APPEND_INT32(&keys, "tenant_id", 1);
        BSON_APPEND_INT32(&keys, "user", 1);
        BSON_APPEND_INT32(&keys, "timestamp", -1);
        mongoc_index_model_t *model = mongoc_index_model_new(&keys, NULL);
        bson_destroy(&keys);
        if (model) {
            mongoc_index_model_t *models[] = { model };
            if (mongoc_collection_create_indexes_with_opts(coll, models, 1, NULL, NULL, &err))
                fprintf(stderr, "[STORAGE] index created: %s.%s { tenant_id: 1, user: 1, timestamp: -1 }\n",
                        STORAGE_CHAT_DB, STORAGE_CHAT_COLLECTION);
            else if (err.code != 85 && err.code != 86)
                fprintf(stderr, "[STORAGE] index tenant_id+user+timestamp failed (code %u): %s\n", (unsigned)err.code, err.message);
            mongoc_index_model_destroy(model);
        }
    }

    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
}

/* Ensure geo_atlas collection and indexes for geo_learning module (.cursor/geo_leanring.md). Auto-create on connect. */
static void storage_ensure_geo_atlas_index(storage_ctx_t *ctx) {
    if (!ctx || !ctx->mongo_client) return;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_GEO_ATLAS_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_GEO_ATLAS_COLLECTION);
    bson_error_t err;

    /* 1. tenant_id + name_normalized (non-unique: allows pending vs verified; drop old unique index manually if migrating) */
    {
        bson_t keys;
        bson_init(&keys);
        BSON_APPEND_INT32(&keys, "tenant_id", 1);
        BSON_APPEND_INT32(&keys, "name_normalized", 1);
        mongoc_index_model_t *model = mongoc_index_model_new(&keys, NULL);
        bson_destroy(&keys);
        if (model) {
            mongoc_index_model_t *models[] = { model };
            if (mongoc_collection_create_indexes_with_opts(coll, models, 1, NULL, NULL, &err))
                fprintf(stderr, "[STORAGE] index created: %s.%s { tenant_id: 1, name_normalized: 1 }\n",
                        STORAGE_GEO_ATLAS_DB, STORAGE_GEO_ATLAS_COLLECTION);
            else if (err.code != 85 && err.code != 86)
                fprintf(stderr, "[STORAGE] index geo_atlas tenant+name failed (code %u): %s\n", (unsigned)err.code, err.message);
            mongoc_index_model_destroy(model);
        }
    }

    /* 2. createdAt (for get_landmarks sort by createdAt desc) */
    {
        bson_t keys;
        bson_init(&keys);
        BSON_APPEND_INT32(&keys, "createdAt", -1);
        mongoc_index_model_t *model = mongoc_index_model_new(&keys, NULL);
        bson_destroy(&keys);
        if (model) {
            mongoc_index_model_t *models[] = { model };
            if (mongoc_collection_create_indexes_with_opts(coll, models, 1, NULL, NULL, &err))
                fprintf(stderr, "[STORAGE] index created: %s.%s { createdAt: -1 }\n",
                        STORAGE_GEO_ATLAS_DB, STORAGE_GEO_ATLAS_COLLECTION);
            else if (err.code != 85 && err.code != 86)
                fprintf(stderr, "[STORAGE] index geo_atlas createdAt failed (code %u): %s\n", (unsigned)err.code, err.message);
            mongoc_index_model_destroy(model);
        }
    }

    /* 3. tenant_id + name_normalized + country (composite lookup / dedup hint, §13) */
    {
        bson_t keys;
        bson_init(&keys);
        BSON_APPEND_INT32(&keys, "tenant_id", 1);
        BSON_APPEND_INT32(&keys, "name_normalized", 1);
        BSON_APPEND_INT32(&keys, "country", 1);
        mongoc_index_model_t *model = mongoc_index_model_new(&keys, NULL);
        bson_destroy(&keys);
        if (model) {
            mongoc_index_model_t *models[] = { model };
            if (mongoc_collection_create_indexes_with_opts(coll, models, 1, NULL, NULL, &err))
                fprintf(stderr, "[STORAGE] index created: %s.%s { tenant_id:1, name_normalized:1, country:1 }\n",
                        STORAGE_GEO_ATLAS_DB, STORAGE_GEO_ATLAS_COLLECTION);
            else if (err.code != 85 && err.code != 86)
                fprintf(stderr, "[STORAGE] index geo_atlas composite failed (code %u): %s\n", (unsigned)err.code, err.message);
            mongoc_index_model_destroy(model);
        }
    }

    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
}
#endif

int storage_connect(storage_ctx_t *ctx) {
    if (!ctx) return -1;
#ifdef USE_MONGOC
    if (ctx->mongo_uri[0] != '\0') {
        mongoc_init();
        ctx->mongo_client = mongoc_client_new(ctx->mongo_uri);
        if (!ctx->mongo_client) {
            fprintf(stderr, "[STORAGE] MongoDB client failed: invalid URI or not running\n");
            mongoc_cleanup();
            return -1;
        }
        storage_ensure_records_index(ctx);
        storage_ensure_geo_atlas_index(ctx);
    }
#endif
    if (ctx->redis_host[0] != '\0') {
        ctx->redis = redis_create(ctx->redis_host, ctx->redis_port);
        if (ctx->redis && redis_initial(ctx->redis) != 0) {
            redis_destroy(ctx->redis);
            ctx->redis = NULL;
        }
    }
    ctx->connected = 1;
    return 0;
}

void storage_disconnect(storage_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->redis) {
        redis_disconnect(ctx->redis);
        redis_destroy(ctx->redis);
        ctx->redis = NULL;
    }
#ifdef USE_MONGOC
    if (ctx->mongo_client) {
        mongoc_client_destroy(ctx->mongo_client);
        ctx->mongo_client = NULL;
    }
#endif
    ctx->connected = 0;
}

int storage_mongo_connected(storage_ctx_t *ctx) {
    if (!ctx) return 0;
#ifdef USE_MONGOC
    /* `connected` is set even when mongo_uri was empty (no client); use real client pointer. */
    return ctx->mongo_client ? 1 : 0;
#else
    (void)ctx;
    return 0;
#endif
}

int storage_redis_connected(storage_ctx_t *ctx) {
    return (ctx && ctx->redis && redis_connected(ctx->redis)) ? 1 : 0;
}

typedef struct {
    storage_rag_hit_cb cb;
    void *u;
    double min_score;
} rag_adapter_t;

static void rag_hit_adapter(const char *payload, size_t payload_len, double score, void *userdata) {
    rag_adapter_t *a = (rag_adapter_t *)userdata;
    if (!a || !a->cb || (a->min_score > 0 && score < a->min_score)) return;
    char buf[RAG_PAYLOAD_MAX];
    size_t n = payload_len < (RAG_PAYLOAD_MAX - 1) ? payload_len : (RAG_PAYLOAD_MAX - 1);
    if (n) memcpy(buf, payload, n);
    buf[n] = '\0';
    a->cb(buf, score, a->u);
}

int storage_rag_search(storage_ctx_t *ctx, const char *tenant_id, const char *user_id,
                      const float *query_vector, size_t dim, size_t k, double min_score,
                      storage_rag_hit_cb callback, void *userdata) {
    if (!ctx || !callback) return -1;
    if (!ctx->redis || !redis_connected(ctx->redis)) return 0;
    const char *tid = tenant_id ? tenant_id : "default";
    (void)user_id;
    rag_adapter_t adapter = { .cb = callback, .u = userdata, .min_score = min_score };
    return redis_search_semantic(ctx->redis, tid, query_vector, dim, k,
                                  REDIS_SEMANTIC_MIN_SCORE_DEFAULT,
                                  rag_hit_adapter, &adapter);
}

static void storage_geo_redis_lane(char *out, size_t out_sz, const char *tenant_id) {
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : "default";
    (void)snprintf(out, out_sz, "%s|m4geo", tid);
}

typedef struct {
    int found;
} geo_redis_hit_t;

static void geo_redis_semantic_cb(const char *payload, size_t payload_len, double score, void *userdata) {
    (void)payload;
    (void)payload_len;
    (void)score;
    geo_redis_hit_t *h = (geo_redis_hit_t *)userdata;
    h->found = 1;
}

int storage_geo_redis_find_similar(storage_ctx_t *ctx, const char *tenant_id,
                                   const float *vector, size_t dim, double threshold) {
    if (!ctx || !vector || dim == 0) return 0;
    if (!ctx->redis || !redis_connected(ctx->redis)) return 0;
    char lane[80];
    storage_geo_redis_lane(lane, sizeof(lane), tenant_id);
    geo_redis_hit_t h = { 0 };
    redis_search_semantic(ctx->redis, lane, vector, dim, 8, threshold, geo_redis_semantic_cb, &h);
    return h.found ? 1 : 0;
}

int storage_geo_redis_index_landmark(storage_ctx_t *ctx, const char *tenant_id, const char *doc_id,
                                     const float *vector, size_t dim, const char *name_label) {
    if (!ctx || !doc_id || !vector || dim == 0) return -1;
    if (!ctx->redis || !redis_connected(ctx->redis)) return 0;
    char lane[80];
    storage_geo_redis_lane(lane, sizeof(lane), tenant_id);
    const char *pay = name_label ? name_label : "";
    size_t plen = strlen(pay);
    return redis_set_vector_ttl(ctx->redis, lane, doc_id, vector, dim, pay, plen, -1);
}

/* Maps to temp.c mongo_batch_upload. Use db=STORAGE_MONGO_DB_NAME, coll=STORAGE_MONGO_COLLECTION. */
int storage_upsert_batch(storage_ctx_t *ctx, const char *tenant_id,
                         const void *records, size_t count) {
    (void)tenant_id;
    (void)records;
    (void)count;
    if (!ctx || !ctx->connected) return -1;
    /* TODO: mongoc_client_get_database(client, STORAGE_MONGO_DB_NAME),
     *       mongoc_database_get_collection(db, STORAGE_MONGO_COLLECTION),
     *       bulk insert with BSON_APPEND_UTF8(doc, "tenant_id", tenant_id), "content", "ts". */
    return 0;
}

/* ELK ingest stub. */
int storage_elk_ingest(storage_ctx_t *ctx, const char *elk_base_url, const char *raw_text) {
    (void)ctx;
    (void)elk_base_url;
    (void)raw_text;
    /* TODO: with libcurl: POST { "raw_content": raw_text } to elk_base_url/ai_index/_doc?pipeline=auto_lang_processor */
    return 0;
}

/* Append one chat message: always INSERT a new document (append-only). We never update an existing record; the "newest" is the doc with latest createdAt. */
int storage_append_chat(storage_ctx_t *ctx, const char *tenant_id, const char *role,
                        const char *content, const char *timestamp) {
    if (!ctx || !ctx->connected) return -1;
    const char *tid = tenant_id ? tenant_id : "";
    const char *r = role ? role : "";
    const char *c = content ? content : "";
    const char *ts = timestamp ? timestamp : "";

#ifdef USE_MONGOC
    if (!ctx->mongo_client) return -1;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_t doc;
    bson_init(&doc);
    /* BSON UTF-8: content and fields support full Unicode (e.g. Vietnamese, CJK). */
    BSON_APPEND_UTF8(&doc, "tenant_id", tid);
    BSON_APPEND_UTF8(&doc, "role", r);
    BSON_APPEND_UTF8(&doc, "content", c);
    BSON_APPEND_UTF8(&doc, "ts", ts);
    /* createdAt: current timestamp (default for all MongoDB records). */
    bson_append_date_time(&doc, "createdAt", -1, (int64_t)time(NULL) * 1000);

    bson_error_t error;
    int ok = mongoc_collection_insert_one(coll, &doc, NULL, NULL, &error);
    bson_destroy(&doc);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);

    if (!ok) {
        fprintf(stderr, "[STORAGE] MongoDB insert failed: %s\n", error.message);
        return -1;
    }
    return 0;
#else
    fprintf(stderr, "[STORAGE] would write chat to MongoDB %s.%s tenant=%s role=%s ts=%s (build with USE_MONGOC=1)\n",
            STORAGE_CHAT_DB, STORAGE_CHAT_COLLECTION, tid, r, ts);
    (void)c;
    return 0;
#endif
}

#ifdef USE_MONGOC
/**
 * Embed provenance for migration/sync when models or backends change (.cursor/embed_migration.md).
 * Chat: fields live under metadata; geo_atlas: top-level next to vector / embed_model_id.
 */
static void storage_bson_append_embed_provenance(bson_t *target, const char *model_id, size_t vector_dim) {
    BSON_APPEND_INT32(target, "embed_schema", 1);
    if (vector_dim > 0 && vector_dim <= (size_t)INT32_MAX)
        BSON_APPEND_INT32(target, "vector_dim", (int32_t)vector_dim);
    if (model_id && model_id[0]) {
        if (strcmp(model_id, VECTOR_GEN_MODEL_ID) == 0)
            BSON_APPEND_UTF8(target, "embed_family", "custom");
        else
            BSON_APPEND_UTF8(target, "embed_family", "ollama");
    } else if (vector_dim > 0)
        BSON_APPEND_UTF8(target, "embed_family", "legacy");
}
#endif

/* Append one turn (input + assistant) as a single document — new shape per .cursor/mongo.md §0. */
int storage_append_turn(storage_ctx_t *ctx, const char *tenant_id, const char *user_id,
                        const char *input, const char *assistant, const char *timestamp,
                        const float *vector, size_t vector_dim,
                        const char *lang, double lang_score,
                        const char *embed_model_id,
                        const char *temp_message_id,
                        int has_logic_conflict) {
    if (!ctx || !ctx->connected) return -1;
    const char *tid = tenant_id ? tenant_id : "default";
    const char *uid = user_id ? user_id : "default";
    const char *inp = input ? input : "";
    const char *ast = assistant ? assistant : "";
    const char *ts = timestamp ? timestamp : "";
    const char *model_id = embed_model_id && embed_model_id[0] ? embed_model_id : "";
    /* lang_vector_phase1.md: if score < 0.5, mark as "mixed" in metadata */
    const char *lang_str = (lang && lang[0] && lang_score >= 0.5) ? lang : "mixed";

#ifdef USE_MONGOC
    if (!ctx->mongo_client) return -1;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_t doc;
    bson_init(&doc);
    BSON_APPEND_UTF8(&doc, "tenant_id", tid);
    BSON_APPEND_UTF8(&doc, "user", uid);
    if (temp_message_id && temp_message_id[0])
        BSON_APPEND_UTF8(&doc, "temp_message_id", temp_message_id);

    /* vector: array of doubles (empty if vector NULL or vector_dim 0) */
    {
        bson_t arr;
        bson_append_array_begin(&doc, "vector", -1, &arr);
        if (vector && vector_dim > 0) {
            char key[16];
            for (size_t i = 0; i < vector_dim; i++) {
                snprintf(key, sizeof(key), "%zu", i);
                bson_append_double(&arr, key, -1, (double)vector[i]);
            }
        }
        bson_append_array_end(&doc, &arr);
    }

    /* turn: { input, assistant } */
    bson_t turn;
    bson_append_document_begin(&doc, "turn", -1, &turn);
    BSON_APPEND_UTF8(&turn, "input", inp);
    BSON_APPEND_UTF8(&turn, "assistant", ast);
    bson_append_document_end(&doc, &turn);
    BSON_APPEND_UTF8(&doc, "timestamp", ts);
    bson_append_date_time(&doc, "createdAt", -1, (int64_t)time(NULL) * 1000);

    /* metadata: ver, encrypted, model_id, lang, score (per .cursor/mongo.md §0) */
    bson_t meta;
    bson_append_document_begin(&doc, "metadata", -1, &meta);
    BSON_APPEND_INT32(&meta, "ver", 1);
    BSON_APPEND_BOOL(&meta, "encrypted", false);
    BSON_APPEND_UTF8(&meta, "model_id", model_id);
    BSON_APPEND_UTF8(&meta, "lang", lang_str);
    BSON_APPEND_DOUBLE(&meta, "score", lang_score);
    BSON_APPEND_BOOL(&meta, "has_logic_conflict", has_logic_conflict ? true : false);
    storage_bson_append_embed_provenance(&meta, model_id, vector_dim);
    bson_append_document_end(&doc, &meta);

    bson_error_t error;
    int ok = mongoc_collection_insert_one(coll, &doc, NULL, NULL, &error);
    bson_destroy(&doc);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);

    if (!ok) {
        fprintf(stderr, "[STORAGE] MongoDB insert turn failed: %s\n", error.message);
        return -1;
    }
    /* Redis L2: index turn for RAG when connected and vector present */
    if (ctx->redis && redis_connected(ctx->redis) && vector && vector_dim > 0) {
        char doc_id[64];
        snprintf(doc_id, sizeof(doc_id), "%s:%s:%ld", tid, uid, (long)time(NULL));
        char payload[RAG_PAYLOAD_MAX];
        size_t plen = (size_t)snprintf(payload, sizeof(payload), "%s\n%s", inp, ast);
        if (plen >= sizeof(payload)) plen = sizeof(payload) - 1;
        redis_set_vector(ctx->redis, tid, doc_id, vector, vector_dim, payload, plen);
    }
    return 0;
#else
    fprintf(stderr, "[STORAGE] would write turn to MongoDB %s.%s tenant=%s user=%s (build with USE_MONGOC=1)\n",
            STORAGE_CHAT_DB, STORAGE_CHAT_COLLECTION, tid, uid);
    (void)inp;
    (void)ast;
    (void)ts;
    (void)model_id;
    (void)vector;
    (void)vector_dim;
    (void)lang_str;
    (void)lang_score;
    (void)temp_message_id;
    if (has_logic_conflict)
        fprintf(stderr, "[STORAGE] turn metadata has_logic_conflict=1 (stub build)\n");
    if (ctx->redis && redis_connected(ctx->redis) && vector && vector_dim > 0) {
        char doc_id[64];
        snprintf(doc_id, sizeof(doc_id), "%s:%s:%ld", tid, uid, (long)time(NULL));
        char payload[RAG_PAYLOAD_MAX];
        size_t plen = (size_t)snprintf(payload, sizeof(payload), "%s\n%s", inp, ast);
        if (plen >= sizeof(payload)) plen = sizeof(payload) - 1;
        redis_set_vector(ctx->redis, tid, doc_id, vector, vector_dim, payload, plen);
    }
    return 0;
#endif
}

/* Insert one log line into ai_logs collection (for stat error/warning counts and ELK). */
int storage_append_ai_log(storage_ctx_t *ctx, const char *tenant_id, const char *level,
                          const char *message) {
    if (!ctx) return -1;
    const char *tid = tenant_id ? tenant_id : "default";
    const char *lvl = level ? level : "info";
    const char *msg = message ? message : "";

    time_t now = time(NULL);
    char ts_buf[32];
    struct tm *tm = localtime(&now);
    if (tm)
        snprintf(ts_buf, sizeof(ts_buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        ts_buf[0] = '\0';

#ifdef USE_MONGOC
    if (!ctx->connected || !ctx->mongo_client) return -1;
    const char *log_db   = (ctx->ai_logs_db[0] != '\0')   ? ctx->ai_logs_db   : STORAGE_AI_LOGS_DB;
    const char *log_coll = (ctx->ai_logs_coll[0] != '\0') ? ctx->ai_logs_coll : STORAGE_AI_LOGS_COLLECTION;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, log_db);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, log_coll);

    bson_t doc;
    bson_init(&doc);
    BSON_APPEND_UTF8(&doc, "tenant_id", tid);
    BSON_APPEND_UTF8(&doc, "level", lvl);
    BSON_APPEND_UTF8(&doc, "message", msg);
    BSON_APPEND_UTF8(&doc, "ts", ts_buf);
    /* createdAt: current timestamp (default for all MongoDB records). */
    bson_append_date_time(&doc, "createdAt", -1, (int64_t)now * 1000);

    bson_error_t error;
    int ok = mongoc_collection_insert_one(coll, &doc, NULL, NULL, &error);
    bson_destroy(&doc);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);

    if (!ok) {
        fprintf(stderr, "[STORAGE] ai_logs insert failed: %s\n", error.message);
        return -1;
    }
    /* Optionally also send to ELK (same message for analytics). */
    if (ctx->es_host[0] != '\0') {
        char elk_url[256];
        snprintf(elk_url, sizeof(elk_url), "http://%s:%d", ctx->es_host, ctx->es_port);
        storage_elk_ingest(ctx, elk_url, msg);
    }
    return 0;
#else
    (void)ts_buf;
    (void)msg;
    (void)tid;
    (void)lvl;
    fprintf(stderr, "[STORAGE] would write ai_logs (USE_MONGOC=0)\n");
    return 0;
#endif
}

int storage_set_ai_logs(storage_ctx_t *ctx, const char *db, const char *collection) {
    if (!ctx || !valid_log_name(db) || !valid_log_name(collection)) return -1;
    strncpy(ctx->ai_logs_db, db, AI_LOGS_NAME_MAX);
    ctx->ai_logs_db[AI_LOGS_NAME_MAX] = '\0';
    strncpy(ctx->ai_logs_coll, collection, AI_LOGS_NAME_MAX);
    ctx->ai_logs_coll[AI_LOGS_NAME_MAX] = '\0';
    return 0;
}

#ifdef USE_MONGOC
static int storage_count_chat_doc_messages(const bson_t *doc) {
    bson_iter_t iter;
    if (bson_iter_init(&iter, doc) && bson_iter_find(&iter, "turn"))
        return 2;
    return 1;
}

typedef struct {
    int skip;   /* drop this many leading messages (keep the tail of max_messages) */
    int remain; /* emit at most this many */
} storage_history_emit_t;

/* Resolve display ts string into ts_buf; returns pointer to ts_buf for UTF-8 ts. */
static const char *storage_chat_doc_ts(const bson_t *doc, char *ts_buf, size_t ts_sz) {
    bson_iter_t iter;
    ts_buf[0] = '\0';
    if (bson_iter_init(&iter, doc) && bson_iter_find(&iter, "createdAt")
        && BSON_ITER_HOLDS_DATE_TIME(&iter)) {
        int64_t epoch_ms = bson_iter_date_time(&iter);
        (void)snprintf(ts_buf, ts_sz, "%lld", (long long)epoch_ms);
    } else {
        if (bson_iter_init(&iter, doc) && bson_iter_find(&iter, "timestamp")) {
            const char *v = bson_iter_utf8(&iter, NULL);
            if (v && v[0]) (void)snprintf(ts_buf, ts_sz, "%s", v);
        }
        if (ts_buf[0] == '\0' && bson_iter_init(&iter, doc) && bson_iter_find(&iter, "ts")) {
            const char *v = bson_iter_utf8(&iter, NULL);
            if (v && v[0]) (void)snprintf(ts_buf, ts_sz, "%s", v);
        }
    }
    return ts_buf;
}

static void storage_emit_one_line(storage_history_emit_t *st, storage_chat_history_cb cb, void *ud,
                                  const char *role, const char *content, const char *ts) {
    if (st->skip > 0) {
        st->skip--;
        return;
    }
    if (st->remain <= 0) return;
    cb(role, content, ts, ud);
    st->remain--;
}

/* One chat document → one or two callbacks (turn shape or legacy), respecting skip/remain. */
static void storage_emit_chat_doc_messages(const bson_t *doc, storage_history_emit_t *st,
                                           storage_chat_history_cb callback, void *userdata) {
    bson_iter_t iter;
    char ts_buf[24];
    const char *ts_str = storage_chat_doc_ts(doc, ts_buf, sizeof(ts_buf));

    if (bson_iter_init(&iter, doc) && bson_iter_find(&iter, "turn")) {
        bson_iter_t turn_iter;
        const char *input_str = "";
        const char *assistant_str = "";
        if (bson_iter_recurse(&iter, &turn_iter)) {
            while (bson_iter_next(&turn_iter)) {
                const char *key = bson_iter_key(&turn_iter);
                if (strcmp(key, "input") == 0)
                    input_str = bson_iter_utf8(&turn_iter, NULL);
                else if (strcmp(key, "assistant") == 0)
                    assistant_str = bson_iter_utf8(&turn_iter, NULL);
            }
        }
        storage_emit_one_line(st, callback, userdata, "user", input_str, ts_str);
        storage_emit_one_line(st, callback, userdata, "assistant", assistant_str, ts_str);
    } else {
        const char *role = "user";
        const char *content = "";
        if (bson_iter_init(&iter, doc) && bson_iter_find(&iter, "role"))
            role = bson_iter_utf8(&iter, NULL);
        if (bson_iter_init(&iter, doc) && bson_iter_find(&iter, "content"))
            content = bson_iter_utf8(&iter, NULL);
        storage_emit_one_line(st, callback, userdata, role, content, ts_str);
    }
}
#endif

/* Get chat history: up to `limit` newest **documents**, then emit at most `limit` **messages** (most recent). */
int storage_get_chat_history(storage_ctx_t *ctx, const char *tenant_id, int limit,
                             storage_chat_history_cb callback, void *userdata) {
    if (!ctx || !callback || limit <= 0) return -1;
#ifndef USE_MONGOC
    (void)tenant_id;
    (void)userdata;
    return 0;
#else
    if (!ctx->connected || !ctx->mongo_client) return -1;
    const char *tid = tenant_id ? tenant_id : "";
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "tenant_id", tid);
    bson_t opts;
    bson_init(&opts);
    bson_t sort;
    bson_init(&sort);
    /*
     * Sort **descending**; cap how many **documents** we read (≤ max messages needed).
     * Messages are trimmed to `limit` (batch size in messages) after load.
     */
    BSON_APPEND_INT32(&sort, "createdAt", -1);
    bson_append_document(&opts, "sort", -1, &sort);
    bson_append_int32(&opts, "limit", -1, limit);
    bson_destroy(&sort);

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(coll, &filter, &opts, NULL);
    bson_destroy(&filter);
    bson_destroy(&opts);

    const bson_t *doc;
    bson_t **batch = (bson_t **)calloc((size_t)limit, sizeof(bson_t *));
    if (!batch) {
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(coll);
        mongoc_database_destroy(db);
        return -1;
    }
    int got = 0;
    while (got < limit && mongoc_cursor_next(cursor, &doc)) {
        batch[got] = bson_copy(doc);
        if (!batch[got]) {
            for (int j = 0; j < got; j++) bson_destroy(batch[j]);
            free(batch);
            mongoc_cursor_destroy(cursor);
            mongoc_collection_destroy(coll);
            mongoc_database_destroy(db);
            return -1;
        }
        got++;
    }
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);

    /* Chronological message count across fetched docs (oldest doc → newest doc). */
    int total_msgs = 0;
    for (int i = got - 1; i >= 0; i--)
        total_msgs += storage_count_chat_doc_messages(batch[i]);
    int skip = (total_msgs > limit) ? (total_msgs - limit) : 0;
    storage_history_emit_t st;
    st.skip = skip;
    st.remain = limit;

    for (int i = got - 1; i >= 0; i--)
        storage_emit_chat_doc_messages(batch[i], &st, callback, userdata);
    for (int i = 0; i < got; i++)
        bson_destroy(batch[i]);
    free(batch);
    return 0;
#endif
}

#ifdef USE_MONGOC
/* Append one message to cache buffer: "role\tcontent\tts\n" with \n in content escaped as \\n. */
static size_t history_serialize_append(char *buf, size_t buf_size, size_t *used,
                                     const char *role, const char *content, const char *ts) {
    size_t u = *used;
    if (u >= buf_size - 2) return 0;
    const char *r = role ? role : "user";
    while (*r && u < buf_size - 2) { buf[u++] = *r++; }
    if (u >= buf_size - 2) return 0;
    buf[u++] = '\t';
    const char *p = content ? content : "";
    while (*p && u < buf_size - 4) {
        if (*p == '\n') { buf[u++] = '\\'; buf[u++] = 'n'; p++; }
        else buf[u++] = *p++;
    }
    if (u >= buf_size - 2) return 0;
    buf[u++] = '\t';
    const char *t = ts ? ts : "";
    while (*t && u < buf_size - 2) { buf[u++] = *t++; }
    if (u >= buf_size - 2) return 0;
    buf[u++] = '\n';
    buf[u] = '\0';
    *used = u;
    return u;
}

typedef struct {
    char *buf;
    size_t buf_size;
    size_t used;
    storage_chat_history_cb cb;
    void *ud;
} history_accum_t;

static void history_accum_cb(const char *role, const char *content, const char *ts, void *userdata) {
    history_accum_t *a = (history_accum_t *)userdata;
    history_serialize_append(a->buf, a->buf_size, &a->used, role, content, ts);
    a->cb(role, content, ts, a->ud);
}
#endif

/* Parse cached blob: "role\tcontent\tts\n" (last \t separates ts); old format "role\tcontent\n" has ts="". */
static void history_deserialize_and_cb(const char *buf, storage_chat_history_cb callback, void *userdata) {
    if (!buf || !callback) return;
    char line[4096];
    size_t i = 0, j = 0;
    for (; buf[i] && j < sizeof(line) - 1; i++) {
        if (buf[i] == '\n') {
            line[j] = '\0';
            j = 0;
            char *first_tab = strchr(line, '\t');
            if (!first_tab) continue;
            *first_tab = '\0';
            const char *role = line;
            char *content = first_tab + 1;
            char *last_tab = strrchr(content, '\t');
            const char *ts = "";
            if (last_tab && last_tab != first_tab) {
                char ts_buf[64];
                size_t tlen = strlen(last_tab + 1);
                if (tlen > 63) tlen = 63;
                memcpy(ts_buf, last_tab + 1, tlen);
                ts_buf[tlen] = '\0';
                ts = ts_buf;
                *last_tab = '\0';
            }
            for (char *c = content; *c; c++) {
                if (c[0] == '\\' && c[1] == 'n') { *c = '\n'; memmove(c + 1, c + 2, strlen(c + 2) + 1); c--; }
            }
            callback(role, content, ts, userdata);
            continue;
        }
        line[j++] = buf[i];
    }
}

/* Get chat history with L1 cache: try Redis first; on miss load from Mongo and set cache with TTL. */
int storage_get_chat_history_cached(storage_ctx_t *ctx, const char *tenant_id, int limit,
                                    storage_chat_history_cb callback, void *userdata) {
    if (!ctx || !callback || limit <= 0) return -1;
    const char *tid = tenant_id ? tenant_id : "default";
    if (ctx->redis && redis_connected(ctx->redis)) {
        char key[128];
        int klen = snprintf(key, sizeof(key), "%s%s", HISTORY_CACHE_KEY_PREFIX, tid);
        if (klen < 0 || (size_t)klen >= sizeof(key)) return storage_get_chat_history(ctx, tenant_id, limit, callback, userdata);
        static char cache_buf[HISTORY_CACHE_BUF_SIZE];
        if (redis_search_value(ctx->redis, key, cache_buf, sizeof(cache_buf)) == 1 && cache_buf[0]) {
            history_deserialize_and_cb(cache_buf, callback, userdata);
            return 0;
        }
    }
    /* Cache miss or no Redis: load from Mongo */
#ifdef USE_MONGOC
    static char accum_buf[HISTORY_CACHE_BUF_SIZE];
    history_accum_t acc = { accum_buf, sizeof(accum_buf), 0, callback, userdata };
    int ret = storage_get_chat_history(ctx, tenant_id, limit, history_accum_cb, &acc);
    if (ret == 0 && ctx->redis && redis_connected(ctx->redis) && acc.used > 0) {
        char key[128];
        snprintf(key, sizeof(key), "%s%s", HISTORY_CACHE_KEY_PREFIX, tid);
        redis_set_value(ctx->redis, key, accum_buf, REDIS_CACHE_TTL_SECONDS);
    }
    return ret;
#else
    return storage_get_chat_history(ctx, tenant_id, limit, callback, userdata);
#endif
}

/* --- Geo atlas (optional place memory, .cursor/geo_leanring.md) --- */
#define GEO_ATLAS_NAME_MAX 256
#define GEO_ATLAS_PROMPT_MAX 8192

#ifdef USE_MONGOC
static double cosine_similarity(const float *a, const double *b, size_t dim) {
    if (dim == 0) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < dim; i++) {
        double av = (double)a[i], bv = b[i];
        dot += av * bv;
        na += av * av;
        nb += bv * bv;
    }
    if (na <= 0.0 || nb <= 0.0) return 0.0;
    return dot / (sqrt(na) * sqrt(nb));
}
#endif

int storage_geo_atlas_insert_doc(storage_ctx_t *ctx, const storage_geo_atlas_doc_t *doc) {
    if (!ctx || !doc) return -1;
#ifdef USE_MONGOC
    if (!ctx->mongo_client) return -1;
    const char *n = doc->name ? doc->name : "";
    const char *nn = doc->name_normalized ? doc->name_normalized : n;
    const char *tid = doc->tenant_id && doc->tenant_id[0] ? doc->tenant_id : "default";
    const char *d = doc->district ? doc->district : "";
    const char *ax = doc->axis ? doc->axis : "";
    const char *cat = doc->category ? doc->category : "";
    const char *c = doc->city ? doc->city : "";
    const char *reg = doc->region ? doc->region : "";
    const char *ctry = doc->country && doc->country[0] ? doc->country : "Vietnam";
    const char *lm = doc->landmarks ? doc->landmarks : "";
    const char *mg = doc->merged_into ? doc->merged_into : "";
    const char *aa = doc->admin_action ? doc->admin_action : "";
    const char *ad = doc->admin_detail ? doc->admin_detail : "";
    const char *mid = doc->embed_model_id && doc->embed_model_id[0] ? doc->embed_model_id : "";
    const char *src = doc->source && doc->source[0] ? doc->source : STORAGE_GEO_SOURCE_USER;
    const char *vst = doc->verification_status && doc->verification_status[0] ? doc->verification_status : STORAGE_GEO_STATUS_VERIFIED;
    double trust = doc->trust_score;
    if (trust < 0.0) trust = 0.0;
    if (trust > 1.0) trust = 1.0;

    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_GEO_ATLAS_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_GEO_ATLAS_COLLECTION);
    bson_t bdoc;
    bson_init(&bdoc);
    BSON_APPEND_UTF8(&bdoc, "tenant_id", tid);
    BSON_APPEND_UTF8(&bdoc, "name", n);
    BSON_APPEND_UTF8(&bdoc, "name_normalized", nn);
    BSON_APPEND_UTF8(&bdoc, "district", d);
    BSON_APPEND_UTF8(&bdoc, "axis", ax);
    BSON_APPEND_UTF8(&bdoc, "category", cat);
    BSON_APPEND_UTF8(&bdoc, "city", c);
    if (reg[0]) BSON_APPEND_UTF8(&bdoc, "region", reg);
    BSON_APPEND_UTF8(&bdoc, "country", ctry);
    if (lm[0]) BSON_APPEND_UTF8(&bdoc, "landmarks", lm);
    if (mg[0]) BSON_APPEND_UTF8(&bdoc, "merged_into", mg);
    if (aa[0]) BSON_APPEND_UTF8(&bdoc, "admin_action", aa);
    if (ad[0]) BSON_APPEND_UTF8(&bdoc, "admin_detail", ad);
    BSON_APPEND_UTF8(&bdoc, "embed_model_id", mid);
    storage_bson_append_embed_provenance(&bdoc, mid, doc->vector_dim);
    BSON_APPEND_UTF8(&bdoc, "source", src);
    BSON_APPEND_UTF8(&bdoc, "verification_status", vst);
    bson_append_double(&bdoc, "trust_score", -1, trust);
    bson_append_date_time(&bdoc, "createdAt", -1, (int64_t)time(NULL) * 1000);
    if (doc->vector && doc->vector_dim > 0) {
        bson_t arr;
        bson_append_array_begin(&bdoc, "vector", -1, &arr);
        char key[16];
        for (size_t i = 0; i < doc->vector_dim; i++) {
            snprintf(key, sizeof(key), "%zu", i);
            bson_append_double(&arr, key, -1, (double)doc->vector[i]);
        }
        bson_append_array_end(&bdoc, &arr);
    }
    bson_error_t err;
    int ok = mongoc_collection_insert_one(coll, &bdoc, NULL, NULL, &err);
    bson_destroy(&bdoc);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    if (!ok) {
        fprintf(stderr, "[STORAGE] geo_atlas insert failed: %s\n", err.message);
        return -1;
    }
    fprintf(stderr, "[STORAGE] geo_atlas inserted: %s tenant=%s status=%s trust=%.2f (%s%s%s)\n", n, tid, vst, trust,
            c[0] ? c : "", (c[0] && d[0]) ? ", " : "", d[0] ? d : "");
    return 0;
#else
    (void)doc;
    fprintf(stderr, "[STORAGE] geo_atlas insert skipped: build without USE_MONGOC=1\n");
    return 0;
#endif
}

int storage_geo_atlas_exists_normalized_country(storage_ctx_t *ctx, const char *tenant_id,
                                                const char *name_normalized, const char *country) {
    if (!ctx || !name_normalized || !name_normalized[0]) return 0;
#ifdef USE_MONGOC
    if (!ctx->mongo_client) return 0;
    const char *tid = tenant_id && tenant_id[0] ? tenant_id : "default";
    const char *ctry = country && country[0] ? country : "Vietnam";

    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_GEO_ATLAS_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_GEO_ATLAS_COLLECTION);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "tenant_id", tid);
    BSON_APPEND_UTF8(&filter, "name_normalized", name_normalized);
    BSON_APPEND_UTF8(&filter, "country", ctry);

    bson_t *opts = bson_new();
    BSON_APPEND_INT32(opts, "limit", 1);
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(coll, &filter, opts, NULL);
    bson_destroy(opts);
    bson_destroy(&filter);
    const bson_t *doc;
    int found = mongoc_cursor_next(cursor, &doc) ? 1 : 0;
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    return found;
#else
    (void)tenant_id;
    (void)name_normalized;
    (void)country;
    return 0;
#endif
}

int storage_geo_atlas_insert(storage_ctx_t *ctx, const char *name, const char *name_normalized,
                             const char *district, const char *axis, const char *category,
                             const char *city, const float *vector, size_t vector_dim,
                             const char *embed_model_id) {
    storage_geo_atlas_doc_t d;
    memset(&d, 0, sizeof(d));
    d.tenant_id = "default";
    d.name = name;
    d.name_normalized = name_normalized;
    d.district = district;
    d.axis = axis;
    d.category = category;
    d.city = city;
    d.vector = vector;
    d.vector_dim = vector_dim;
    d.embed_model_id = embed_model_id;
    d.source = STORAGE_GEO_SOURCE_USER;
    d.verification_status = STORAGE_GEO_STATUS_VERIFIED;
    d.trust_score = 1.0;
    return storage_geo_atlas_insert_doc(ctx, &d);
}

int storage_geo_atlas_seed_conflict(storage_ctx_t *ctx, const char *tenant_id,
                                    const char *name_normalized, const char *district, const char *city) {
    if (!ctx || !name_normalized || !name_normalized[0]) return 0;
#ifdef USE_MONGOC
    if (!ctx->mongo_client) return 0;
    const char *tid = tenant_id && tenant_id[0] ? tenant_id : "default";
    const char *d_new = district ? district : "";
    const char *c_new = city ? city : "";

    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_GEO_ATLAS_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_GEO_ATLAS_COLLECTION);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "name_normalized", name_normalized);
    BSON_APPEND_UTF8(&filter, "source", STORAGE_GEO_SOURCE_SEED);
    bson_t inwrap, inarr;
    bson_init(&inwrap);
    bson_init(&inarr);
    BSON_APPEND_UTF8(&inarr, "0", tid);
    BSON_APPEND_UTF8(&inarr, "1", STORAGE_GEO_TENANT_GLOBAL);
    BSON_APPEND_ARRAY(&inwrap, "$in", &inarr);
    BSON_APPEND_DOCUMENT(&filter, "tenant_id", &inwrap);
    bson_destroy(&inarr);
    bson_destroy(&inwrap); /* inwrap holds $in */

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(coll, &filter, NULL, NULL);
    bson_destroy(&filter);
    const bson_t *doc;
    int conflict = 0;
    while (mongoc_cursor_next(cursor, &doc)) {
        char d_seed[GEO_ATLAS_NAME_MAX] = "";
        char c_seed[GEO_ATLAS_NAME_MAX] = "";
        bson_iter_t iter;
        if (bson_iter_init(&iter, doc)) {
            while (bson_iter_next(&iter)) {
                const char *key = bson_iter_key(&iter);
                if (strcmp(key, "district") == 0 && BSON_ITER_HOLDS_UTF8(&iter))
                    strncpy(d_seed, bson_iter_utf8(&iter, NULL), sizeof(d_seed) - 1);
                else if (strcmp(key, "city") == 0 && BSON_ITER_HOLDS_UTF8(&iter))
                    strncpy(c_seed, bson_iter_utf8(&iter, NULL), sizeof(c_seed) - 1);
            }
        }
        if (d_new[0] && d_seed[0] && strcasecmp(d_new, d_seed) != 0) { conflict = 1; break; }
        if (c_new[0] && c_seed[0] && strcasecmp(c_new, c_seed) != 0) { conflict = 1; break; }
    }
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    if (conflict)
        fprintf(stderr, "[STORAGE] geo_atlas seed conflict for %s (tenant=%s)\n", name_normalized, tid);
    return conflict;
#else
    (void)tenant_id;
    (void)name_normalized;
    (void)district;
    (void)city;
    return 0;
#endif
}

int storage_geo_atlas_find_similar(storage_ctx_t *ctx, const char *tenant_id,
                                   const float *vector, size_t dim, double threshold) {
    if (!ctx || !vector || dim == 0) return 0;
#ifdef USE_MONGOC
    if (!ctx->mongo_client) return 0;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_GEO_ATLAS_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_GEO_ATLAS_COLLECTION);
    bson_t *filter = bson_new();
    if (tenant_id && tenant_id[0]) {
        bson_t q0, q1, q2, arr, ex;
        bson_init(&q0);
        BSON_APPEND_UTF8(&q0, "tenant_id", tenant_id);
        bson_init(&q1);
        BSON_APPEND_UTF8(&q1, "tenant_id", STORAGE_GEO_TENANT_GLOBAL);
        bson_append_array_begin(filter, "$or", -1, &arr);
        bson_append_document(&arr, "0", 1, &q0);
        bson_append_document(&arr, "1", 1, &q1);
        /* Legacy docs without tenant_id behave as "default" */
        if (strcmp(tenant_id, "default") == 0) {
            bson_init(&q2);
            bson_init(&ex);
            BSON_APPEND_BOOL(&ex, "$exists", false);
            BSON_APPEND_DOCUMENT(&q2, "tenant_id", &ex);
            bson_destroy(&ex);
            bson_append_document(&arr, "2", 1, &q2);
            bson_destroy(&q2);
        }
        bson_append_array_end(filter, &arr);
        bson_destroy(&q0);
        bson_destroy(&q1);
    }
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(coll, filter, NULL, NULL);
    bson_destroy(filter);
    const bson_t *doc;
    double vec_buf[2048];
    size_t vec_dim = dim > 2048 ? 2048 : dim;
    int found = 0;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (!bson_iter_init(&iter, doc)) continue;
        size_t got = 0;
        while (bson_iter_next(&iter)) {
            if (strcmp(bson_iter_key(&iter), "vector") != 0) continue;
            if (BSON_ITER_HOLDS_ARRAY(&iter)) {
                bson_iter_t arr_iter;
                bson_iter_recurse(&iter, &arr_iter);
                while (bson_iter_next(&arr_iter) && got < vec_dim) {
                    if (BSON_ITER_HOLDS_DOUBLE(&arr_iter))
                        vec_buf[got++] = bson_iter_double(&arr_iter);
                }
                break;
            }
            break;
        }
        if (got == dim) {
            double sim = cosine_similarity(vector, vec_buf, dim);
            if (sim >= threshold) { found = 1; break; }
        }
    }
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    return found;
#else
    (void)tenant_id;
    (void)dim;
    (void)threshold;
    return 0;
#endif
}

size_t storage_geo_atlas_get_landmarks_for_prompt(storage_ctx_t *ctx, char *out, size_t size) {
    if (!ctx || !out || size == 0) return 0;
#ifdef USE_MONGOC
    if (!ctx->mongo_client) return 0;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_GEO_ATLAS_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_GEO_ATLAS_COLLECTION);
    bson_t *filter = bson_new();
    {
        bson_t nin_arr;
        bson_init(&nin_arr);
        BSON_APPEND_UTF8(&nin_arr, "0", STORAGE_GEO_STATUS_PENDING_VERIFICATION);
        BSON_APPEND_UTF8(&nin_arr, "1", STORAGE_GEO_STATUS_MERGED);
        bson_t nin_wrap;
        bson_init(&nin_wrap);
        BSON_APPEND_ARRAY(&nin_wrap, "$nin", &nin_arr);
        BSON_APPEND_DOCUMENT(filter, "verification_status", &nin_wrap);
        bson_destroy(&nin_arr);
        bson_destroy(&nin_wrap);
    }
    bson_t *opts = bson_new();
    BSON_APPEND_INT32(opts, "limit", 30);
    bson_t sort;
    bson_init(&sort);
    BSON_APPEND_INT32(&sort, "createdAt", -1);
    BSON_APPEND_DOCUMENT(opts, "sort", &sort);
    bson_destroy(&sort);
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(coll, filter, opts, NULL);
    bson_destroy(filter);
    bson_destroy(opts);
    size_t n = 0;
    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc) && n < size - 1) {
        char name[GEO_ATLAS_NAME_MAX] = "";
        char district[GEO_ATLAS_NAME_MAX] = "";
        char city[GEO_ATLAS_NAME_MAX] = "";
        bson_iter_t iter;
        if (bson_iter_init(&iter, doc)) {
            while (bson_iter_next(&iter)) {
                const char *key = bson_iter_key(&iter);
                if (strcmp(key, "name") == 0 && BSON_ITER_HOLDS_UTF8(&iter))
                    strncpy(name, bson_iter_utf8(&iter, NULL), sizeof(name) - 1);
                else if (strcmp(key, "district") == 0 && BSON_ITER_HOLDS_UTF8(&iter))
                    strncpy(district, bson_iter_utf8(&iter, NULL), sizeof(district) - 1);
                else if (strcmp(key, "city") == 0 && BSON_ITER_HOLDS_UTF8(&iter))
                    strncpy(city, bson_iter_utf8(&iter, NULL), sizeof(city) - 1);
            }
        }
        if (name[0]) {
            int w;
            if (city[0] && district[0])
                w = snprintf(out + n, size - n, "%s (%s, %s)\n", name, district, city);
            else if (city[0])
                w = snprintf(out + n, size - n, "%s (%s)\n", name, city);
            else if (district[0])
                w = snprintf(out + n, size - n, "%s (%s)\n", name, district);
            else
                w = snprintf(out + n, size - n, "%s\n", name);
            if (w > 0 && n + (size_t)w < size) n += (size_t)w;
            else break;
        }
    }
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    if (n < size) out[n] = '\0';
    return n;
#else
    (void)size;
    out[0] = '\0';
    return 0;
#endif
}

#ifdef USE_MONGOC
static unsigned long long geo_atlas_reply_modified_count(const bson_t *reply) {
    bson_iter_t it;
    if (!reply || !bson_iter_init_find(&it, reply, "modifiedCount"))
        return 0;
    if (BSON_ITER_HOLDS_INT64(&it))
        return (unsigned long long)bson_iter_int64(&it);
    if (BSON_ITER_HOLDS_INT32(&it))
        return (unsigned long long)bson_iter_int32(&it);
    return 0;
}
#endif

int storage_geo_atlas_migrate_legacy(storage_ctx_t *ctx, unsigned long long *modified_out) {
    if (modified_out)
        *modified_out = 0;
    if (!ctx)
        return -1;
#ifdef USE_MONGOC
    if (!ctx->mongo_client)
        return 0;

    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_GEO_ATLAS_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_GEO_ATLAS_COLLECTION);
    bson_error_t err;
    unsigned long long total = 0;

    /* 1) country missing, null, or "" → Vietnam (composite dedup + §13). */
    {
        bson_t orarr, e0, e1, e2, c0, filter, set, update, reply;
        bson_init(&orarr);
        bson_init(&e0);
        bson_init(&c0);
        BSON_APPEND_BOOL(&c0, "$exists", false);
        BSON_APPEND_DOCUMENT(&e0, "country", &c0);
        bson_destroy(&c0);
        BSON_APPEND_DOCUMENT(&orarr, "0", &e0);
        bson_destroy(&e0);
        bson_init(&e1);
        BSON_APPEND_NULL(&e1, "country");
        BSON_APPEND_DOCUMENT(&orarr, "1", &e1);
        bson_destroy(&e1);
        bson_init(&e2);
        BSON_APPEND_UTF8(&e2, "country", "");
        BSON_APPEND_DOCUMENT(&orarr, "2", &e2);
        bson_destroy(&e2);
        bson_init(&filter);
        BSON_APPEND_ARRAY(&filter, "$or", &orarr);
        bson_destroy(&orarr);
        bson_init(&set);
        BSON_APPEND_UTF8(&set, "country", "Vietnam");
        bson_init(&update);
        BSON_APPEND_DOCUMENT(&update, "$set", &set);
        bson_destroy(&set);
        bson_init(&reply);
        if (!mongoc_collection_update_many(coll, &filter, &update, NULL, &reply, &err)) {
            fprintf(stderr, "[STORAGE] geo_atlas migrate (country) failed: %s\n", err.message);
            bson_destroy(&reply);
            bson_destroy(&update);
            bson_destroy(&filter);
            mongoc_collection_destroy(coll);
            mongoc_database_destroy(db);
            return -1;
        }
        total += geo_atlas_reply_modified_count(&reply);
        bson_destroy(&reply);
        bson_destroy(&update);
        bson_destroy(&filter);
    }

    /* 2) merged_into set, status not merged → align with geo_learning insert path (§13). */
    {
        bson_t filter, mg_gt, ne_st, set, update, reply;
        bson_init(&filter);
        bson_init(&mg_gt);
        BSON_APPEND_UTF8(&mg_gt, "$gt", "");
        BSON_APPEND_DOCUMENT(&filter, "merged_into", &mg_gt);
        bson_destroy(&mg_gt);
        bson_init(&ne_st);
        BSON_APPEND_UTF8(&ne_st, "$ne", STORAGE_GEO_STATUS_MERGED);
        BSON_APPEND_DOCUMENT(&filter, "verification_status", &ne_st);
        bson_destroy(&ne_st);
        bson_init(&set);
        BSON_APPEND_UTF8(&set, "verification_status", STORAGE_GEO_STATUS_MERGED);
        bson_append_double(&set, "trust_score", -1, 1.0);
        BSON_APPEND_UTF8(&set, "admin_action", "merge");
        bson_init(&update);
        BSON_APPEND_DOCUMENT(&update, "$set", &set);
        bson_destroy(&set);
        bson_init(&reply);
        if (!mongoc_collection_update_many(coll, &filter, &update, NULL, &reply, &err)) {
            fprintf(stderr, "[STORAGE] geo_atlas migrate (merged_into) failed: %s\n", err.message);
            bson_destroy(&reply);
            bson_destroy(&update);
            bson_destroy(&filter);
            mongoc_collection_destroy(coll);
            mongoc_database_destroy(db);
            return -1;
        }
        total += geo_atlas_reply_modified_count(&reply);
        bson_destroy(&reply);
        bson_destroy(&update);
        bson_destroy(&filter);
    }

    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    if (modified_out)
        *modified_out = total;
    fprintf(stderr, "[STORAGE] geo_atlas migrate_legacy: modified %llu document(s)\n", total);
    return 0;
#else
    (void)ctx;
    return 0;
#endif
}

#ifdef USE_MONGOC
static int storage_embed_migration_parse_doc(const bson_t *doc, storage_embed_migration_turn_row_t *row) {
    memset(row, 0, sizeof(*row));
    bson_iter_t it, sub, in_it;
    if (bson_iter_init_find(&it, doc, "_id")) {
        if (BSON_ITER_HOLDS_OID(&it)) {
            bson_oid_to_string(bson_iter_oid(&it), row->oid_hex);
        } else
            return -1;
    } else
        return -1;

    if (!bson_iter_init_find(&it, doc, "turn") || !BSON_ITER_HOLDS_DOCUMENT(&it))
        return -1;
    bson_iter_recurse(&it, &sub);
    if (!bson_iter_find(&sub, "input") || !BSON_ITER_HOLDS_UTF8(&sub))
        return -1;
    {
        uint32_t ulen = 0;
        const char *inp = bson_iter_utf8(&sub, &ulen);
        (void)ulen;
        if (!inp) return -1;
        size_t n = strlen(inp);
        if (n >= STORAGE_EMBED_MIG_INPUT_MAX) n = STORAGE_EMBED_MIG_INPUT_MAX - 1;
        memcpy(row->input, inp, n);
        row->input[n] = '\0';
    }

    row->vector_dim = 0;
    if (bson_iter_init_find(&it, doc, "vector") && BSON_ITER_HOLDS_ARRAY(&it)) {
        bson_iter_recurse(&it, &in_it);
        while (bson_iter_next(&in_it))
            row->vector_dim++;
    }

    row->has_model_id = 0;
    row->model_id[0] = '\0';
    if (bson_iter_init_find(&it, doc, "metadata") && BSON_ITER_HOLDS_DOCUMENT(&it)) {
        bson_iter_recurse(&it, &sub);
        if (bson_iter_find(&sub, "model_id") && BSON_ITER_HOLDS_UTF8(&sub)) {
            uint32_t ulen = 0;
            const char *mid = bson_iter_utf8(&sub, &ulen);
            (void)ulen;
            if (mid && mid[0]) {
                row->has_model_id = 1;
                snprintf(row->model_id, sizeof(row->model_id), "%s", mid);
            }
        }
    }
    return 0;
}
#endif

int storage_embed_migration_fetch_turns_needing_provenance(storage_ctx_t *ctx, const char *tenant_id, int limit,
                                                           storage_embed_migration_turn_row_t *rows, int *out_n) {
    if (!rows || !out_n) return -1;
    *out_n = 0;
    if (!ctx || limit <= 0) return 0;
    const char *tid = tenant_id && tenant_id[0] ? tenant_id : "default";
#ifndef USE_MONGOC
    (void)tid;
    return 0;
#else
    if (!ctx->mongo_client) return -1;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "tenant_id", tid);
    {
        bson_t ti;
        bson_init(&ti);
        BSON_APPEND_BOOL(&ti, "$exists", true);
        BSON_APPEND_UTF8(&ti, "$ne", "");
        BSON_APPEND_DOCUMENT(&filter, "turn.input", &ti);
        bson_destroy(&ti);
    }
    {
        bson_t v0;
        bson_init(&v0);
        BSON_APPEND_BOOL(&v0, "$exists", true);
        BSON_APPEND_DOCUMENT(&filter, "vector.0", &v0);
        bson_destroy(&v0);
    }
    {
        bson_t or_arr, elem, inner;
        const char *paths[] = {"metadata.embed_schema", "metadata.vector_dim", "metadata.embed_family"};
        bson_append_array_begin(&filter, "$or", -1, &or_arr);
        for (int i = 0; i < 3; i++) {
            char k[8];
            snprintf(k, sizeof(k), "%d", i);
            bson_init(&inner);
            BSON_APPEND_BOOL(&inner, "$exists", false);
            bson_init(&elem);
            BSON_APPEND_DOCUMENT(&elem, paths[i], &inner);
            bson_destroy(&inner);
            bson_append_document(&or_arr, k, -1, &elem);
            bson_destroy(&elem);
        }
        bson_append_array_end(&filter, &or_arr);
    }

    bson_t opts;
    bson_init(&opts);
    {
        bson_t sort;
        bson_init(&sort);
        BSON_APPEND_INT32(&sort, "createdAt", 1);
        BSON_APPEND_DOCUMENT(&opts, "sort", &sort);
        bson_destroy(&sort);
    }
    BSON_APPEND_INT32(&opts, "limit", limit);

    mongoc_cursor_t *cur = mongoc_collection_find_with_opts(coll, &filter, &opts, NULL);
    bson_destroy(&opts);
    bson_destroy(&filter);

    const bson_t *doc;
    int n = 0;
    while (n < limit && mongoc_cursor_next(cur, &doc)) {
        if (storage_embed_migration_parse_doc(doc, &rows[n]) == 0)
            n++;
    }
    bson_error_t err;
    if (mongoc_cursor_error(cur, &err))
        fprintf(stderr, "[STORAGE] embed_migration fetch provenance cursor error: %s\n", err.message);
    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    *out_n = n;
    return 0;
#endif
}

int storage_embed_migration_fetch_turns_model_mismatch(storage_ctx_t *ctx, const char *tenant_id,
                                                       const char *target_model_id, int limit,
                                                       storage_embed_migration_turn_row_t *rows, int *out_n) {
    if (!rows || !out_n || !target_model_id) return -1;
    *out_n = 0;
    if (!ctx || limit <= 0) return 0;
    const char *tid = tenant_id && tenant_id[0] ? tenant_id : "default";
#ifndef USE_MONGOC
    (void)tid;
    return 0;
#else
    if (!ctx->mongo_client) return -1;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "tenant_id", tid);
    {
        bson_t ti;
        bson_init(&ti);
        BSON_APPEND_BOOL(&ti, "$exists", true);
        BSON_APPEND_UTF8(&ti, "$ne", "");
        BSON_APPEND_DOCUMENT(&filter, "turn.input", &ti);
        bson_destroy(&ti);
    }
    {
        bson_t v0;
        bson_init(&v0);
        BSON_APPEND_BOOL(&v0, "$exists", true);
        BSON_APPEND_DOCUMENT(&filter, "vector.0", &v0);
        bson_destroy(&v0);
    }
    {
        bson_t or_arr, elem, inner, ne_doc;
        bson_append_array_begin(&filter, "$or", -1, &or_arr);
        bson_init(&inner);
        BSON_APPEND_BOOL(&inner, "$exists", false);
        bson_init(&elem);
        BSON_APPEND_DOCUMENT(&elem, "metadata.model_id", &inner);
        bson_destroy(&inner);
        bson_append_document(&or_arr, "0", -1, &elem);
        bson_destroy(&elem);

        bson_init(&elem);
        BSON_APPEND_UTF8(&elem, "metadata.model_id", "");
        bson_append_document(&or_arr, "1", -1, &elem);
        bson_destroy(&elem);

        bson_init(&ne_doc);
        BSON_APPEND_UTF8(&ne_doc, "$ne", target_model_id);
        bson_init(&elem);
        BSON_APPEND_DOCUMENT(&elem, "metadata.model_id", &ne_doc);
        bson_destroy(&ne_doc);
        bson_append_document(&or_arr, "2", -1, &elem);
        bson_destroy(&elem);

        bson_append_array_end(&filter, &or_arr);
    }

    bson_t opts;
    bson_init(&opts);
    {
        bson_t sort;
        bson_init(&sort);
        BSON_APPEND_INT32(&sort, "createdAt", 1);
        BSON_APPEND_DOCUMENT(&opts, "sort", &sort);
        bson_destroy(&sort);
    }
    BSON_APPEND_INT32(&opts, "limit", limit);

    mongoc_cursor_t *cur = mongoc_collection_find_with_opts(coll, &filter, &opts, NULL);
    bson_destroy(&opts);
    bson_destroy(&filter);

    const bson_t *doc;
    int n = 0;
    while (n < limit && mongoc_cursor_next(cur, &doc)) {
        if (storage_embed_migration_parse_doc(doc, &rows[n]) == 0)
            n++;
    }
    bson_error_t err;
    if (mongoc_cursor_error(cur, &err))
        fprintf(stderr, "[STORAGE] embed_migration fetch mismatch cursor error: %s\n", err.message);
    mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    *out_n = n;
    return 0;
#endif
}

int storage_embed_migration_set_turn_provenance(storage_ctx_t *ctx, const storage_embed_migration_turn_row_t *row) {
    if (!ctx || !row || !row->oid_hex[0]) return -1;
#ifndef USE_MONGOC
    (void)row;
    return -1;
#else
    if (!ctx->mongo_client) return -1;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_oid_t oid;
    if (!bson_oid_is_valid(row->oid_hex, strlen(row->oid_hex)))
        return -1;
    bson_oid_init_from_string(&oid, row->oid_hex);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_OID(&filter, "_id", &oid);

    bson_t set;
    bson_init(&set);
    BSON_APPEND_INT32(&set, "metadata.embed_schema", 1);
    if (row->vector_dim > 0 && row->vector_dim <= INT32_MAX)
        BSON_APPEND_INT32(&set, "metadata.vector_dim", (int32_t)row->vector_dim);
    if (row->has_model_id && row->model_id[0]) {
        if (strcmp(row->model_id, VECTOR_GEN_MODEL_ID) == 0)
            BSON_APPEND_UTF8(&set, "metadata.embed_family", "custom");
        else
            BSON_APPEND_UTF8(&set, "metadata.embed_family", "ollama");
    } else if (row->vector_dim > 0)
        BSON_APPEND_UTF8(&set, "metadata.embed_family", "legacy");

    bson_t update;
    bson_init(&update);
    BSON_APPEND_DOCUMENT(&update, "$set", &set);
    bson_destroy(&set);

    bson_error_t err;
    if (!mongoc_collection_update_one(coll, &filter, &update, NULL, NULL, &err)) {
        fprintf(stderr, "[STORAGE] embed_migration set provenance failed: %s\n", err.message);
        bson_destroy(&update);
        bson_destroy(&filter);
        mongoc_collection_destroy(coll);
        mongoc_database_destroy(db);
        return -1;
    }
    bson_destroy(&update);
    bson_destroy(&filter);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    return 0;
#endif
}

int storage_embed_migration_update_turn_embedding(storage_ctx_t *ctx, const char *oid_hex, const float *vector,
                                                  size_t vector_dim, const char *model_id, const char *embed_family) {
    if (!ctx || !oid_hex || !oid_hex[0] || !vector || vector_dim == 0 || !model_id || !embed_family) return -1;
#ifndef USE_MONGOC
    (void)vector;
    (void)vector_dim;
    return -1;
#else
    if (!ctx->mongo_client) return -1;
    if (!bson_oid_is_valid(oid_hex, strlen(oid_hex))) return -1;
    bson_oid_t oid;
    bson_oid_init_from_string(&oid, oid_hex);

    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_OID(&filter, "_id", &oid);

    bson_t set, arr;
    bson_init(&set);
    bson_append_array_begin(&set, "vector", -1, &arr);
    if (vector_dim > (size_t)INT32_MAX) {
        bson_destroy(&arr);
        bson_destroy(&set);
        bson_destroy(&filter);
        mongoc_collection_destroy(coll);
        mongoc_database_destroy(db);
        return -1;
    }
    {
        char key[16];
        for (size_t i = 0; i < vector_dim; i++) {
            snprintf(key, sizeof(key), "%zu", i);
            bson_append_double(&arr, key, -1, (double)vector[i]);
        }
    }
    bson_append_array_end(&set, &arr);
    BSON_APPEND_INT32(&set, "metadata.embed_schema", 1);
    BSON_APPEND_INT32(&set, "metadata.vector_dim", (int32_t)vector_dim);
    BSON_APPEND_UTF8(&set, "metadata.embed_family", embed_family);
    BSON_APPEND_UTF8(&set, "metadata.model_id", model_id);

    bson_t update;
    bson_init(&update);
    BSON_APPEND_DOCUMENT(&update, "$set", &set);
    bson_destroy(&set);

    bson_error_t err;
    if (!mongoc_collection_update_one(coll, &filter, &update, NULL, NULL, &err)) {
        fprintf(stderr, "[STORAGE] embed_migration update embedding failed: %s\n", err.message);
        bson_destroy(&update);
        bson_destroy(&filter);
        mongoc_collection_destroy(coll);
        mongoc_database_destroy(db);
        return -1;
    }
    bson_destroy(&update);
    bson_destroy(&filter);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);
    return 0;
#endif
}
