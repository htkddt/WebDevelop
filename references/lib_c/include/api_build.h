#ifndef M4_API_BUILD_H
#define M4_API_BUILD_H

/**
 * Compile-time Ollama defaults from `ollama.h` (this shared library). For bindings: use these instead of
 * duplicating host/port/model tag literals in Python or other hosts. No HTTP and no model discovery.
 */
const char *api_build_ollama_default_host(void);
int api_build_ollama_default_port(void);
const char *api_build_ollama_default_chat_model(void);
const char *api_build_ollama_default_embed_model(void);
/** Max embedding elements accepted (`OLLAMA_EMBED_MAX_DIM`); size for float buffers passed to `ollama_embeddings`. */
int api_build_ollama_embed_max_dim(void);

#endif /* M4_API_BUILD_H */
