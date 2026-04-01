#include "debug_monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

struct debug_monitor {
    char pipe_path[256];
    int pipe_fd;
    int started;
};

debug_monitor_t *debug_monitor_create(const char *pipe_path) {
    debug_monitor_t *dm = (debug_monitor_t *)malloc(sizeof(debug_monitor_t));
    if (!dm) return NULL;
    memset(dm, 0, sizeof(*dm));
    if (pipe_path)
        strncpy(dm->pipe_path, pipe_path, sizeof(dm->pipe_path) - 1);
    else
        strncpy(dm->pipe_path, DEBUG_PIPE_PATH, sizeof(dm->pipe_path) - 1);
    dm->pipe_fd = -1;
    return dm;
}

void debug_monitor_destroy(debug_monitor_t *dm) {
    if (!dm) return;
    debug_monitor_stop(dm);
    free(dm);
}

int debug_monitor_start(debug_monitor_t *dm) {
    if (!dm) return -1;
    dm->pipe_fd = open(dm->pipe_path, O_WRONLY);
    if (dm->pipe_fd < 0) {
        /* Pipe may not exist; monitor listener can create it. Non-fatal. */
        return 0;
    }
    dm->started = 1;
    return 0;
}

void debug_monitor_stop(debug_monitor_t *dm) {
    if (!dm) return;
    if (dm->pipe_fd >= 0) {
        close(dm->pipe_fd);
        dm->pipe_fd = -1;
    }
    dm->started = 0;
}

void debug_monitor_log(debug_monitor_t *dm, const char *fmt, ...) {
    if (!dm || dm->pipe_fd < 0) return;
    char buf[DEBUG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)write(dm->pipe_fd, buf, strlen(buf));
}

void debug_monitor_log_stats(debug_monitor_t *dm, uint64_t processed, uint64_t errors) {
    if (!dm || dm->pipe_fd < 0) return;
    char buf[DEBUG_MSG_MAX];
    snprintf(buf, sizeof(buf), "STATS processed=%lu errors=%lu\n", (unsigned long)processed, (unsigned long)errors);
    (void)write(dm->pipe_fd, buf, strlen(buf));
}
