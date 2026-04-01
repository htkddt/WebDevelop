#include "dispatcher.h"
#include <stdlib.h>
#include <string.h>

/* Simple policy: hybrid = round-robin style by tenant hash; otherwise use single backend. */
static unsigned hash_tenant(const char *tenant_id) {
    unsigned h = 0;
    if (!tenant_id) return 0;
    for (; *tenant_id; tenant_id++)
        h = 31 * h + (unsigned char)*tenant_id;
    return h;
}

backend_t dispatcher_select_backend(run_mode_t mode, const task_spec_t *spec) {
    if (!spec) return BACKEND_M4_NPU;
    switch (mode) {
        case MODE_M4_NPU:      return BACKEND_M4_NPU;
        case MODE_CUDA_REMOTE: return BACKEND_CUDA_REMOTE;
        case MODE_HYBRID:
            return (hash_tenant(spec->tenant_id) % 2 == 0) ? BACKEND_M4_NPU : BACKEND_CUDA_REMOTE;
        default: return BACKEND_M4_NPU;
    }
}

int dispatcher_dispatch(backend_t backend, const task_spec_t *spec) {
    (void)spec;
    if (backend == BACKEND_M4_NPU) {
        /* TODO: enqueue for M4 NPU or execute locally. */
        return 0;
    }
    if (backend == BACKEND_CUDA_REMOTE) {
        /* TODO: send to remote CUDA node. */
        return 0;
    }
    return -1;
}
