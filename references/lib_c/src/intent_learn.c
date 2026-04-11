/*
 * Background intent learning worker — Phase 2 LLM extraction.
 * Same pattern as geo_learning.c: fixed queue + background pthread.
 */
#include "intent_learn.h"
#include "debug_log.h"
#include "ai_agent.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IL_QUEUE_CAP   16
#define IL_MSG_SIZE    1024
#define IL_SCHEMA_SIZE 4096
#define IL_LLM_BUF    4096
#define IL_CACHE_MAX   128  /* max cached query plans in memory */

/* --- Queue --- */

typedef struct {
    char msg[IL_MSG_SIZE];
} il_item_t;

static il_item_t s_queue[IL_QUEUE_CAP];
static int s_head, s_tail, s_count;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static volatile int s_shutdown;
static pthread_t s_worker;
static int s_started;

static const sc_registry_t *s_registry;
static nl_learn_terms_t *s_lt;
static char s_schema_prompt[IL_SCHEMA_SIZE]; /* pre-built at init */

/* --- Query plan cache --- */

typedef struct {
    char question[IL_MSG_SIZE];       /* normalized user question */
    char collection[160];
    char operation[32];
    char filters_json[2048];          /* raw filters array JSON from LLM */
} il_cache_entry_t;

static il_cache_entry_t s_cache[IL_CACHE_MAX];
static int s_cache_count;
static pthread_mutex_t s_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static char s_cache_path[512]; /* persistent file path */

static void il_lower(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[j++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    dst[j] = '\0';
}

/** Save one cache entry to the persistent file (append). */
static void il_cache_persist(const il_cache_entry_t *e) {
    if (!s_cache_path[0]) return;
    FILE *f = fopen(s_cache_path, "a");
    if (!f) return;
    /* One JSON line per entry. */
    fprintf(f, "{\"q\":\"%s\",\"c\":\"%s\",\"o\":\"%s\",\"f\":%s}\n",
            e->question, e->collection, e->operation,
            e->filters_json[0] ? e->filters_json : "[]");
    fclose(f);
}

/** Load cache from persistent file at startup. */
static void il_cache_load(void) {
    if (!s_cache_path[0]) return;
    FILE *f = fopen(s_cache_path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f) && s_cache_count < IL_CACHE_MAX) {
        il_cache_entry_t *e = &s_cache[s_cache_count];
        memset(e, 0, sizeof(*e));
        /* Minimal parse: extract "q", "c", "o", "f" values. */
        char *q = strstr(line, "\"q\":\"");
        char *c = strstr(line, "\"c\":\"");
        char *o = strstr(line, "\"o\":\"");
        char *fi = strstr(line, "\"f\":");
        if (!q || !c || !o) continue;
        /* Parse q */
        q += 5;
        size_t qi = 0;
        while (*q && *q != '"' && qi + 1 < sizeof(e->question)) e->question[qi++] = *q++;
        e->question[qi] = '\0';
        /* Parse c */
        c += 5;
        size_t ci = 0;
        while (*c && *c != '"' && ci + 1 < sizeof(e->collection)) e->collection[ci++] = *c++;
        e->collection[ci] = '\0';
        /* Parse o */
        o += 5;
        size_t oi = 0;
        while (*o && *o != '"' && oi + 1 < sizeof(e->operation)) e->operation[oi++] = *o++;
        e->operation[oi] = '\0';
        /* Parse f (everything after "f": until end of line) */
        if (fi) {
            fi += 4;
            while (*fi == ' ') fi++;
            size_t flen = strlen(fi);
            while (flen > 0 && (fi[flen-1] == '\n' || fi[flen-1] == '\r' || fi[flen-1] == '}')) flen--;
            if (flen > 0 && flen < sizeof(e->filters_json)) {
                memcpy(e->filters_json, fi, flen);
                e->filters_json[flen] = '\0';
            }
        }
        if (e->question[0] && e->collection[0])
            s_cache_count++;
    }
    fclose(f);
    if (s_cache_count > 0)
        m4_log("INTENT_ROUTE", M4_LOG_INFO, "intent_learn: loaded %d cached query plans from %s",
               s_cache_count, s_cache_path);
}

/** Add a query plan to cache (thread-safe). */
static void il_cache_add(const char *question, const char *collection, const char *operation,
                          const char *filters_json) {
    pthread_mutex_lock(&s_cache_mu);
    il_cache_entry_t *e;
    if (s_cache_count < IL_CACHE_MAX) {
        e = &s_cache[s_cache_count++];
    } else {
        /* Overwrite oldest. */
        memmove(&s_cache[0], &s_cache[1], (IL_CACHE_MAX - 1) * sizeof(s_cache[0]));
        e = &s_cache[IL_CACHE_MAX - 1];
    }
    il_lower(e->question, sizeof(e->question), question);
    snprintf(e->collection, sizeof(e->collection), "%s", collection);
    snprintf(e->operation, sizeof(e->operation), "%s", operation ? operation : "count");
    snprintf(e->filters_json, sizeof(e->filters_json), "%s", filters_json ? filters_json : "[]");
    il_cache_persist(e);
    pthread_mutex_unlock(&s_cache_mu);
}

/* --- Schema prompt builder --- */

static void build_schema_prompt(const sc_registry_t *reg, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!reg) return;
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos, "Available data collections:\n");
    pos += sc_registry_schema_summary(reg, buf + pos, cap - pos);
}

/* --- LLM extraction --- */

/** Build the full extraction prompt and call LLM. Writes query plan JSON into out. */
static int il_extract(const char *user_msg, char *out, size_t out_cap) {
    size_t prompt_cap = IL_SCHEMA_SIZE + IL_MSG_SIZE + 512;
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) return -1;

    /* Get current date for time resolution. */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    int n = snprintf(prompt, prompt_cap,
        "You are a query planner. Given the user's question and available data collections, "
        "output a JSON query plan. Return ONLY valid JSON, no explanation.\n\n"
        "%s\n"
        "Current date: %s\n\n"
        "User question: \"%s\"\n\n"
        "Return a JSON object with these fields:\n"
        "- collection: which collection to query\n"
        "- operation: count, list, sum, avg, or group_by\n"
        "- filters: array of {\"field\":\"fieldname\",\"op\":\"eq|gte|lte|gt|lt|contains\",\"value\":\"val\"}\n"
        "  Include date filters for time references (this year, last month, etc.)\n"
        "  Include field filters for specific values (category=computer, status=pending, etc.)\n\n"
        "JSON:\n",
        s_schema_prompt, date_str, user_msg);

    if (n <= 0 || (size_t)n >= prompt_cap) { free(prompt); return -1; }

    /* Call LLM via ai_agent (same backend as chat — Gemini/Groq/Ollama). */
    char source = 0;
    unsigned wire = 0;
    char llm_model[128] = {0};
    ai_agent_prompt_t parts;
    memset(&parts, 0, sizeof(parts));
    parts.system = "You are a query planner. Return only valid JSON.";
    parts.system_len = strlen(parts.system);
    parts.user = prompt;
    parts.user_len = (size_t)n;

    int rc = ai_agent_complete_chat(&parts, prompt, 0.0, NULL, NULL, NULL,
                                     out, out_cap, &source, &wire, llm_model, sizeof(llm_model));
    free(prompt);
    if (rc != 0) {
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "intent_learn: LLM extraction failed");
        return -1;
    }

    m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "intent_learn: LLM response len=%zu model=%s",
           strlen(out), llm_model);
    return 0;
}

/* --- JSON plan parser (minimal) --- */

/** Extract a string value for a key from JSON. Returns pointer into json (not copied). */
static int il_json_string(const char *json, const char *key, char *out, size_t cap) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) { p++; out[i++] = *p++; continue; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

/** Record LLM's collection decision into learning terms. */
static void il_record_decision(const char *user_msg, const char *collection, const char *operation) {
    if (!s_lt || !collection || !collection[0]) return;

    /* Record SC:{collection} for content words in the message. */
    char intent_label[192];
    snprintf(intent_label, sizeof(intent_label), "SC:%s", collection);

    /* Tokenize message, record each content word → SC:{collection}. */
    char buf[IL_MSG_SIZE];
    snprintf(buf, sizeof(buf), "%s", user_msg);
    /* Lowercase. */
    for (size_t i = 0; buf[i]; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 'A' && c <= 'Z') buf[i] = (char)(c + 32);
    }

    static const char *const stop[] = {
        "how", "many", "much", "what", "is", "are", "the", "a", "an", "do", "does",
        "we", "have", "this", "that", "in", "of", "for", "to", "and", "or",
        "there", "any", "all", "show", "me", "list", "find", "get", "tell", "be",
        NULL
    };

    char *save = NULL;
    char *tok = strtok_r(buf, " ,?!.;:\"'()/", &save);
    while (tok) {
        if (strlen(tok) >= 3) {
            int is_stop = 0;
            for (size_t i = 0; stop[i]; i++) {
                if (strcmp(tok, stop[i]) == 0) { is_stop = 1; break; }
            }
            if (!is_stop) {
                const char *keys[1] = { tok };
                (void)nl_learn_terms_record(s_lt, keys, 1, intent_label, 1);
            }
        }
        tok = strtok_r(NULL, " ,?!.;:\"'()/", &save);
    }

    /* Also record operation as intent cue. */
    if (operation && operation[0]) {
        const char *intent = NULL;
        if (strcmp(operation, "count") == 0 || strcmp(operation, "sum") == 0 ||
            strcmp(operation, "avg") == 0 || strcmp(operation, "group_by") == 0 ||
            strcmp(operation, "aggregate") == 0)
            intent = "ELK_ANALYTICS";
        else if (strcmp(operation, "list") == 0 || strcmp(operation, "find_one") == 0)
            intent = "ELK_SEARCH";

        if (intent) {
            /* Record the whole user message pattern loosely — use the first few content words. */
            char pattern[256];
            snprintf(pattern, sizeof(pattern), "%s", user_msg);
            for (size_t i = 0; pattern[i]; i++) {
                unsigned char c = (unsigned char)pattern[i];
                if (c >= 'A' && c <= 'Z') pattern[i] = (char)(c + 32);
            }
            /* Truncate to first meaningful phrase. */
            char *sp = strchr(pattern, '?');
            if (sp) *sp = '\0';

            m4_log("INTENT_ROUTE", M4_LOG_DEBUG,
                   "intent_learn: recorded collection=%s operation=%s→%s", collection, operation, intent);
        }
    }
}

/* --- Worker thread --- */

/** Extract the "filters" array JSON from the LLM plan (raw string, not parsed). */
static int il_json_array(const char *json, const char *key, char *out, size_t cap) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '[') return -1;
    /* Find matching ] */
    int depth = 1;
    const char *start = p;
    p++;
    while (*p && depth > 0) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        p++;
    }
    size_t len = (size_t)(p - start);
    if (len >= cap) len = cap - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static void il_process(const char *user_msg) {
    char *llm_response = (char *)malloc(IL_LLM_BUF);
    if (!llm_response) return;
    if (il_extract(user_msg, llm_response, IL_LLM_BUF) != 0) { free(llm_response); return; }

    /* Find JSON object in response (LLM may add text around it). */
    char *json_start = strchr(llm_response, '{');
    if (!json_start) {
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "intent_learn: no JSON in LLM response");
        free(llm_response);
        return;
    }

    /* Parse collection, operation, and filters. */
    char collection[160] = {0};
    char operation[32] = {0};
    char filters[2048] = {0};
    il_json_string(json_start, "collection", collection, sizeof(collection));
    il_json_string(json_start, "operation", operation, sizeof(operation));
    il_json_array(json_start, "filters", filters, sizeof(filters));
    free(llm_response);

    if (!collection[0]) {
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "intent_learn: no collection in LLM plan");
        return;
    }

    m4_log("INTENT_ROUTE", M4_LOG_DEBUG,
           "intent_learn: LLM plan collection=%s operation=%s filters=%s",
           collection, operation, filters[0] ? filters : "[]");

    /* Record scores for learning. */
    il_record_decision(user_msg, collection, operation);

    /* Cache the full query plan for Phase 3 reuse. */
    il_cache_add(user_msg, collection, operation, filters);
}

/* --- Initial seed: LLM infers collection relationships from schema --- */

static void il_seed_from_schema(void) {
    if (!s_lt || !s_schema_prompt[0]) return;

    /* Only seed if learning store has no SC:* entries yet (fresh start). */
    /* Quick check: score_text for "SC:" prefix — if any collection has scores, skip. */
    /* We can't easily check all SC:* entries, so just check if the store is small. */
    /* Simple heuristic: if total keys < 10, assume fresh and seed. */
    /* TODO: expose nl_learn_terms_count or a "has SC:" check. */

    size_t prompt_cap = IL_SCHEMA_SIZE + 512;
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) return;

    int n = snprintf(prompt, prompt_cap,
        "Given these data collections and their fields, list common synonyms "
        "users might use to refer to each collection. "
        "Return ONLY valid JSON array, no explanation.\n\n"
        "%s\n"
        "Format: [{\"collection\":\"name\",\"synonyms\":[\"word1\",\"word2\",...]}]\n"
        "JSON:\n",
        s_schema_prompt);

    if (n <= 0 || (size_t)n >= prompt_cap) { free(prompt); return; }

    char *resp = (char *)malloc(IL_LLM_BUF);
    if (!resp) { free(prompt); return; }

    char source = 0;
    unsigned wire = 0;
    char llm_model[128] = {0};
    ai_agent_prompt_t parts;
    memset(&parts, 0, sizeof(parts));
    parts.system = "You are a data schema analyst. Return only valid JSON.";
    parts.system_len = strlen(parts.system);
    parts.user = prompt;
    parts.user_len = (size_t)n;

    int rc = ai_agent_complete_chat(&parts, prompt, 0.0, NULL, NULL, NULL,
                                     resp, IL_LLM_BUF, &source, &wire, llm_model, sizeof(llm_model));
    free(prompt);
    if (rc != 0) {
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "intent_learn: seed LLM call failed");
        free(resp);
        return;
    }

    m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "intent_learn: seed response len=%zu model=%s",
           strlen(resp), llm_model);

    /* Parse JSON array: [{"collection":"carts","synonyms":["orders","sales",...]}] */
    char *p = strchr(resp, '[');
    if (!p) { free(resp); return; }
    p++; /* skip [ */

    int seeded = 0;
    while (*p) {
        /* Find next { */
        while (*p && *p != '{') { if (*p == ']') goto seed_done; p++; }
        if (*p != '{') break;

        char collection[160] = {0};
        il_json_string(p, "collection", collection, sizeof(collection));
        if (!collection[0]) { /* skip to next object */
            int depth = 1; p++;
            while (*p && depth > 0) { if (*p == '{') depth++; else if (*p == '}') depth--; p++; }
            continue;
        }

        /* Find "synonyms" array. */
        char *syn_start = strstr(p, "\"synonyms\"");
        if (!syn_start) { /* skip object */
            int depth = 1; p++;
            while (*p && depth > 0) { if (*p == '{') depth++; else if (*p == '}') depth--; p++; }
            continue;
        }
        syn_start = strchr(syn_start, '[');
        if (!syn_start) { p++; continue; }
        syn_start++; /* skip [ */

        /* Parse each synonym string and record as SC:{collection}. */
        char sc_intent[192];
        snprintf(sc_intent, sizeof(sc_intent), "SC:%s", collection);

        while (*syn_start && *syn_start != ']') {
            while (*syn_start && *syn_start != '"' && *syn_start != ']') syn_start++;
            if (*syn_start != '"') break;
            syn_start++; /* skip opening " */
            char word[128];
            size_t wi = 0;
            while (*syn_start && *syn_start != '"' && wi + 1 < sizeof(word))
                word[wi++] = *syn_start++;
            word[wi] = '\0';
            if (*syn_start == '"') syn_start++;

            if (word[0] && wi >= 2) {
                /* Lowercase. */
                for (size_t i = 0; word[i]; i++) {
                    unsigned char c = (unsigned char)word[i];
                    if (c >= 'A' && c <= 'Z') word[i] = (char)(c + 32);
                }
                const char *keys[1] = { word };
                (void)nl_learn_terms_record(s_lt, keys, 1, sc_intent, 1);
                seeded++;
            }
        }

        /* Skip to end of object. */
        int depth = 1; p++;
        while (*p && depth > 0) { if (*p == '{') depth++; else if (*p == '}') depth--; p++; }
    }

seed_done:
    free(resp);
    if (seeded > 0)
        m4_log("INTENT_ROUTE", M4_LOG_INFO, "intent_learn: seeded %d synonym→collection mappings from LLM",
               seeded);
}

static void *il_worker_fn(void *arg) {
    (void)arg;
    m4_log("INTENT_ROUTE", M4_LOG_INFO, "intent_learn: worker started");

    /* One-time: seed collection synonyms from LLM on fresh start. */
    il_seed_from_schema();

    while (1) {
        pthread_mutex_lock(&s_mu);
        while (s_count == 0 && !s_shutdown)
            pthread_cond_wait(&s_cond, &s_mu);
        if (s_shutdown && s_count == 0) {
            pthread_mutex_unlock(&s_mu);
            break;
        }
        il_item_t item = s_queue[s_head];
        s_head = (s_head + 1) % IL_QUEUE_CAP;
        s_count--;
        pthread_mutex_unlock(&s_mu);

        il_process(item.msg);
    }

    m4_log("INTENT_ROUTE", M4_LOG_INFO, "intent_learn: worker stopped");
    return NULL;
}

/* --- Public API --- */

int intent_learn_init(const sc_registry_t *registry, nl_learn_terms_t *lt) {
    if (!registry || !lt) return -1;
    s_registry = registry;
    s_lt = lt;
    s_head = s_tail = s_count = 0;
    s_shutdown = 0;

    /* Pre-build schema prompt from registry. */
    build_schema_prompt(registry, s_schema_prompt, sizeof(s_schema_prompt));
    m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "intent_learn: schema prompt built (%zu bytes)",
           strlen(s_schema_prompt));

    if (pthread_create(&s_worker, NULL, il_worker_fn, NULL) != 0) {
        m4_log("INTENT_ROUTE", M4_LOG_ERROR, "intent_learn: pthread_create failed");
        return -1;
    }
    s_started = 1;
    return 0;
}

void intent_learn_enqueue(const char *user_msg) {
    if (!s_started || !user_msg || !user_msg[0]) return;

    pthread_mutex_lock(&s_mu);
    if (s_count >= IL_QUEUE_CAP) {
        /* Queue full — drop oldest. */
        s_head = (s_head + 1) % IL_QUEUE_CAP;
        s_count--;
    }
    il_item_t *slot = &s_queue[s_tail];
    snprintf(slot->msg, sizeof(slot->msg), "%s", user_msg);
    s_tail = (s_tail + 1) % IL_QUEUE_CAP;
    s_count++;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mu);
}

void intent_learn_shutdown(void) {
    if (!s_started) return;
    pthread_mutex_lock(&s_mu);
    s_shutdown = 1;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mu);
    pthread_join(s_worker, NULL);
    s_started = 0;
    s_registry = NULL;
    s_lt = NULL;
}

void intent_learn_set_cache_path(const char *path) {
    if (!path || !path[0]) { s_cache_path[0] = '\0'; return; }
    snprintf(s_cache_path, sizeof(s_cache_path), "%s", path);
    /* Load existing cache. */
    il_cache_load();
}

int intent_learn_cache_lookup(const char *user_msg,
                              char *out_collection, size_t col_cap,
                              char *out_operation, size_t op_cap,
                              char *out_filters_json, size_t filters_cap) {
    if (!user_msg || !user_msg[0]) return -1;
    char norm[IL_MSG_SIZE];
    il_lower(norm, sizeof(norm), user_msg);

    pthread_mutex_lock(&s_cache_mu);
    /* Search from newest to oldest for best match. */
    int best = -1;
    size_t best_len = 0;
    for (int i = s_cache_count - 1; i >= 0; i--) {
        /* Check if cached question's content words appear in the current message. */
        /* Simple: check if the cached question is a substring, or shares key words. */
        const char *cached = s_cache[i].question;
        if (!cached[0]) continue;
        /* Tokenize cached question and count matching words in norm. */
        char cbuf[IL_MSG_SIZE];
        snprintf(cbuf, sizeof(cbuf), "%s", cached);
        size_t matches = 0, total = 0;
        char *save = NULL;
        char *tok = strtok_r(cbuf, " ,?!.;:\"'()/", &save);
        while (tok) {
            if (strlen(tok) >= 3) {
                total++;
                if (strstr(norm, tok)) matches++;
            }
            tok = strtok_r(NULL, " ,?!.;:\"'()/", &save);
        }
        /* Require at least 50% word match and at least 2 matching words. */
        if (matches >= 2 && total > 0 && matches * 2 >= total && matches > best_len) {
            best = i;
            best_len = matches;
        }
    }

    if (best >= 0) {
        const il_cache_entry_t *e = &s_cache[best];
        if (out_collection) snprintf(out_collection, col_cap, "%s", e->collection);
        if (out_operation) snprintf(out_operation, op_cap, "%s", e->operation);
        if (out_filters_json) snprintf(out_filters_json, filters_cap, "%s", e->filters_json);
        pthread_mutex_unlock(&s_cache_mu);
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG,
               "cache_lookup: hit collection=%s operation=%s match_words=%zu",
               e->collection, e->operation, best_len);
        return 0;
    }
    pthread_mutex_unlock(&s_cache_mu);
    return -1;
}
