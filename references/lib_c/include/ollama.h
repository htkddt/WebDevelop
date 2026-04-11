#ifndef M4_OLLAMA_H
#define M4_OLLAMA_H

#include <stddef.h>

/*
 * Default Ollama tags — single source of truth for compile-time fallbacks.
 * When changing OLLAMA_DEFAULT_*: follow `.cursor/default_models.md` (Python reads via `api_build_ollama_*`, docs, examples).
 * Cursor: `.cursor/rules/default-models.mdc` (always on).
 */
/* Rule §8: 32KB buffer for AI responses to avoid UTF-8 cutting. */
#define OL_BUF_SIZE 32768

#define OLLAMA_DEFAULT_HOST "127.0.0.1"
#define OLLAMA_DEFAULT_PORT 11434
/* Light local default when no model passed and OLLAMA_MODEL unset (low RAM/CPU on Mac).
 * Also the intended last tier after free-cloud rate limits when a cloud router exists.
 * Setup: ollama pull llama3.2:1b
 * Embed: set OLLAMA_EMBED_MODEL if this chat model is not used for /api/embed. */
#define OLLAMA_DEFAULT_MODEL "gemma4:26b"
/* Fallback name only when `ollama_resolve_embed_model` cannot resolve (e.g. failure); normal path uses chat defaults. */
#define OLLAMA_DEFAULT_EMBED_MODEL "nomic-embed-text"
/* Max embedding dimension we accept (buffer size). Stored vector length = *out_dim from ollama_embeddings (model-dependent, e.g. 768 or 2048). */
#define OLLAMA_EMBED_MAX_DIM 2048

/* Call Ollama /api/generate with prompt; write response into out (null-terminated). */
int ollama_query(const char *host, int port, const char *model, const char *prompt,
                 char *out, size_t out_size);

/**
 * Same as ollama_query but with temperature. temperature < 0 means default (no options).
 * TECH: 0.0, CHAT: 0.8, DEFAULT: 0.5 (see smart_topic intent routing).
 */
int ollama_query_with_options(const char *host, int port, const char *model, const char *prompt,
                              double temperature, char *out, size_t out_size);

/**
 * Streaming POST /api/generate with "stream":true (NDJSON lines).
 * Invokes on_token for each parsed text fragment from a line: "response", then "content",
 * then delta.content, "thinking", then "text" (see ollama.c extract_stream_token_json).
 * Aggregates into full_out when non-NULL (null-terminated if room).
 * The curl write callback uses WRITEDATA for line buffering; each request must use a fresh on_token_ud if concurrent.
 * Returns 0 on success, -1 on error.
 */
typedef void (*ollama_stream_token_cb)(const char *token, void *userdata);
int ollama_query_stream(const char *host, int port, const char *model, const char *prompt,
                        double temperature,
                        ollama_stream_token_cb on_token, void *on_token_ud,
                        char *full_out, size_t full_out_size);

/**
 * Query running Ollama GET /api/tags and write the first model's "name" into out (null-terminated).
 * Use when you want to use whatever model is currently installed (e.g. after ollama pull).
 * Returns 0 on success, -1 if Ollama unreachable or no models. host/port default like query.
 */
int ollama_get_first_model(const char *host, int port, char *out, size_t out_size);

/**
 * Lightweight health check: GET /api/tags with short timeout. For use in api_get_stats (frequent).
 * Returns 0 if Ollama is running and responds, -1 otherwise. host/port default like query.
 */
int ollama_check_running(const char *host, int port);

/**
 * Elasticsearch HTTP reachability: GET http://host:port/ with short timeout.
 * Returns 0 if a response is received (2xx–499), -1 if host empty or request failed.
 */
int elasticsearch_check_reachable(const char *host, int port);

/**
 * Resolve which model ollama_embeddings will use (same rules as embed call). Writes null-terminated name to out.
 * Resolution: preferred → OLLAMA_EMBED_MODEL (optional split) → same as chat NULL-model: first /api/tags name, OLLAMA_MODEL, OLLAMA_DEFAULT_MODEL.
 * Use before ollama_embeddings to store embed_model_id in Mongo (geo_atlas, chat `metadata`, etc.).
 * Returns 0 on success, -1 if out/out_size invalid.
 */
int ollama_resolve_embed_model(const char *host, int port, const char *preferred, char *out, size_t out_size);

/**
 * Fetch embedding vector from Ollama POST /api/embed (VectorGen per .cursor/lang_vector_phase1.md).
 * model: NULL/empty → `ollama_resolve_embed_model` (unified with chat defaults unless OLLAMA_EMBED_MODEL set).
 * text: input to embed.
 * vector: output float array (caller-allocated, at least max_dim elements).
 * max_dim: max elements to write; actual dimension written to *out_dim.
 * Returns 0 on success, -1 on error. *out_dim set to 0 on error.
 */
int ollama_embeddings(const char *host, int port, const char *model, const char *text,
                     float *vector, size_t max_dim, size_t *out_dim);

#endif /* M4_OLLAMA_H */
