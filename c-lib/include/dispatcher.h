#ifndef M4_DISPATCHER_H
#define M4_DISPATCHER_H

#include <stddef.h>
#include "engine.h"

/* Load balancer: dispatch tasks between M4 NPU and remote CUDA nodes. */
typedef enum {
    BACKEND_M4_NPU,
    BACKEND_CUDA_REMOTE
} backend_t;

typedef struct task_spec {
    const char *tenant_id;
    const void *payload;
    size_t payload_size;
    int priority;
} task_spec_t;

backend_t dispatcher_select_backend(run_mode_t mode, const task_spec_t *spec);
int dispatcher_dispatch(backend_t backend, const task_spec_t *spec);

#endif /* M4_DISPATCHER_H */
