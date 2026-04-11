/*
 * AI Agent: LLM routing (cloud pool + local Ollama fallback) — libcurl, same process as api_chat.
 * Spec: .cursor/models/ai_agent.md
 */
#include "api.h"
#include "ai_agent.h"
#include "ollama.h"
#include "debug_log.h"
#include <curl/curl.h>

/* Sync with python_ai/m4_debug_env.M4_ENV_LOG_CLOUD_HTTP_FULL */
#define M4_ENV_LOG_CLOUD_HTTP_FULL "M4_LOG_CLOUD_HTTP_FULL"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Dynamic prompt helpers --- */

void ai_agent_prompt_init(ai_agent_prompt_t *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

void ai_agent_prompt_set_system(ai_agent_prompt_t *p, const char *text) {
    if (!p) return;
    free(p->system);
    if (text && text[0]) {
        p->system = strdup(text);
        p->system_len = p->system ? strlen(p->system) : 0;
    } else {
        p->system = NULL;
        p->system_len = 0;
    }
}

void ai_agent_prompt_set_user(ai_agent_prompt_t *p, const char *text) {
    if (!p) return;
    free(p->user);
    if (text && text[0]) {
        p->user = strdup(text);
        p->user_len = p->user ? strlen(p->user) : 0;
    } else {
        p->user = NULL;
        p->user_len = 0;
    }
}

void ai_agent_prompt_add_history(ai_agent_prompt_t *p, const char *role, const char *content) {
    if (!p || !role || !content) return;
    if (p->history_count >= p->history_cap) {
        int new_cap = p->history_cap ? p->history_cap * 2 : 16;
        struct ai_agent_prompt_msg *nh = (struct ai_agent_prompt_msg *)realloc(
            p->history, (size_t)new_cap * sizeof(struct ai_agent_prompt_msg));
        if (!nh) return;
        p->history = nh;
        p->history_cap = new_cap;
    }
    struct ai_agent_prompt_msg *m = &p->history[p->history_count];
    snprintf(m->role, sizeof(m->role), "%s", role);
    m->content = strdup(content);
    p->history_count++;
}

void ai_agent_prompt_free(ai_agent_prompt_t *p) {
    if (!p) return;
    free(p->system);
    free(p->user);
    if (p->history) {
        for (int i = 0; i < p->history_count; i++)
            free(p->history[i].content);
        free(p->history);
    }
    memset(p, 0, sizeof(*p));
}

/* --- */

typedef struct {
    char *data;
    size_t size;
    size_t used;
} cl_buf_t;

static size_t cl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    cl_buf_t *b = (cl_buf_t *)userdata;
    size_t n = size * nmemb;
    if (b->used + n >= b->size) n = b->size - b->used - 1;
    if (n) {
        memcpy(b->data + b->used, ptr, n);
        b->used += n;
        b->data[b->used] = '\0';
    }
    return size * nmemb;
}

static void json_escape_in(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t j = 0;
    for (; in && *in && j < out_size - 1; in++) {
        if (*in == '"' || *in == '\\') {
            if (j + 2 >= out_size) break;
            out[j++] = '\\';
            out[j++] = *in;
        } else if (*in == '\n') {
            if (j + 2 >= out_size) break;
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (*in == '\r') {
            if (j + 2 >= out_size) break;
            out[j++] = '\\';
            out[j++] = 'r';
        } else {
            out[j++] = *in;
        }
    }
    out[j] = '\0';
}

/**
 * Build OpenAI-compatible JSON body from structured prompt parts.
 * Returns heap-allocated body string (caller must free), or NULL on error.
 * Format: {"model":"...", "messages":[{"role":"system","content":"..."}, {"role":"user","content":"..."}, ...], "temperature":...}
 */
static char *build_openai_messages_body(const ai_agent_prompt_t *p, const char *model, double temperature) {
    if (!p || !model || !model[0]) return NULL;
    const char *sys = p->system ? p->system : "";
    const char *usr = p->user ? p->user : "";

    /* Estimate buffer size: system + history + user, all escaped */
    size_t est = strlen(model) + 512;
    size_t sys_len = strlen(sys);
    est += sys_len * 4 + 64;
    for (int i = 0; i < p->history_count; i++)
        est += (p->history[i].content ? strlen(p->history[i].content) : 0) * 4 + 64;
    est += strlen(usr) * 4 + 64;

    char *body = (char *)malloc(est);
    if (!body) return NULL;

    /* Temp buffers for escaped text */
    size_t esc_cap = sys_len * 4 + 64;
    for (int i = 0; i < p->history_count; i++) {
        size_t cl = (p->history[i].content ? strlen(p->history[i].content) : 0) * 4 + 64;
        if (cl > esc_cap) esc_cap = cl;
    }
    size_t ul = strlen(usr) * 4 + 64;
    if (ul > esc_cap) esc_cap = ul;

    char *esc = (char *)malloc(esc_cap);
    if (!esc) { free(body); return NULL; }

    size_t n = 0;
    n += (size_t)snprintf(body + n, est - n, "{\"model\":\"%s\",\"messages\":[", model);

    /* System message */
    json_escape_in(sys, esc, esc_cap);
    n += (size_t)snprintf(body + n, est - n, "{\"role\":\"system\",\"content\":\"%s\"}", esc);

    /* History messages */
    for (int i = 0; i < p->history_count; i++) {
        json_escape_in(p->history[i].content ? p->history[i].content : "", esc, esc_cap);
        n += (size_t)snprintf(body + n, est - n, ",{\"role\":\"%s\",\"content\":\"%s\"}",
                              p->history[i].role, esc);
    }

    /* Current user message */
    json_escape_in(usr, esc, esc_cap);
    n += (size_t)snprintf(body + n, est - n, ",{\"role\":\"user\",\"content\":\"%s\"}]", esc);

    /* Temperature */
    if (temperature >= 0.0)
        n += (size_t)snprintf(body + n, est - n, ",\"temperature\":%.4f", temperature);
    n += (size_t)snprintf(body + n, est - n, "}");

    free(esc);
    return body;
}

static int extract_json_string_after(const char *body, const char *needle, char *out, size_t cap) {
    const char *p = strstr(body, needle);
    if (!p) return -1;
    p += strlen(needle);
    size_t i = 0;
    while (*p && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n')
                out[i++] = '\n';
            else if (*p == 'r')
                out[i++] = '\r';
            else if (*p == 't')
                out[i++] = '\t';
            else
                out[i++] = *p;
            p++;
            continue;
        }
        if (*p == '"') break;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* OpenAI-style chat.completions: first "content":" after "choices". */
static int extract_openai_assistant(const char *body, char *out, size_t cap) {
    const char *scope = strstr(body, "\"choices\"");
    if (!scope) scope = body;
    return extract_json_string_after(scope, "\"content\":\"", out, cap);
}

/* Gemini generateContent: first "text":" */
static int extract_gemini_text(const char *body, char *out, size_t cap) {
    /* Gemini JSON may have "text":"..." or "text": "..." (with space) */
    int r = extract_json_string_after(body, "\"text\":\"", out, cap);
    if (r != 0) r = extract_json_string_after(body, "\"text\": \"", out, cap);
    return r;
}

static void cl_write_llm_label(char *out, size_t cap, const char *provider, const char *model) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!provider || !model || !model[0])
        return;
    snprintf(out, cap, "%s:%s", provider, model);
}

static void ollama_completion_label(char *out, size_t cap, const char *model_pin) {
    if (!out || cap == 0)
        return;
    const char *m = model_pin && model_pin[0] ? model_pin : getenv("OLLAMA_MODEL");
    if (m && m[0])
        snprintf(out, cap, "%s:%s", API_LLM_ROUTE_PREFIX_OLLAMA, m);
    else
        snprintf(out, cap, "%s:%s", API_LLM_ROUTE_PREFIX_OLLAMA, OLLAMA_DEFAULT_MODEL);
}

/*
 * Free-tier provider limits — used to auto-trim prompts before calling each tier.
 *
 * These are APPROXIMATE and may change over time. Check vendor docs periodically:
 *   Groq:     https://console.groq.com/docs/rate-limits
 *   Cerebras: https://inference-docs.cerebras.ai/
 *   Gemini:   https://ai.google.dev/gemini-api/docs/rate-limits
 *   Ollama:   Local — limited by model context window, not quota.
 *
 * Last verified: 2026-04 (update when vendors change free-tier limits).
 *
 * prompt_max_chars is a conservative char estimate (~4 chars per token).
 * We trim the prompt to fit, prioritizing: user message > recent history > system.
 */
static const ai_agent_provider_limits_t PROVIDER_LIMITS[] = {
    /* name        ctx_window  prompt_max_chars  daily_input  save_quota */
    { "groq",         8192,      20000,              0,           0 },  /* 8K ctx, generous daily */
    { "cerebras",     8192,      20000,              0,           0 },  /* 8K ctx, varies by plan */
    { "gemini",    1048576,       8000,        1500000,           1 },  /* huge ctx, but daily quota is low on free tier → trim to save */
    { "ollama",       2048,       5000,              0,           0 },  /* local 1B model: small ctx */
};
static const int PROVIDER_LIMITS_COUNT = (int)(sizeof(PROVIDER_LIMITS) / sizeof(PROVIDER_LIMITS[0]));

const ai_agent_provider_limits_t *ai_agent_ai_agent_get_provider_limits(const char *name) {
    for (int i = 0; i < PROVIDER_LIMITS_COUNT; i++) {
        if (strcmp(PROVIDER_LIMITS[i].name, name) == 0)
            return &PROVIDER_LIMITS[i];
    }
    return NULL;
}

/* Estimate char count of structured prompt */

static int cl_ai_agent_diag(void) {
    /* Check unified debug log system first */
    if (m4_log_debug_enabled("ai_agent")) return 1;
    /* Fallback: legacy env var */
    const char *e = getenv("M4_LOG_CLOUD_LLM");
    if (!e || !e[0]) return 0;
    char b[16];
    size_t i = 0;
    for (; e[i] && i < sizeof(b) - 1; i++)
        b[i] = (char)tolower((unsigned char)e[i]);
    b[i] = '\0';
    if (strcmp(b, "1") == 0 || strcmp(b, "true") == 0 || strcmp(b, "yes") == 0 || strcmp(b, "verbose") == 0)
        return 1;
    return 0;
}

/* DEBUG ONLY: prints full URL, raw token in Authorization / query, and JSON body to stderr. */
static int cl_ai_agent_log_full_http(void) {
    const char *e = getenv(M4_ENV_LOG_CLOUD_HTTP_FULL);
    if (!e || !e[0]) return 0;
    char b[16];
    size_t i = 0;
    for (; e[i] && i < sizeof(b) - 1; i++)
        b[i] = (char)tolower((unsigned char)e[i]);
    b[i] = '\0';
    return (strcmp(b, "1") == 0 || strcmp(b, "true") == 0 || strcmp(b, "yes") == 0);
}

static void cl_sanitize_resp_prefix(char *dst, size_t dst_cap, const char *src, size_t max_take) {
    if (!dst || dst_cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t j = 0;
    for (size_t i = 0; src[i] && i < max_take && j + 1 < dst_cap; i++) {
        char c = src[i];
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        dst[j++] = c;
    }
    dst[j] = '\0';
}

/* Trim, strip wrapping quotes, strip leading "Bearer " (env sometimes copies full header → double Bearer). */
static int cl_key_prefix_ci(const char *s, const char *pre) {
    for (; *pre; pre++, s++) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre))
            return 0;
    }
    return 1;
}

static void cl_cloud_api_key_normalize(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!raw)
        return;
    while (*raw && isspace((unsigned char)*raw))
        raw++;
    size_t L = strlen(raw);
    while (L > 0 && isspace((unsigned char)raw[L - 1]))
        L--;
    if (L >= 2 && raw[0] == '"' && raw[L - 1] == '"') {
        raw++;
        L -= 2;
    } else if (L >= 2 && raw[0] == '\'' && raw[L - 1] == '\'') {
        raw++;
        L -= 2;
    }
    if (L >= cap)
        L = cap - 1;
    memcpy(out, raw, L);
    out[L] = '\0';
    if (strlen(out) >= 7 && cl_key_prefix_ci(out, "bearer "))
        memmove(out, out + 7, strlen(out + 7) + 1);
}

static int cl_backend_mode(void) {
    const char *e = getenv("M4_CHAT_BACKEND");
    if (!e || !e[0]) return 1; /* cloud_then_ollama */
    char buf[32];
    size_t i = 0;
    for (; e[i] && i < sizeof(buf) - 1; i++) buf[i] = (char)tolower((unsigned char)e[i]);
    buf[i] = '\0';
    if (strcmp(buf, API_CHAT_BACKEND_ENV_OLLAMA) == 0) return 0;
    if (strcmp(buf, API_CHAT_BACKEND_ENV_CLOUD) == 0) return 2;
    return 1; /* cloud_then_ollama or unknown */
}

static int http_post(const char *url, const char *hdr1, const char *hdr2, const char *body, long *http_code,
                     char *resp, size_t resp_cap, CURLcode *curl_out) {
    cl_buf_t b = {.data = resp, .size = resp_cap, .used = 0};
    if (resp_cap) resp[0] = '\0';
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (curl_out) *curl_out = CURLE_FAILED_INIT;
        return -1;
    }
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (hdr1) headers = curl_slist_append(headers, hdr1);
    if (hdr2) headers = curl_slist_append(headers, hdr2);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (http_code) *http_code = code;
    if (curl_out) *curl_out = res;
    return (res == CURLE_OK) ? 0 : -1;
}

static int try_openai_chat(const char *tier, const char *base, const char *key, const char *model,
                           const char *prompt_esc, double temperature, char *out, size_t out_cap) {
    if (!key || !key[0] || !model || !model[0]) return -1;
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", base);
    char auth[2048];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
    size_t cap = strlen(prompt_esc) + strlen(model) + 512;
    char *body = NULL;
    for (;;) {
        char *nb = (char *)realloc(body, cap);
        if (!nb) {
            free(body);
            return -1;
        }
        body = nb;
        int w;
        if (temperature >= 0.0)
            w = snprintf(body, cap,
                         "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],\"temperature\":%.4f}",
                         model, prompt_esc, temperature);
        else
            w = snprintf(body, cap, "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}", model,
                         prompt_esc);
        if (w >= 0 && (size_t)w < cap)
            break;
        if (w < 0) {
            free(body);
            return -1;
        }
        cap = (size_t)w + 64;
    }
    if (cl_ai_agent_log_full_http()) {
        fprintf(stderr,
                "[ai_agent][FULL_HTTP] tier=%s\nPOST %s\nAuthorization: Bearer %s\nContent-Type: "
                "application/json\nBody: %s\n",
                tier ? tier : "openai", url, key, body);
    }
    size_t resp_cap = 256 * 1024;
    char *resp = (char *)malloc(resp_cap);
    if (!resp) { free(body); return -1; }
    long code = 0;
    CURLcode cr = CURLE_OK;
    int hr = http_post(url, auth, NULL, body, &code, resp, resp_cap, &cr);
    free(body);
    if (hr != 0) {
        if (cl_ai_agent_diag())
            fprintf(stderr, "[ai_agent] %s curl_error=%d %s url=%s\n", tier ? tier : "openai", (int)cr,
                    curl_easy_strerror(cr), url);
        free(resp);
        return -1;
    }
    if (code != 200) {
        if (cl_ai_agent_diag()) {
            char pre[420];
            cl_sanitize_resp_prefix(pre, sizeof(pre), resp, 400);
            fprintf(stderr, "[ai_agent] %s HTTP %ld url=%s body_prefix=%s\n", tier ? tier : "openai", code, url,
                    pre);
        }
        free(resp);
        return -1;
    }
    if (extract_openai_assistant(resp, out, out_cap) != 0) {
        if (cl_ai_agent_diag()) {
            char pre[420];
            cl_sanitize_resp_prefix(pre, sizeof(pre), resp, 400);
            fprintf(stderr, "[ai_agent] %s HTTP 200 assistant_parse_fail url=%s body_prefix=%s\n",
                    tier ? tier : "openai", url, pre);
        }
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

/** OpenAI-compatible chat with structured messages (system + history + user). */
static int try_openai_chat_structured(const char *tier, const char *base, const char *key, const char *model,
                                      const ai_agent_prompt_t *prompt, double temperature, char *out, size_t out_cap) {
    if (!key || !key[0] || !model || !model[0] || !prompt) return -1;
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", base);
    char auth[2048];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);

    char *body = build_openai_messages_body(prompt, model, temperature);
    if (!body) return -1;

    if (cl_ai_agent_log_full_http()) {
        fprintf(stderr, "[ai_agent][FULL_HTTP] tier=%s (structured)\nPOST %s\nBody: %.2000s...\n",
                tier ? tier : "openai", url, body);
    }

    size_t resp_cap = 256 * 1024;
    char *resp = (char *)malloc(resp_cap);
    if (!resp) { free(body); return -1; }
    long code = 0;
    CURLcode cr = CURLE_OK;
    int hr = http_post(url, auth, NULL, body, &code, resp, resp_cap, &cr);
    free(body);
    if (hr != 0) {
        if (cl_ai_agent_diag())
            fprintf(stderr, "[ai_agent] %s curl_error=%d %s url=%s\n", tier ? tier : "openai", (int)cr,
                    curl_easy_strerror(cr), url);
        free(resp);
        return -1;
    }
    if (code != 200) {
        if (cl_ai_agent_diag()) {
            char pre[420];
            cl_sanitize_resp_prefix(pre, sizeof(pre), resp, 400);
            fprintf(stderr, "[ai_agent] %s HTTP %ld url=%s body_prefix=%s\n", tier ? tier : "openai", code, url, pre);
        }
        free(resp);
        return -1;
    }
    if (extract_openai_assistant(resp, out, out_cap) != 0) {
        if (cl_ai_agent_diag()) {
            char pre[420];
            cl_sanitize_resp_prefix(pre, sizeof(pre), resp, 400);
            fprintf(stderr, "[ai_agent] %s HTTP 200 assistant_parse_fail url=%s body_prefix=%s\n",
                    tier ? tier : "openai", url, pre);
        }
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

static int try_gemini(const char *key, const char *model, const char *prompt_esc, char *out, size_t out_cap) {
    if (!key || !key[0] || !model || !model[0]) return -1;
    const char *m = model;
    if (strncmp(m, "models/", 7) == 0) m += 7;
    CURL *eh = curl_easy_init();
    if (!eh) return -1;
    char *ek = curl_easy_escape(eh, key, 0);
    char url[768];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s", m,
             ek ? ek : "");
    if (ek) curl_free(ek);
    curl_easy_cleanup(eh);

    size_t cap = strlen(prompt_esc) + 512;
    char *body = NULL;
    for (;;) {
        char *nb = (char *)realloc(body, cap);
        if (!nb) {
            free(body);
            return -1;
        }
        body = nb;
        int w = snprintf(body, cap, "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"%s\"}]}]}", prompt_esc);
        if (w >= 0 && (size_t)w < cap)
            break;
        if (w < 0) {
            free(body);
            return -1;
        }
        cap = (size_t)w + 64;
    }
    if (cl_ai_agent_log_full_http()) {
        fprintf(stderr,
                "[ai_agent][FULL_HTTP] tier=gemini\nPOST %s\nContent-Type: application/json\nBody: %s\n", url,
                body);
    }
    size_t resp_cap = 256 * 1024;
    char *resp = (char *)malloc(resp_cap);
    if (!resp) { free(body); return -1; }
    long code = 0;
    CURLcode cr = CURLE_OK;
    int hr = http_post(url, NULL, NULL, body, &code, resp, resp_cap, &cr);
    free(body);
    if (hr != 0) {
        if (cl_ai_agent_diag())
            fprintf(stderr, "[ai_agent] gemini curl_error=%d %s model=%s\n", (int)cr, curl_easy_strerror(cr), m);
        free(resp);
        return -1;
    }
    if (code != 200) {
        if (cl_ai_agent_diag()) {
            char pre[420];
            cl_sanitize_resp_prefix(pre, sizeof(pre), resp, 400);
            fprintf(stderr, "[ai_agent] gemini HTTP %ld model=%s body_prefix=%s\n", code, m, pre);
        }
        free(resp);
        return -1;
    }
    if (extract_gemini_text(resp, out, out_cap) != 0) {
        if (cl_ai_agent_diag()) {
            char pre[420];
            cl_sanitize_resp_prefix(pre, sizeof(pre), resp, 400);
            fprintf(stderr, "[ai_agent] gemini HTTP 200 text_parse_fail model=%s body_prefix=%s\n", m, pre);
        }
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

static int run_cloud_tiers(const ai_agent_prompt_t *prompt, const char *prompt_esc,
                           double temperature, char *out, size_t out_cap,
                           unsigned *wire_out, char *llm_out, size_t llm_cap) {
    char groq_kbuf[2048], cb_kbuf[2048], gem_kbuf[2048];
    cl_cloud_api_key_normalize(getenv("GROQ_API_KEY"), groq_kbuf, sizeof(groq_kbuf));
    cl_cloud_api_key_normalize(getenv("CEREBRAS_API_KEY"), cb_kbuf, sizeof(cb_kbuf));
    cl_cloud_api_key_normalize(getenv("GEMINI_API_KEY"), gem_kbuf, sizeof(gem_kbuf));
    const char *groq_key = groq_kbuf[0] ? groq_kbuf : NULL;
    const char *cb_key = cb_kbuf[0] ? cb_kbuf : NULL;
    const char *gem_key = gem_kbuf[0] ? gem_kbuf : NULL;
    const char *groq_m = getenv("M4_CLOUD_GROQ_MODEL");
    if (!groq_m || !groq_m[0]) groq_m = "llama-3.1-8b-instant";
    const char *cb_m = getenv("M4_CLOUD_CEREBRAS_MODEL");
    if (!cb_m || !cb_m[0]) cb_m = "llama3.1-8b";
    const char *gm_m = getenv("M4_CLOUD_GEMINI_MODEL");
    if (!gm_m || !gm_m[0]) gm_m = "gemini-2.5-flash";
    const char *cb_base = getenv("CEREBRAS_API_BASE");
    if (!cb_base || !cb_base[0]) cb_base = "https://api.cerebras.ai/v1";

    const char *order[8];
    int norder = 0;
    {
        const char *e = getenv("M4_CLOUD_TRY_ORDER");
        if (!e || !e[0]) e = "groq,cerebras,gemini";
        char buf[128];
        strncpy(buf, e, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *saveptr = NULL;
        for (char *tok = strtok_r(buf, ",", &saveptr); tok && norder < 8; tok = strtok_r(NULL, ",", &saveptr)) {
            while (*tok == ' ' || *tok == '\t') tok++;
            size_t L = strlen(tok);
            while (L > 0 && (tok[L - 1] == ' ' || tok[L - 1] == '\t')) tok[--L] = '\0';
            if (!L) continue;
            for (size_t i = 0; i < L; i++) tok[i] = (char)tolower((unsigned char)tok[i]);
            if (strcmp(tok, "groq") == 0) order[norder++] = "groq";
            else if (strcmp(tok, "cerebras") == 0)
                order[norder++] = "cerebras";
            else if (strcmp(tok, "gemini") == 0)
                order[norder++] = "gemini";
        }
        if (norder == 0) {
            order[0] = "groq";
            order[1] = "cerebras";
            order[2] = "gemini";
            norder = 3;
        }
    }

    m4_log("ai_agent", M4_LOG_DEBUG, "cloud_tiers: order=%s%s%s (%d tiers) groq_key=%s cerebras_key=%s gemini_key=%s",
           norder > 0 ? order[0] : "", norder > 1 ? "," : "", norder > 1 ? order[1] : "", norder,
           groq_key ? "set" : "MISSING", cb_key ? "set" : "MISSING", gem_key ? "set" : "MISSING");
    m4_log("ai_agent", M4_LOG_DEBUG, "cloud_tiers: groq_model=%s cerebras_model=%s gemini_model=%s prompt_structured=%s",
           groq_m, cb_m, gm_m, prompt ? "yes" : "no(flat)");

    for (int i = 0; i < norder; i++) {
        if (strcmp(order[i], "groq") == 0) {
            if (!groq_key || !groq_key[0]) {
                m4_log("ai_agent", M4_LOG_DEBUG, "tier %d: groq SKIPPED — no GROQ_API_KEY", i + 1);
                continue;
            }
            m4_log("ai_agent", M4_LOG_INFO, "tier %d: trying groq model=%s structured=%s", i + 1, groq_m, prompt ? "yes" : "no");
            int ok = (prompt)
                ? try_openai_chat_structured("groq", "https://api.groq.com/openai/v1", groq_key, groq_m,
                                             prompt, temperature, out, out_cap)
                : try_openai_chat("groq", "https://api.groq.com/openai/v1", groq_key, groq_m,
                                  prompt_esc, temperature, out, out_cap);
            if (ok == 0) {
                if (wire_out) *wire_out = API_CHAT_WIRE_OPENAI_CHAT;
                cl_write_llm_label(llm_out, llm_cap, "groq", groq_m);
                m4_log("ai_agent", M4_LOG_INFO, "tier %d: groq SUCCESS model=%s", i + 1, groq_m);
                return 0;
            }
            m4_log("ai_agent", M4_LOG_WARN, "tier %d: groq FAILED", i + 1);
        } else if (strcmp(order[i], "cerebras") == 0) {
            if (!cb_key || !cb_key[0]) {
                m4_log("ai_agent", M4_LOG_DEBUG, "tier %d: cerebras SKIPPED — no CEREBRAS_API_KEY", i + 1);
                continue;
            }
            m4_log("ai_agent", M4_LOG_INFO, "tier %d: trying cerebras model=%s base=%s", i + 1, cb_m, cb_base);
            int ok = (prompt)
                ? try_openai_chat_structured("cerebras", cb_base, cb_key, cb_m,
                                             prompt, temperature, out, out_cap)
                : try_openai_chat("cerebras", cb_base, cb_key, cb_m,
                                  prompt_esc, temperature, out, out_cap);
            if (ok == 0) {
                if (wire_out) *wire_out = API_CHAT_WIRE_OPENAI_CHAT;
                cl_write_llm_label(llm_out, llm_cap, "cerebras", cb_m);
                m4_log("ai_agent", M4_LOG_INFO, "tier %d: cerebras SUCCESS model=%s", i + 1, cb_m);
                return 0;
            }
            m4_log("ai_agent", M4_LOG_WARN, "tier %d: cerebras FAILED", i + 1);
        } else if (strcmp(order[i], "gemini") == 0) {
            if (!gem_key || !gem_key[0]) {
                m4_log("ai_agent", M4_LOG_DEBUG, "tier %d: gemini SKIPPED — no GEMINI_API_KEY", i + 1);
                continue;
            }
            /* Gemini: use flat prompt (prompt_esc is already trimmed by ctx_build_prompt_parts) */
            m4_log("ai_agent", M4_LOG_INFO, "tier %d: trying gemini model=%s prompt_len=%zu", i + 1, gm_m, strlen(prompt_esc));
            if (try_gemini(gem_key, gm_m, prompt_esc, out, out_cap) == 0) {
                if (wire_out) *wire_out = API_CHAT_WIRE_GEMINI;
                cl_write_llm_label(llm_out, llm_cap, "gemini", gm_m);
                m4_log("ai_agent", M4_LOG_INFO, "tier %d: gemini SUCCESS model=%s", i + 1, gm_m);
                return 0;
            }
            m4_log("ai_agent", M4_LOG_WARN, "tier %d: gemini FAILED", i + 1);
        }
    }
    return -1;
}

int ai_agent_complete_chat(const ai_agent_prompt_t *prompt,
                            const char *context_prompt, double temperature, const char *ollama_model_lane_pin,
                            const char *lane_api_url, const char *lane_api_key,
                            char *bot_reply_out, size_t out_size, char *source_out, unsigned *wire_out,
                            char *llm_model_out, size_t llm_model_cap) {
    if (!context_prompt || !bot_reply_out || out_size == 0 || !source_out) return -1;
    bot_reply_out[0] = '\0';
    *source_out = 0;
    if (llm_model_out && llm_model_cap > 0)
        llm_model_out[0] = '\0';

    m4_log("ai_agent", M4_LOG_DEBUG, "complete_chat: model_pin=%s lane_url=%s temp=%.2f prompt_parts=%s",
           ollama_model_lane_pin ? ollama_model_lane_pin : "(none)",
           lane_api_url ? lane_api_url : "(none)",
           temperature,
           prompt ? "yes" : "no");

    /* Priority 1: Lane has a direct endpoint (api_url + api_key) → try it first */
    if (lane_api_url && lane_api_url[0]) {
        const char *model = (ollama_model_lane_pin && ollama_model_lane_pin[0])
                                ? ollama_model_lane_pin : "default";
        const char *key = (lane_api_key && lane_api_key[0]) ? lane_api_key : NULL;
        m4_log("ai_agent", M4_LOG_INFO, "priority 1: lane endpoint url=%s model=%s", lane_api_url, model);
        if (!key) {
            m4_log("ai_agent", M4_LOG_WARN, "lane api_url set but no api_key — falling through to pool");
        } else {
            size_t plen = strlen(context_prompt);
            size_t esc_cap = plen * 4 + 64;
            char *esc = (char *)malloc(esc_cap);
            if (esc) {
                json_escape_in(context_prompt, esc, esc_cap);
                char base[512];
                strncpy(base, lane_api_url, sizeof(base) - 1);
                base[sizeof(base) - 1] = '\0';
                char *suffix = strstr(base, "/chat/completions");
                if (suffix) *suffix = '\0';

                if (try_openai_chat("lane", base, key, model,
                                    esc, temperature, bot_reply_out, out_size) == 0) {
                    free(esc);
                    *source_out = API_SOURCE_CLOUD;
                    if (wire_out) *wire_out = API_CHAT_WIRE_OPENAI_CHAT;
                    cl_write_llm_label(llm_model_out, llm_model_cap, "lane", model);
                    m4_log("ai_agent", M4_LOG_INFO, "priority 1: lane SUCCESS model=%s", model);
                    return 0;
                }
                free(esc);
                m4_log("ai_agent", M4_LOG_WARN, "priority 1: lane endpoint FAILED — falling through to cloud pool");
            }
        }
        /* Lane failed → fall through to cloud pool → Ollama (same as no-lane path) */
    }

    /* Priority 2: Normal routing — cloud pool then Ollama as final fallback.
     * ollama_model_lane_pin (if set) is passed as model preference to Ollama,
     * but does NOT skip the cloud pool. */
    int mode = cl_backend_mode();
    m4_log("ai_agent", M4_LOG_DEBUG, "backend_mode=%d (0=ollama_only, 1=cloud_then_ollama, 2=cloud_only)", mode);

    /* mode 0 = M4_CHAT_BACKEND=ollama → skip cloud, go straight to Ollama */
    if (mode == 0) {
        const char *om = (ollama_model_lane_pin && ollama_model_lane_pin[0]) ? ollama_model_lane_pin : NULL;
        m4_log("ai_agent", M4_LOG_INFO, "priority 2: M4_CHAT_BACKEND=ollama → Ollama only, model=%s",
               om ? om : "(default)");
        int r = (temperature >= 0.0)
                    ? ollama_query_with_options(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, om, context_prompt,
                                                temperature, bot_reply_out, out_size)
                    : ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, om, context_prompt, bot_reply_out,
                                   out_size);
        if (r != 0) return -1;
        *source_out = API_SOURCE_OLLAMA;
        if (wire_out) *wire_out = API_CHAT_WIRE_OLLAMA;
        ollama_completion_label(llm_model_out, llm_model_cap, om);
        return 0;
    }

    /* mode 1 (cloud_then_ollama) or 2 (cloud only) → try cloud pool first */
    m4_log("ai_agent", M4_LOG_INFO, "priority 2: trying cloud pool (mode=%d)", mode);
    size_t plen = strlen(context_prompt);
    size_t esc_cap = plen * 4 + 64;
    char *esc = (char *)malloc(esc_cap);
    if (!esc) return -1;
    json_escape_in(context_prompt, esc, esc_cap);

    if (run_cloud_tiers(prompt, esc, temperature, bot_reply_out, out_size, wire_out, llm_model_out, llm_model_cap) == 0) {
        free(esc);
        *source_out = API_SOURCE_CLOUD;
        m4_log("ai_agent", M4_LOG_INFO, "cloud pool SUCCESS source=CLOUD");
        return 0;
    }
    free(esc);
    m4_log("ai_agent", M4_LOG_WARN, "cloud pool: ALL tiers failed");

    /* Cloud failed → Ollama final fallback (unless mode=2 cloud-only) */
    if (mode == 2) {
        m4_log("ai_agent", M4_LOG_ERROR, "M4_CHAT_BACKEND=cloud but all cloud tiers failed — no Ollama fallback");
        return -1;
    }

    const char *om = (ollama_model_lane_pin && ollama_model_lane_pin[0]) ? ollama_model_lane_pin : NULL;
    m4_log("ai_agent", M4_LOG_INFO, "priority 3: Ollama final fallback model=%s host=%s port=%d",
           om ? om : "(default)", OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT);
    int r = (temperature >= 0.0)
                ? ollama_query_with_options(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, om, context_prompt, temperature,
                                            bot_reply_out, out_size)
                : ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, om, context_prompt, bot_reply_out, out_size);
    if (r != 0) {
        m4_log("ai_agent", M4_LOG_ERROR, "Ollama fallback FAILED rc=%d", r);
        return -1;
    }
    *source_out = API_SOURCE_OLLAMA;
    if (wire_out) *wire_out = API_CHAT_WIRE_OLLAMA;
    ollama_completion_label(llm_model_out, llm_model_cap, NULL);
    return 0;
}
