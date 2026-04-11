#include "elk_sync_pool.h"
#include "m4_elk_log.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *index;
    char *doc_id;
    char *json_body;
} elk_job_t;

struct elk_sync_pool {
    elk_ctx_t *elk;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    elk_job_t *queue;
    size_t cap;
    size_t head;
    size_t count;
    int stop;
    int started;
    int n_started;
    pthread_t *threads;
    int n_workers;
};

static void elk_job_clear(elk_job_t *j) {
    free(j->index);
    free(j->doc_id);
    free(j->json_body);
    j->index = j->doc_id = j->json_body = NULL;
}

static int elk_job_push(elk_sync_pool_t *p, elk_job_t *j) {
    size_t tail = (p->head + p->count) % p->cap;
    p->queue[tail] = *j;
    memset(j, 0, sizeof(*j));
    p->count++;
    return 0;
}

static int elk_job_pop(elk_sync_pool_t *p, elk_job_t *out) {
    if (p->count == 0)
        return 0;
    *out = p->queue[p->head];
    memset(&p->queue[p->head], 0, sizeof(p->queue[p->head]));
    p->head = (p->head + 1) % p->cap;
    p->count--;
    return 1;
}

static void *elk_worker_main(void *arg) {
    elk_sync_pool_t *p = (elk_sync_pool_t *)arg;
    for (;;) {
        elk_job_t job;
        memset(&job, 0, sizeof(job));

        pthread_mutex_lock(&p->mu);
        while (p->count == 0 && !p->stop)
            pthread_cond_wait(&p->not_empty, &p->mu);
        if (p->stop && p->count == 0) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        if (!elk_job_pop(p, &job)) {
            pthread_mutex_unlock(&p->mu);
            continue;
        }
        pthread_cond_signal(&p->not_full);
        pthread_mutex_unlock(&p->mu);

        if (job.index && job.json_body && p->elk) {
            size_t blen = strlen(job.json_body);
            if (elk_index_json(p->elk, job.index, job.doc_id && job.doc_id[0] ? job.doc_id : NULL,
                               job.json_body, blen) != 0) {
                fprintf(stderr, "[ELK flow] index HTTP failed index=%s id=%s\n", job.index,
                        job.doc_id ? job.doc_id : "(auto)");
            } else if (m4_elk_log_level() >= 2) {
                fprintf(stderr, "[ELK flow] index HTTP ok index=%s id=%s body_bytes=%zu\n", job.index,
                        job.doc_id ? job.doc_id : "(auto)", blen);
            }
        }
        elk_job_clear(&job);
    }
    return NULL;
}

elk_sync_pool_t *elk_sync_pool_create(elk_ctx_t *elk, int n_workers, size_t queue_cap) {
    if (!elk || n_workers < 1 || queue_cap < 4)
        return NULL;
    elk_sync_pool_t *p = (elk_sync_pool_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->elk = elk;
    p->cap = queue_cap;
    p->queue = (elk_job_t *)calloc(queue_cap, sizeof(elk_job_t));
    p->threads = (pthread_t *)calloc((size_t)n_workers, sizeof(pthread_t));
    if (!p->queue || !p->threads) {
        free(p->queue);
        free(p->threads);
        free(p);
        return NULL;
    }
    p->n_workers = n_workers;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    pthread_cond_init(&p->not_full, NULL);
    return p;
}

void elk_sync_pool_start(elk_sync_pool_t *p) {
    if (!p || p->started)
        return;
    p->stop = 0;
    p->n_started = 0;
    for (int i = 0; i < p->n_workers; i++) {
        if (pthread_create(&p->threads[i], NULL, elk_worker_main, p) != 0) {
            fprintf(stderr, "[ELK pool] pthread_create failed\n");
            break;
        }
        p->n_started++;
    }
    p->started = (p->n_started > 0);
}

void elk_sync_pool_stop_destroy(elk_sync_pool_t *p) {
    if (!p)
        return;
    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_cond_broadcast(&p->not_full);
    pthread_mutex_unlock(&p->mu);

    if (p->started) {
        for (int i = 0; i < p->n_started; i++)
            pthread_join(p->threads[i], NULL);
    }

    for (size_t i = 0; i < p->cap; i++)
        elk_job_clear(&p->queue[i]);

    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->not_empty);
    pthread_cond_destroy(&p->not_full);
    free(p->queue);
    free(p->threads);
    free(p);
}

int elk_sync_pool_enqueue(elk_sync_pool_t *p, const char *index, const char *doc_id,
                          const char *json_body) {
    if (!p || !index || !json_body)
        return -1;

    elk_job_t job;
    memset(&job, 0, sizeof(job));
    job.index = strdup(index);
    job.doc_id = doc_id && doc_id[0] ? strdup(doc_id) : NULL;
    job.json_body = strdup(json_body);
    if (!job.index || !job.json_body || (doc_id && doc_id[0] && !job.doc_id)) {
        elk_job_clear(&job);
        return -1;
    }

    pthread_mutex_lock(&p->mu);
    while (p->count >= p->cap && !p->stop)
        pthread_cond_wait(&p->not_full, &p->mu);
    if (p->stop) {
        pthread_mutex_unlock(&p->mu);
        elk_job_clear(&job);
        return -1;
    }
    elk_job_push(p, &job);
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->mu);
    return 0;
}
