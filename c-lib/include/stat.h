#ifndef M4_STAT_H
#define M4_STAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "engine.h"

/**
 * Snapshot of all stats: memory, connection status for every mod, error/warning counts.
 * Use stat_get_snapshot() to fill; safe to read from any thread if stat_inc_* are the only writers.
 */
typedef struct stat_snapshot {
    /* Memory: library heap estimate (bytes). 0 = unknown. Set via stat_set_memory_bytes(). */
    uint64_t memory_bytes;
    /* Connection status: 0 = disconnected/disabled, 1 = connected/enabled */
    int mongo_connected;
    int redis_connected;
    int elk_enabled;   /* 1 if ELK is configured and should be used */
    int elk_connected; /* 1 if ELK ingest is reachable */
    /* Counts (monotonically increasing) */
    uint64_t error_count;
    uint64_t warning_count;
    /* Engine-style counts (optional; can mirror engine_get_stats) */
    uint64_t processed;
    uint64_t errors;
} stat_snapshot_t;

typedef struct stat_ctx stat_ctx_t;

stat_ctx_t *stat_create(void);
void stat_destroy(stat_ctx_t *ctx);

/** Set execution mode (for future use; counters only for now). */
void stat_set_execution_mode(stat_ctx_t *ctx, execution_mode_t mode);

/** Fill snapshot with current values. */
void stat_get_snapshot(stat_ctx_t *ctx, stat_snapshot_t *out);

/** Increment error/warning counters. */
void stat_inc_error(stat_ctx_t *ctx);
void stat_inc_warning(stat_ctx_t *ctx);

/** Set connection status (call after storage_connect or when you know the state). */
void stat_set_mongo_connected(stat_ctx_t *ctx, int connected);
void stat_set_redis_connected(stat_ctx_t *ctx, int connected);
void stat_set_elk_enabled(stat_ctx_t *ctx, int enabled);
void stat_set_elk_connected(stat_ctx_t *ctx, int connected);

/** Set memory estimate (e.g. from allocator or heap usage). */
void stat_set_memory_bytes(stat_ctx_t *ctx, uint64_t bytes);

/** Set engine-style processed/errors (optional; can be updated from engine_get_stats). */
void stat_set_processed(stat_ctx_t *ctx, uint64_t n);
void stat_set_errors(stat_ctx_t *ctx, uint64_t n);

#endif /* M4_STAT_H */
