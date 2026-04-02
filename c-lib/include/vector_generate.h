#ifndef M4_VECTOR_GENERATE_H
#define M4_VECTOR_GENERATE_H

#include <stddef.h>

/**
 * Built-in RAG embedding (no Ollama): bag-of-words + bigrams → hashed sparse counts → L2-normalized.
 * Dimension is fixed; must match between index and query. See `.cursor/vector_generate.md`.
 */
#define VECTOR_GEN_CUSTOM_DIM 384

/** Stored in Mongo `embed_model_id` / metadata when the custom backend produced the vector. */
#define VECTOR_GEN_MODEL_ID "m4-vector-hash-v1-384"

/**
 * Fill `out` with a deterministic embedding of `text`.
 * Requires max_dim >= VECTOR_GEN_CUSTOM_DIM. On success sets *out_dim = VECTOR_GEN_CUSTOM_DIM and returns 0.
 */
int vector_generate_custom(const char *text, float *out, size_t max_dim, size_t *out_dim);

#endif /* M4_VECTOR_GENERATE_H */
