#ifndef M4_MODEL_SWITCH_H
#define M4_MODEL_SWITCH_H

#include <stddef.h>
#include <stdint.h>
#include "smart_topic.h"

/** Max length for lane key strings (session override + profile output). */
#define MODEL_SWITCH_LANE_KEY_MAX 64

#define MODEL_SWITCH_MODEL_MAX   256
#define MODEL_SWITCH_INJECT_MAX  4096

/** Merge smart_topic micro-intent into lane key when session is DEFAULT (empty / "DEFAULT"). */
#define MODEL_SWITCH_FLAG_MERGE_SMART_TOPIC_INTENT  (1u << 0)
/** Reserved: future adaptive router scores / A-B. */
#define MODEL_SWITCH_FLAG_ADAPTIVE_RESERVED         (1u << 1)

/**
 * One row of app config: arbitrary lane key → Ollama model + optional inject.
 * Example: { "EDUCATION", "", "You are a patient tutor." } — empty model string uses
 * M4_MODEL_EDUCATION, then OLLAMA_MODEL, then OLLAMA_DEFAULT_MODEL (see .cursor/default_models.md).
 * Keys are matched case-insensitively. model NULL or "" tries getenv M4_MODEL_<KEY> (KEY uppercased).
 */
typedef struct model_switch_lane_entry {
    const char *key;
    const char *model;
    const char *inject;
} model_switch_lane_entry_t;

/**
 * Dynamic routing table (pointer stable for engine lifetime).
 * If `lanes` is NULL or `lane_count` is 0, only getenv `M4_MODEL_<KEY>` / `OLLAMA_MODEL` apply.
 */
typedef struct model_switch_options {
    const model_switch_lane_entry_t *lanes;
    size_t lane_count;
    /** If no row matches effective key: use this, then `OLLAMA_MODEL`, then `OLLAMA_DEFAULT_MODEL`. */
    const char *fallback_model;
    uint32_t flags;
    const char *adaptive_profile_id;
} model_switch_options_t;

typedef struct model_switch_profile {
    char model[MODEL_SWITCH_MODEL_MAX];
    char inject[MODEL_SWITCH_INJECT_MAX];
    /** Effective lane key this turn (e.g. EDUCATION, TECH, DEFAULT). */
    char lane_key[MODEL_SWITCH_LANE_KEY_MAX];
    uint32_t adaptive_token; /* reserved */
} model_switch_profile_t;

/**
 * Resolve Ollama model + optional inject from dynamic table + smart_topic intent.
 *
 * @param session_lane_key  NULL, "", or "DEFAULT" → use merge flag + st_intent to pick key;
 *                          else force this key (e.g. user "Study mode" → "EDUCATION").
 * @param st_intent         From smart_topic_classify_for_query when merge flag is set.
 */
int model_switch_resolve(const model_switch_options_t *opts,
                         const char *session_lane_key,
                         smart_topic_intent_t st_intent,
                         model_switch_profile_t *out);

/** Map intent enum to canonical lane key (for logging / tests). */
const char *smart_topic_intent_lane_key(smart_topic_intent_t st);

#endif /* M4_MODEL_SWITCH_H */
