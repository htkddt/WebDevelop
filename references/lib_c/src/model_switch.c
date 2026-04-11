/*
 * model_switch — dynamic lane key → Ollama model + inject (.cursor/model_switch.md).
 */
#include "model_switch.h"
#include "ollama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void strncpy_safe(char *dst, size_t dsize, const char *src) {
    if (!dst || dsize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dsize) n = dsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

const char *smart_topic_intent_lane_key(smart_topic_intent_t st) {
    switch (st) {
        case SMART_TOPIC_INTENT_TECH:       return "TECH";
        case SMART_TOPIC_INTENT_CHAT:       return "CHAT";
        case SMART_TOPIC_INTENT_EDUCATION: return "EDUCATION";
        case SMART_TOPIC_INTENT_BUSINESS:  return "BUSINESS";
        case SMART_TOPIC_INTENT_DEFAULT:
        default:                            return "DEFAULT";
    }
}

/** Append "M4_MODEL_" + KEY uppercased (A-Z0-9_) into buf. */
static void build_model_env_name(char *buf, size_t bufsz, const char *lane_key) {
    if (!buf || bufsz < 12) return;
    const char *p = lane_key ? lane_key : "DEFAULT";
    size_t j = 0;
    const char *prefix = "M4_MODEL_";
    for (; prefix[j] && j < bufsz - 1; j++)
        buf[j] = prefix[j];
    for (; *p && j < bufsz - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            buf[j++] = (char)c;
    }
    buf[j] = '\0';
}

static void build_inject_env_name(char *buf, size_t bufsz, const char *lane_key) {
    if (!buf || bufsz < 14) return;
    const char *p = lane_key ? lane_key : "DEFAULT";
    size_t j = 0;
    const char *prefix = "M4_INJECT_";
    for (; prefix[j] && j < bufsz - 1; j++)
        buf[j] = prefix[j];
    for (; *p && j < bufsz - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            buf[j++] = (char)c;
    }
    buf[j] = '\0';
}

static int session_is_default(const char *session_lane_key) {
    if (!session_lane_key || !session_lane_key[0])
        return 1;
    return strcasecmp(session_lane_key, "DEFAULT") == 0;
}

static void resolve_effective_key(char *out, size_t osz,
                                  const char *session_lane_key,
                                  uint32_t flags,
                                  smart_topic_intent_t st) {
    if (!out || osz == 0) return;
    out[0] = '\0';
    if (!session_is_default(session_lane_key)) {
        strncpy_safe(out, osz, session_lane_key);
        return;
    }
    if ((flags & MODEL_SWITCH_FLAG_MERGE_SMART_TOPIC_INTENT) == 0) {
        strncpy_safe(out, osz, "DEFAULT");
        return;
    }
    strncpy_safe(out, osz, smart_topic_intent_lane_key(st));
}

static int lookup_table(const model_switch_options_t *opts, const char *key,
                        const char **model_out, const char **inject_out,
                        const char **api_url_out, const char **api_key_out) {
    *model_out = *inject_out = NULL;
    *api_url_out = *api_key_out = NULL;
    if (!opts || !opts->lanes || opts->lane_count == 0 || !key)
        return -1;
    for (size_t i = 0; i < opts->lane_count; i++) {
        const char *k = opts->lanes[i].key;
        if (k && strcasecmp(k, key) == 0) {
            *model_out = opts->lanes[i].model;
            *inject_out = opts->lanes[i].inject;
            *api_url_out = opts->lanes[i].api_url;
            *api_key_out = opts->lanes[i].api_key;
            return 0;
        }
    }
    return -1;
}

static const char *resolve_model_string(const model_switch_options_t *opts,
                                        const char *lane_key,
                                        const char *from_table) {
    if (from_table && from_table[0])
        return from_table;
    char envname[96];
    build_model_env_name(envname, sizeof(envname), lane_key);
    const char *e = getenv(envname);
    if (e && e[0])
        return e;
    if (strcasecmp(lane_key, "DEFAULT") == 0) {
        e = getenv("M4_MODEL_DEFAULT");
        if (e && e[0]) return e;
    }
    if (opts && opts->fallback_model && opts->fallback_model[0])
        return opts->fallback_model;
    e = getenv("OLLAMA_MODEL");
    if (e && e[0]) return e;
    return NULL;
}

static const char *resolve_inject_string(const char *lane_key, const char *from_table) {
    if (from_table && from_table[0])
        return from_table;
    char envname[96];
    build_inject_env_name(envname, sizeof(envname), lane_key);
    const char *e = getenv(envname);
    if (e && e[0]) return e;
    if (strcasecmp(lane_key, "DEFAULT") == 0) {
        e = getenv("M4_INJECT_DEFAULT");
        if (e && e[0]) return e;
    }
    return NULL;
}

int model_switch_resolve(const model_switch_options_t *opts,
                         const char *session_lane_key,
                         smart_topic_intent_t st_intent,
                         model_switch_profile_t *out) {
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    out->adaptive_token = 0;

    uint32_t flags = opts ? opts->flags : 0;
    resolve_effective_key(out->lane_key, sizeof(out->lane_key), session_lane_key, flags, st_intent);

    const char *tab_model = NULL;
    const char *tab_inject = NULL;
    const char *tab_url = NULL;
    const char *tab_key = NULL;
    (void)lookup_table(opts, out->lane_key, &tab_model, &tab_inject, &tab_url, &tab_key);

    const char *mid = resolve_model_string(opts, out->lane_key, tab_model);
    if (!mid || !mid[0])
        mid = OLLAMA_DEFAULT_MODEL;
    strncpy_safe(out->model, sizeof(out->model), mid);

    const char *inj = resolve_inject_string(out->lane_key, tab_inject);
    if (inj && inj[0])
        strncpy_safe(out->inject, sizeof(out->inject), inj);

    /* Per-lane endpoint override (direct pin) */
    if (tab_url && tab_url[0])
        strncpy_safe(out->api_url, sizeof(out->api_url), tab_url);
    if (tab_key && tab_key[0])
        strncpy_safe(out->api_key, sizeof(out->api_key), tab_key);

    return 0;
}
