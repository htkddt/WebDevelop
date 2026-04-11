/*
 * ELK — stateless HTTP to Elasticsearch (libcurl).
 * Rules: .cursor/elk.md, include/elk.h
 */
#include "elk.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELK_HTTP_TIMEOUT_SEC 10L

struct elk_ctx {
    char host[128];
    int port;
};

struct elk_write_buf {
    char *data;
    size_t len;
};

static size_t elk_write_discard(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static size_t elk_write_append(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct elk_write_buf *wb = (struct elk_write_buf *)userdata;
    size_t n = size * nmemb;
    size_t need = wb->len + n + 1;
    char *p = (char *)realloc(wb->data, need);
    if (!p)
        return 0;
    memcpy(p + wb->len, ptr, n);
    wb->len += n;
    p[wb->len] = '\0';
    wb->data = p;
    return n;
}

static int elk_curl_post_json(const char *url, const char *body, size_t body_len,
                              char *err_out, size_t err_cap, long *http_code_out) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    if (body_len > 0)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ELK_HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, elk_write_discard);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (http_code_out)
        *http_code_out = code;
    if (res != CURLE_OK) {
        if (err_out && err_cap)
            snprintf(err_out, err_cap, "curl: %s", curl_easy_strerror(res));
        return -1;
    }
    if (code < 200 || code >= 300) {
        if (err_out && err_cap)
            snprintf(err_out, err_cap, "HTTP %ld", code);
        return -1;
    }
    return 0;
}

elk_ctx_t *elk_create(const char *host, int port) {
    elk_ctx_t *ctx = (elk_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    if (host && host[0])
        strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    else
        strncpy(ctx->host, "127.0.0.1", sizeof(ctx->host) - 1);
    ctx->port = port > 0 ? port : ELK_DEFAULT_PORT;
    return ctx;
}

void elk_destroy(elk_ctx_t *ctx) {
    free(ctx);
}

int elk_initial(elk_ctx_t *ctx) {
    (void)ctx;
    return 0;
}

static size_t json_escape_utf8(const char *in, char *out, size_t out_cap) {
    size_t o = 0;
    if (!in)
        in = "";
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 8 < out_cap; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (o + 2 >= out_cap)
                break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 6 >= out_cap)
                break;
            o += (size_t)snprintf(out + o, out_cap - o, "\\u%04x", c);
        } else
            out[o++] = (char)c;
    }
    if (o < out_cap)
        out[o] = '\0';
    else if (out_cap > 0)
        out[out_cap - 1] = '\0';
    return o;
}

int elk_set_ingest(elk_ctx_t *ctx, const char *base_url, const char *raw_text) {
    if (!ctx || !base_url || !raw_text)
        return -1;

    char esc[8192];
    json_escape_utf8(raw_text, esc, sizeof(esc));

    char body[9000];
    int bl = snprintf(body, sizeof(body), "{\"raw_content\":\"%s\"}", esc);
    if (bl < 0 || (size_t)bl >= sizeof(body))
        return -1;

    char url[512];
    int n = snprintf(url, sizeof(url), "%s/ai_index/_doc?pipeline=%s", base_url, ELK_PIPELINE_AUTO_LANG);
    if (n < 0 || (size_t)n >= sizeof(url))
        return -1;

    char err[128];
    long http = 0;
    if (elk_curl_post_json(url, body, (size_t)bl, err, sizeof(err), &http) != 0) {
        fprintf(stderr, "[ELK] ingest failed: %s\n", err);
        return -1;
    }
    return 0;
}

int elk_index_json(elk_ctx_t *ctx, const char *index, const char *doc_id,
                   const char *json_body, size_t body_len) {
    if (!ctx || !index || !index[0] || !json_body)
        return -1;
    if (body_len == 0)
        body_len = strlen(json_body);

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    char *id_esc = NULL;
    if (doc_id && doc_id[0])
        id_esc = curl_easy_escape(curl, doc_id, 0);

    char url[640];
    if (id_esc && id_esc[0])
        snprintf(url, sizeof(url), "http://%s:%d/%s/_doc/%s", ctx->host, ctx->port, index, id_esc);
    else
        snprintf(url, sizeof(url), "http://%s:%d/%s/_doc", ctx->host, ctx->port, index);

    if (id_esc)
        curl_free(id_esc);

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ELK_HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, elk_write_discard);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[ELK] index %s: curl %s\n", index, curl_easy_strerror(res));
        return -1;
    }
    if (code < 200 || code >= 300) {
        fprintf(stderr, "[ELK] index %s: HTTP %ld\n", index, code);
        return -1;
    }
    return 0;
}

int elk_set_doc(elk_ctx_t *ctx, const char *tenant_id, const char *doc_id,
                const char *json_body, size_t body_len) {
    (void)tenant_id;
    return elk_index_json(ctx, "ai_index", doc_id && doc_id[0] ? doc_id : NULL, json_body, body_len);
}

int elk_search(elk_ctx_t *ctx, const char *index, const char *query_json, char *out, size_t out_size) {
    if (!ctx || !index || !query_json || !out || out_size == 0)
        return -1;

    char url[384];
    snprintf(url, sizeof(url), "http://%s:%d/%s/_search", ctx->host, ctx->port, index);

    struct elk_write_buf wb = { 0 };
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    size_t qlen = strlen(query_json);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query_json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)qlen);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ELK_HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, elk_write_append);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wb);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code < 200 || code >= 300) {
        free(wb.data);
        return -1;
    }

    if (wb.data && wb.len < out_size) {
        memcpy(out, wb.data, wb.len + 1);
        free(wb.data);
        return 0;
    }
    if (wb.data && out_size > 1) {
        memcpy(out, wb.data, out_size - 1);
        out[out_size - 1] = '\0';
        free(wb.data);
        return 0;
    }
    free(wb.data);
    return -1;
}

int elk_bulk_ndjson(elk_ctx_t *ctx, const char *ndjson_body, size_t body_len) {
    if (!ctx || !ndjson_body || body_len == 0)
        return -1;

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/_bulk", ctx->host, ctx->port);

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist *headers =
        curl_slist_append(NULL, "Content-Type: application/x-ndjson; charset=UTF-8");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ndjson_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ELK_HTTP_TIMEOUT_SEC * 3L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, elk_write_discard);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[ELK] bulk: curl %s\n", curl_easy_strerror(res));
        return -1;
    }
    if (code < 200 || code >= 300) {
        fprintf(stderr, "[ELK] bulk: HTTP %ld\n", code);
        return -1;
    }
    return 0;
}
