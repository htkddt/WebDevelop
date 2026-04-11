/*
 * ELK pipeline logging (stderr). Env: M4_ELK_LOG
 *   unset / empty               → flow (same as 1)
 *   0 / false / no / off        → off
 *   1 / true / yes              → flow milestones (init, pool, per-collection enqueue counts)
 *   2 / verbose                 → also log each successful index POST (noisy for large backfills)
 */
#ifndef M4_ELK_LOG_H
#define M4_ELK_LOG_H

#include <stdlib.h>
#include <strings.h>

static inline int m4_elk_log_level(void) {
    const char *e = getenv("M4_ELK_LOG");
    if (!e || !e[0])
        return 1;
    if (e[0] == '0' && e[1] == '\0')
        return 0;
    if (strcasecmp(e, "false") == 0 || strcasecmp(e, "no") == 0 || strcasecmp(e, "off") == 0)
        return 0;
    if (e[0] == '2' && e[1] == '\0')
        return 2;
    if (strcasecmp(e, "verbose") == 0)
        return 2;
    if (e[0] == '1' && e[1] == '\0')
        return 1;
    if (strcasecmp(e, "true") == 0 || strcasecmp(e, "yes") == 0)
        return 1;
    return 0;
}

#endif /* M4_ELK_LOG_H */
