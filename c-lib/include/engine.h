#ifndef M4_ENGINE_H
#define M4_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define ENGINE_VERSION "1.0.0"
#define TENANT_ID_MAX 64
#define RECORD_ID_MAX 32

/* Compute backend: M4 NPU vs remote CUDA (for inference). */
typedef enum {
    MODE_M4_NPU,
    MODE_CUDA_REMOTE,
    MODE_HYBRID
} run_mode_t;

/* Execution / storage mode (rule.md §7 – 4 types for <2s latency path). */
typedef enum {
    MODE_ONLY_MEMORY,     /* A: RAM/mmap only, no Mongo/Redis. Nano-second latency. */
    MODE_ONLY_MONGO,      /* B: MongoDB Atlas only, batch 100–500, mongoc_bulk_operation_t. */
    MODE_MONGO_REDIS,     /* C: Mongo + Redis. Reads from Redis Hash, writes async Mongo, Change Streams sync. */
    MODE_MONGO_REDIS_ELK  /* D: Mongo + Redis + ELK. pipeline=auto_lang_processor for localization. */
} execution_mode_t;  /* rule.md §7 A/B/C/D */

struct smart_topic_options;
struct model_switch_options;

typedef struct engine_config {
    run_mode_t mode;
    execution_mode_t execution_mode;  /* A/B/C/D from rule.md */
    const char *mongo_uri;
    const char *redis_host;
    int redis_port;
    const char *es_host;
    int es_port;
    size_t batch_size;
    bool vector_search_enabled;
    bool debug_mode;  /* from temp.c EngineConfig */
    /** Optional: when set and enable==true, initial_smart_topic() is called at engine_init. */
    const struct smart_topic_options *smart_topic_opts;
    /** Optional: per-lane Ollama model + inject text; see model_switch.h / .cursor/model_switch.md */
    const struct model_switch_options *model_switch_opts;
    /** When true, geo_learning_init() at engine_init and enqueue turn after append_turn (.cursor/geo_leanring.md). */
    bool geo_learning_enabled;
    /** When true, geo_authority L1 cache + optional prompt hint + response audit (.cursor/auth_geo.md). */
    bool geo_authority_enabled;
    /**
     * RAG / turn-storage embedding: 0 = built-in hash vector (no Ollama, see vector_generate.h);
     * 1 = Ollama POST /api/embed. Default 0. Mirrors api_options_t.vector_gen_backend.
     */
    int vector_gen_backend;
    /** When vector_gen_backend==1: preferred Ollama embed model; NULL/empty = same resolution as ollama_embeddings(NULL). */
    const char *vector_ollama_model;
    /** When true with Mongo modes: engine_init may enqueue provenance migration (also see M4_EMBED_MIGRATION_ON_START). Default off. */
    bool embed_migration_autostart;
} engine_config_t;

typedef struct engine engine_t;

engine_t *engine_create(const engine_config_t *config);
void engine_destroy(engine_t *e);

int engine_init(engine_t *e);
int engine_process_batch(engine_t *e, const char *tenant_id, const void *records, size_t count);
int engine_get_stats(engine_t *e, uint64_t *processed, uint64_t *errors);
/** Increment processed counter (e.g. one successful api_chat / api_query turn). */
void engine_inc_processed(engine_t *e, uint64_t n);
/** Read-only config (pointers inside remain valid for engine lifetime). */
const engine_config_t *engine_get_config(const engine_t *e);
int engine_append_chat(engine_t *e, const char *tenant_id, const char *role,
                      const char *content, const char *timestamp);
/** Append one turn (input + assistant) as single document — new shape per .cursor/mongo.md §0. vector/lang/score from Phase 1 when set. embed_model_id: model that generated vector (NULL → ""). */
int engine_append_turn(engine_t *e, const char *tenant_id, const char *user_id,
                      const char *input, const char *assistant, const char *timestamp,
                      const float *vector, size_t vector_dim,
                      const char *lang, double lang_score,
                      const char *embed_model_id,
                      const char *temp_message_id,
                      int has_logic_conflict);

/** For api/stat: get storage (e.g. for storage_set_ai_logs). */
struct storage_ctx;
struct storage_ctx *engine_get_storage(engine_t *e);

/** 1 if RAG (vector search) is enabled for this engine; 0 otherwise. */
int engine_vector_search_enabled(engine_t *e);

/**
 * Enqueue embed migration jobs (bounded queue, single worker).
 * flags: EMBED_MIG_FLAG_PROVENANCE / EMBED_MIG_FLAG_REEMBED (embed_worker.h), mirrored as API_EMBED_MIG_FLAG_* in api.h.
 * Returns 0 on success, -1 on error or no worker.
 */
int engine_embed_migration_enqueue(engine_t *e, const char *tenant_id, unsigned flags);

#endif /* M4_ENGINE_H */
