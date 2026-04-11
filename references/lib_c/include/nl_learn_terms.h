#ifndef NL_LEARN_TERMS_H
#define NL_LEARN_TERMS_H

#include <stddef.h>
#include <stdint.h>

/** File format first line (TSV v1). */
#define NL_LEARN_TERMS_FILE_MAGIC "nl_learn_terms_v1"
/** JSON document `schema` string (v2); see elk_nl_routing.md §8. */
#define NL_LEARN_TERMS_JSON_SCHEMA "nl_learn_terms_v2"

/**
 * Stderr logging (prefix `[nl_learn_terms]`). All module messages use this tag so you can `grep` or filter logs.
 *
 * - **Unset** (default): **lifecycle** — open, load summary, save, close / auto-save.
 * - **`M4_NL_LEARN_LOG=0`** / `false` / `no` / `off` — **errors only** (fopen/rename/OOM failures).
 * - **`M4_NL_LEARN_LOG=1`** — same as default (explicit lifecycle).
 * - **`M4_NL_LEARN_LOG=verbose`** or **`2`** — also log every **`record`** and **`score_sum`** (chatty).
 */

typedef struct nl_learn_terms nl_learn_terms_t;

/**
 * Open or create an in-memory store backed by path.
 * Format: **JSON v2** if the path ends in `.json`, or if the file begins with `{` (after optional BOM);
 * otherwise **TSV v1** (magic line `NL_LEARN_TERMS_FILE_MAGIC` + `term\\tintent\\tcount` rows).
 * Save uses the same format as detected at open (or `.json` for a new file).
 * @param path non-empty filesystem path
 * @param enable_write non-zero: nl_learn_terms_record updates counts; close/save may persist
 */
nl_learn_terms_t *nl_learn_terms_open(const char *path, int enable_write);

void nl_learn_terms_close(nl_learn_terms_t *lt);

/** Write atomically to path (same directory, .tmp + rename). Returns 0 or -1. */
int nl_learn_terms_save(nl_learn_terms_t *lt);

/**
 * Add delta to each (term_i, intent) bucket. No-op when enable_write was 0 at open.
 * term_keys must not contain TAB/NL. intent must be [A-Za-z0-9_]+ and, by default, one of the
 * closed routing labels from elk_nl_routing.md §4: CHAT, ELK_ANALYTICS, ELK_SEARCH, RAG_VECTOR.
 * Set **M4_NL_LEARN_RELAX_INTENT** to a truthy value to allow any syntactically valid intent (legacy TSV).
 * Returns 0 on success, -1 on invalid input or OOM.
 */
int nl_learn_terms_record(nl_learn_terms_t *lt, const char *const *term_keys, size_t nkeys,
                          const char *intent, int delta);

/** Sum stored counts for intent across all given term keys (missing keys count as 0). */
int64_t nl_learn_terms_score_sum(nl_learn_terms_t *lt, const char *const *term_keys, size_t nkeys,
                                 const char *intent);

/**
 * Score a normalized text against ALL stored terms for a given intent.
 * For each stored (term, intent) pair: if strstr(text, term) != NULL, add its count to the total.
 * This uses the learned data directly — no hardcoded phrase tables needed.
 */
int64_t nl_learn_terms_score_text(nl_learn_terms_t *lt, const char *normalized_text, const char *intent);

#endif /* NL_LEARN_TERMS_H */
