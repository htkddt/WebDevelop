/*
 * Validate environment (migrated from temp.c).
 * Uses nc for port checks so we don't require libcurl in default build.
 */
#include "validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_port(const char *host, int port) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "nc -z %s %d > /dev/null 2>&1", host ? host : "127.0.0.1", port);
    return system(cmd);
}

int validate_environment(const engine_config_t *config) {
    if (!config) return -1;

    printf("\033[1;33m[VALIDATE]\033[0m Checking M4 Environment...\n");

    /* Optional: check internet (for Mongo Atlas). Skip if no nc to external. */
    if (check_port("8.8.8.8", 53) != 0) {
        printf("  \033[1;33m[WARN]\033[0m Internet unreachable. Cloud Atlas may be unavailable.\n");
    } else {
        printf("  \033[1;32m[PASS]\033[0m Internet: OK\n");
    }

    /* Redis (local cache) */
    const char *redis_host = config->redis_host ? config->redis_host : "127.0.0.1";
    int rp = config->redis_port > 0 ? config->redis_port : 6379;
    if (check_port(redis_host, rp) != 0) {
        printf("  \033[1;31m[FAIL]\033[0m Redis is DOWN. Counters disabled!\n");
    } else {
        printf("  \033[1;32m[PASS]\033[0m Redis: OK\n");
    }

    /* ELK (Elasticsearch) */
    const char *es_host = config->es_host ? config->es_host : "127.0.0.1";
    int ep = config->es_port > 0 ? config->es_port : 9200;
    if (check_port(es_host, ep) != 0) {
        printf("  \033[1;33m[WARN]\033[0m ELK/Elasticsearch is DOWN. Analytics disabled.\n");
    } else {
        printf("  \033[1;32m[PASS]\033[0m ELK: OK\n");
    }

    return 0;
}
