#ifndef M4_TENANT_H
#define M4_TENANT_H

#include <stddef.h>
#include <stdbool.h>

#define TENANT_ID_LEN 64

typedef struct tenant_ctx tenant_ctx_t;

/* Multi-tenant: strict tenant_id isolation for 1B records. */
tenant_ctx_t *tenant_create(const char *tenant_id);
void tenant_destroy(tenant_ctx_t *ctx);

bool tenant_validate_id(const char *tenant_id);
const char *tenant_get_id(const tenant_ctx_t *ctx);

#endif /* M4_TENANT_H */
