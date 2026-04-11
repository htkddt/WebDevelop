#include "embed.h"
#include "ollama.h"
#include "vector_generate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int env_truthy(const char *e) {
    if (!e || !e[0]) return 0;
    if (e[0] == '1' && e[1] == '\0') return 1;
    if (strcasecmp(e, "true") == 0 || strcasecmp(e, "yes") == 0 || strcasecmp(e, "on") == 0) return 1;
    return 0;
}

void m4_embed_options_from_engine(const engine_config_t *cfg, m4_embed_options_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cfg) {
        m4_embed_options_geo_env(out);
        return;
    }
    if (cfg->vector_gen_backend == M4_EMBED_BACKEND_OLLAMA) {
        out->backend = M4_EMBED_BACKEND_OLLAMA;
        out->ollama_model_pref = cfg->vector_ollama_model;
        out->ollama_fail_use_custom = env_truthy(getenv("M4_EMBED_FALLBACK_CUSTOM"));
    } else {
        out->backend = M4_EMBED_BACKEND_CUSTOM;
    }
}

void m4_embed_options_geo_env(m4_embed_options_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const char *be = getenv("M4_EMBED_BACKEND");
    if (be && be[0]) {
        if (strcasecmp(be, "custom") == 0 || strcmp(be, "0") == 0) {
            out->backend = M4_EMBED_BACKEND_CUSTOM;
            return;
        }
    }
    out->backend = M4_EMBED_BACKEND_OLLAMA;
    const char *p = getenv("OLLAMA_EMBED_MODEL");
    out->ollama_model_pref = (p && p[0]) ? p : NULL;
    out->ollama_fail_use_custom = env_truthy(getenv("M4_EMBED_FALLBACK_CUSTOM"));
}

static int embed_custom(const char *text, float *vec, size_t max_dim, size_t *out_dim, char *model_id_out,
                        size_t model_id_out_sz) {
    if (max_dim < VECTOR_GEN_CUSTOM_DIM) return -1;
    if (vector_generate_custom(text, vec, max_dim, out_dim) != 0) return -1;
    snprintf(model_id_out, model_id_out_sz, "%s", VECTOR_GEN_MODEL_ID);
    return 0;
}

static int embed_ollama(const char *pref_in, const char *text, float *vec, size_t max_dim, size_t *out_dim,
                        char *model_id_out, size_t model_id_out_sz) {
    const char *pref = pref_in;
    if (pref && !pref[0]) pref = NULL;
    (void)ollama_resolve_embed_model(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, pref, model_id_out, model_id_out_sz);
    if (ollama_embeddings(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, pref, text, vec, max_dim, out_dim) != 0)
        return -1;
    if (*out_dim == 0) return -1;
    if (model_id_out[0] == '\0') {
        if (ollama_resolve_embed_model(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, pref, model_id_out, model_id_out_sz)
                != 0
            || model_id_out[0] == '\0') {
            strncpy(model_id_out, OLLAMA_DEFAULT_EMBED_MODEL, model_id_out_sz - 1);
            model_id_out[model_id_out_sz - 1] = '\0';
        }
    }
    return 0;
}

int m4_embed_text(const m4_embed_options_t *opts, const char *text, float *vec, size_t max_dim, size_t *out_dim,
                  char *model_id_out, size_t model_id_out_sz) {
    if (!opts || !vec || !out_dim || max_dim == 0 || !model_id_out || model_id_out_sz == 0) return -1;
    const char *t = text ? text : "";
    model_id_out[0] = '\0';
    *out_dim = 0;

    if (opts->backend == M4_EMBED_BACKEND_CUSTOM)
        return embed_custom(t, vec, max_dim, out_dim, model_id_out, model_id_out_sz);

    if (embed_ollama(opts->ollama_model_pref, t, vec, max_dim, out_dim, model_id_out, model_id_out_sz) == 0)
        return 0;

    if (opts->ollama_fail_use_custom)
        return embed_custom(t, vec, max_dim, out_dim, model_id_out, model_id_out_sz);

    return -1;
}

int m4_embed_for_engine(engine_t *engine, const char *text, float *vec, size_t max_dim, size_t *out_dim,
                        char *model_id_out, size_t model_id_out_sz) {
    m4_embed_options_t o;
    if (engine)
        m4_embed_options_from_engine(engine_get_config(engine), &o);
    else
        m4_embed_options_geo_env(&o);
    return m4_embed_text(&o, text, vec, max_dim, out_dim, model_id_out, model_id_out_sz);
}
