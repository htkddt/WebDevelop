/*
 * Geo authority L1 in-memory cache (.cursor/auth_geo.md).
 */
#include "geo_authority.h"
#include "utils.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static m4_ht_t *s_ht;
static pthread_rwlock_t s_lock = PTHREAD_RWLOCK_INITIALIZER;
static int s_init;

static const char *GEO_SEED_DISPLAY[] = {
    "Hà Nội", "TP. Hồ Chí Minh", "Đà Nẵng", "Hải Phòng", "Cần Thơ",
    "An Giang", "Bắc Ninh", "Vĩnh Phúc", "Thanh Hóa", "Lâm Đồng",
    "Kiên Giang", "Đồng Nai", "Bình Dương", "Khánh Hòa", "Quảng Ninh",
    "Thừa Thiên Huế", "Nghệ An", "Gia Lai", "Lào Cai", "Hải Dương",
    "Quảng Ngãi", "Bình Định", "Phú Yên", "Ninh Thuận", "Bình Thuận",
    "Đắk Lắk", "Hưng Yên", "Vĩnh Long", "Đồng Tháp", "Cà Mau",
    "Sóc Trăng", "Bến Tre", "Tiền Giang", "Long An", "Tây Ninh",
};

static void geo_entry_free(void *p) {
    geo_authority_entry_t *e = (geo_authority_entry_t *)p;
    if (!e) return;
    free(e->display_name);
    free(e->merged_into_key);
    free(e);
}

void geo_authority_normalize_key(const char *name, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!name) {
        out[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; name[i] && i < out_size - 1; i++) {
        int c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c == ' ' || c == '\t') c = '_';
        out[i] = (char)c;
    }
    out[i] = '\0';
}

static const geo_authority_entry_t *lookup_unlocked(const char *name_normalized) {
    if (!s_ht || !name_normalized || !name_normalized[0]) return NULL;
    return (const geo_authority_entry_t *)m4_ht_get(s_ht, name_normalized);
}

const geo_authority_entry_t *geo_authority_lookup(const char *name_normalized) {
    if (!s_init || !name_normalized) return NULL;
    pthread_rwlock_rdlock(&s_lock);
    const geo_authority_entry_t *e = lookup_unlocked(name_normalized);
    pthread_rwlock_unlock(&s_lock);
    return e;
}

static int insert_entry(const char *display, const char *key, int32_t parent_id, double trust,
                        const char *merged_into_norm) {
    if (!s_ht || m4_ht_count(s_ht) >= GEO_AUTHORITY_MAX_ENTRIES) return -1;
    if (lookup_unlocked(key)) return 0;
    geo_authority_entry_t *ent = (geo_authority_entry_t *)calloc(1, sizeof(*ent));
    if (!ent) return -1;
    ent->display_name = strdup(display ? display : key);
    if (!ent->display_name) {
        free(ent);
        return -1;
    }
    ent->parent_id = parent_id;
    ent->trust_score = trust;
    if (merged_into_norm && merged_into_norm[0]) {
        ent->merged_into_key = strdup(merged_into_norm);
        if (!ent->merged_into_key) {
            geo_entry_free(ent);
            return -1;
        }
    }
    if (m4_ht_set(s_ht, key, ent) != 0) {
        geo_entry_free(ent);
        return -1;
    }
    return 1;
}

static int seed_provinces(void) {
    char key[256];
    for (size_t i = 0; i < sizeof(GEO_SEED_DISPLAY) / sizeof(GEO_SEED_DISPLAY[0]); i++) {
        geo_authority_normalize_key(GEO_SEED_DISPLAY[i], key, sizeof(key));
        if (insert_entry(GEO_SEED_DISPLAY[i], key, -1, 1.0, NULL) < 0) return -1;
    }
    return 0;
}

int geo_authority_init(void) {
    pthread_rwlock_wrlock(&s_lock);
    if (s_init) {
        pthread_rwlock_unlock(&s_lock);
        return 0;
    }
    s_ht = m4_ht_create(16384);
    if (!s_ht) {
        pthread_rwlock_unlock(&s_lock);
        return -1;
    }
    if (seed_provinces() != 0) {
        m4_ht_destroy(s_ht, geo_entry_free);
        s_ht = NULL;
        pthread_rwlock_unlock(&s_lock);
        return -1;
    }
    s_init = 1;
    pthread_rwlock_unlock(&s_lock);
    fprintf(stderr, "[GEO_AUTH] L1 cache ready (%zu seed provinces)\n",
            sizeof(GEO_SEED_DISPLAY) / sizeof(GEO_SEED_DISPLAY[0]));
    return 0;
}

void geo_authority_shutdown(void) {
    pthread_rwlock_wrlock(&s_lock);
    if (!s_init) {
        pthread_rwlock_unlock(&s_lock);
        return;
    }
    if (s_ht) {
        m4_ht_destroy(s_ht, geo_entry_free);
        s_ht = NULL;
    }
    s_init = 0;
    pthread_rwlock_unlock(&s_lock);
}

int geo_authority_upsert_learned(const char *display_name, const char *name_normalized,
                                 int32_t parent_id, double trust_score,
                                 const char *merged_into_normalized_or_null) {
    if (!s_init || !s_ht || !name_normalized || !name_normalized[0]) return -1;
    pthread_rwlock_wrlock(&s_lock);
    if (m4_ht_count(s_ht) >= GEO_AUTHORITY_MAX_ENTRIES) {
        pthread_rwlock_unlock(&s_lock);
        return -1;
    }
    geo_authority_entry_t *old = (geo_authority_entry_t *)m4_ht_get(s_ht, name_normalized);
    if (old) {
        free(old->display_name);
        free(old->merged_into_key);
        old->merged_into_key = NULL;
        old->display_name = strdup(display_name && display_name[0] ? display_name : name_normalized);
        old->parent_id = parent_id;
        old->trust_score = trust_score;
        if (merged_into_normalized_or_null && merged_into_normalized_or_null[0])
            old->merged_into_key = strdup(merged_into_normalized_or_null);
        pthread_rwlock_unlock(&s_lock);
        return 0;
    }
    geo_authority_entry_t *ent = (geo_authority_entry_t *)calloc(1, sizeof(*ent));
    if (!ent) {
        pthread_rwlock_unlock(&s_lock);
        return -1;
    }
    ent->display_name = strdup(display_name && display_name[0] ? display_name : name_normalized);
    if (!ent->display_name) {
        free(ent);
        pthread_rwlock_unlock(&s_lock);
        return -1;
    }
    ent->parent_id = parent_id;
    ent->trust_score = trust_score;
    if (merged_into_normalized_or_null && merged_into_normalized_or_null[0]) {
        ent->merged_into_key = strdup(merged_into_normalized_or_null);
        if (!ent->merged_into_key) {
            geo_entry_free(ent);
            pthread_rwlock_unlock(&s_lock);
            return -1;
        }
    }
    if (m4_ht_set(s_ht, name_normalized, ent) != 0) {
        geo_entry_free(ent);
        pthread_rwlock_unlock(&s_lock);
        return -1;
    }
    pthread_rwlock_unlock(&s_lock);
    return 0;
}

void geo_authority_prompt_hint(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    size_t n = (size_t)snprintf(out, out_size,
        "[GEO_AUTHORITY_HINT] Prefer only these provincial-level names when listing Vietnam places "
        "(system seed; user cache may add more): ");
    if (n >= out_size) {
        out[out_size - 1] = '\0';
        return;
    }
    size_t pos = strlen(out);
    for (size_t i = 0; i < sizeof(GEO_SEED_DISPLAY) / sizeof(GEO_SEED_DISPLAY[0]) && pos + 2 < out_size; i++) {
        int w = snprintf(out + pos, out_size - pos, "%s%s",
                           i > 0 ? ", " : "",
                           GEO_SEED_DISPLAY[i]);
        if (w <= 0 || (size_t)w >= out_size - pos) break;
        pos += (size_t)w;
    }
    if (pos + 2 < out_size) {
        out[pos++] = '.';
        out[pos++] = '\n';
        out[pos] = '\0';
    }
}

static int is_boundary(unsigned char c) {
    return c <= ' ' || c == ',' || c == '.' || c == ';' || c == ':' || c == '(' || c == ')'
        || c == '"' || c == '\'' || c == '-' || c == '\n' || c == '\r';
}

static int word_has_alpha(const char *w, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)w[i];
        if (isalpha(c) || c >= 0x80) return 1;
    }
    return 0;
}

void geo_authority_audit_response_text(const char *utf8) {
    if (!s_init || !s_ht || !utf8 || !utf8[0]) return;
    const char *p = utf8;
    int unknowns = 0;
    char word[384];
    char key[384];
    while (*p && unknowns < 24) {
        while (*p && is_boundary((unsigned char)*p)) p++;
        if (!*p) break;
        const char *w0 = p;
        while (*p && !is_boundary((unsigned char)*p)) p++;
        size_t wl = (size_t)(p - w0);
        if (wl < 4 || wl >= sizeof(word)) continue;
        memcpy(word, w0, wl);
        word[wl] = '\0';
        if (!word_has_alpha(word, wl)) continue;
        geo_authority_normalize_key(word, key, sizeof(key));
        if (strlen(key) < 4) continue;
        pthread_rwlock_rdlock(&s_lock);
        const geo_authority_entry_t *e = lookup_unlocked(key);
        pthread_rwlock_unlock(&s_lock);
        if (!e) {
            fprintf(stderr, "[GEO_AUTH] not in authority cache: \"%s\" (key=%s)\n", word, key);
            unknowns++;
        }
    }
}

/** Extract first CSV field (quoted or unquoted). Returns bytes written to out. */
static size_t csv_first_field(const char *line, char *out, size_t outsz) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t n = 0;
        while (*p && *p != '"' && n < outsz - 1) {
            if (*p == '\\' && p[1] == '"') {
                out[n++] = '"';
                p += 2;
                continue;
            }
            out[n++] = *p++;
        }
        out[n] = '\0';
        return n;
    }
    size_t n = 0;
    while (*p && *p != ',' && *p != '\r' && *p != '\n' && n < outsz - 1)
        out[n++] = *p++;
    out[n] = '\0';
    return n;
}

/** Pointer to start of second CSV field, or NULL if no comma. */
static const char *csv_after_first_field(const char *line) {
    const char *p = line;
    while (*p && *p != ',') {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        } else
            p++;
    }
    return (*p == ',') ? p + 1 : NULL;
}

int geo_authority_load_buffer(const char *data) {
    if (!data || !s_init || !s_ht) return -1;
    while (*data == ' ' || *data == '\t' || *data == '\n' || *data == '\r') data++;
    if (data[0] == '{' || data[0] == '[') {
        fprintf(stderr, "[GEO_AUTH] JSON buffer load not implemented; use CSV\n");
        return -1;
    }

    const char *line = data;
    while (*line && *line != '\n' && *line != '\r') line++;
    if (line == data) return -1;

    int inserted = 0;
    if (*line == '\r') line++;
    if (*line == '\n') line++;

    char namebuf[512];
    char key[512];
    while (*line) {
        size_t linelen = 0;
        const char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') {
            eol++;
            linelen++;
        }
        if (linelen == 0) {
            if (*eol == '\r') eol++;
            if (*eol == '\n') eol++;
            line = eol;
            continue;
        }
        char rowbuf[2048];
        if (linelen >= sizeof(rowbuf)) linelen = sizeof(rowbuf) - 1;
        memcpy(rowbuf, line, linelen);
        rowbuf[linelen] = '\0';
        csv_first_field(rowbuf, namebuf, sizeof(namebuf));
        if (namebuf[0]) {
            geo_authority_normalize_key(namebuf, key, sizeof(key));
            char merge_norm[512];
            merge_norm[0] = '\0';
            const char *rest = csv_after_first_field(rowbuf);
            if (rest) {
                char mergebuf[512];
                csv_first_field(rest, mergebuf, sizeof(mergebuf));
                if (mergebuf[0])
                    geo_authority_normalize_key(mergebuf, merge_norm, sizeof(merge_norm));
            }
            pthread_rwlock_wrlock(&s_lock);
            int r = insert_entry(namebuf, key, -1, 1.0, merge_norm[0] ? merge_norm : NULL);
            pthread_rwlock_unlock(&s_lock);
            if (r > 0) inserted += r;
        }
        line = eol;
        if (*line == '\r') line++;
        if (*line == '\n') line++;
    }
    fprintf(stderr, "[GEO_AUTH] load_buffer: inserted %d rows\n", inserted);
    return inserted;
}
