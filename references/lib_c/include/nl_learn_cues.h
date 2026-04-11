#ifndef NL_LEARN_CUES_H
#define NL_LEARN_CUES_H

#include "nl_learn_terms.h"
#include "shared_collection.h"

/**
 * Internal NL routing enrichment: scan `utterance_utf8` for phrase→intent cues (same tables as
 * python_ai `nl_learn_terms_bridge` / elk_nl_routing.md §8), call `nl_learn_terms_record` per hit.
 * Does not save — caller holds `nl_learn_terms` lock and calls `nl_learn_terms_save` once if desired.
 * Strips a trailing profile block after the last "\\n---\\n" (app-layer prefix) before matching.
 *
 * When `vocab` is non-NULL, also scans for SharedCollection entity terms (collection/field names)
 * and records them with "SC:{collection}" intent label for entity-level learning.
 */
void nl_learn_cues_record_from_utterance(nl_learn_terms_t *lt, const char *utterance_utf8,
                                         const sc_term_vocab_t *vocab);

#endif /* NL_LEARN_CUES_H */
