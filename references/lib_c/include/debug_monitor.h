#ifndef M4_DEBUG_MONITOR_H
#define M4_DEBUG_MONITOR_H

#include <stdint.h>

/* Pipe-based debug monitor (separate process or thread). */
#define DEBUG_PIPE_PATH "/tmp/m4_ai_debug.pipe"
#define DEBUG_MSG_MAX 256

typedef struct debug_monitor debug_monitor_t;

debug_monitor_t *debug_monitor_create(const char *pipe_path);
void debug_monitor_destroy(debug_monitor_t *dm);

int debug_monitor_start(debug_monitor_t *dm);
void debug_monitor_stop(debug_monitor_t *dm);

void debug_monitor_log(debug_monitor_t *dm, const char *fmt, ...);
void debug_monitor_log_stats(debug_monitor_t *dm, uint64_t processed, uint64_t errors);

#endif /* M4_DEBUG_MONITOR_H */
