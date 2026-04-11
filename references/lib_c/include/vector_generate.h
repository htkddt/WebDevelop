#ifndef M4_VECTOR_GENERATE_H
#define M4_VECTOR_GENERATE_H

#include <stddef.h>

/**
 * Built-in RAG embedding (no Ollama):
 * - Synonym normalization (canonical form lookup)
 * - Word unigrams + bigrams + character trigrams
 * - Hashed sparse counts → L2-normalized
 * Dimension is fixed; must match between index and query.
 * See `.cursor/vector_generate.md`, `.cursor/vector_synonym.md`.
 */
#define VECTOR_GEN_CUSTOM_DIM 768

/** Stored in Mongo `embed_model_id` / metadata when the custom backend produced the vector. */
#define VECTOR_GEN_MODEL_ID "m4-vector-hash-v2-768"

/**
 * Fill `out` with a deterministic embedding of `text`.
 * Requires max_dim >= VECTOR_GEN_CUSTOM_DIM. On success sets *out_dim = VECTOR_GEN_CUSTOM_DIM and returns 0.
 */
int vector_generate_custom(const char *text, float *out, size_t max_dim, size_t *out_dim);

/**
 * Synonym table: maps alternative names to canonical forms.
 * Thread-safe after init (read-only lookups, write-locked adds).
 */
typedef struct m4_synonym_table m4_synonym_table_t;

/** Create an empty synonym table. */
m4_synonym_table_t *m4_synonym_create(void);

/** Destroy and free. */
void m4_synonym_destroy(m4_synonym_table_t *st);

/** Add a synonym: alias → canonical. Both strings are copied. Thread-safe. */
void m4_synonym_add(m4_synonym_table_t *st, const char *alias, const char *canonical);

/** Lookup: returns canonical form if alias is known, or NULL. Thread-safe. */
const char *m4_synonym_lookup(const m4_synonym_table_t *st, const char *text);

/** Set the global synonym table used by vector_generate_custom. */
void m4_synonym_set_global(m4_synonym_table_t *st);

/** Get the current global synonym table (may be NULL). */
m4_synonym_table_t *m4_synonym_get_global(void);

/** Add built-in Vietnamese place synonyms (common names). */
void m4_synonym_add_builtins(m4_synonym_table_t *st);

#endif /* M4_VECTOR_GENERATE_H */
