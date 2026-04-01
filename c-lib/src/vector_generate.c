#include "vector_generate.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

static uint64_t fnv1a64_bytes(const unsigned char *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void fold_word_ascii(unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c >= 'A' && c <= 'Z')
            buf[i] = (unsigned char)(c + 32);
    }
}

static void add_at_hash(float *vec, size_t dim, const unsigned char *data, size_t len, float weight) {
    if (!len || dim == 0) return;
    uint64_t h = fnv1a64_bytes(data, len);
    vec[(size_t)(h % (uint64_t)dim)] += weight;
    h = h * 1099511628211ULL ^ (uint64_t)(len + 7);
    vec[(size_t)(h % (uint64_t)dim)] += weight * 0.5f;
}

#define WORD_BUF 320

static void emit_word(const unsigned char *w, size_t wlen, float *vec, size_t dim,
                      unsigned char *prev, size_t *prev_len) {
    if (wlen == 0 || wlen >= WORD_BUF) return;
    unsigned char word[WORD_BUF];
    memcpy(word, w, wlen);
    word[wlen] = '\0';
    fold_word_ascii(word, wlen);

    add_at_hash(vec, dim, word, wlen, 1.0f);

    if (*prev_len > 0 && *prev_len + 1 + wlen < WORD_BUF) {
        unsigned char pair[WORD_BUF];
        memcpy(pair, prev, *prev_len);
        pair[*prev_len] = (unsigned char)' ';
        memcpy(pair + *prev_len + 1, word, wlen);
        add_at_hash(vec, dim, pair, *prev_len + 1 + wlen, 0.75f);
    }
    memcpy(prev, word, wlen);
    *prev_len = wlen;
}

static void normalize_l2(float *vec, size_t dim) {
    double s = 0.0;
    for (size_t i = 0; i < dim; i++)
        s += (double)vec[i] * (double)vec[i];
    if (s <= 1e-20) {
        vec[0] = 1.0f;
        for (size_t i = 1; i < dim; i++)
            vec[i] = 0.0f;
        return;
    }
    float inv = (float)(1.0 / sqrt(s));
    for (size_t i = 0; i < dim; i++)
        vec[i] *= inv;
}

int vector_generate_custom(const char *text, float *out, size_t max_dim, size_t *out_dim) {
    if (!out || !out_dim || max_dim < VECTOR_GEN_CUSTOM_DIM) return -1;
    const char *t = text ? text : "";
    size_t dim = VECTOR_GEN_CUSTOM_DIM;
    memset(out, 0, dim * sizeof(float));

    unsigned char prev[WORD_BUF];
    size_t prev_len = 0;

    size_t n = strlen(t);
    size_t wstart = (size_t)-1;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = (i < n) ? (unsigned char)t[i] : 0;
        int is_word = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 128 && c != 0);
        if (is_word) {
            if (wstart == (size_t)-1)
                wstart = i;
        } else {
            if (wstart != (size_t)-1) {
                size_t wlen = i - wstart;
                if (wlen > 0)
                    emit_word((const unsigned char *)t + wstart, wlen, out, dim, prev, &prev_len);
                wstart = (size_t)-1;
            }
        }
    }

    normalize_l2(out, dim);
    *out_dim = dim;
    return 0;
}
