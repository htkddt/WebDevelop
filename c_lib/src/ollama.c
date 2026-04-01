/*
 * Ollama client: POST /api/generate via libcurl, parse "response" from JSON.
 * Embeddings: POST /api/embed, parse "embeddings"[0] into float vector (VectorGen per lang_vector_phase1.md).
 */
#include "ollama.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* On by default; M4_DEBUG_CHAT=0|false|no|off disables. =1|true|yes — unparsed NDJSON lines; =2|verbose — each parsed fragment. */
static int ol_debug_chat_on(void) {
    const char *e = getenv("M4_DEBUG_CHAT");
    if (!e || !e[0]) return 1;
    if (e[0] == '0' && e[1] == '\0') return 0;
    if (strcmp(e, "false") == 0) return 0;
    if (strcmp(e, "no") == 0) return 0;
    if (strcmp(e, "off") == 0) return 0;
    if (e[0] == '1' && e[1] == '\0') return 1;
    if (e[0] == '2' && e[1] == '\0') return 1;
    if (strcmp(e, "verbose") == 0) return 1;
    if (e[0] == 't' && strcmp(e, "true") == 0) return 1;
    if (e[0] == 'y' && strcmp(e, "yes") == 0) return 1;
    return 0;
}

static int ol_debug_chat_verbose(void) {
    const char *e = getenv("M4_DEBUG_CHAT");
    if (!e) return 0;
    if (e[0] == '2' && e[1] == '\0') return 1;
    if (strcmp(e, "verbose") == 0) return 1;
    return 0;
}

typedef struct {
    char *data;
    size_t size;
    size_t used;
} ol_buffer_t;

int ollama_get_first_model(const char *host, int port, char *out, size_t out_size);  /* forward for query_impl */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ol_buffer_t *b = (ol_buffer_t *)userdata;
    size_t n = size * nmemb;
    if (b->used + n >= b->size) n = b->size - b->used - 1;
    if (n) {
        memcpy(b->data + b->used, ptr, n);
        b->used += n;
        b->data[b->used] = '\0';
    }
    return size * nmemb;
}

static void json_escape(const char *in, char *out, size_t out_size) {
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
        } else {
            out[j++] = *in;
        }
    }
    out[j] = '\0';
}

/* GET /api/tags: parse first model name from {"models":[{"name":"...", ...}, ...]}. */
static int parse_first_model_name(const char *body, char *out, size_t out_size) {
    const char *p = strstr(body, "\"name\":\"");
    if (!p || out_size == 0) return -1;
    p += 8;  /* skip "name":" */
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && p[1]) { p++; out[i++] = *p++; continue; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* Extract a JSON string value immediately after needle "key":" (streaming NDJSON lines). */
static int extract_quoted_json_field(const char *line, const char *needle, char *out, size_t cap) {
    const char *p = strstr(line, needle);
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
    return 0;
}

/* Extract "response":"...\"..." from body; handle \" inside value. */
static int extract_response(const char *body, char *out, size_t out_size) {
    return extract_quoted_json_field(body, "\"response\":\"", out, out_size);
}

/* Non-stream /api/generate body: prefer "response", then chat-shaped "message" "content",
 * then "thinking" (reasoning models) — same order as extract_stream_token_json. */
static int extract_generate_reply_body(const char *body, char *out, size_t out_size) {
    if (!body || !out || out_size == 0) return -1;
    out[0] = '\0';
    if (extract_response(body, out, out_size) == 0 && out[0]) return 0;
    out[0] = '\0';
    if (extract_quoted_json_field(body, "\"content\":\"", out, out_size) == 0 && out[0]) return 0;
    out[0] = '\0';
    {
        const char *delta = strstr(body, "\"delta\"");
        if (delta && extract_quoted_json_field(delta, "\"content\":\"", out, out_size) == 0 && out[0])
            return 0;
    }
    out[0] = '\0';
    if (extract_quoted_json_field(body, "\"thinking\":\"", out, out_size) == 0 && out[0]) return 0;
    out[0] = '\0';
    if (extract_quoted_json_field(body, "\"text\":\"", out, out_size) == 0 && out[0]) return 0;
    return -1;
}

/* NDJSON line from POST /api/generate: try in order —
 *   "response" — normal Ollama tokens;
 *   "content" — /api/chat-shaped or top-level chunks (no need for a parent "message" key);
 *   "delta" + "content" — OpenAI-compat style streaming;
 *   "thinking" — reasoning models stream thought while "response" stays "".
 * Empty string for a present key is skipped so we can try the next field on the same line. */
static int extract_stream_token_json(const char *line, char *frag, size_t frag_sz) {
    if (!frag || frag_sz == 0) return -1;
    frag[0] = '\0';
    if (extract_quoted_json_field(line, "\"response\":\"", frag, frag_sz) == 0 && frag[0]) return 0;
    frag[0] = '\0';
    if (extract_quoted_json_field(line, "\"content\":\"", frag, frag_sz) == 0 && frag[0]) return 0;
    frag[0] = '\0';
    {
        const char *delta = strstr(line, "\"delta\"");
        if (delta && extract_quoted_json_field(delta, "\"content\":\"", frag, frag_sz) == 0 && frag[0])
            return 0;
    }
    frag[0] = '\0';
    if (extract_quoted_json_field(line, "\"thinking\":\"", frag, frag_sz) == 0 && frag[0]) return 0;
    /* Rare: plain "text":" fragment */
    frag[0] = '\0';
    if (extract_quoted_json_field(line, "\"text\":\"", frag, frag_sz) == 0 && frag[0]) return 0;
    return -1;
}

typedef struct {
    char pending[65536];
    size_t pending_len;
    ollama_stream_token_cb on_token;
    void *on_ud;
    char *full_out;
    size_t full_cap;
    size_t full_used;
    int stream_api_error; /* NDJSON line was Ollama {"error":"..."} (e.g. model not found) */
} ol_stream_wrdata_t;

static void ol_stream_flush_line(ol_stream_wrdata_t *st) {
    if (!st || st->pending_len == 0) return;
    while (st->pending_len > 0 && st->pending[st->pending_len - 1] == '\r')
        st->pending_len--;
    st->pending[st->pending_len] = '\0';
    /* Ollama often returns HTTP 200 with a single NDJSON object like {"error":"model 'x' not found"}. */
    if (strstr(st->pending, "\"error\"")) {
        char emsg[768];
        emsg[0] = '\0';
        (void)extract_quoted_json_field(st->pending, "\"error\":\"", emsg, sizeof(emsg));
        st->stream_api_error = 1;
        if (ol_debug_chat_on()) {
            if (emsg[0])
                fprintf(stderr, "[OLLAMA][stream] API error (not a token line): %s\n", emsg);
            else {
                size_t n = st->pending_len < 280 ? st->pending_len : 280;
                fprintf(stderr, "[OLLAMA][stream] API error object (no extractable string): %.*s%s\n", (int)n,
                        st->pending, st->pending_len > n ? "..." : "");
            }
        }
        st->pending_len = 0;
        return;
    }
    char frag[8192];
    if (extract_stream_token_json(st->pending, frag, sizeof(frag)) != 0) {
        if (ol_debug_chat_on() && st->pending_len > 0) {
            size_t n = st->pending_len < 280 ? st->pending_len : 280;
            fprintf(stderr,
                    "[OLLAMA][stream] unparsed NDJSON line (%zu bytes, no response/content/thinking/text): %.*s%s\n",
                    st->pending_len, (int)n, st->pending, st->pending_len > n ? "..." : "");
        }
        st->pending_len = 0;
        return;
    }
    if (ol_debug_chat_verbose() && frag[0])
        fprintf(stderr, "[OLLAMA][stream] parsed_frag len=%zu: %.120s%s\n", strlen(frag), frag,
                strlen(frag) > 120 ? "..." : "");
    if (frag[0] && st->on_token)
        st->on_token(frag, st->on_ud);
    if (st->full_out && st->full_cap > 0 && frag[0]) {
        size_t tlen = strlen(frag);
        size_t rem = (st->full_cap > st->full_used) ? (st->full_cap - st->full_used) : 0;
        if (rem <= 1) {
            st->pending_len = 0;
            return;
        }
        if (tlen >= rem)
            tlen = rem - 1;
        memcpy(st->full_out + st->full_used, frag, tlen);
        st->full_used += tlen;
        st->full_out[st->full_used] = '\0';
    }
    st->pending_len = 0;
}

static size_t ol_stream_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ol_stream_wrdata_t *st = (ol_stream_wrdata_t *)userdata;
    size_t n = size * nmemb;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)ptr[i];
        if (c == '\n')
            ol_stream_flush_line(st);
        else if (st->pending_len < sizeof(st->pending) - 1)
            st->pending[st->pending_len++] = (char)c;
    }
    return size * nmemb;
}

static int ollama_query_impl(const char *host, int port, const char *model, const char *prompt,
                             double temperature, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (!host) host = OLLAMA_DEFAULT_HOST;
    if (port <= 0) port = OLLAMA_DEFAULT_PORT;
    if (!model || !*model) {
        static char discovered_model[128];
        if (ollama_get_first_model(host, port, discovered_model, sizeof(discovered_model)) == 0) {
            model = discovered_model;  /* use first model from running Ollama */
        } else {
            const char *env = getenv("OLLAMA_MODEL");
            model = env && *env ? env : OLLAMA_DEFAULT_MODEL;
        }
    }
    if (!prompt) prompt = "";

    char escaped[4096];
    json_escape(prompt, escaped, sizeof(escaped));

    /* When temperature >= 0: send options.temperature to /api/generate. When < 0: omit options (Ollama uses model default). */
    char body[8192];
    int body_len;
    if (temperature >= 0.0) {
        body_len = snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false,\"options\":{\"temperature\":%.2g}}",
            model, escaped, temperature);
    } else {
        body_len = snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false}",
            model, escaped);
    }
    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) return -1;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/generate", host, port);

    ol_buffer_t buf = { .data = malloc(OL_BUF_SIZE), .size = OL_BUF_SIZE, .used = 0 };
    if (!buf.data) return -1;
    buf.data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(buf.data); return -1; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);   /* rule §7: 10s for <2s latency path */
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L); /* rule §7: avoid Nagle */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int ret = -1;
    if (res == CURLE_OK && buf.used > 0) {
        ret = extract_generate_reply_body(buf.data, out, out_size);
        /* Still return 0 with empty out so api_chat can append_turn (audit trail). Returning -1
         * skipped persistence entirely — worse than storing turn.assistant "". */
        if (ret != 0) {
            fprintf(stderr,
                    "[OLLAMA] /api/generate: no extractable reply in JSON (%zu bytes); "
                    "empty assistant, api_chat will still persist the turn.\n",
                    buf.used);
            out[0] = '\0';
            ret = 0;
        }
    }
    free(buf.data);
    return ret;
}

int ollama_query(const char *host, int port, const char *model, const char *prompt,
                 char *out, size_t out_size) {
    return ollama_query_impl(host, port, model, prompt, -1.0, out, out_size);
}

int ollama_query_with_options(const char *host, int port, const char *model, const char *prompt,
                              double temperature, char *out, size_t out_size) {
    return ollama_query_impl(host, port, model, prompt, temperature, out, out_size);
}

int ollama_query_stream(const char *host, int port, const char *model, const char *prompt,
                        double temperature,
                        ollama_stream_token_cb on_token, void *on_token_ud,
                        char *full_out, size_t full_out_size) {
    if (full_out && full_out_size > 0)
        full_out[0] = '\0';
    if (!host) host = OLLAMA_DEFAULT_HOST;
    if (port <= 0) port = OLLAMA_DEFAULT_PORT;
    if (!model || !*model) {
        static char discovered_model[128];
        if (ollama_get_first_model(host, port, discovered_model, sizeof(discovered_model)) == 0) {
            model = discovered_model;
        } else {
            const char *env = getenv("OLLAMA_MODEL");
            model = env && *env ? env : OLLAMA_DEFAULT_MODEL;
        }
    }
    if (!prompt) prompt = "";

    char escaped[4096];
    json_escape(prompt, escaped, sizeof(escaped));

    char body[8192];
    int body_len;
    if (temperature >= 0.0) {
        body_len = snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":true,\"options\":{\"temperature\":%.2g}}",
            model, escaped, temperature);
    } else {
        body_len = snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":true}",
            model, escaped);
    }
    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) return -1;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/generate", host, port);

    ol_stream_wrdata_t wr;
    memset(&wr, 0, sizeof(wr));
    wr.on_token = on_token;
    wr.on_ud = on_token_ud;
    wr.full_out = full_out;
    wr.full_cap = full_out_size;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    /* Map this handle to stream state: WRITEDATA points at wr (per-request isolation). */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ol_stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wr);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    ol_stream_flush_line(&wr);

    if (res != CURLE_OK)
        return -1;
    if (wr.stream_api_error)
        return -1;
    if (http_code >= 400)
        return -1;
    return 0;
}

int ollama_get_first_model(const char *host, int port, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (!host) host = OLLAMA_DEFAULT_HOST;
    if (port <= 0) port = OLLAMA_DEFAULT_PORT;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/tags", host, port);

    ol_buffer_t buf = { .data = malloc(8192), .size = 8192, .used = 0 };
    if (!buf.data) return -1;
    buf.data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(buf.data); return -1; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    int ret = -1;
    if (res == CURLE_OK && buf.used > 0)
        ret = parse_first_model_name(buf.data, out, out_size);
    free(buf.data);
    return ret;
}

/* Lightweight health check for stats: GET /api/tags, short timeout. Returns 0 if running. */
int ollama_check_running(const char *host, int port) {
    if (!host) host = OLLAMA_DEFAULT_HOST;
    if (port <= 0) port = OLLAMA_DEFAULT_PORT;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/tags", host, port);

    ol_buffer_t buf = { .data = malloc(4096), .size = 4096, .used = 0 };
    if (!buf.data) return -1;
    buf.data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(buf.data); return -1; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);   /* 2s for frequent stats check */
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    int ret = (res == CURLE_OK && buf.used > 0) ? 0 : -1;
    free(buf.data);
    return ret;
}

int elasticsearch_check_reachable(const char *host, int port) {
    if (!host || !host[0]) return -1;
    if (port <= 0) port = 9200;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/", host, port);

    ol_buffer_t buf = { .data = malloc(4096), .size = 4096, .used = 0 };
    if (!buf.data) return -1;
    buf.data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(buf.data);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_easy_cleanup(curl);
    free(buf.data);

    /* Any HTTP status from the server counts as reachable (incl. 401/403). */
    if (res == CURLE_OK && code > 0 && code < 600)
        return 0;
    return -1;
}

/* Parse first embedding vector from {"embeddings":[[n,n,...]],...}. Write up to max_dim floats; set *out_dim. */
static int parse_embeddings_first(const char *body, float *vector, size_t max_dim, size_t *out_dim) {
    if (!body || !vector || !out_dim || max_dim == 0) return -1;
    *out_dim = 0;
    const char *p = strstr(body, "\"embeddings\":");
    if (!p) return -1;
    p = strchr(p + 12, '[');  /* first [ of embeddings array */
    if (!p) return -1;
    p = strchr(p + 1, '[');   /* first [ of first vector */
    if (!p) return -1;
    p++;
    size_t n = 0;
    while (n < max_dim) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == ']') break;
        char *end;
        errno = 0;
        double v = strtod(p, &end);
        if (end == p || errno != 0) break;
        vector[n++] = (float)v;
        p = end;
        while (*p == ' ' || *p == ',') p++;
    }
    *out_dim = n;
    return (n > 0) ? 0 : -1;
}

int ollama_resolve_embed_model(const char *host, int port, const char *preferred, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    if (!host) host = OLLAMA_DEFAULT_HOST;
    if (port <= 0) port = OLLAMA_DEFAULT_PORT;
    if (preferred && preferred[0]) {
        size_t n = strlen(preferred);
        if (n >= out_size) n = out_size - 1;
        memcpy(out, preferred, n);
        out[n] = '\0';
        return 0;
    }
    /* Optional: force a different embed model than chat (e.g. nomic-only setups). */
    {
        const char *env = getenv("OLLAMA_EMBED_MODEL");
        if (env && env[0]) {
            size_t n = strlen(env);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, env, n);
            out[n] = '\0';
            return 0;
        }
    }
    /* Unify with chat NULL-model path: GET /api/tags first name, OLLAMA_MODEL, OLLAMA_DEFAULT_MODEL. */
    if (ollama_get_first_model(host, port, out, out_size) == 0)
        return 0;
    {
        const char *e = getenv("OLLAMA_MODEL");
        if (e && e[0]) {
            size_t n = strlen(e);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, e, n);
            out[n] = '\0';
            return 0;
        }
    }
    {
        const char *def = OLLAMA_DEFAULT_MODEL;
        size_t n = strlen(def);
        if (n >= out_size) n = out_size - 1;
        memcpy(out, def, n);
        out[n] = '\0';
    }
    return 0;
}

int ollama_embeddings(const char *host, int port, const char *model, const char *text,
                     float *vector, size_t max_dim, size_t *out_dim) {
    if (!vector || !out_dim || max_dim == 0) return -1;
    *out_dim = 0;
    if (!host) host = OLLAMA_DEFAULT_HOST;
    if (port <= 0) port = OLLAMA_DEFAULT_PORT;
    const char *txt = text ? text : "";

    char model_buf[128];
    if (ollama_resolve_embed_model(host, port, model, model_buf, sizeof(model_buf)) != 0)
        return -1;
    const char *use_model = model_buf;

    char escaped[4096];
    json_escape(txt, escaped, sizeof(escaped));
    char body[8192];
    int body_len = snprintf(body, sizeof(body), "{\"model\":\"%s\",\"input\":\"%s\"}",
                            use_model, escaped);
    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) return -1;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/embed", host, port);

    ol_buffer_t buf = { .data = malloc(65536), .size = 65536, .used = 0 };
    if (!buf.data) return -1;
    buf.data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(buf.data); return -1; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int ret = -1;
    if (res == CURLE_OK && buf.used > 0) {
        ret = parse_embeddings_first(buf.data, vector, max_dim, out_dim);
        if (ret != 0) {
            fprintf(stderr, "[OLLAMA] embeddings parse failed (HTTP %ld, body: %.200s...)\n", http_code, buf.data);
        }
    } else {
        if (res != CURLE_OK) {
            fprintf(stderr, "[OLLAMA] embeddings curl failed: %s (HTTP %ld)\n", curl_easy_strerror(res), http_code);
        } else if (buf.used == 0) {
            fprintf(stderr, "[OLLAMA] embeddings: empty response (HTTP %ld)\n", http_code);
        }
    }
    free(buf.data);
    return ret;
}
