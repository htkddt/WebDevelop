#ifndef M4_ELK_SYNC_POOL_H
#define M4_ELK_SYNC_POOL_H

#include "elk.h"
#include <stddef.h>

typedef struct elk_sync_pool elk_sync_pool_t;

elk_sync_pool_t *elk_sync_pool_create(elk_ctx_t *elk, int n_workers, size_t queue_cap);
void elk_sync_pool_start(elk_sync_pool_t *pool);
void elk_sync_pool_stop_destroy(elk_sync_pool_t *pool);

/**
 * Enqueue index job (strdup index, doc_id, json_body). Blocks if queue full (with cond).
 * Returns 0 on success, -1 on OOM or stopped pool.
 */
int elk_sync_pool_enqueue(elk_sync_pool_t *pool, const char *index, const char *doc_id,
                          const char *json_body);

#endif
