/*
 * Intent routing — Phase 1 (classify) using NL learning scores + vocab.
 * Design: .cursor/intent_routing.md
 */
#ifndef M4_INTENT_ROUTE_H
#define M4_INTENT_ROUTE_H

#include <stddef.h>
#include <stdint.h>
#include "nl_learn_terms.h"
#include "shared_collection.h"

/** Routed intent from Phase 1 classify. */
typedef enum {
    INTENT_ROUTE_CHAT = 0,       /* no data routing — normal LLM chat */
    INTENT_ROUTE_ELK_ANALYTICS,  /* count, sum, avg, group_by — ELK aggs */
    INTENT_ROUTE_ELK_SEARCH,     /* find, list, show — ELK multi_match */
    INTENT_ROUTE_RAG_VECTOR,     /* knowledge base / documentation lookup */
} intent_route_t;

/** Result of Phase 1 classify. */
typedef struct {
    intent_route_t intent;
    int64_t intent_score;          /* highest score among ELK/RAG intents */
    char collection[160];          /* best matching collection (empty if unknown) */
    char field[64];                /* best matching field (empty if unknown) */
    int64_t collection_score;      /* SC:{collection} score */
} intent_route_result_t;

/**
 * Phase 1: classify user message using NL learning scores + vocab lookup.
 * Tokenizes message, computes score_sum for each intent, picks highest.
 * If score > min_score_threshold, routes to that intent; else CHAT.
 * vocab may be NULL (no entity resolution, intent-only).
 * lt may be NULL (no scores, always CHAT).
 * Returns 0 on success.
 */
int intent_route_classify(const char *user_msg,
                          nl_learn_terms_t *lt,
                          const sc_term_vocab_t *vocab,
                          const sc_registry_t *registry,
                          int64_t min_score_threshold,
                          intent_route_result_t *out);

/** Intent label string for logging. */
const char *intent_route_label(intent_route_t intent);

/* ---------- Filter validation ---------- */

/**
 * Validate a field name against known sources before using in ELK query.
 * Checks: (1) SharedCollection field_hints, (2) vocab table field entries.
 * Returns 1 if field is known/valid, 0 if unknown.
 */
int intent_route_validate_field(const char *collection, const char *field,
                                const sc_registry_t *registry,
                                const sc_term_vocab_t *vocab);

/* ---------- Phase 3: EXECUTE — build ELK query + run ---------- */

struct storage_ctx;

/** Result of Phase 3 execute. */
typedef struct {
    int executed;                   /* 1 if ELK query was executed, 0 if skipped */
    char elk_index[160];            /* resolved ELK index name */
    char elk_query[2048];           /* the query JSON sent to ELK */
    char elk_response[8192];        /* raw ELK response JSON */
    long long result_count;         /* parsed: total hits or count */
    char result_snippet[2048];      /* parsed: first few hits as text (for ELK_SEARCH) */
} intent_route_elk_result_t;

/**
 * Phase 3: build ELK query from classify result + execute via storage.
 * For ELK_ANALYTICS: builds _search with size:0 + match or count query.
 * For ELK_SEARCH: builds _search with multi_match, returns top hits.
 * registry is needed to resolve collection → elk index name.
 * storage is needed to call elk_search.
 * user_msg is used as the search text for multi_match.
 * Returns 0 on success, -1 if ELK unavailable or collection not found.
 */
int intent_route_execute(const intent_route_result_t *classify,
                         const char *user_msg,
                         const sc_registry_t *registry,
                         struct storage_ctx *storage,
                         intent_route_elk_result_t *out);

/* ---------- Phase 4: FORMAT — inject [DATA_RESULT] into prompt ---------- */

/**
 * Phase 4: build a [DATA_RESULT] block to prepend to the LLM prompt.
 * Writes into `out` buffer, e.g.:
 *   [DATA_RESULT] {"question":"how many...","result":42,"source":"idx_products"}
 *   Answer the user's question using the data above.
 * Returns length written, or 0 if nothing to inject.
 */
size_t intent_route_format_data_result(const intent_route_result_t *classify,
                                       const intent_route_elk_result_t *elk,
                                       const char *user_msg,
                                       char *out, size_t out_size);

#endif /* M4_INTENT_ROUTE_H */
