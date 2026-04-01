#include "tenant.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

struct tenant_ctx {
    char tenant_id[TENANT_ID_LEN];
};

tenant_ctx_t *tenant_create(const char *tenant_id) {
    if (!tenant_id || !tenant_validate_id(tenant_id))
        return NULL;
    tenant_ctx_t *ctx = (tenant_ctx_t *)malloc(sizeof(tenant_ctx_t));
    if (!ctx) return NULL;
    strncpy(ctx->tenant_id, tenant_id, TENANT_ID_LEN - 1);
    ctx->tenant_id[TENANT_ID_LEN - 1] = '\0';
    return ctx;
}

void tenant_destroy(tenant_ctx_t *ctx) {
    free(ctx);
}

bool tenant_validate_id(const char *tenant_id) {
    if (!tenant_id) return false;
    size_t n = 0;
    for (; tenant_id[n] && n < TENANT_ID_LEN; n++) {
        if (!isalnum((unsigned char)tenant_id[n]) && tenant_id[n] != '_' && tenant_id[n] != '-')
            return false;
    }
    return n > 0 && n < TENANT_ID_LEN;
}

const char *tenant_get_id(const tenant_ctx_t *ctx) {
    return ctx ? ctx->tenant_id : NULL;
}
