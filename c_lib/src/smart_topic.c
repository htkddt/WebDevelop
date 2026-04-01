/*
 * Smart topic (mini AI switch) — implementation per .cursor/smart_topic_ai_switch.md.
 * Public API: initial_smart_topic(), get_smart_topic().
 */

#include "smart_topic.h"
#include "ollama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SMART_TOPIC_RESULT_MAX  4096
#define COLLECTION_NAME_MAX     63

#define LOG(level, component, context, msg, ...) \
    fprintf(stderr, "[" level "] " component " " context " " msg " (%s:%d)\n", ##__VA_ARGS__, __FILE__, __LINE__)

static int valid_collection_name(const char *s) {
    if (!s || !*s) return 0;
    size_t n = 0;
    for (; *s && n <= COLLECTION_NAME_MAX; s++, n++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
            (*s >= '0' && *s <= '9') || *s == '_')
            continue;
        return 0;
    }
    return (*s == '\0' && n >= 1 && n <= COLLECTION_NAME_MAX) ? 1 : 0;
}

#define TINY_MODEL_NAME_MAX 63
static struct {
    int initialized;
    int enabled;
    char last_result[SMART_TOPIC_RESULT_MAX];
    char collection[COLLECTION_NAME_MAX + 1];
    char tiny_model[TINY_MODEL_NAME_MAX + 1];  /* for micro-query intent */
    int ollama_available;  /* 1 = ok, 0 = unavailable after pre-query */
} g_smart_topic;

/* Micro-prompt: one word → lane key for model_switch (see model_switch.h). */
static const char INTENT_PROMPT[] =
    "Classify this user message into exactly one word: "
    "TECH (C, code, Mongo, programming), "
    "CHAT (greeting, small talk), "
    "EDUCATION (learning, school, science, homework, how to study), "
    "BUSINESS (work, company, career, finance), "
    "or DEFAULT. "
    "Reply only with one word: TECH, CHAT, EDUCATION, BUSINESS, or DEFAULT.\n\nUser: ";

static const char *model_for_type(mini_ai_type_t t, const char *override_tiny, const char *override_b2) {
    switch (t) {
        case MINI_AI_TYPE_TINY:  return (override_tiny && override_tiny[0]) ? override_tiny : "tinyllama";
        case MINI_AI_TYPE_B2:    return (override_b2 && override_b2[0]) ? override_b2 : "phi2";
        case MINI_AI_TYPE_SMALL:
        case MINI_AI_TYPE_COUNT:
        default:                 return "tinyllama";
    }
}

int initial_smart_topic(const smart_topic_options_t *opts, struct storage_ctx *storage) {
    (void)storage;
    memset(&g_smart_topic, 0, sizeof(g_smart_topic));
    g_smart_topic.initialized = 1;

    if (!opts) {
        LOG("ERROR", "smart_topic", "init", "opts is NULL");
        return -1;
    }

    if (!opts->enable) {
        g_smart_topic.enabled = 0;
        strncpy(g_smart_topic.last_result, "disabled", sizeof(g_smart_topic.last_result) - 1);
        g_smart_topic.last_result[sizeof(g_smart_topic.last_result) - 1] = '\0';
        return 0;
    }

    /* Resolve collection: mini_ai_collection if valid, else smart_topic. */
    if (opts->mini_ai_collection && opts->mini_ai_collection[0] && valid_collection_name(opts->mini_ai_collection)) {
        strncpy(g_smart_topic.collection, opts->mini_ai_collection, COLLECTION_NAME_MAX);
        g_smart_topic.collection[COLLECTION_NAME_MAX] = '\0';
    } else {
        strncpy(g_smart_topic.collection, SMART_TOPIC_COLLECTION_DEFAULT, COLLECTION_NAME_MAX);
        g_smart_topic.collection[COLLECTION_NAME_MAX] = '\0';
    }

    g_smart_topic.enabled = 1;

    /* Store tiny model name for micro-query intent. */
    {
        const char *model = model_for_type(opts->library_type, opts->model_tiny, opts->model_b2);
        strncpy(g_smart_topic.tiny_model, model, TINY_MODEL_NAME_MAX);
        g_smart_topic.tiny_model[TINY_MODEL_NAME_MAX] = '\0';
    }

    /* Pre-query Ollama to detect connection early. */
    {
        char buf[256];
        int ret = ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, g_smart_topic.tiny_model, "Hi", buf, sizeof(buf));
        if (ret == 0 && buf[0] != '\0') {
            g_smart_topic.ollama_available = 1;
            strncpy(g_smart_topic.last_result, "enabled", sizeof(g_smart_topic.last_result) - 1);
            g_smart_topic.last_result[sizeof(g_smart_topic.last_result) - 1] = '\0';
            LOG("INFO", "smart_topic", "init", "enabled library_type=%d model=%s", (int)opts->library_type, g_smart_topic.tiny_model);
        } else {
            g_smart_topic.ollama_available = 0;
            strncpy(g_smart_topic.last_result, "ollama_unavailable", sizeof(g_smart_topic.last_result) - 1);
            g_smart_topic.last_result[sizeof(g_smart_topic.last_result) - 1] = '\0';
            LOG("WARN", "smart_topic", "pre_query", "Ollama unreachable; using cache-only");
        }
    }

    return 0;
}

int get_smart_topic(char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';

    if (!g_smart_topic.initialized) return -1;
    if (!g_smart_topic.enabled) {
        strncpy(out, "disabled", out_size - 1);
        out[out_size - 1] = '\0';
        return 0;
    }

    size_t len = strlen(g_smart_topic.last_result);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, g_smart_topic.last_result, len);
    out[len] = '\0';
    return 0;
}

/* Parse intent from micro-query response. */
static double intent_to_temperature(const char *response) {
    if (!response) return 0.5;
    const char *p = response;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncasecmp(p, "TECH", 4) == 0) return 0.0;
    if (strncasecmp(p, "CHAT", 4) == 0) return 0.8;
    if (strncasecmp(p, "EDUCATION", 9) == 0) return 0.35;
    if (strncasecmp(p, "BUSINESS", 8) == 0) return 0.35;
    return 0.5;
}

static smart_topic_intent_t intent_to_enum(const char *response) {
    if (!response) return SMART_TOPIC_INTENT_DEFAULT;
    const char *p = response;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    /* Longer labels first */
    if (strncasecmp(p, "EDUCATION", 9) == 0) return SMART_TOPIC_INTENT_EDUCATION;
    if (strncasecmp(p, "BUSINESS", 8) == 0) return SMART_TOPIC_INTENT_BUSINESS;
    if (strncasecmp(p, "TECH", 4) == 0) return SMART_TOPIC_INTENT_TECH;
    if (strncasecmp(p, "CHAT", 4) == 0) return SMART_TOPIC_INTENT_CHAT;
    return SMART_TOPIC_INTENT_DEFAULT;
}

/** One micro-query; writes raw reply into intent_buf. Returns 0 if intent_buf filled (maybe empty on error). */
static int run_intent_micro_query(const char *query, char *intent_buf, size_t intent_buf_size) {
    if (!intent_buf || intent_buf_size == 0) return -1;
    intent_buf[0] = '\0';
    if (!g_smart_topic.initialized || !g_smart_topic.enabled) return -1;
    if (!query) query = "";

    char micro_prompt[1024];
    size_t qlen = strnlen(query, 400);
    int n = snprintf(micro_prompt, sizeof(micro_prompt), "%.*s%.*s",
                     (int)(sizeof(INTENT_PROMPT) - 1), INTENT_PROMPT, (int)qlen, query);
    if (n <= 0 || (size_t)n >= sizeof(micro_prompt))
        return -1;

    if (ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, g_smart_topic.tiny_model,
                     micro_prompt, intent_buf, intent_buf_size) != 0)
        return -1;
    return 0;
}

int smart_topic_classify_for_query(const char *query, double *out_temperature, smart_topic_intent_t *out_intent) {
    if (out_temperature) *out_temperature = 0.5;
    if (out_intent) *out_intent = SMART_TOPIC_INTENT_DEFAULT;

    char intent_buf[64];
    memset(intent_buf, 0, sizeof(intent_buf));
    if (run_intent_micro_query(query, intent_buf, sizeof(intent_buf)) != 0)
        return 0;

    if (out_temperature) *out_temperature = intent_to_temperature(intent_buf);
    if (out_intent) *out_intent = intent_to_enum(intent_buf);
    return 0;
}

int smart_topic_temperature_for_query(const char *query, double *out_temperature) {
    if (!out_temperature) return -1;
    return smart_topic_classify_for_query(query, out_temperature, NULL);
}
