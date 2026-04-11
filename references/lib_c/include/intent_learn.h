/*
 * Background intent learning worker — Phase 2 LLM extraction.
 * Async: enqueue user turn → worker calls LLM with collection schemas →
 * parses query plan → records SC:{collection} + intent to nl_learn_terms.
 * Same pattern as geo_learning (queue + background thread).
 * Design: .cursor/intent_routing.md
 */
#ifndef M4_INTENT_LEARN_H
#define M4_INTENT_LEARN_H

#include "nl_learn_terms.h"
#include "shared_collection.h"

/**
 * Initialize background intent learning worker.
 * registry: SharedCollection registry (for building schema prompt).
 * lt: learning terms store (for recording LLM decisions). Must outlive shutdown.
 * Returns 0 on success, -1 on error.
 */
int intent_learn_init(const sc_registry_t *registry, nl_learn_terms_t *lt);

/**
 * Enqueue a user message for background LLM extraction.
 * Non-blocking: copies input, returns immediately.
 * Worker processes in background and records decisions to lt.
 */
void intent_learn_enqueue(const char *user_msg);

/** Stop worker thread and free resources. */
void intent_learn_shutdown(void);

/**
 * Query plan cache: stores full LLM query plans for reuse by Phase 3.
 * The background worker saves plans after extraction.
 * Phase 3 looks up cached plans by normalized message similarity.
 */

/** Set the cache file path. Call before init or after init. */
void intent_learn_set_cache_path(const char *path);

/**
 * Look up a cached query plan for a user message.
 * Searches by normalized substring matching against cached questions.
 * If found: writes collection, operation, filters_json into out buffers. Returns 0.
 * If not found: returns -1.
 */
int intent_learn_cache_lookup(const char *user_msg,
                              char *out_collection, size_t col_cap,
                              char *out_operation, size_t op_cap,
                              char *out_filters_json, size_t filters_cap);

#endif /* M4_INTENT_LEARN_H */
