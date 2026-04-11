#ifndef M4_EMBED_WORKER_H
#define M4_EMBED_WORKER_H

#include <stddef.h>

struct engine;
typedef struct embed_worker embed_worker_t;

/** Job kinds for the single-consumer embed migration queue (.cursor/embed_migration.md). */
#define EMBED_MIG_JOB_PROVENANCE 0
#define EMBED_MIG_JOB_REEMBED    1
/** Bit flags for engine_embed_migration_enqueue / api_embed_migration_enqueue (must match api.h). */
#define EMBED_MIG_FLAG_PROVENANCE 1u
#define EMBED_MIG_FLAG_REEMBED    2u

embed_worker_t *embed_worker_create(struct engine *engine);
void embed_worker_destroy(embed_worker_t *w);
/** Start the background thread (idempotent). */
int embed_worker_start(embed_worker_t *w);
/** Join the thread; safe to call multiple times. */
void embed_worker_stop(embed_worker_t *w);
/**
 * Enqueue migration work for tenant_id. Coalesces: duplicate pending tenant+kind is ignored (returns 0).
 * Returns 0 on success, -1 on invalid args or queue full.
 */
int embed_worker_enqueue(embed_worker_t *w, const char *tenant_id, int job_kind);

#endif /* M4_EMBED_WORKER_H */
