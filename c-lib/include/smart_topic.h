#ifndef M4_SMART_TOPIC_H
#define M4_SMART_TOPIC_H

#include <stddef.h>
#include <stdbool.h>
#include "engine.h"

/* Default Mongo collection when mini_ai_collection is not set. */
#define SMART_TOPIC_COLLECTION_DEFAULT "smart_topic"

/** Library / model size tier for mini AI. */
typedef enum {
    MINI_AI_TYPE_TINY,   /* Smallest, fastest (e.g. tinyllama, 1B). */
    MINI_AI_TYPE_B2,     /* Small (e.g. ~2–3B). */
    MINI_AI_TYPE_SMALL,  /* Optional: small but stronger. */
    MINI_AI_TYPE_COUNT
} mini_ai_type_t;

/** Intent from micro-query (single Ollama call shared with temperature + model_switch lane key). */
typedef enum {
    SMART_TOPIC_INTENT_DEFAULT = 0,
    SMART_TOPIC_INTENT_TECH,
    SMART_TOPIC_INTENT_CHAT,
    SMART_TOPIC_INTENT_EDUCATION,
    SMART_TOPIC_INTENT_BUSINESS
} smart_topic_intent_t;

/** Options for smart topic (mini AI switch). Reuse same base as engine (execution_mode, etc.). */
typedef struct smart_topic_options {
    bool enable;                        /* Master switch: use mini AI for topic routing / caching. */
    mini_ai_type_t library_type;         /* Tiny | B2 | ... which model tier to use. */
    execution_mode_t execution_mode;     /* A/B/C/D: determines storage path (memory, mongo, redis, elk). */
    const char *mini_ai_collection;      /* NULL or "" => use SMART_TOPIC_COLLECTION_DEFAULT. Validated 1..63, [a-zA-Z0-9_]. */
    const char *model_tiny;              /* Optional model name override (e.g. "tinyllama"). */
    const char *model_b2;                /* Optional model name override (e.g. "phi2"). */
} smart_topic_options_t;

struct storage_ctx;

/**
 * Initialize smart topic (mini AI switch). Uses opts for collection (else smart_topic), mode, and model tier.
 * When opts->enable is false, no-ops and returns 0.
 * storage can be NULL for MODE_ONLY_MEMORY.
 * Returns 0 on success, -1 on error (e.g. invalid opts).
 */
int initial_smart_topic(const smart_topic_options_t *opts, struct storage_ctx *storage);

/**
 * Get current smart topic state / result (e.g. topic label, cache hit, selected model).
 * out: caller buffer; out_size: buffer size. Result is null-terminated.
 * Returns 0 on success, -1 if not initialized or disabled.
 */
int get_smart_topic(char *out, size_t out_size);

/**
 * Intent-based temperature for query (when smart_topic enabled).
 * Runs a micro-query to Ollama (TinyLlama/Gemma-2B) to classify intent:
 *   TECH (C, code, Mongo) -> *out_temperature = 0.0
 *   CHAT (greeting/general) -> *out_temperature = 0.8
 *   DEFAULT -> *out_temperature = 0.5
 * On failure or not initialized, sets 0.5 and returns 0.
 */
int smart_topic_temperature_for_query(const char *query, double *out_temperature);

/**
 * Same micro-query as smart_topic_temperature_for_query, but also sets intent (no second Ollama call).
 * out_intent / out_temperature may be NULL if not needed. On failure: DEFAULT intent, 0.5 temperature.
 */
int smart_topic_classify_for_query(const char *query, double *out_temperature, smart_topic_intent_t *out_intent);

#endif /* M4_SMART_TOPIC_H */
