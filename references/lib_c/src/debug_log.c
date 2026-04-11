/*
 * Unified logging module — m4_log(module, level, fmt, ...).
 * DEBUG filtered by api_options_t.debug_modules; INFO/WARN/ERROR always logged.
 */
#include "debug_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define MAX_DEBUG_MODULES 32
#define LOG_MSG_MAX 2048

static const char *s_known_modules[] = {
    "API", "ai_agent", "STORAGE", "GEO_LEARNING", "GEO_AUTH",
    "OLLAMA", "ELK", "EMBED_MIGRATION", "ENGINE", "CHAT",
    "nl_learn_terms", "nl_learn_cues", "LOGIC_CONFLICT",
    "INTENT_ROUTE", "SHARED_COLLECTION", "SMART_TOPIC"
};
static const int s_known_count = (int)(sizeof(s_known_modules) / sizeof(s_known_modules[0]));

static char *s_debug_modules[MAX_DEBUG_MODULES];
static int s_debug_count = 0;

static m4_log_handler_fn s_handler = NULL;
static void *s_handler_ud = NULL;

static const char *level_label(int level) {
    switch (level) {
        case M4_LOG_DEBUG: return "DEBUG";
        case M4_LOG_INFO:  return "INFO";
        case M4_LOG_WARN:  return "WARN";
        case M4_LOG_ERROR: return "ERROR";
        default:           return "LOG";
    }
}

static int is_known_module(const char *key) {
    for (int i = 0; i < s_known_count; i++) {
        if (strcmp(s_known_modules[i], key) == 0)
            return 1;
    }
    return 0;
}

static void add_module(const char *name) {
    if (!name || !name[0] || s_debug_count >= MAX_DEBUG_MODULES) return;
    if (!is_known_module(name)) {
        fprintf(stderr, "[DEBUG_LOG][WARN] module '%s' not found. Valid:", name);
        for (int j = 0; j < s_known_count; j++)
            fprintf(stderr, " %s", s_known_modules[j]);
        fprintf(stderr, "\n");
    }
    s_debug_modules[s_debug_count] = strdup(name);
    if (s_debug_modules[s_debug_count])
        s_debug_count++;
}

void m4_log_init(const char *const *modules, int count) {
    m4_log_shutdown();

    /* Priority 1: env var M4_DEBUG_MODULES (comma-separated) */
    const char *env = getenv("M4_DEBUG_MODULES");
    if (env && env[0]) {
        char buf[1024];
        strncpy(buf, env, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            while (*tok == ' ') tok++;
            size_t len = strlen(tok);
            while (len > 0 && tok[len - 1] == ' ') tok[--len] = '\0';
            if (len > 0) add_module(tok);
        }
    }

    /* Priority 2: api_options_t.debug_modules (additive) */
    if (modules && count > 0) {
        int n = count < MAX_DEBUG_MODULES ? count : MAX_DEBUG_MODULES;
        for (int i = 0; i < n; i++)
            add_module(modules[i]);
    }

    if (s_debug_count > 0) {
        fprintf(stderr, "[DEBUG_LOG][INFO] debug enabled for %d module(s):", s_debug_count);
        for (int i = 0; i < s_debug_count; i++)
            fprintf(stderr, " %s", s_debug_modules[i]);
        fprintf(stderr, "\n");
    }
}

void m4_log_shutdown(void) {
    for (int i = 0; i < s_debug_count; i++) {
        free(s_debug_modules[i]);
        s_debug_modules[i] = NULL;
    }
    s_debug_count = 0;
    s_handler = NULL;
    s_handler_ud = NULL;
}

int m4_log_debug_enabled(const char *module) {
    if (!module || s_debug_count == 0) return 0;
    for (int i = 0; i < s_debug_count; i++) {
        if (s_debug_modules[i] && strcmp(s_debug_modules[i], module) == 0)
            return 1;
    }
    return 0;
}

void m4_log_set_handler(m4_log_handler_fn handler, void *userdata) {
    s_handler = handler;
    s_handler_ud = userdata;
}

void m4_log(const char *module, int level, const char *fmt, ...) {
    /* DEBUG: only if module is in debug list */
    if (level == M4_LOG_DEBUG && !m4_log_debug_enabled(module))
        return;

    char buf[LOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Custom handler (future: file, ELK, callback to app layer) */
    if (s_handler) {
        s_handler(module, level, buf, s_handler_ud);
        return;
    }

    /* Default: stderr */
    fprintf(stderr, "[%s][%s] %s\n", module ? module : "?", level_label(level), buf);
}
