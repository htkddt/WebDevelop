#ifndef M4_VALIDATE_H
#define M4_VALIDATE_H

#include "engine.h"

/* Validate environment before boot (from temp.c). Checks Redis, ELK; optional internet. */
int validate_environment(const engine_config_t *config);

#endif /* M4_VALIDATE_H */
