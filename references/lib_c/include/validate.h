#ifndef M4_VALIDATE_H
#define M4_VALIDATE_H

#include <stddef.h>
#include "engine.h"

/* Validate environment before boot (from temp.c). Checks Redis, ELK; optional internet. */
int validate_environment(const engine_config_t *config);

/**
 * NULL or empty = valid (optional). Non-empty must start with mongodb:// or mongodb+srv://.
 * Returns 0 if valid, -1 if invalid.
 */
int m4_validate_mongo_connection_uri(const char *uri);

/** NULL or empty = valid. Otherwise length must be <= max_len and NUL-terminated before max_len+1. */
int m4_validate_optional_path_string(const char *s, size_t max_len);

#endif /* M4_VALIDATE_H */
