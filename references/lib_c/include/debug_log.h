#ifndef M4_DEBUG_LOG_H
#define M4_DEBUG_LOG_H

#include <stdio.h>

/**
 * Unified logging module for c-lib.
 *
 * Usage:
 *   m4_log("GEO_LEARNING", M4_LOG_INFO, "worker started queue=%d", count);
 *   m4_log("ai_agent", M4_LOG_ERROR, "curl failed rc=%d", rc);
 *   m4_log("STORAGE", M4_LOG_DEBUG, "mongo insert tenant=%s", tid);
 *
 * Output format:
 *   [GEO_LEARNING][INFO] worker started queue=3
 *   [ai_agent][ERROR] curl failed rc=-1
 *
 * Filtering:
 *   api_options_t.debug_modules = {"GEO_LEARNING", "ai_agent"};
 *   - DEBUG level: only logged if module is in debug_modules list
 *   - INFO level:  always logged
 *   - WARN level:  always logged
 *   - ERROR level: always logged
 *
 * Valid module keys:
 *   API, ai_agent, STORAGE, GEO_LEARNING, GEO_AUTH, OLLAMA, ELK,
 *   EMBED_MIGRATION, ENGINE, CHAT, nl_learn_terms, nl_learn_cues,
 *   LOGIC_CONFLICT, INTENT_ROUTE, SHARED_COLLECTION, SMART_TOPIC
 */

/** Log levels */
#define M4_LOG_DEBUG  0   /* only when module is in debug_modules */
#define M4_LOG_INFO   1   /* always logged */
#define M4_LOG_WARN   2   /* always logged */
#define M4_LOG_ERROR  3   /* always logged */

/** Initialize: set which modules have DEBUG enabled. Called from api_create. */
void m4_log_init(const char *const *debug_modules, int count);

/** Shutdown: free debug state. Called from api_destroy. */
void m4_log_shutdown(void);

/** Returns 1 if module has DEBUG logging enabled. */
int m4_log_debug_enabled(const char *module);

/**
 * Log a message.
 * - level >= INFO: always logged to stderr.
 * - level == DEBUG: only logged if module is in the debug list.
 */
void m4_log(const char *module, int level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * Optional: set a custom log handler for future use (e.g. write to file, ELK, callback).
 * NULL = use default stderr handler.
 */
typedef void (*m4_log_handler_fn)(const char *module, int level, const char *message, void *userdata);
void m4_log_set_handler(m4_log_handler_fn handler, void *userdata);

/** Convenience macros */
#define M4_LOG_DEBUG_MSG(module, fmt, ...) m4_log(module, M4_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define M4_LOG_INFO_MSG(module, fmt, ...)  m4_log(module, M4_LOG_INFO,  fmt, ##__VA_ARGS__)
#define M4_LOG_WARN_MSG(module, fmt, ...)  m4_log(module, M4_LOG_WARN,  fmt, ##__VA_ARGS__)
#define M4_LOG_ERROR_MSG(module, fmt, ...) m4_log(module, M4_LOG_ERROR, fmt, ##__VA_ARGS__)

#endif /* M4_DEBUG_LOG_H */
