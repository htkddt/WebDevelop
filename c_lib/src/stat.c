#include "stat.h"
#include <stdlib.h>
#include <string.h>

struct stat_ctx {
    execution_mode_t execution_mode;
    uint64_t memory_bytes;
    int mongo_connected;
    int redis_connected;
    int elk_enabled;
    int elk_connected;
    uint64_t error_count;
    uint64_t warning_count;
    uint64_t processed;
    uint64_t errors;
};

stat_ctx_t *stat_create(void) {
    stat_ctx_t *ctx = (stat_ctx_t *)malloc(sizeof(stat_ctx_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->execution_mode = MODE_ONLY_MEMORY; /* safe default: no Mongo/ELK for logs */
    return ctx;
}

void stat_set_execution_mode(stat_ctx_t *ctx, execution_mode_t mode) {
    if (ctx) ctx->execution_mode = mode;
}

void stat_destroy(stat_ctx_t *ctx) {
    free(ctx);
}

void stat_get_snapshot(stat_ctx_t *ctx, stat_snapshot_t *out) {
    if (!ctx || !out) return;
    out->memory_bytes    = ctx->memory_bytes;
    out->mongo_connected = ctx->mongo_connected;
    out->redis_connected = ctx->redis_connected;
    out->elk_enabled     = ctx->elk_enabled;
    out->elk_connected   = ctx->elk_connected;
    out->error_count     = ctx->error_count;
    out->warning_count   = ctx->warning_count;
    out->processed       = ctx->processed;
    out->errors          = ctx->errors;
}

void stat_inc_error(stat_ctx_t *ctx) {
    if (ctx) ctx->error_count++;
}

void stat_inc_warning(stat_ctx_t *ctx) {
    if (ctx) ctx->warning_count++;
}

void stat_set_mongo_connected(stat_ctx_t *ctx, int connected) {
    if (ctx) ctx->mongo_connected = connected ? 1 : 0;
}

void stat_set_redis_connected(stat_ctx_t *ctx, int connected) {
    if (ctx) ctx->redis_connected = connected ? 1 : 0;
}

void stat_set_elk_enabled(stat_ctx_t *ctx, int enabled) {
    if (ctx) ctx->elk_enabled = enabled ? 1 : 0;
}

void stat_set_elk_connected(stat_ctx_t *ctx, int connected) {
    if (ctx) ctx->elk_connected = connected ? 1 : 0;
}

void stat_set_memory_bytes(stat_ctx_t *ctx, uint64_t bytes) {
    if (ctx) ctx->memory_bytes = bytes;
}

void stat_set_processed(stat_ctx_t *ctx, uint64_t n) {
    if (ctx) ctx->processed = n;
}

void stat_set_errors(stat_ctx_t *ctx, uint64_t n) {
    if (ctx) ctx->errors = n;
}

