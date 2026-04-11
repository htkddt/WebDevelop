#ifndef M4_EMBED_H
#define M4_EMBED_H

#include "engine.h"
#include <stddef.h>

/**
 * Central embedding policy for text → float vector + model id string (Mongo metadata).
 * All chat RAG, `bot.records` vectors, and **geo_atlas** landmark names go through **`m4_embed_text`**
 * via **`m4_embed_options_from_engine`** or **`m4_embed_options_geo_env`** — do not call
 * **`ollama_embeddings`** / **`vector_generate_custom`** directly from feature code.
 *
 * Backends (numeric values match **`api_options_t.vector_gen_backend`** / **`engine_config_t.vector_gen_backend`**):
 */
#define M4_EMBED_BACKEND_CUSTOM  0
#define M4_EMBED_BACKEND_OLLAMA  1

/** Stored `metadata.embed_family` / geo — must match `.cursor/embed-vector-metadata.mdc` (no drift). */
#define M4_STORED_EMBED_FAMILY_CUSTOM "custom"
#define M4_STORED_EMBED_FAMILY_OLLAMA "ollama"
#define M4_STORED_EMBED_FAMILY_LEGACY "legacy"

/** Log / BSON label for `m4_embed_options_t.backend` (not a default route; reflects actual embed path). */
static inline const char *m4_embed_backend_label(int backend) {
    return backend == M4_EMBED_BACKEND_CUSTOM ? M4_STORED_EMBED_FAMILY_CUSTOM : M4_STORED_EMBED_FAMILY_OLLAMA;
}

typedef struct m4_embed_options {
    int backend; /** `M4_EMBED_BACKEND_*` */
    /** Ollama preferred model; NULL/empty → full resolve chain (`OLLAMA_EMBED_MODEL` / tags / env). */
    const char *ollama_model_pref;
    /**
     * When `backend == OLLAMA`: if HTTP/embed fails and this is non-zero, fall back to custom hash
     * (`VECTOR_GEN_MODEL_ID`). Default from **`m4_embed_options_from_engine`**: see **`M4_EMBED_FALLBACK_CUSTOM`**.
     * **Warning:** mixed dimensions in an existing Redis geo index if you flip mid-flight.
     */
    int ollama_fail_use_custom;
} m4_embed_options_t;

/**
 * Policy from **`engine_config_t`** (from **`api_create`** / **`engine_create`**).
 * Also reads **`M4_EMBED_FALLBACK_CUSTOM`** (1/true/yes) when backend is Ollama.
 */
void m4_embed_options_from_engine(const engine_config_t *cfg, m4_embed_options_t *out);

/**
 * Policy for **geo_learning** (no `engine_t` in hand): reads env only.
 * - **`M4_EMBED_BACKEND`**: `custom` / `0` → hash only; `ollama` / `1` or **unset** → Ollama (default).
 * - **`OLLAMA_EMBED_MODEL`**: optional preferred embed model (same as elsewhere).
 * - **`M4_EMBED_FALLBACK_CUSTOM`**: if Ollama fails, use custom (default **off** for geo).
 */
void m4_embed_options_geo_env(m4_embed_options_t *out);

/**
 * Run embedding. Fills *out_dim and null-terminated model_id_out on success. Returns 0 on success, -1 on failure.
 */
int m4_embed_text(const m4_embed_options_t *opts, const char *text, float *vec, size_t max_dim, size_t *out_dim,
                  char *model_id_out, size_t model_id_out_sz);

/** Convenience: **`engine`** NULL → geo-env policy + embed (standalone / tests). */
int m4_embed_for_engine(engine_t *engine, const char *text, float *vec, size_t max_dim, size_t *out_dim,
                        char *model_id_out, size_t model_id_out_sz);

#endif /* M4_EMBED_H */
