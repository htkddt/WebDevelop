/*
 * Geo learning module — optional background place learning (.cursor/geo_leanring.md).
 * After a turn is persisted, enqueue for async worker (non-blocking for the user response).
 * Worker: extract places (any region) via Ollama JSON, dedupe by vector similarity, insert geo_atlas.
 * [KNOWLEDGE_BASE] in prompts is opt-in: api_options.inject_geo_knowledge (see api.h).
 */
#ifndef M4_GEO_LEARNING_H
#define M4_GEO_LEARNING_H

#include <stddef.h>

struct storage_ctx;

/** Initialize worker and queue. Call once at engine_init when geo_learning is enabled. st must outlive shutdown. */
int geo_learning_init(struct storage_ctx *st);

/** Enqueue one turn for async processing. Non-blocking; copies input/assistant/tenant_id. */
void geo_learning_enqueue_turn(const char *tenant_id, const char *user_id,
                               const char *input, const char *assistant,
                               const char *timestamp);

/** Stop worker and free resources. Call from engine_destroy. */
void geo_learning_shutdown(void);

#endif /* M4_GEO_LEARNING_H */
