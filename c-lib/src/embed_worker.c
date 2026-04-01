#include "embed_worker.h"
#include "engine.h"
#include "embed.h"
#include "ollama.h"
#include "storage.h"
#include "tenant.h"
#include "vector_generate.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define EMBED_MIG_QUEUE_CAP 64
#define EMBED_MIG_TENANT_BUF 64

struct embed_worker {
    struct engine *engine;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cond;
    int stop;
    int started;
    uint64_t job_seq;
    char tenants[EMBED_MIG_QUEUE_CAP][EMBED_MIG_TENANT_BUF];
    int kinds[EMBED_MIG_QUEUE_CAP];
    size_t head;
    size_t count;
};

#ifdef USE_MONGOC
static int env_u32(const char *name, unsigned def, unsigned max_val) {
    const char *e = getenv(name);
    if (!e || !e[0]) return (int)(def > max_val ? max_val : def);
    char *end = NULL;
    unsigned long v = strtoul(e, &end, 10);
    if (end == e || v == 0 || v > max_val) return (int)(def > max_val ? max_val : def);
    return (int)v;
}

static int env_ms(void) {
    int v = env_u32("M4_EMBED_MIGRATION_INTERVAL_MS", 100, 60000);
    return v < 1 ? 100 : v;
}

static int env_batch(void) {
    int v = env_u32("M4_EMBED_MIGRATION_BATCH", 32, 500);
    return v < 1 ? 32 : v;
}

static void resolve_target_model_id(engine_t *eng, char *buf, size_t sz) {
    if (!buf || sz == 0) return;
    buf[0] = '\0';
    const engine_config_t *c = engine_get_config(eng);
    if (!c) {
        strncpy(buf, VECTOR_GEN_MODEL_ID, sz - 1);
        buf[sz - 1] = '\0';
        return;
    }
    if (c->vector_gen_backend != M4_EMBED_BACKEND_OLLAMA) {
        snprintf(buf, sz, "%s", VECTOR_GEN_MODEL_ID);
        return;
    }
    const char *pref = c->vector_ollama_model;
    if (pref && !pref[0]) pref = NULL;
    if (ollama_resolve_embed_model(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, pref, buf, sz) != 0 || buf[0] == '\0') {
        snprintf(buf, sz, "%s", OLLAMA_DEFAULT_EMBED_MODEL);
    }
}
#endif

static int job_coalesce_pending(embed_worker_t *w, const char *tenant_id, int job_kind) {
    for (size_t i = 0; i < w->count; i++) {
        size_t idx = (w->head + i) % EMBED_MIG_QUEUE_CAP;
        if (w->kinds[idx] == job_kind && strcmp(w->tenants[idx], tenant_id) == 0)
            return 1;
    }
    return 0;
}

static int job_pop(embed_worker_t *w, char *tenant_out, size_t tenant_sz, int *kind_out) {
    if (w->count == 0) return 0;
    snprintf(tenant_out, tenant_sz, "%s", w->tenants[w->head]);
    *kind_out = w->kinds[w->head];
    w->head = (w->head + 1) % EMBED_MIG_QUEUE_CAP;
    w->count--;
    return 1;
}

static void *embed_worker_thread(void *arg) {
    embed_worker_t *w = (embed_worker_t *)arg;
#ifdef USE_MONGOC
    int batch = env_batch();
    int interval_ms = env_ms();
    storage_embed_migration_turn_row_t *rows =
        (storage_embed_migration_turn_row_t *)malloc((size_t)batch * sizeof(*rows));
#else
    storage_embed_migration_turn_row_t *rows = NULL;
#endif

    for (;;) {
        char tenant[EMBED_MIG_TENANT_BUF];
        int kind = EMBED_MIG_JOB_PROVENANCE;
        uint64_t job_id = 0;

        pthread_mutex_lock(&w->mu);
        while (w->count == 0 && !w->stop)
            pthread_cond_wait(&w->cond, &w->mu);
        if (w->stop && w->count == 0) {
            pthread_mutex_unlock(&w->mu);
            break;
        }
        if (!job_pop(w, tenant, sizeof(tenant), &kind)) {
            pthread_mutex_unlock(&w->mu);
            continue;
        }
        job_id = ++w->job_seq;
        pthread_mutex_unlock(&w->mu);

        engine_t *eng = w->engine;

#ifndef USE_MONGOC
        fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=skip reason=USE_MONGOC_0\n",
                (unsigned long long)job_id, tenant);
        (void)kind;
        (void)eng;
        continue;
#else
        storage_ctx_t *st = eng ? engine_get_storage(eng) : NULL;
        if (!rows) {
            fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=error reason=alloc\n",
                    (unsigned long long)job_id, tenant);
            continue;
        }
        if (!st || !storage_mongo_connected(st)) {
            fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=skip reason=no_mongo\n",
                    (unsigned long long)job_id, tenant);
            continue;
        }

        if (kind == EMBED_MIG_JOB_PROVENANCE) {
            fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=scan kind=provenance\n",
                    (unsigned long long)job_id, tenant);
            for (;;) {
                pthread_mutex_lock(&w->mu);
                if (w->stop) {
                    pthread_mutex_unlock(&w->mu);
                    break;
                }
                pthread_mutex_unlock(&w->mu);
                int n = 0;
                if (storage_embed_migration_fetch_turns_needing_provenance(st, tenant, batch, rows, &n) != 0) {
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=error where=fetch_provenance\n",
                            (unsigned long long)job_id, tenant);
                    break;
                }
                if (n == 0) {
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=scan done=provenance\n",
                            (unsigned long long)job_id, tenant);
                    break;
                }
                for (int i = 0; i < n; i++) {
                    pthread_mutex_lock(&w->mu);
                    int stopped = w->stop;
                    pthread_mutex_unlock(&w->mu);
                    if (stopped) break;
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=mongo_patch oid=%s\n",
                            (unsigned long long)job_id, tenant, rows[i].oid_hex);
                    if (storage_embed_migration_set_turn_provenance(st, &rows[i]) != 0)
                        fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=error oid=%s where=set_provenance\n",
                                (unsigned long long)job_id, tenant, rows[i].oid_hex);
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=redis_skip oid=%s\n",
                            (unsigned long long)job_id, tenant, rows[i].oid_hex);
                    if (interval_ms > 0)
                        usleep((useconds_t)interval_ms * 1000u);
                }
            }
        } else {
            char target_mid[128];
            resolve_target_model_id(eng, target_mid, sizeof(target_mid));
            fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=scan kind=reembed target_model=%s\n",
                    (unsigned long long)job_id, tenant, target_mid);
            for (;;) {
                pthread_mutex_lock(&w->mu);
                if (w->stop) {
                    pthread_mutex_unlock(&w->mu);
                    break;
                }
                pthread_mutex_unlock(&w->mu);
                int n = 0;
                if (storage_embed_migration_fetch_turns_model_mismatch(st, tenant, target_mid, batch, rows, &n) != 0) {
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=error where=fetch_mismatch\n",
                            (unsigned long long)job_id, tenant);
                    break;
                }
                if (n == 0) {
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=scan done=reembed\n",
                            (unsigned long long)job_id, tenant);
                    break;
                }
                float vec[OLLAMA_EMBED_MAX_DIM];
                for (int i = 0; i < n; i++) {
                    pthread_mutex_lock(&w->mu);
                    int stopped = w->stop;
                    pthread_mutex_unlock(&w->mu);
                    if (stopped) break;
                    size_t out_dim = 0;
                    char mid_out[128];
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=embed oid=%s\n",
                            (unsigned long long)job_id, tenant, rows[i].oid_hex);
                    if (m4_embed_for_engine(eng, rows[i].input, vec, OLLAMA_EMBED_MAX_DIM, &out_dim, mid_out,
                                            sizeof(mid_out))
                        != 0
                        || out_dim == 0) {
                        fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=error oid=%s where=embed\n",
                                (unsigned long long)job_id, tenant, rows[i].oid_hex);
                        if (interval_ms > 0)
                            usleep((useconds_t)interval_ms * 1000u);
                        continue;
                    }
                    const char *family = (strcmp(mid_out, VECTOR_GEN_MODEL_ID) == 0) ? "custom" : "ollama";
                    if (storage_embed_migration_update_turn_embedding(st, rows[i].oid_hex, vec, out_dim, mid_out,
                                                                      family)
                        != 0)
                        fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=error oid=%s where=mongo_update\n",
                                (unsigned long long)job_id, tenant, rows[i].oid_hex);
                    fprintf(stderr, "[EMBED_MIGRATION] job=%llu tenant=%s phase=redis_skip oid=%s\n",
                            (unsigned long long)job_id, tenant, rows[i].oid_hex);
                    if (interval_ms > 0)
                        usleep((useconds_t)interval_ms * 1000u);
                }
            }
        }
#endif
    }

    free(rows);
    return NULL;
}

embed_worker_t *embed_worker_create(engine_t *engine) {
    if (!engine) return NULL;
    embed_worker_t *w = (embed_worker_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->engine = engine;
    if (pthread_mutex_init(&w->mu, NULL) != 0) {
        free(w);
        return NULL;
    }
    if (pthread_cond_init(&w->cond, NULL) != 0) {
        pthread_mutex_destroy(&w->mu);
        free(w);
        return NULL;
    }
    return w;
}

void embed_worker_destroy(embed_worker_t *w) {
    if (!w) return;
    embed_worker_stop(w);
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->mu);
    free(w);
}

int embed_worker_start(embed_worker_t *w) {
    if (!w) return -1;
    pthread_mutex_lock(&w->mu);
    if (w->started) {
        pthread_mutex_unlock(&w->mu);
        return 0;
    }
    w->stop = 0;
    if (pthread_create(&w->thread, NULL, embed_worker_thread, w) != 0) {
        pthread_mutex_unlock(&w->mu);
        return -1;
    }
    w->started = 1;
    pthread_mutex_unlock(&w->mu);
    return 0;
}

void embed_worker_stop(embed_worker_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    if (!w->started) {
        pthread_mutex_unlock(&w->mu);
        return;
    }
    w->stop = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    pthread_mutex_lock(&w->mu);
    w->started = 0;
    w->stop = 0;
    pthread_mutex_unlock(&w->mu);
}

int embed_worker_enqueue(embed_worker_t *w, const char *tenant_id, int job_kind) {
    if (!w || (job_kind != EMBED_MIG_JOB_PROVENANCE && job_kind != EMBED_MIG_JOB_REEMBED)) return -1;
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : "default";
    if (!tenant_validate_id(tid)) return -1;

    pthread_mutex_lock(&w->mu);
    if (job_coalesce_pending(w, tid, job_kind)) {
        pthread_mutex_unlock(&w->mu);
        return 0;
    }
    if (w->count >= EMBED_MIG_QUEUE_CAP) {
        pthread_mutex_unlock(&w->mu);
        fprintf(stderr, "[EMBED_MIGRATION] enqueue failed: queue full (tenant=%s kind=%d)\n", tid, job_kind);
        return -1;
    }
    size_t tail = (w->head + w->count) % EMBED_MIG_QUEUE_CAP;
    snprintf(w->tenants[tail], EMBED_MIG_TENANT_BUF, "%s", tid);
    w->kinds[tail] = job_kind;
    w->count++;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mu);
    return 0;
}
