#ifndef M4_STORAGE_H
#define M4_STORAGE_H

/*
 * Storage facade — composes mongo / redis / elk modules.
 * Module rules: .cursor/mongo.md, .cursor/redis.md, .cursor/elk.md.
 * Discussion (config-driven data flow): .cursor/STORAGE_MODULES_DISCUSSION.md.
 */
#include <stddef.h>
#include <stdint.h>

/* MongoDB: default database and collection (multi-tenant; every doc has tenant_id). */
#define STORAGE_MONGO_DB_NAME        "m4_ai"
#define STORAGE_MONGO_COLLECTION    "records"
/* Chat persistence: db and collection for user/bot messages (for recording and analytics). */
#define STORAGE_CHAT_DB             "bot"
#define STORAGE_CHAT_COLLECTION     "records"
/* AI logs: errors/warnings for stats and ELK; same db as chat. */
#define STORAGE_AI_LOGS_DB          "bot"
#define STORAGE_AI_LOGS_COLLECTION  "ai_logs"
/* Localization: all string fields (content, tenant_id, etc.) are UTF-8. Set locale (e.g. setlocale(LC_ALL, "")) in app for correct display. */

/* Abstraction for MongoDB (vector search), Redis (counters), Elasticsearch (analytics). */

typedef struct storage_ctx storage_ctx_t;

storage_ctx_t *storage_create(const char *mongo_uri, const char *redis_host, int redis_port,
                              const char *es_host, int es_port, const char *shared_collection_mongo_uri,
                              const char *shared_collection_json_path, const char *shared_collection_backfill_db);
void storage_destroy(storage_ctx_t *ctx);

int storage_connect(storage_ctx_t *ctx);
void storage_disconnect(storage_ctx_t *ctx);

/* MongoDB batch upsert. */
int storage_upsert_batch(storage_ctx_t *ctx, const char *tenant_id,
                         const void *records, size_t count);

/* ELK auto-language ingest. Posts to pipeline=auto_lang_processor. */
int storage_elk_ingest(storage_ctx_t *ctx, const char *elk_base_url, const char *raw_text);

/* Append one chat message to MongoDB db=STORAGE_CHAT_DB, collection=STORAGE_CHAT_COLLECTION. */
int storage_append_chat(storage_ctx_t *ctx, const char *tenant_id, const char *role,
                        const char *content, const char *timestamp);

/**
 * Append one turn (user input + assistant reply) as a single document — new shape per .cursor/mongo.md §0.
 * Writes: tenant_id, user, optional temp_message_id (streaming correlation), vector (if non-NULL), turn, timestamp, createdAt, metadata: { ver, encrypted, model_id, lang, score }.
 * vector: optional; NULL or dim 0 → store empty array []. lang: optional; NULL → "mixed". score: use 0 if not set.
 * embed_model_id: embedding model that produced vector (e.g. nomic-embed-text); NULL → "". Used to know which docs need vector regen when model changes.
 * llm_model_id: completion model / route label (e.g. groq:llama-3.1-8b-instant, ollama:qwen2.5); NULL or "" → omit metadata field.
 * temp_message_id: NULL or "" → omit field; else stored for client sync (.cursor/streaming.md).
 * has_logic_conflict: stored in metadata when conflict_detector flags intra-turn inconsistency (.cursor/conflict_detector.md).
 */
int storage_append_turn(storage_ctx_t *ctx, const char *tenant_id, const char *user_id,
                        const char *input, const char *assistant, const char *timestamp,
                        const float *vector, size_t vector_dim,
                        const char *lang, double lang_score,
                        const char *embed_model_id,
                        const char *llm_model_id,
                        const char *temp_message_id,
                        int has_logic_conflict);

/** 0 = not connected, 1 = connected. For stat module and connection status. */
int storage_mongo_connected(storage_ctx_t *ctx);
/** 1 if Redis L2 is connected (for RAG); 0 otherwise. */
int storage_redis_connected(storage_ctx_t *ctx);

/** SharedCollection registry pointer (NULL if no registry loaded). */
struct sc_registry;
struct sc_registry *storage_get_sc_registry(storage_ctx_t *ctx);

/** ELK search: POST /{index}/_search with query_json body. Returns 0 on success. */
int storage_elk_search(storage_ctx_t *ctx, const char *index, const char *query_json,
                       char *out, size_t out_size);

/** Enable incremental ELK sync (schedule_refresh). Call before storage_connect. */
void storage_set_schedule_refresh(storage_ctx_t *ctx, int enable);

/**
 * RAG search: query by vector (tenant, user). Uses Redis L2 when connected; otherwise no-op (0 hits).
 * Invokes callback(snippet, score, userdata) for each hit above min_score. Returns 0 on success, -1 on error.
 */
typedef void (*storage_rag_hit_cb)(const char *snippet, double score, void *userdata);
int storage_rag_search(storage_ctx_t *ctx, const char *tenant_id, const char *user_id,
                      const float *query_vector, size_t dim, size_t k, double min_score,
                      storage_rag_hit_cb callback, void *userdata);


/** Append one log line to ai_logs collection (and optionally ELK). level = "error" | "warning". */
int storage_append_ai_log(storage_ctx_t *ctx, const char *tenant_id, const char *level,
                          const char *message);

/**
 * Override ai_logs db/collection (only via this setter with validated input).
 * db and coll must be non-NULL, length 1..63, only [a-zA-Z0-9_]. Returns -1 if invalid.
 */
int storage_set_ai_logs(storage_ctx_t *ctx, const char *db, const char *collection);

/**
 * Get chat history for a tenant from MongoDB (bot.records).
 * Fetches up to `limit` **newest documents** (createdAt desc), then emits at most **`limit` messages**
 * (the most recent lines: user/assistant rows), oldest-first, so API index 0 = oldest in that window.
 * Turn documents count as 2 messages; legacy role/content as 1. `limit` = batch size in **messages**
 * (same intent as context_batch_size in api_load_chat_history).
 * `user_id`: NULL or "" → all documents for `tenant_id` (legacy). Non-empty → only documents where field **`user`**
 * matches (same charset rules as tenant_id: alphanumeric, `_`, `-`, length under 64). Isolates prompt context per user.
 * role/content/ts are UTF-8; ts may be "" if not present. Returns 0 on success, -1 if not connected, invalid user_id, or no Mongo.
 */
typedef void (*storage_chat_history_cb)(const char *role, const char *content, const char *ts, void *userdata);
int storage_get_chat_history(storage_ctx_t *ctx, const char *tenant_id, const char *user_id, int limit,
                             storage_chat_history_cb callback, void *userdata);

/**
 * Get chat history with L1 cache: try Redis first (`m4:cache:history:{tenant_id}` or, when `user_id` is set,
 * `m4:cache:history:{tenant_id}:{user_id}`); on miss load from MongoDB and SET with TTL REDIS_CACHE_TTL_SECONDS.
 * Same callback/limit semantics as storage_get_chat_history. Heap buffers (no static scratch) for concurrent safety.
 * Returns 0 on success, -1 on error.
 */
int storage_get_chat_history_cached(storage_ctx_t *ctx, const char *tenant_id, const char *user_id, int limit,
                                    storage_chat_history_cb callback, void *userdata);

/* --- Geo atlas (optional place memory, .cursor/geo_leanring.md) --- */
#define STORAGE_GEO_ATLAS_DB         "bot"
#define STORAGE_GEO_ATLAS_COLLECTION "geo_atlas"
#define GEO_ATLAS_SIMILARITY_THRESHOLD 0.9

/** Seed / global rows use this tenant_id for conflict checks visible to all tenants. */
#define STORAGE_GEO_TENANT_GLOBAL    "__global__"

/** source: learned from chat vs curated seed (§7). */
#define STORAGE_GEO_SOURCE_USER      "user"
#define STORAGE_GEO_SOURCE_SEED      "seed"
/** Curated rows after schema cleanup / standardization (.cursor/geo_leanring.md §13). */
#define STORAGE_GEO_SOURCE_MANUAL_SEED_CLEANUP   "manual_seed_cleanup"

/** verification_status (§7). */
#define STORAGE_GEO_STATUS_VERIFIED              "verified"
#define STORAGE_GEO_STATUS_PENDING_VERIFICATION  "pending_verification"
/** Administrative merge (e.g. 2025–26 re-org); excluded from [KNOWLEDGE_BASE] by default. */
#define STORAGE_GEO_STATUS_MERGED                "merged"

typedef struct storage_geo_atlas_doc {
    const char *tenant_id;           /* isolation; NULL/"" → "default" */
    const char *name;
    const char *name_normalized;
    const char *district;            /* capital / admin center when applicable */
    const char *axis;                /* routes / nav infra — not geographic macro-region */
    const char *category;            /* Province, District, Landmark, … */
    const char *city;
    const char *region;              /* e.g. Mekong Delta, South — optional */
    const char *country;             /* mandatory in schema; default Vietnam in geo_learning */
    const char *landmarks;           /* optional POI list text */
    const char *merged_into;         /* normalized target name after admin merge; optional */
    /** Optional geo-dynamics hints (§16): short action tag + detail text from extraction. */
    const char *admin_action;
    const char *admin_detail;
    const float *vector;
    size_t vector_dim;
    const char *embed_model_id;
    const char *source;              /* STORAGE_GEO_SOURCE_* */
    const char *verification_status; /* STORAGE_GEO_STATUS_* */
    double trust_score;              /* 0.0–1.0 */
} storage_geo_atlas_doc_t;

/** Full insert with integrity fields (§7). Preferred for geo_learning. */
int storage_geo_atlas_insert_doc(storage_ctx_t *ctx, const storage_geo_atlas_doc_t *doc);

/**
 * Returns 1 if a document exists with same tenant_id + name_normalized + country
 * (composite key for dedup; §13). country NULL/empty treated as "Vietnam".
 */
int storage_geo_atlas_exists_normalized_country(storage_ctx_t *ctx, const char *tenant_id,
                                                const char *name_normalized, const char *country);

/** Legacy insert: tenant default, source user, verified, trust 1.0. */
int storage_geo_atlas_insert(storage_ctx_t *ctx, const char *name, const char *name_normalized,
                             const char *district, const char *axis, const char *category,
                             const char *city, const float *vector, size_t vector_dim,
                             const char *embed_model_id);

/**
 * Returns 1 if a seed row exists for this name_normalized (tenant or global) with different
 * non-empty district/city than proposed — do not replace seed; insert as pending (§7).
 */
int storage_geo_atlas_seed_conflict(storage_ctx_t *ctx, const char *tenant_id,
                                    const char *name_normalized, const char *district, const char *city);

/**
 * Returns 1 if at least one doc has cosine similarity >= threshold.
 * tenant_id NULL or "" = scan all tenants (legacy); else only that tenant's docs.
 */
int storage_geo_atlas_find_similar(storage_ctx_t *ctx, const char *tenant_id,
                                   const float *vector, size_t dim, double threshold);

/**
 * Redis L2 geo lane (`tenant|m4geo`): fast duplicate check before Mongo scan. Returns 1 if similar vector exists.
 * No-op if Redis not connected.
 */
int storage_geo_redis_find_similar(storage_ctx_t *ctx, const char *tenant_id,
                                   const float *vector, size_t dim, double threshold);

/**
 * After a successful Mongo insert, index embedding in Redis (no TTL eviction). doc_id must be unique per landmark.
 * Returns 0 on success or if Redis unavailable; -1 on invalid args.
 */
int storage_geo_redis_index_landmark(storage_ctx_t *ctx, const char *tenant_id, const char *doc_id,
                                    const float *vector, size_t dim, const char *name_label);

/** Prompt injection: only verified (or legacy docs without verification_status); excludes pending. */
size_t storage_geo_atlas_get_landmarks_for_prompt(storage_ctx_t *ctx, char *out, size_t size);

/**
 * One-shot legacy backfill for pre–§13 `geo_atlas` docs (see `.cursor/geo_leanring.md` §17).
 * Sets missing/empty `country` to Vietnam; rows with non-empty `merged_into` but status != merged
 * get `verification_status` merged, `trust_score` 1.0, `admin_action` merge.
 * Returns 0 on success; writes total modified count to *modified_out when non-NULL.
 * No-op (returns 0, *modified_out = 0) without Mongo.
 */
int storage_geo_atlas_migrate_legacy(storage_ctx_t *ctx, unsigned long long *modified_out);

/* --- Embed migration (async worker, .cursor/embed_migration.md) --- */
#define STORAGE_EMBED_MIG_INPUT_MAX 4096

typedef struct {
    char oid_hex[25];
    char input[STORAGE_EMBED_MIG_INPUT_MAX];
    char model_id[128];
    int has_model_id;
    int vector_dim;
} storage_embed_migration_turn_row_t;

/** Find up to `limit` bot.records for tenant needing metadata.embed_* backfill. Sets *out_n. Returns 0. */
int storage_embed_migration_fetch_turns_needing_provenance(storage_ctx_t *ctx, const char *tenant_id, int limit,
                                                           storage_embed_migration_turn_row_t *rows, int *out_n);

/** $set metadata.embed_schema, vector_dim, embed_family (and model_id if was empty → legacy kept via family). */
int storage_embed_migration_set_turn_provenance(storage_ctx_t *ctx, const storage_embed_migration_turn_row_t *row);

/** Rows where vector exists and metadata.model_id != target_model_id (or missing/empty). */
int storage_embed_migration_fetch_turns_model_mismatch(storage_ctx_t *ctx, const char *tenant_id,
                                                         const char *target_model_id, int limit,
                                                         storage_embed_migration_turn_row_t *rows, int *out_n);

/** $set vector[] + metadata fields for re-embed. */
int storage_embed_migration_update_turn_embedding(storage_ctx_t *ctx, const char *oid_hex, const float *vector,
                                                  size_t vector_dim, const char *model_id, const char *embed_family);

#endif /* M4_STORAGE_H */
