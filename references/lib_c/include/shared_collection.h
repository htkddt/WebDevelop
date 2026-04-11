#ifndef M4_SHARED_COLLECTION_H
#define M4_SHARED_COLLECTION_H

#include <stddef.h>

typedef struct sc_registry sc_registry_t;

sc_registry_t *sc_registry_load_file(const char *path);
void sc_registry_free(sc_registry_t *reg);

/** Entries with elk.allow true (for pool / backfill). */
size_t sc_registry_elk_count(const sc_registry_t *reg);

/** Resolved Elasticsearch index: explicit elk.index, or "." / empty → idx_{collection}. */
int sc_registry_elk_index(const sc_registry_t *reg, const char *collection, char *out, size_t out_sz);

/** 1 if collection is registered and elk.allow is true. */
int sc_registry_elk_allowed(const sc_registry_t *reg, const char *collection);

/** Invokes fn(collection, resolved_elk_index, u) for each elk.allow entry. */
void sc_registry_foreach_elk(const sc_registry_t *reg,
                             void (*fn)(const char *collection, const char *elk_index, void *u),
                             void *u);

/**
 * Every parsed registry entry (non-empty collection name). elk_allow is 1 if JSON had elk.allow true.
 * Used for validation against Mongo (registry must not be trusted blindly).
 */
typedef void (*sc_registry_foreach_fn)(const char *collection, int elk_allow, void *user);
void sc_registry_foreach(const sc_registry_t *reg, sc_registry_foreach_fn fn, void *user);

/** Check if a field name exists in a collection's field_hints. Returns 1 if found, 0 if not. */
int sc_registry_has_field(const sc_registry_t *reg, const char *collection, const char *field);

/** Build a schema summary for LLM prompts: "- collection: {field1, field2, ...}\n" per elk.allow entry.
 *  Uses field_hints keys as field names. Writes into buf (null-terminated). Returns bytes written. */
size_t sc_registry_schema_summary(const sc_registry_t *reg, char *buf, size_t cap);

/* ---------- Term vocabulary for NL cue learning (§5b) ---------- */

typedef struct sc_term_vocab sc_term_vocab_t;

/**
 * Build a read-only vocabulary table from registry metadata.
 * Sources: collection names (+ singular), alias tokens, field_hints keys + value tokens.
 * Call once at startup after sc_registry_load_file; result is read-only, thread-safe for lookups.
 */
sc_term_vocab_t *sc_term_vocab_build(const sc_registry_t *reg);
void sc_term_vocab_free(sc_term_vocab_t *v);

/**
 * Look up a user-facing word in the vocab.
 * Returns 0 on hit: *out_collection = matched collection name, *out_field = field name or NULL (collection-level).
 * Returns -1 on miss. Term is matched case-insensitively.
 */
int sc_term_vocab_lookup(const sc_term_vocab_t *v, const char *term,
                         const char **out_collection, const char **out_field);

/** Number of entries in the vocab table. */
size_t sc_term_vocab_count(const sc_term_vocab_t *v);

#endif
