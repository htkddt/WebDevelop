#include "engine.h"
#include "embed_worker.h"
#include "geo_authority.h"
#include "geo_learning.h"
#include "smart_topic.h"
#include "tenant.h"
#include "dispatcher.h"
#include "storage.h"
#include "vector_generate.h"
#include "api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct engine {
    engine_config_t config;
    storage_ctx_t *storage;
    embed_worker_t *embed_worker;
    uint64_t processed;
    uint64_t errors;
};

engine_t *engine_create(const engine_config_t *config) {
    if (!config) return NULL;
    engine_t *e = (engine_t *)malloc(sizeof(engine_t));
    if (!e) return NULL;
    memcpy(&e->config, config, sizeof(engine_config_t));
    e->storage = storage_create(
        config->mongo_uri, config->redis_host, config->redis_port,
        config->es_host, config->es_port, config->shared_collection_mongo_uri,
        config->shared_collection_json_path, config->shared_collection_backfill_db);
    e->processed = 0;
    e->errors = 0;
    e->embed_worker = NULL;
    if (!e->storage) { free(e); return NULL; }
    if (config->execution_mode != MODE_ONLY_MEMORY) {
        e->embed_worker = embed_worker_create(e);
        if (!e->embed_worker) {
            storage_destroy(e->storage);
            free(e);
            return NULL;
        }
    }
    return e;
}

void engine_destroy(engine_t *e) {
    if (!e) return;
    if (e->embed_worker) {
        embed_worker_destroy(e->embed_worker);
        e->embed_worker = NULL;
    }
    if (e->config.geo_authority_enabled)
        geo_authority_shutdown();
    if (e->config.geo_learning_enabled)
        geo_learning_shutdown();
    storage_destroy(e->storage);
    /* Cleanup synonym table */
    m4_synonym_set_global(NULL);
    /* Free strdup'd config strings (owned by engine since fill_default_config) */
    free((void *)e->config.mongo_uri);
    free((void *)e->config.redis_host);
    free((void *)e->config.es_host);
    free((void *)e->config.vector_ollama_model);
    free((void *)e->config.shared_collection_mongo_uri);
    free((void *)e->config.shared_collection_json_path);
    free((void *)e->config.shared_collection_backfill_db);
    free(e);
}

int engine_init(engine_t *e) {
    if (!e) return -1;
    if (storage_connect(e->storage) != 0) return -1;
    {
        const char *m = getenv("GEO_ATLAS_MIGRATE_LEGACY");
        if (m && m[0] == '1' && m[1] == '\0') {
            unsigned long long n = 0;
            if (storage_geo_atlas_migrate_legacy(e->storage, &n) != 0)
                fprintf(stderr, "[ENGINE] GEO_ATLAS_MIGRATE_LEGACY=1: migrate failed (see stderr)\n");
            else
                fprintf(stderr, "[ENGINE] GEO_ATLAS_MIGRATE_LEGACY=1: updated %llu geo_atlas doc(s)\n", n);
        }
    }
    /* Start smart_topic when option is set and enabled. */
    if (e->config.smart_topic_opts && e->config.smart_topic_opts->enable) {
        if (initial_smart_topic(e->config.smart_topic_opts, e->storage) != 0)
            return -1;
    }
    if (e->config.geo_authority_enabled) {
        if (geo_authority_init() != 0)
            fprintf(stderr, "[ENGINE] geo_authority cache failed — continuing without L1 authority\n");
    }
    /* Initialize synonym table for vector_generate_custom */
    {
        m4_synonym_table_t *syn = m4_synonym_create();
        if (syn) {
            m4_synonym_add_builtins(syn);
            m4_synonym_set_global(syn);
            fprintf(stderr, "[ENGINE] synonym table initialized (builtins loaded)\n");
        }
    }
    /* Vector dimension migration: log warning if changing from v1 (384) to v2 (768).
     * Redis L2 vectors with old dimensions will not match — they need to be re-indexed.
     * Mongo vectors will be re-embedded by embed_migration_worker if autostart is set. */
    if (e->config.vector_gen_backend == API_VECTOR_GEN_CUSTOM) {
        fprintf(stderr, "[ENGINE] vector_generate v2: dim=%d model=%s\n", VECTOR_GEN_CUSTOM_DIM, VECTOR_GEN_MODEL_ID);
        if (VECTOR_GEN_CUSTOM_DIM != 384)
            fprintf(stderr, "[ENGINE] WARNING: dimension changed from 384 → %d. "
                    "Set embed_migration_autostart=1 to re-embed existing Mongo vectors. "
                    "Redis L2 cache will self-heal as new vectors are written.\n", VECTOR_GEN_CUSTOM_DIM);
    }
    if (e->config.geo_learning_enabled) {
        if (geo_learning_init(e->storage, e->config.vector_gen_backend) != 0)
            fprintf(stderr, "[ENGINE] geo_learning worker failed to start — continuing without background geo (optional)\n");
    }
    if (e->embed_worker && embed_worker_start(e->embed_worker) != 0) {
        fprintf(stderr, "[ENGINE] embed migration worker failed to start\n");
        embed_worker_destroy(e->embed_worker);
        e->embed_worker = NULL;
    }
    if (e->embed_worker) {
        int autostart = 0;
        if (e->config.embed_migration_autostart)
            autostart = 1;
        else {
            const char *ev = getenv("M4_EMBED_MIGRATION_ON_START");
            if (ev && ev[0] == '1' && ev[1] == '\0')
                autostart = 1;
            else if (ev && (strcasecmp(ev, "true") == 0 || strcasecmp(ev, "yes") == 0 || strcasecmp(ev, "on") == 0))
                autostart = 1;
        }
        if (autostart)
            embed_worker_enqueue(e->embed_worker, "default", EMBED_MIG_JOB_PROVENANCE);
    }
    return 0;
}

int engine_embed_migration_enqueue(engine_t *e, const char *tenant_id, unsigned flags) {
    if (!e || !e->embed_worker || flags == 0) return -1;
    int ok = 0;
    if (flags & EMBED_MIG_FLAG_PROVENANCE) {
        if (embed_worker_enqueue(e->embed_worker, tenant_id, EMBED_MIG_JOB_PROVENANCE) != 0)
            return -1;
        ok = 1;
    }
    if (flags & EMBED_MIG_FLAG_REEMBED) {
        if (embed_worker_enqueue(e->embed_worker, tenant_id, EMBED_MIG_JOB_REEMBED) != 0)
            return -1;
        ok = 1;
    }
    return ok ? 0 : -1;
}

storage_ctx_t *engine_get_storage(engine_t *e) {
    return e ? e->storage : NULL;
}

int engine_vector_search_enabled(engine_t *e) {
    return (e && e->config.vector_search_enabled) ? 1 : 0;
}

int engine_process_batch(engine_t *e, const char *tenant_id, const void *records, size_t count) {
    if (!e || !tenant_id || !records) return -1;
    if (!tenant_validate_id(tenant_id)) return -1;

    task_spec_t spec = {
        .tenant_id = tenant_id,
        .payload = records,
        .payload_size = count * sizeof(char),
        .priority = 0
    };
    backend_t backend = dispatcher_select_backend(e->config.mode, &spec);
    if (dispatcher_dispatch(backend, &spec) != 0) {
        e->errors += count;
        return -1;
    }

    if (storage_upsert_batch(e->storage, tenant_id, records, count) != 0) {
        e->errors += count;
        return -1;
    }
    e->processed += count;
    return 0;
}

int engine_get_stats(engine_t *e, uint64_t *processed, uint64_t *errors) {
    if (!e) return -1;
    if (processed) *processed = e->processed;
    if (errors) *errors = e->errors;
    return 0;
}

void engine_inc_processed(engine_t *e, uint64_t n) {
    if (e && n > 0) e->processed += n;
}

const engine_config_t *engine_get_config(const engine_t *e) {
    return e ? &e->config : NULL;
}

int engine_append_chat(engine_t *e, const char *tenant_id, const char *role,
                      const char *content, const char *timestamp) {
    if (!e) return -1;
    if (e->config.execution_mode == MODE_ONLY_MEMORY)
        return 0;  /* rule §7 A: no external DB, volatile only */
    if (!e->storage) return -1;
    return storage_append_chat(e->storage, tenant_id, role, content, timestamp);
}

int engine_append_turn(engine_t *e, const char *tenant_id, const char *user_id,
                      const char *input, const char *assistant, const char *timestamp,
                      const float *vector, size_t vector_dim,
                      const char *lang, double lang_score,
                      const char *embed_model_id,
                      const char *llm_model_id,
                      const char *temp_message_id,
                      int has_logic_conflict) {
    if (!e) return -1;
    if (e->config.execution_mode == MODE_ONLY_MEMORY)
        return 0;
    if (!e->storage) return -1;
    int ret = storage_append_turn(e->storage, tenant_id, user_id, input, assistant, timestamp,
                                  vector, vector_dim, lang, lang_score, embed_model_id, llm_model_id,
                                  temp_message_id, has_logic_conflict);
    /* Geo extract prompt needs both sides; empty assistant (stream parse miss, etc.) cannot yield
     * useful extraction — skip queue to avoid extra Ollama calls and noisy stderr. */
    if (ret == 0 && e->config.geo_learning_enabled && assistant && assistant[0])
        geo_learning_enqueue_turn(tenant_id, user_id, input, assistant, timestamp);
    return ret;
}
