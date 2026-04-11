#include "storage.h"
#include "m4_elk_log.h"
#include "embed.h"
#include "tenant.h"
#include "redis.h"
#include "elk.h"
#include "shared_collection.h"
#include "elk_sync_pool.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef USE_MONGOC
#include <limits.h>
#include <stdbool.h>
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
    elk_ctx_t *elk;
    sc_registry_t *sc_reg;
    elk_sync_pool_t *elk_pool;
    pthread_mutex_t elk_sc_mu;
    pthread_t sc_watch_tid;
    int sc_watch_started;
    volatile int sc_watch_stop;
#ifdef USE_MONGOC
    mongoc_client_t *mongo_client;
    /** Optional second cluster for SharedCollection ELK backfill / validation (``shared_collection_mongo_uri`` only). */
    mongoc_client_t *mongo_client_shared;
#endif
    /** Copied from api_options_t / engine_config_t; empty = use getenv only. */
    char shared_collection_mongo_uri[256];
    char shared_collection_json_path[512];
    char shared_collection_backfill_db[128];
    /** Incremental ELK sync: schedule_refresh option. */
    int schedule_refresh;
    char elk_sync_state_path[512]; /* auto-derived from shared_collection_json_path */
};

#ifdef USE_MONGOC
static void storage_elk_cold_backfill(storage_ctx_t *ctx);
static void storage_elk_start_change_stream(storage_ctx_t *ctx);
#endif
static void storage_elk_workers_stop(storage_ctx_t *ctx);
static void storage_elk_fini(storage_ctx_t *ctx);

#ifdef USE_MONGOC
static const char *storage_sc_backfill_db(const storage_ctx_t *ctx) {
    if (ctx && ctx->shared_collection_backfill_db[0])
        return ctx->shared_collection_backfill_db;
    return STORAGE_MONGO_DB_NAME;
}
#endif

static const char *storage_sc_json_path(const storage_ctx_t *ctx) {
    if (ctx && ctx->shared_collection_json_path[0])
        return ctx->shared_collection_json_path;
    return NULL;
}

#ifdef USE_MONGOC
static const char *storage_sc_mongo_uri_str(const storage_ctx_t *ctx) {
    if (ctx && ctx->shared_collection_mongo_uri[0])
        return ctx->shared_collection_mongo_uri;
    return NULL;
}
#endif

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
                              const char *es_host, int es_port, const char *shared_collection_mongo_uri,
                              const char *shared_collection_json_path, const char *shared_collection_backfill_db) {
    storage_ctx_t *ctx = (storage_ctx_t *)malloc(sizeof(storage_ctx_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    if (mongo_uri) strncpy(ctx->mongo_uri, mongo_uri, sizeof(ctx->mongo_uri) - 1);
    if (redis_host) strncpy(ctx->redis_host, redis_host, sizeof(ctx->redis_host) - 1);
    ctx->redis_port = redis_port;
    if (es_host) strncpy(ctx->es_host, es_host, sizeof(ctx->es_host) - 1);
    ctx->es_port = es_port;
    if (shared_collection_mongo_uri && shared_collection_mongo_uri[0])
        strncpy(ctx->shared_collection_mongo_uri, shared_collection_mongo_uri,
                sizeof(ctx->shared_collection_mongo_uri) - 1);
    if (shared_collection_json_path && shared_collection_json_path[0])
        strncpy(ctx->shared_collection_json_path, shared_collection_json_path,
                sizeof(ctx->shared_collection_json_path) - 1);
    if (shared_collection_backfill_db && shared_collection_backfill_db[0])
        strncpy(ctx->shared_collection_backfill_db, shared_collection_backfill_db,
                sizeof(ctx->shared_collection_backfill_db) - 1);
    pthread_mutex_init(&ctx->elk_sc_mu, NULL);
    return ctx;
}

void storage_destroy(storage_ctx_t *ctx) {
    if (!ctx) return;
    storage_elk_fini(ctx);
    if (ctx->redis) {
        redis_destroy(ctx->redis);
        ctx->redis = NULL;
    }
#ifdef USE_MONGOC
    if (ctx->mongo_client_shared) {
        mongoc_client_destroy(ctx->mongo_client_shared);
        ctx->mongo_client_shared = NULL;
    }
    if (ctx->mongo_client) {
        mongoc_client_destroy(ctx->mongo_client);
        ctx->mongo_client = NULL;
        mongoc_cleanup();
    }
#endif
    pthread_mutex_destroy(&ctx->elk_sc_mu);
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

typedef struct {
    mongoc_client_t *client;
    const char *dbn;
    const char *json_path;
    int missing;
} sc_mongo_val_ud_t;

/** Dedicated SharedCollection cluster when ``shared_collection_mongo_uri`` differs from chat ``mongo_uri``. */
static mongoc_client_t *storage_sc_mongo_for_registry(const storage_ctx_t *ctx) {
    if (!ctx)
        return NULL;
    if (ctx->mongo_client_shared)
        return ctx->mongo_client_shared;
    return ctx->mongo_client;
}

static void storage_sc_validate_one_collection(const char *collection, int elk_allow, void *u) {
    sc_mongo_val_ud_t *ud = (sc_mongo_val_ud_t *)u;
    (void)elk_allow;
    if (!collection || !collection[0] || !ud->client)
        return;
    bson_error_t err;
    memset(&err, 0, sizeof err);
    mongoc_database_t *db = mongoc_client_get_database(ud->client, ud->dbn);
    if (!db)
        return;
    bool ex = mongoc_database_has_collection(db, collection, &err);
    mongoc_database_destroy(db);
    if (!ex) {
        ud->missing++;
        if (err.code != 0)
            fprintf(stderr,
                    "[STORAGE][SharedCollection] WARNING: Mongo check failed for collection \"%s\" "
                    "(db=%s): %s\n",
                    collection, ud->dbn, err.message);
        else
            fprintf(stderr,
                    "[STORAGE][SharedCollection] WARNING: Mongo db \"%s\" has no collection \"%s\" "
                    "(registry JSON: %s); fix integration or create the collection\n",
                    ud->dbn, collection, ud->json_path ? ud->json_path : "(unknown)");
    }
}

/** Registry JSON is untrusted input — verify each collection name exists before ELK/backfill. */
static void storage_shared_collection_validate_mongo(storage_ctx_t *ctx, sc_registry_t *reg, const char *json_path,
                                                   int elk_log) {
    if (!ctx || !reg)
        return;
    mongoc_client_t *mc = storage_sc_mongo_for_registry(ctx);
    if (!mc) {
        fprintf(stderr,
                "[STORAGE][SharedCollection] WARNING: registry loaded but no Mongo client — "
                "cannot verify collection names (set mongo_uri or shared_collection_mongo_uri in api_options)\n");
        return;
    }
    const char *dbn = storage_sc_backfill_db(ctx);
    sc_mongo_val_ud_t ud = { .client = mc, .dbn = dbn, .json_path = json_path, .missing = 0 };
    sc_registry_foreach(reg, storage_sc_validate_one_collection, &ud);
    if (ud.missing > 0) {
        fprintf(stderr,
                "[STORAGE][SharedCollection] WARNING: %d registry collection(s) missing in Mongo db \"%s\" "
                "(JSON: %s)\n",
                ud.missing, dbn, json_path ? json_path : "");
    } else if (elk_log >= 1)
        fprintf(stderr, "[STORAGE][SharedCollection] Mongo collection names in registry OK (db=%s, path=%s)\n", dbn,
                json_path ? json_path : "");
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
    {
        const char *sc_uri = storage_sc_mongo_uri_str(ctx);
        if (sc_uri && sc_uri[0]) {
            if (ctx->mongo_client && strcmp(ctx->mongo_uri, sc_uri) == 0) {
                /* Same cluster as chat — one client; storage_sc_mongo_for_registry uses mongo_client. */
            } else if (ctx->mongo_client) {
                ctx->mongo_client_shared = mongoc_client_new(sc_uri);
                if (!ctx->mongo_client_shared)
                    fprintf(stderr,
                            "[STORAGE] shared_collection_mongo_uri: second Mongo client failed "
                            "(invalid URI)\n");
                else if (m4_elk_log_level() >= 1)
                    fprintf(stderr,
                            "[STORAGE][SharedCollection] separate Mongo URI for registry/ELK backfill "
                            "(chat mongo_uri unchanged)\n");
            } else {
                mongoc_init();
                ctx->mongo_client_shared = mongoc_client_new(sc_uri);
                if (!ctx->mongo_client_shared)
                    fprintf(stderr, "[STORAGE] shared_collection_mongo_uri: Mongo client failed\n");
                else if (m4_elk_log_level() >= 1)
                    fprintf(stderr,
                            "[STORAGE][SharedCollection] Mongo client only for SharedCollection "
                            "(mongo_uri empty)\n");
            }
        }
    }
#endif
    if (ctx->redis_host[0] != '\0') {
        ctx->redis = redis_create(ctx->redis_host, ctx->redis_port);
        if (ctx->redis && redis_initial(ctx->redis) != 0) {
            redis_destroy(ctx->redis);
            ctx->redis = NULL;
        }
    }

    if (ctx->es_host[0] != '\0') {
        int eport = ctx->es_port > 0 ? ctx->es_port : ELK_DEFAULT_PORT;
        const char *jpath = storage_sc_json_path(ctx);
        int elk_log = m4_elk_log_level();
        if (elk_log >= 1)
            fprintf(stderr, "[ELK flow] init host=%s port=%d\n", ctx->es_host, eport);
        if (!ctx->elk)
            ctx->elk = elk_create(ctx->es_host, eport);
        if (jpath && jpath[0] && !ctx->sc_reg) {
            sc_registry_t *r = sc_registry_load_file(jpath);
            if (r) {
                ctx->sc_reg = r;
                if (elk_log >= 1)
                    fprintf(stderr, "[ELK flow] registry loaded path=%s elk_allow_count=%zu\n", jpath,
                            sc_registry_elk_count(r));
#ifdef USE_MONGOC
                storage_shared_collection_validate_mongo(ctx, r, jpath, elk_log);
#endif
            } else if (elk_log >= 1)
                fprintf(stderr, "[ELK flow] registry load failed path=%s\n", jpath);
        }
        const char *pool0 = getenv("M4_ELK_SYNC_POOL");
        int pool_disabled = (pool0 && pool0[0] == '0');
        if (!pool_disabled && ctx->elk && ctx->sc_reg && sc_registry_elk_count(ctx->sc_reg) > 0 &&
            !ctx->elk_pool) {
            int nw = 2;
            const char *ew = getenv("M4_ELK_SYNC_WORKERS");
            if (ew && ew[0]) {
                nw = atoi(ew);
                if (nw < 1)
                    nw = 1;
                if (nw > 16)
                    nw = 16;
            }
            size_t qcap = 512;
            const char *qc = getenv("M4_ELK_SYNC_QUEUE");
            if (qc && qc[0]) {
                unsigned long v = strtoul(qc, NULL, 10);
                if (v >= 16 && v <= 65536)
                    qcap = (size_t)v;
            }
            ctx->elk_pool = elk_sync_pool_create(ctx->elk, nw, qcap);
            if (ctx->elk_pool) {
                elk_sync_pool_start(ctx->elk_pool);
                if (elk_log >= 1)
                    fprintf(stderr, "[ELK flow] pool started workers=%d queue_cap=%zu\n", nw, (unsigned long)qcap);
#ifdef USE_MONGOC
                if (storage_sc_mongo_for_registry(ctx)) {
                    storage_elk_cold_backfill(ctx);
                    /* Start change stream for real-time sync (if schedule_refresh enabled). */
                    storage_elk_start_change_stream(ctx);
                } else if (elk_log >= 1)
                    fprintf(stderr,
                            "[ELK flow] cold_backfill skipped (no Mongo client for SharedCollection — "
                            "set mongo_uri or shared_collection_mongo_uri in api_options)\n");
#endif
            } else if (elk_log >= 1)
                fprintf(stderr, "[ELK flow] elk_sync_pool_create failed\n");
        } else if (elk_log >= 1 && !ctx->elk_pool) {
            if (pool_disabled)
                fprintf(stderr, "[ELK flow] pool not started: M4_ELK_SYNC_POOL=0\n");
            else if (!ctx->elk)
                fprintf(stderr, "[ELK flow] pool not started: elk client missing\n");
            else if (!ctx->sc_reg)
                fprintf(stderr,
                        "[ELK flow] pool not started: no registry — set api_options.shared_collection_json_path to a "
                        "SharedCollection JSON file (see .cursor/shared_collection.md; elk.allow in JSON)\n");
            else if (sc_registry_elk_count(ctx->sc_reg) == 0)
                fprintf(stderr, "[ELK flow] pool not started: elk_allow_count=0\n");
        }
        const char *diag = getenv("M4_ELK_DIAG");
        if (diag && diag[0] == '1') {
            fprintf(stderr, "[STORAGE][ELK_DIAG] target http://%s:%d\n", ctx->es_host, eport);
            fprintf(stderr, "[STORAGE][ELK_DIAG] shared_collection_json_path=%s\n",
                    jpath && jpath[0] ? jpath : "(unset)");
            fprintf(stderr, "[STORAGE][ELK_DIAG] registry=%s elk_allow_count=%zu elk_pool=%s\n",
                    ctx->sc_reg ? "loaded" : "null",
                    ctx->sc_reg ? sc_registry_elk_count(ctx->sc_reg) : (size_t)0, ctx->elk_pool ? "yes" : "no");
#ifdef USE_MONGOC
            fprintf(stderr, "[STORAGE][ELK_DIAG] mongoc client=%s (required for cold backfill)\n",
                    ctx->mongo_client ? "yes" : "no");
#else
            fprintf(stderr, "[STORAGE][ELK_DIAG] built without USE_MONGOC — no Mongo backfill path\n");
#endif
            fprintf(stderr, "[STORAGE][ELK_DIAG] M4_ELK_SYNC_POOL=%s M4_ELK_BACKFILL=%s\n",
                    (pool0 && pool0[0] == '0') ? "0 (pool off)" : "unset/nonzero (pool allowed)",
                    (getenv("M4_ELK_BACKFILL") && getenv("M4_ELK_BACKFILL")[0] == '0') ? "0 (skip backfill)"
                                                                                        : "unset/nonzero");
        }
    }

    ctx->connected = 1;
    return 0;
}

void storage_disconnect(storage_ctx_t *ctx) {
    if (!ctx) return;
    storage_elk_workers_stop(ctx);
    if (ctx->redis) {
        redis_disconnect(ctx->redis);
        redis_destroy(ctx->redis);
        ctx->redis = NULL;
    }
#ifdef USE_MONGOC
    if (ctx->mongo_client_shared) {
        mongoc_client_destroy(ctx->mongo_client_shared);
        ctx->mongo_client_shared = NULL;
    }
    if (ctx->mongo_client) {
        mongoc_client_destroy(ctx->mongo_client);
        ctx->mongo_client = NULL;
    }
#endif
    ctx->connected = 0;
}

static void storage_elk_workers_stop(storage_ctx_t *ctx) {
    if (!ctx)
        return;
    if (ctx->sc_watch_started) {
        ctx->sc_watch_stop = 1;
        pthread_join(ctx->sc_watch_tid, NULL);
        ctx->sc_watch_started = 0;
    }
    if (ctx->elk_pool) {
        elk_sync_pool_stop_destroy(ctx->elk_pool);
        ctx->elk_pool = NULL;
    }
}

static void storage_elk_fini(storage_ctx_t *ctx) {
    if (!ctx)
        return;
    storage_elk_workers_stop(ctx);
    pthread_mutex_lock(&ctx->elk_sc_mu);
    sc_registry_t *reg = ctx->sc_reg;
    ctx->sc_reg = NULL;
    pthread_mutex_unlock(&ctx->elk_sc_mu);
    if (reg)
        sc_registry_free(reg);
    if (ctx->elk) {
        elk_destroy(ctx->elk);
        ctx->elk = NULL;
    }
}

#ifdef USE_MONGOC
typedef struct {
    storage_ctx_t *ctx;
    const char *dbn;
} elk_bf_ud_t;

/* Optional: M4_ELK_INDEX_PREFIX (e.g. "m4_") prepended to SharedCollection-resolved ES index names
 * so Kibana / _cat/indices shows a clear M4 namespace. ES index names should be lowercase. */
#define M4_ELK_INDEX_PREFIX_CAP 48
#define M4_ELK_INDEX_OUT_MAX    256

static void storage_elk_apply_index_prefix(const char *resolved, char *out, size_t out_sz) {
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!resolved || !resolved[0])
        return;
    const char *pfx = getenv("M4_ELK_INDEX_PREFIX");
    if (!pfx || !pfx[0]) {
        (void)snprintf(out, out_sz, "%s", resolved);
        return;
    }
    size_t pl = strnlen(pfx, M4_ELK_INDEX_PREFIX_CAP);
    if (pl == 0) {
        (void)snprintf(out, out_sz, "%s", resolved);
        return;
    }
    if (strncmp(resolved, pfx, pl) == 0) {
        (void)snprintf(out, out_sz, "%s", resolved);
        return;
    }
    size_t rl = strlen(resolved);
    if (pl + rl + 1 >= out_sz) {
        fprintf(stderr, "[STORAGE] M4_ELK_INDEX_PREFIX: combined index name too long; using unprefixed name\n");
        (void)snprintf(out, out_sz, "%s", resolved);
        return;
    }
    (void)snprintf(out, out_sz, "%.*s%s", (int)pl, pfx, resolved);
}

/* ---------- BSON → plain JSON for Elasticsearch ---------- */

/** Escape a string for JSON output. Returns bytes written (excluding NUL). */
static size_t elk_json_escape(char *dst, size_t cap, const char *src, size_t slen) {
    size_t j = 0;
    for (size_t i = 0; i < slen && j + 6 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"')       { dst[j++] = '\\'; dst[j++] = '"'; }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
        else if (c < 0x20u) { j += (size_t)snprintf(dst + j, cap - j, "\\u%04x", c); }
        else                { dst[j++] = (char)c; }
    }
    dst[j] = '\0';
    return j;
}

static int elk_bson_iter_to_json(bson_iter_t *iter, char *buf, size_t cap, size_t *pos);

/** Convert a BSON value (at current iterator position) to plain JSON. */
static int elk_bson_value_to_json(bson_iter_t *iter, char *buf, size_t cap, size_t *pos) {
    size_t p = *pos;
    switch (bson_iter_type(iter)) {
        case BSON_TYPE_UTF8: {
            uint32_t len = 0;
            const char *s = bson_iter_utf8(iter, &len);
            if (p + len * 2 + 3 >= cap) return -1;
            buf[p++] = '"';
            p += elk_json_escape(buf + p, cap - p, s, len);
            buf[p++] = '"';
            break;
        }
        case BSON_TYPE_INT32:
            p += (size_t)snprintf(buf + p, cap - p, "%d", bson_iter_int32(iter));
            break;
        case BSON_TYPE_INT64:
            p += (size_t)snprintf(buf + p, cap - p, "%lld", (long long)bson_iter_int64(iter));
            break;
        case BSON_TYPE_DOUBLE:
            p += (size_t)snprintf(buf + p, cap - p, "%g", bson_iter_double(iter));
            break;
        case BSON_TYPE_BOOL:
            p += (size_t)snprintf(buf + p, cap - p, "%s", bson_iter_bool(iter) ? "true" : "false");
            break;
        case BSON_TYPE_NULL:
            p += (size_t)snprintf(buf + p, cap - p, "null");
            break;
        case BSON_TYPE_OID: {
            char oid_str[25];
            bson_oid_to_string(bson_iter_oid(iter), oid_str);
            p += (size_t)snprintf(buf + p, cap - p, "\"%s\"", oid_str);
            break;
        }
        case BSON_TYPE_DATE_TIME: {
            int64_t ms = bson_iter_date_time(iter);
            /* ISO 8601 for ES: "2026-04-08T10:00:00.000Z" */
            time_t sec = (time_t)(ms / 1000);
            int millis = (int)(ms % 1000);
            if (millis < 0) millis = -millis;
            struct tm tm;
            gmtime_r(&sec, &tm);
            p += (size_t)snprintf(buf + p, cap - p,
                                  "\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\"",
                                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                  tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
            break;
        }
        case BSON_TYPE_DOCUMENT: {
            bson_iter_t child;
            if (!bson_iter_recurse(iter, &child)) return -1;
            *pos = p;
            if (elk_bson_iter_to_json(&child, buf, cap, pos) != 0) return -1;
            p = *pos;
            break;
        }
        case BSON_TYPE_ARRAY: {
            bson_iter_t child;
            if (!bson_iter_recurse(iter, &child)) return -1;
            if (p + 1 >= cap) return -1;
            buf[p++] = '[';
            int first = 1;
            while (bson_iter_next(&child)) {
                if (!first) { if (p + 1 >= cap) return -1; buf[p++] = ','; }
                first = 0;
                *pos = p;
                if (elk_bson_value_to_json(&child, buf, cap, pos) != 0) return -1;
                p = *pos;
            }
            if (p + 1 >= cap) return -1;
            buf[p++] = ']';
            break;
        }
        default:
            /* Skip unsupported types (binary, regex, etc.) */
            p += (size_t)snprintf(buf + p, cap - p, "null");
            break;
    }
    *pos = p;
    return 0;
}

/** Convert a BSON document (iterator at start) to plain JSON object. Skips _id field. */
static int elk_bson_iter_to_json(bson_iter_t *iter, char *buf, size_t cap, size_t *pos) {
    size_t p = *pos;
    if (p + 1 >= cap) return -1;
    buf[p++] = '{';
    int first = 1;
    while (bson_iter_next(iter)) {
        const char *key = bson_iter_key(iter);
        if (strcmp(key, "_id") == 0) continue; /* ES uses doc ID from URL, not body */
        if (!first) { if (p + 1 >= cap) return -1; buf[p++] = ','; }
        first = 0;
        /* key */
        size_t klen = strlen(key);
        if (p + klen + 4 >= cap) return -1;
        buf[p++] = '"';
        memcpy(buf + p, key, klen);
        p += klen;
        buf[p++] = '"';
        buf[p++] = ':';
        /* value */
        *pos = p;
        if (elk_bson_value_to_json(iter, buf, cap, pos) != 0) return -1;
        p = *pos;
    }
    if (p + 1 >= cap) return -1;
    buf[p++] = '}';
    buf[p] = '\0';
    *pos = p;
    return 0;
}

/** Convert a bson_t document to ES-friendly plain JSON. Caller must free() result. */
static char *elk_bson_to_plain_json(const bson_t *doc) {
    size_t cap = 65536;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    bson_iter_t iter;
    if (!bson_iter_init(&iter, doc)) { free(buf); return NULL; }
    size_t pos = 0;
    if (elk_bson_iter_to_json(&iter, buf, cap, &pos) != 0) { free(buf); return NULL; }
    return buf;
}

/** Milliseconds for ES ``@timestamp``: first BSON date among known keys, else wall clock now. */
static int64_t storage_elk_resolve_timestamp_ms(const bson_t *doc) {
    static const char *const keys[] = {"@timestamp", "createdAt", "updatedAt", "timestamp", "created_at",
                                       "updated_at", NULL};
    bson_iter_t it;
    for (size_t i = 0; keys[i]; i++) {
        if (bson_iter_init_find(&it, doc, keys[i]) && BSON_ITER_HOLDS_DATE_TIME(&it))
            return bson_iter_date_time(&it);
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return (int64_t)time(NULL) * 1000;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000LL;
}

/* ---------- ELK sync state: track last indexed _id per collection ---------- */

/** Read last indexed _id for a collection from state file. Returns 1 if found, 0 if not. */
static int elk_sync_state_read(const char *state_path, const char *collection,
                               char *out_oid, size_t oid_cap) {
    out_oid[0] = '\0';
    if (!state_path || !state_path[0]) return 0;
    FILE *f = fopen(state_path, "r");
    if (!f) return 0;
    char line[512];
    size_t clen = strlen(collection);
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        /* Format: collection:oid_hex */
        if (strncmp(line, collection, clen) == 0 && line[clen] == ':') {
            const char *oid = line + clen + 1;
            size_t olen = strlen(oid);
            while (olen > 0 && (oid[olen - 1] == '\n' || oid[olen - 1] == '\r')) olen--;
            if (olen > 0 && olen < oid_cap) {
                memcpy(out_oid, oid, olen);
                out_oid[olen] = '\0';
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

/** Write/update last indexed _id for a collection in state file. */
static void elk_sync_state_write(const char *state_path, const char *collection, const char *oid_hex) {
    if (!state_path || !state_path[0] || !collection || !oid_hex) return;

    /* Read existing lines, update or append. */
    char lines[64][512];
    int nlines = 0;
    int found = 0;
    size_t clen = strlen(collection);

    FILE *f = fopen(state_path, "r");
    if (f) {
        while (nlines < 64 && fgets(lines[nlines], sizeof(lines[0]), f)) {
            /* Remove trailing newline for comparison */
            size_t L = strlen(lines[nlines]);
            while (L > 0 && (lines[nlines][L - 1] == '\n' || lines[nlines][L - 1] == '\r'))
                lines[nlines][--L] = '\0';
            if (lines[nlines][0] != '#' && strncmp(lines[nlines], collection, clen) == 0
                && lines[nlines][clen] == ':') {
                snprintf(lines[nlines], sizeof(lines[0]), "%s:%s", collection, oid_hex);
                found = 1;
            }
            nlines++;
        }
        fclose(f);
    }
    if (!found && nlines < 64) {
        snprintf(lines[nlines], sizeof(lines[0]), "%s:%s", collection, oid_hex);
        nlines++;
    }

    f = fopen(state_path, "w");
    if (!f) {
        fprintf(stderr, "[ELK flow] sync_state: cannot write %s: %s\n", state_path, strerror(errno));
        return;
    }
    for (int i = 0; i < nlines; i++)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
}

static void storage_elk_backfill_collection(const char *collection, const char *index, void *u) {
    elk_bf_ud_t *ud = (elk_bf_ud_t *)u;
    storage_ctx_t *ctx = ud->ctx;
    mongoc_client_t *mc = storage_sc_mongo_for_registry(ctx);
    if (!mc || !ctx->elk_pool)
        return;

    size_t enqueued = 0;
    char index_disp[M4_ELK_INDEX_OUT_MAX];
    storage_elk_apply_index_prefix(index, index_disp, sizeof index_disp);
    mongoc_database_t *db = mongoc_client_get_database(mc, ud->dbn);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, collection);

    /* Build filter: incremental (_id > saved) or full (empty filter). */
    bson_t filter;
    bson_init(&filter);
    char last_oid[96] = {0};
    int incremental = 0;

    if (ctx->schedule_refresh && ctx->elk_sync_state_path[0]) {
        if (elk_sync_state_read(ctx->elk_sync_state_path, collection, last_oid, sizeof last_oid) && last_oid[0]) {
            /* Try to parse as ObjectId for $gt filter. */
            bson_oid_t oid;
            if (bson_oid_is_valid(last_oid, strlen(last_oid))) {
                bson_oid_init_from_string(&oid, last_oid);
                bson_t child;
                BSON_APPEND_DOCUMENT_BEGIN(&filter, "_id", &child);
                BSON_APPEND_OID(&child, "$gt", &oid);
                bson_append_document_end(&filter, &child);
                incremental = 1;
                if (m4_elk_log_level() >= 1)
                    fprintf(stderr, "[ELK flow] incremental backfill collection=%s since _id > %s\n",
                            collection, last_oid);
            }
        }
    }

    /* Sort by _id ascending so we can track the last one. */
    bson_t opts;
    bson_init(&opts);
    {
        bson_t sort_child;
        BSON_APPEND_DOCUMENT_BEGIN(&opts, "sort", &sort_child);
        BSON_APPEND_INT32(&sort_child, "_id", 1);
        bson_append_document_end(&opts, &sort_child);
    }
    BSON_APPEND_INT32(&opts, "batchSize", 500);

    mongoc_cursor_t *cur = mongoc_collection_find_with_opts(coll, &filter, &opts, NULL);
    bson_destroy(&opts);
    const bson_t *doc;
    char last_id_hex[96] = {0};

    while (mongoc_cursor_next(cur, &doc)) {
        char id_hex[96];
        id_hex[0] = '\0';
        bson_iter_t it;
        if (bson_iter_init_find(&it, doc, "_id")) {
            if (BSON_ITER_HOLDS_OID(&it)) {
                bson_oid_to_string(bson_iter_oid(&it), id_hex);
            } else if (BSON_ITER_HOLDS_UTF8(&it)) {
                uint32_t ulen = 0;
                const char *s = bson_iter_utf8(&it, &ulen);
                size_t cpy = ulen < sizeof(id_hex) - 1 ? ulen : sizeof(id_hex) - 1;
                memcpy(id_hex, s, cpy);
                id_hex[cpy] = '\0';
            } else if (BSON_ITER_HOLDS_INT32(&it)) {
                snprintf(id_hex, sizeof id_hex, "%d", bson_iter_int32(&it));
            } else if (BSON_ITER_HOLDS_INT64(&it)) {
                snprintf(id_hex, sizeof id_hex, "%lld", (long long)bson_iter_int64(&it));
            }
        }
        if (!id_hex[0])
            continue;

        /* Track last _id for sync state. */
        snprintf(last_id_hex, sizeof last_id_hex, "%s", id_hex);

        bson_t doc_elk;
        bson_init(&doc_elk);
        bson_copy_to_excluding_noinit(doc, &doc_elk, "@timestamp", NULL);
        if (!BSON_APPEND_DATE_TIME(&doc_elk, "@timestamp", storage_elk_resolve_timestamp_ms(doc))) {
            bson_destroy(&doc_elk);
            continue;
        }

        char *json = elk_bson_to_plain_json(&doc_elk);
        bson_destroy(&doc_elk);
        if (!json)
            continue;
        char index_out[M4_ELK_INDEX_OUT_MAX];
        storage_elk_apply_index_prefix(index, index_out, sizeof index_out);
        if (elk_sync_pool_enqueue(ctx->elk_pool, index_out, id_hex, json) != 0)
            fprintf(stderr, "[STORAGE] ELK backfill enqueue failed %s\n", collection);
        else
            enqueued++;
        free(json);
    }

    mongoc_cursor_destroy(cur);
    bson_destroy(&filter);
    mongoc_collection_destroy(coll);
    mongoc_database_destroy(db);

    /* Save last _id for next incremental run. */
    if (ctx->schedule_refresh && ctx->elk_sync_state_path[0] && last_id_hex[0])
        elk_sync_state_write(ctx->elk_sync_state_path, collection, last_id_hex);

    if (m4_elk_log_level() >= 1)
        fprintf(stderr, "[ELK flow] pump enqueue collection=%s es_index=%s docs=%zu mode=%s\n",
                collection, index_disp, enqueued, incremental ? "incremental" : "full");
}

#ifdef USE_MONGOC
/* ---------- Change stream: real-time Mongo → ELK sync ---------- */

typedef struct {
    storage_ctx_t *ctx;
    const char *dbn;
} cs_watch_ud_t;

/** Process one change stream event: insert or update → index to ELK. */
static void cs_process_event(storage_ctx_t *ctx, const char *dbn, const bson_t *event) {
    (void)dbn;
    bson_iter_t iter;

    /* Get operation type. */
    char op[32] = {0};
    if (bson_iter_init_find(&iter, event, "operationType") && BSON_ITER_HOLDS_UTF8(&iter)) {
        uint32_t len = 0;
        const char *s = bson_iter_utf8(&iter, &len);
        size_t cpy = len < sizeof(op) - 1 ? len : sizeof(op) - 1;
        memcpy(op, s, cpy);
        op[cpy] = '\0';
    }
    /* Only handle insert and update (replace counts as update). */
    if (strcmp(op, "insert") != 0 && strcmp(op, "update") != 0 && strcmp(op, "replace") != 0)
        return;

    /* Get collection name from ns.coll. */
    char coll_name[160] = {0};
    if (bson_iter_init_find(&iter, event, "ns")) {
        bson_iter_t ns_iter;
        if (BSON_ITER_HOLDS_DOCUMENT(&iter) && bson_iter_recurse(&iter, &ns_iter)) {
            if (bson_iter_find(&ns_iter, "coll") && BSON_ITER_HOLDS_UTF8(&ns_iter)) {
                uint32_t len = 0;
                const char *s = bson_iter_utf8(&ns_iter, &len);
                size_t cpy = len < sizeof(coll_name) - 1 ? len : sizeof(coll_name) - 1;
                memcpy(coll_name, s, cpy);
                coll_name[cpy] = '\0';
            }
        }
    }
    if (!coll_name[0]) return;

    /* Check if collection is elk.allow. */
    if (!sc_registry_elk_allowed(ctx->sc_reg, coll_name)) return;

    /* Resolve ELK index. */
    char elk_idx[M4_ELK_INDEX_OUT_MAX];
    if (sc_registry_elk_index(ctx->sc_reg, coll_name, elk_idx, sizeof elk_idx) != 0) return;
    char index_out[M4_ELK_INDEX_OUT_MAX];
    storage_elk_apply_index_prefix(elk_idx, index_out, sizeof index_out);

    /* For insert: fullDocument is in the event. For update: need fullDocument (use fullDocument:"updateLookup"). */
    bson_iter_t doc_iter;
    if (!bson_iter_init_find(&doc_iter, event, "fullDocument") || !BSON_ITER_HOLDS_DOCUMENT(&doc_iter))
        return;

    uint32_t doc_len = 0;
    const uint8_t *doc_data = NULL;
    bson_iter_document(&doc_iter, &doc_len, &doc_data);
    if (!doc_data || doc_len == 0) return;

    bson_t full_doc;
    if (!bson_init_static(&full_doc, doc_data, doc_len)) return;

    /* Extract _id. */
    char id_hex[96] = {0};
    bson_iter_t id_iter;
    if (bson_iter_init_find(&id_iter, &full_doc, "_id")) {
        if (BSON_ITER_HOLDS_OID(&id_iter))
            bson_oid_to_string(bson_iter_oid(&id_iter), id_hex);
        else if (BSON_ITER_HOLDS_UTF8(&id_iter)) {
            uint32_t ulen = 0;
            const char *s = bson_iter_utf8(&id_iter, &ulen);
            size_t cpy = ulen < sizeof(id_hex) - 1 ? ulen : sizeof(id_hex) - 1;
            memcpy(id_hex, s, cpy);
            id_hex[cpy] = '\0';
        }
    }
    if (!id_hex[0]) return;

    /* Build ELK doc with @timestamp. */
    bson_t doc_elk;
    bson_init(&doc_elk);
    bson_copy_to_excluding_noinit(&full_doc, &doc_elk, "@timestamp", NULL);
    BSON_APPEND_DATE_TIME(&doc_elk, "@timestamp", storage_elk_resolve_timestamp_ms(&full_doc));

    char *json = elk_bson_to_plain_json(&doc_elk);
    bson_destroy(&doc_elk);
    if (!json) return;

    if (elk_sync_pool_enqueue(ctx->elk_pool, index_out, id_hex, json) == 0) {
        if (m4_elk_log_level() >= 2)
            fprintf(stderr, "[ELK flow] change_stream: %s %s/%s → %s\n", op, coll_name, id_hex, index_out);
    }
    free(json);

    /* Update sync state with latest _id. */
    if (ctx->schedule_refresh && ctx->elk_sync_state_path[0])
        elk_sync_state_write(ctx->elk_sync_state_path, coll_name, id_hex);
}

/** Change stream watcher thread: watches the database for all elk.allow collections. */
static void *storage_cs_watch_thread(void *arg) {
    cs_watch_ud_t *ud = (cs_watch_ud_t *)arg;
    storage_ctx_t *ctx = ud->ctx;
    const char *dbn = ud->dbn;

    mongoc_client_t *mc = storage_sc_mongo_for_registry(ctx);
    if (!mc) { free(ud); return NULL; }

    mongoc_database_t *db = mongoc_client_get_database(mc, dbn);

    /* Pipeline: filter to only elk.allow collections. */
    /* We watch the whole database and filter in cs_process_event. */
    bson_t pipeline;
    bson_init(&pipeline);
    bson_t empty_array;
    bson_init(&empty_array);
    /* Empty pipeline = watch all changes in the database. */

    /* Options: fullDocument for updates, batch size for efficiency. */
    bson_t opts;
    bson_init(&opts);
    BSON_APPEND_UTF8(&opts, "fullDocument", "updateLookup");
    BSON_APPEND_INT32(&opts, "batchSize", 100);

    mongoc_change_stream_t *stream = mongoc_database_watch(db, &pipeline, &opts);
    bson_destroy(&pipeline);
    bson_destroy(&empty_array);
    bson_destroy(&opts);

    if (!stream) {
        fprintf(stderr, "[ELK flow] change_stream: failed to open (is MongoDB a replica set?)\n");
        mongoc_database_destroy(db);
        free(ud);
        return NULL;
    }

    fprintf(stderr, "[ELK flow] change_stream: watching db=%s for elk.allow collections (real-time sync)\n", dbn);

    const bson_t *event;
    while (!ctx->sc_watch_stop) {
        /* mongoc_change_stream_next blocks until next event or timeout. */
        if (mongoc_change_stream_next(stream, &event)) {
            cs_process_event(ctx, dbn, event);
        } else {
            /* Check for error. */
            bson_error_t error;
            if (mongoc_change_stream_error_document(stream, &error, NULL)) {
                fprintf(stderr, "[ELK flow] change_stream: error %d.%d: %s\n",
                        error.domain, error.code, error.message);
                /* Brief pause before retry. */
                if (!ctx->sc_watch_stop) {
                    struct timespec ts = {1, 0};
                    nanosleep(&ts, NULL);
                }
            }
            /* No event within timeout — loop and check sc_watch_stop. */
        }
    }

    mongoc_change_stream_destroy(stream);
    mongoc_database_destroy(db);
    free(ud);
    fprintf(stderr, "[ELK flow] change_stream: stopped\n");
    return NULL;
}

/** Preflight check: can we open a change stream? Test and report what's missing. */
static int storage_elk_cs_preflight(storage_ctx_t *ctx) {
    if (!ctx) return -1;

    /* Check 1: schedule_refresh must be enabled. */
    if (!ctx->schedule_refresh) return -1; /* silent — user didn't ask for it */

    /* Check 2: ELK pool must be running. */
    if (!ctx->elk_pool) {
        fprintf(stderr, "[ELK flow][ERROR] change_stream requires ELK pool (es_host must be set and reachable)\n");
        return -1;
    }

    /* Check 3: SharedCollection registry with elk.allow collections. */
    if (!ctx->sc_reg || sc_registry_elk_count(ctx->sc_reg) == 0) {
        fprintf(stderr, "[ELK flow][ERROR] change_stream requires SharedCollection registry with elk.allow collections "
                        "(set shared_collection_json_path with at least one collection having elk.allow: true)\n");
        return -1;
    }

    /* Check 4: MongoDB client available. */
    mongoc_client_t *mc = storage_sc_mongo_for_registry(ctx);
    if (!mc) {
        fprintf(stderr, "[ELK flow][ERROR] change_stream requires MongoDB connection "
                        "(set mongo_uri or shared_collection_mongo_uri)\n");
        return -1;
    }

    /* Check 5: MongoDB must be a replica set — test by trying to open a change stream. */
    const char *dbn = storage_sc_backfill_db(ctx);
    mongoc_database_t *db = mongoc_client_get_database(mc, dbn);
    bson_t pipeline;
    bson_init(&pipeline);
    bson_t opts;
    bson_init(&opts);
    BSON_APPEND_INT32(&opts, "maxAwaitTimeMS", 1000); /* short timeout for test */

    mongoc_change_stream_t *test_stream = mongoc_database_watch(db, &pipeline, &opts);
    bson_destroy(&pipeline);
    bson_destroy(&opts);

    if (!test_stream) {
        fprintf(stderr, "[ELK flow][ERROR] change_stream: MongoDB does not support change streams.\n"
                        "  Requirements:\n"
                        "    - MongoDB must be a replica set (even single-node: mongod --replSet rs0, then rs.initiate())\n"
                        "    - Standalone mongod does NOT support change streams\n"
                        "    - MongoDB version 3.6+ required\n"
                        "  Falling back to incremental _id backfill only (new docs synced on restart).\n");
        mongoc_database_destroy(db);
        return -1;
    }

    /* Check for immediate errors (e.g. not a replica set). */
    bson_error_t error;
    const bson_t *event;
    /* Try one next() — will fail fast if not a replica set. */
    (void)mongoc_change_stream_next(test_stream, &event);
    if (mongoc_change_stream_error_document(test_stream, &error, NULL)) {
        fprintf(stderr, "[ELK flow][ERROR] change_stream: MongoDB rejected change stream request.\n"
                        "  Error: %s (code %d)\n"
                        "  Common cause: MongoDB is not running as a replica set.\n"
                        "  Fix: start mongod with --replSet rs0, then run rs.initiate() in mongosh.\n"
                        "  Falling back to incremental _id backfill only.\n",
                        error.message, error.code);
        mongoc_change_stream_destroy(test_stream);
        mongoc_database_destroy(db);
        return -1;
    }

    mongoc_change_stream_destroy(test_stream);
    mongoc_database_destroy(db);
    return 0; /* All checks passed. */
}

/** Start change stream watcher if preflight passes. */
static void storage_elk_start_change_stream(storage_ctx_t *ctx) {
    if (storage_elk_cs_preflight(ctx) != 0) return;

    mongoc_client_t *mc = storage_sc_mongo_for_registry(ctx);
    if (!mc) return;

    const char *dbn = storage_sc_backfill_db(ctx);
    cs_watch_ud_t *ud = (cs_watch_ud_t *)malloc(sizeof(*ud));
    if (!ud) return;
    ud->ctx = ctx;
    ud->dbn = dbn;
    ctx->sc_watch_stop = 0;

    if (pthread_create(&ctx->sc_watch_tid, NULL, storage_cs_watch_thread, ud) != 0) {
        fprintf(stderr, "[ELK flow][ERROR] change_stream: pthread_create failed\n");
        free(ud);
        return;
    }
    ctx->sc_watch_started = 1;
}
#endif /* USE_MONGOC — change stream */

static void storage_elk_cold_backfill(storage_ctx_t *ctx) {
    if (!ctx || !ctx->elk_pool || !storage_sc_mongo_for_registry(ctx))
        return;
    pthread_mutex_lock(&ctx->elk_sc_mu);
    sc_registry_t *reg = ctx->sc_reg;
    pthread_mutex_unlock(&ctx->elk_sc_mu);
    if (!reg)
        return;
    const char *dbn = storage_sc_backfill_db(ctx);
    if (m4_elk_log_level() >= 1)
        fprintf(stderr, "[ELK flow] cold_backfill scanning Mongo db=%s (foreach elk.allow collection)\n", dbn);
    elk_bf_ud_t ud = { .ctx = ctx, .dbn = dbn };
    sc_registry_foreach_elk(reg, storage_elk_backfill_collection, &ud);
}
#endif

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

sc_registry_t *storage_get_sc_registry(storage_ctx_t *ctx) {
    return ctx ? ctx->sc_reg : NULL;
}

int storage_elk_search(storage_ctx_t *ctx, const char *index, const char *query_json,
                       char *out, size_t out_size) {
    if (!ctx || !ctx->elk || !index || !query_json || !out || out_size == 0)
        return -1;
    return elk_search(ctx->elk, index, query_json, out, out_size);
}

void storage_set_schedule_refresh(storage_ctx_t *ctx, int enable) {
    if (!ctx) return;
    ctx->schedule_refresh = enable ? 1 : 0;
    if (enable && ctx->shared_collection_json_path[0]) {
        /* Derive state file path: same directory as registry JSON, named .elk_sync_state */
        const char *p = ctx->shared_collection_json_path;
        const char *last_slash = strrchr(p, '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - p + 1);
            if (dir_len + 16 < sizeof(ctx->elk_sync_state_path)) {
                memcpy(ctx->elk_sync_state_path, p, dir_len);
                memcpy(ctx->elk_sync_state_path + dir_len, ".elk_sync_state", 16);
            }
        }
    }
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

int storage_elk_ingest(storage_ctx_t *ctx, const char *elk_base_url, const char *raw_text) {
    if (!ctx || !elk_base_url || !raw_text)
        return -1;
    if (!ctx->elk) {
        int eport = ctx->es_port > 0 ? ctx->es_port : ELK_DEFAULT_PORT;
        elk_ctx_t *e = elk_create(ctx->es_host[0] ? ctx->es_host : "127.0.0.1", eport);
        if (!e)
            return -1;
        int r = elk_set_ingest(e, elk_base_url, raw_text);
        elk_destroy(e);
        return r;
    }
    return elk_set_ingest(ctx->elk, elk_base_url, raw_text);
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
            BSON_APPEND_UTF8(target, "embed_family", M4_STORED_EMBED_FAMILY_CUSTOM);
        else
            BSON_APPEND_UTF8(target, "embed_family", M4_STORED_EMBED_FAMILY_OLLAMA);
    } else if (vector_dim > 0)
        BSON_APPEND_UTF8(target, "embed_family", M4_STORED_EMBED_FAMILY_LEGACY);
}
#endif

/* Append one turn (input + assistant) as a single document — new shape per .cursor/mongo.md §0. */
int storage_append_turn(storage_ctx_t *ctx, const char *tenant_id, const char *user_id,
                        const char *input, const char *assistant, const char *timestamp,
                        const float *vector, size_t vector_dim,
                        const char *lang, double lang_score,
                        const char *embed_model_id,
                        const char *llm_model_id,
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
    if (llm_model_id && llm_model_id[0])
        BSON_APPEND_UTF8(&meta, "llm_model_id", llm_model_id);
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
    (void)llm_model_id;
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
static int storage_history_user_id_valid(const char *user_id) {
    if (!user_id || !user_id[0]) return 1;
    return tenant_validate_id(user_id) ? 1 : 0;
}

static int storage_history_tenant_resolve(const char *tenant_id, const char **out_tid) {
    const char *t = (tenant_id && tenant_id[0]) ? tenant_id : "default";
    if (!tenant_validate_id(t)) return -1;
    *out_tid = t;
    return 0;
}

static int storage_history_cache_key(char *key, size_t ksz, const char *tenant_id, const char *user_id) {
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : "default";
    if (!user_id || !user_id[0]) {
        int n = snprintf(key, ksz, "%s%s", HISTORY_CACHE_KEY_PREFIX, tid);
        return (n < 0 || (size_t)n >= ksz) ? -1 : 0;
    }
    int n = snprintf(key, ksz, "%s%s:%s", HISTORY_CACHE_KEY_PREFIX, tid, user_id);
    return (n < 0 || (size_t)n >= ksz) ? -1 : 0;
}

int storage_get_chat_history(storage_ctx_t *ctx, const char *tenant_id, const char *user_id, int limit,
                             storage_chat_history_cb callback, void *userdata) {
    if (!ctx || !callback || limit <= 0) return -1;
    if (!storage_history_user_id_valid(user_id)) return -1;
    const char *tid_norm = NULL;
    if (storage_history_tenant_resolve(tenant_id, &tid_norm) != 0) return -1;
#ifndef USE_MONGOC
    (void)user_id;
    (void)userdata;
    return 0;
#else
    if (!ctx->connected || !ctx->mongo_client) return -1;
    mongoc_database_t *db = mongoc_client_get_database(ctx->mongo_client, STORAGE_CHAT_DB);
    mongoc_collection_t *coll = mongoc_database_get_collection(db, STORAGE_CHAT_COLLECTION);

    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "tenant_id", tid_norm);
    if (user_id && user_id[0])
        BSON_APPEND_UTF8(&filter, "user", user_id);
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
int storage_get_chat_history_cached(storage_ctx_t *ctx, const char *tenant_id, const char *user_id, int limit,
                                    storage_chat_history_cb callback, void *userdata) {
    if (!ctx || !callback || limit <= 0) return -1;
    if (!storage_history_user_id_valid(user_id)) return -1;
    const char *tid_norm = NULL;
    if (storage_history_tenant_resolve(tenant_id, &tid_norm) != 0) return -1;
    char key[160];
    if (storage_history_cache_key(key, sizeof(key), tid_norm, user_id) != 0)
        return storage_get_chat_history(ctx, tid_norm, user_id, limit, callback, userdata);

    char *cache_buf = (char *)malloc(HISTORY_CACHE_BUF_SIZE);
    if (!cache_buf) return -1;

    if (ctx->redis && redis_connected(ctx->redis)) {
        if (redis_search_value(ctx->redis, key, cache_buf, HISTORY_CACHE_BUF_SIZE) == 1 && cache_buf[0]) {
            history_deserialize_and_cb(cache_buf, callback, userdata);
            free(cache_buf);
            return 0;
        }
    }
#ifdef USE_MONGOC
    char *accum_buf = (char *)malloc(HISTORY_CACHE_BUF_SIZE);
    if (!accum_buf) {
        free(cache_buf);
        return -1;
    }
    history_accum_t acc = { accum_buf, HISTORY_CACHE_BUF_SIZE, 0, callback, userdata };
    int ret = storage_get_chat_history(ctx, tid_norm, user_id, limit, history_accum_cb, &acc);
    if (ret == 0 && ctx->redis && redis_connected(ctx->redis) && acc.used > 0)
        redis_set_value(ctx->redis, key, accum_buf, REDIS_CACHE_TTL_SECONDS);
    free(accum_buf);
    free(cache_buf);
    return ret;
#else
    int ret = storage_get_chat_history(ctx, tid_norm, user_id, limit, callback, userdata);
    free(cache_buf);
    return ret;
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
            BSON_APPEND_UTF8(&set, "metadata.embed_family", M4_STORED_EMBED_FAMILY_CUSTOM);
        else
            BSON_APPEND_UTF8(&set, "metadata.embed_family", M4_STORED_EMBED_FAMILY_OLLAMA);
    } else if (row->vector_dim > 0)
        BSON_APPEND_UTF8(&set, "metadata.embed_family", M4_STORED_EMBED_FAMILY_LEGACY);

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
