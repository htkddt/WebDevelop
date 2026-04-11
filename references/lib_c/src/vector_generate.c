#include "vector_generate.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>

/* ---- FNV-1a hash ---- */

static uint64_t fnv1a64_bytes(const unsigned char *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- Synonym table ---- */

#define SYN_BUCKETS 1024
#define SYN_KEY_MAX 512

typedef struct syn_entry {
    char *alias;        /* normalized key */
    char *canonical;    /* canonical form */
    struct syn_entry *next;
} syn_entry_t;

struct m4_synonym_table {
    syn_entry_t *buckets[SYN_BUCKETS];
    pthread_rwlock_t lock;
    int count;
};

static m4_synonym_table_t *s_global_synonyms = NULL;

static unsigned syn_hash(const char *s) {
    unsigned h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h % SYN_BUCKETS;
}

/* Normalize: lowercase + accents stripped to ASCII approximation + spaces→underscores */
static void syn_normalize(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < cap - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 'A' && c <= 'Z') { out[j++] = (char)(c + 32); }
        else if (c == ' ' || c == '\t') { out[j++] = '_'; }
        else if (c == '.' || c == '-') { out[j++] = '_'; }
        else if (c < 128) { out[j++] = (char)c; }
        else {
            /* UTF-8 multi-byte: keep bytes as-is (Vietnamese chars preserved for matching) */
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

m4_synonym_table_t *m4_synonym_create(void) {
    m4_synonym_table_t *st = (m4_synonym_table_t *)calloc(1, sizeof(*st));
    if (!st) return NULL;
    pthread_rwlock_init(&st->lock, NULL);
    return st;
}

void m4_synonym_destroy(m4_synonym_table_t *st) {
    if (!st) return;
    for (int i = 0; i < SYN_BUCKETS; i++) {
        syn_entry_t *e = st->buckets[i];
        while (e) {
            syn_entry_t *next = e->next;
            free(e->alias);
            free(e->canonical);
            free(e);
            e = next;
        }
    }
    pthread_rwlock_destroy(&st->lock);
    free(st);
}

void m4_synonym_add(m4_synonym_table_t *st, const char *alias, const char *canonical) {
    if (!st || !alias || !alias[0] || !canonical || !canonical[0]) return;
    char norm_alias[SYN_KEY_MAX], norm_canon[SYN_KEY_MAX];
    syn_normalize(alias, norm_alias, sizeof(norm_alias));
    syn_normalize(canonical, norm_canon, sizeof(norm_canon));
    if (!norm_alias[0] || !norm_canon[0]) return;
    if (strcmp(norm_alias, norm_canon) == 0) return; /* don't map to self */

    unsigned h = syn_hash(norm_alias);

    pthread_rwlock_wrlock(&st->lock);
    /* Check if already exists */
    for (syn_entry_t *e = st->buckets[h]; e; e = e->next) {
        if (strcmp(e->alias, norm_alias) == 0) {
            pthread_rwlock_unlock(&st->lock);
            return; /* already mapped */
        }
    }
    syn_entry_t *ne = (syn_entry_t *)malloc(sizeof(*ne));
    if (!ne) { pthread_rwlock_unlock(&st->lock); return; }
    ne->alias = strdup(norm_alias);
    ne->canonical = strdup(norm_canon);
    ne->next = st->buckets[h];
    st->buckets[h] = ne;
    st->count++;
    pthread_rwlock_unlock(&st->lock);
}

const char *m4_synonym_lookup(const m4_synonym_table_t *st, const char *text) {
    if (!st || !text || !text[0]) return NULL;
    char norm[SYN_KEY_MAX];
    syn_normalize(text, norm, sizeof(norm));
    unsigned h = syn_hash(norm);

    pthread_rwlock_rdlock((pthread_rwlock_t *)&st->lock);
    for (syn_entry_t *e = st->buckets[h]; e; e = e->next) {
        if (strcmp(e->alias, norm) == 0) {
            const char *r = e->canonical;
            pthread_rwlock_unlock((pthread_rwlock_t *)&st->lock);
            return r;
        }
    }
    pthread_rwlock_unlock((pthread_rwlock_t *)&st->lock);
    return NULL;
}

void m4_synonym_set_global(m4_synonym_table_t *st) {
    s_global_synonyms = st;
}

m4_synonym_table_t *m4_synonym_get_global(void) {
    return s_global_synonyms;
}

/* ---- Built-in Vietnamese synonyms ---- */

void m4_synonym_add_builtins(m4_synonym_table_t *st) {
    if (!st) return;
    /* Place names */
    m4_synonym_add(st, "Saigon", "Ho Chi Minh City");
    m4_synonym_add(st, "Sài Gòn", "Ho Chi Minh City");
    m4_synonym_add(st, "TPHCM", "Ho Chi Minh City");
    m4_synonym_add(st, "TP.HCM", "Ho Chi Minh City");
    m4_synonym_add(st, "TP HCM", "Ho Chi Minh City");
    m4_synonym_add(st, "Thành phố Hồ Chí Minh", "Ho Chi Minh City");
    m4_synonym_add(st, "Hà Nội", "Hanoi");
    m4_synonym_add(st, "Ha Noi", "Hanoi");
    m4_synonym_add(st, "Đà Nẵng", "Da Nang");
    m4_synonym_add(st, "Da Nang", "Da Nang");
    m4_synonym_add(st, "Huế", "Hue");
    m4_synonym_add(st, "Nha Trang", "Nha Trang");
    m4_synonym_add(st, "Cần Thơ", "Can Tho");
    m4_synonym_add(st, "Vũng Tàu", "Vung Tau");
    m4_synonym_add(st, "Đà Lạt", "Da Lat");

    /* Common Vietnamese → English terms */
    m4_synonym_add(st, "thành phố", "city");
    m4_synonym_add(st, "quận", "district");
    m4_synonym_add(st, "huyện", "district");
    m4_synonym_add(st, "phường", "ward");
    m4_synonym_add(st, "xã", "commune");
    m4_synonym_add(st, "tỉnh", "province");
    m4_synonym_add(st, "đường", "street");
    m4_synonym_add(st, "chợ", "market");
    m4_synonym_add(st, "cầu", "bridge");
    m4_synonym_add(st, "sông", "river");
    m4_synonym_add(st, "núi", "mountain");
    m4_synonym_add(st, "biển", "sea");
    m4_synonym_add(st, "bãi biển", "beach");
    m4_synonym_add(st, "sân bay", "airport");
    m4_synonym_add(st, "ga", "station");
    m4_synonym_add(st, "bến xe", "bus station");
}

/* ---- Improved vector generation ---- */

static void add_at_hash(float *vec, size_t dim, const unsigned char *data, size_t len, float weight) {
    if (!len || dim == 0) return;
    uint64_t h = fnv1a64_bytes(data, len);
    vec[(size_t)(h % (uint64_t)dim)] += weight;
    h = h * 1099511628211ULL ^ (uint64_t)(len + 7);
    vec[(size_t)(h % (uint64_t)dim)] += weight * 0.5f;
    /* Third slot for better distribution */
    h = h * 1099511628211ULL ^ (uint64_t)(len + 13);
    vec[(size_t)(h % (uint64_t)dim)] += weight * 0.25f;
}

#define WORD_BUF 320

static void fold_word_ascii(unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c >= 'A' && c <= 'Z')
            buf[i] = (unsigned char)(c + 32);
    }
}

static void emit_word(const unsigned char *w, size_t wlen, float *vec, size_t dim,
                      unsigned char *prev, size_t *prev_len) {
    if (wlen == 0 || wlen >= WORD_BUF) return;
    unsigned char word[WORD_BUF];
    memcpy(word, w, wlen);
    word[wlen] = '\0';
    fold_word_ascii(word, wlen);

    /* Check synonym table for this word */
    const char *canonical = s_global_synonyms ? m4_synonym_lookup(s_global_synonyms, (const char *)word) : NULL;
    if (canonical) {
        /* Use canonical form instead */
        size_t clen = strlen(canonical);
        if (clen < WORD_BUF) {
            memcpy(word, canonical, clen);
            word[clen] = '\0';
            wlen = clen;
        }
    }

    /* Word unigram */
    add_at_hash(vec, dim, word, wlen, 1.0f);

    /* Word bigram (with previous word) */
    if (*prev_len > 0 && *prev_len + 1 + wlen < WORD_BUF) {
        unsigned char pair[WORD_BUF];
        memcpy(pair, prev, *prev_len);
        pair[*prev_len] = (unsigned char)' ';
        memcpy(pair + *prev_len + 1, word, wlen);
        add_at_hash(vec, dim, pair, *prev_len + 1 + wlen, 0.75f);
    }

    /* Character trigrams (handles accent variants via partial overlap) */
    if (wlen >= 3) {
        for (size_t i = 0; i <= wlen - 3; i++) {
            add_at_hash(vec, dim, word + i, 3, 0.3f);
        }
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

    /* Check if entire text is a known synonym */
    const char *full_canonical = s_global_synonyms ? m4_synonym_lookup(s_global_synonyms, t) : NULL;
    if (full_canonical) t = full_canonical;

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
