/*
 * Minimal SharedCollection registry from JSON file (path from api_options_t.shared_collection_json_path).
 * Spec: .cursor/shared_collection.md §6.1, §2.3
 */
#include "shared_collection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_COL_MAX 64
#define SC_STR_MAX 160
#define SC_FIELD_MAX 32
#define SC_HINT_VAL_MAX 256

typedef struct {
    char key[64];
    char hint[SC_HINT_VAL_MAX];
} sc_field_hint_t;

typedef struct {
    char collection[SC_STR_MAX];
    int elk_allow;
    char elk_index[SC_STR_MAX];
    char alias[SC_STR_MAX];
    sc_field_hint_t field_hints[SC_FIELD_MAX];
    size_t n_hints;
} sc_entry_t;

struct sc_registry {
    sc_entry_t entries[SC_COL_MAX];
    size_t n;
};

static const char *sc_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
    return p;
}

static int sc_parse_string(const char **pp, char *out, size_t cap) {
    const char *p = sc_skip_ws(*pp);
    if (*p != '"')
        return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (i + 1 < cap)
                out[i++] = *p++;
            else
                p++;
            continue;
        }
        if (i + 1 < cap)
            out[i++] = *p++;
        else
            p++;
    }
    if (*p != '"')
        return -1;
    out[i] = '\0';
    *pp = p + 1;
    return 0;
}

static int sc_parse_bool(const char **pp, int *out) {
    const char *p = sc_skip_ws(*pp);
    if (strncmp(p, "true", 4) == 0) {
        *pp = p + 4;
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *pp = p + 5;
        *out = 0;
        return 0;
    }
    return -1;
}

/** Skip a JSON value (string, number, bool, null, object, array) starting at *pp. */
static void sc_skip_value(const char **pp, const char *end) {
    const char *p = sc_skip_ws(*pp);
    if (p >= end) return;
    if (*p == '"') { /* string */
        p++;
        while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) p++; p++; }
        if (p < end) p++;
    } else if (*p == '{') { /* object */
        int d = 1; p++;
        while (p < end && d > 0) { if (*p == '{') d++; else if (*p == '}') d--; p++; }
    } else if (*p == '[') { /* array */
        int d = 1; p++;
        while (p < end && d > 0) { if (*p == '[') d++; else if (*p == ']') d--; p++; }
    } else { /* number, bool, null */
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    }
    *pp = p;
}

/** Parse field_hints: { "key": "hint", ... } into ent->field_hints[]. */
static void sc_parse_field_hints(const char *start, const char *end, sc_entry_t *ent) {
    const char *p = sc_skip_ws(start);
    if (p >= end || *p != '{') return;
    p++;
    while (p < end && *p != '}') {
        p = sc_skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p != '"') break;
        char key[64], val[SC_HINT_VAL_MAX];
        if (sc_parse_string(&p, key, sizeof(key)) != 0) break;
        p = sc_skip_ws(p);
        if (*p != ':') break;
        p++;
        p = sc_skip_ws(p);
        if (sc_parse_string(&p, val, sizeof(val)) != 0) break;
        if (ent->n_hints < SC_FIELD_MAX) {
            snprintf(ent->field_hints[ent->n_hints].key, sizeof(ent->field_hints[0].key), "%s", key);
            snprintf(ent->field_hints[ent->n_hints].hint, sizeof(ent->field_hints[0].hint), "%s", val);
            ent->n_hints++;
        }
    }
}

/** Parse "metadata": { "field_hints": {...}, ... } block. */
static void sc_parse_metadata_block(const char *inner, size_t ilen, sc_entry_t *ent) {
    const char *end = inner + ilen;
    for (const char *ip = inner; ip < end - 12; ip++) {
        if (*ip != '"') continue;
        const char *iq = ip + 1;
        if (strncmp(iq, "field_hints", 11) == 0 && iq[11] == '"') {
            iq += 12;
            iq = sc_skip_ws(iq);
            if (*iq != ':') continue;
            iq++;
            iq = sc_skip_ws(iq);
            sc_parse_field_hints(iq, end, ent);
            return;
        }
    }
}

/* Parse one JSON object body (content between outer { }) for collection, elk, alias, metadata */
static void sc_parse_collection_object(const char *start, size_t len, sc_entry_t *ent) {
    memset(ent, 0, sizeof(*ent));
    ent->elk_allow = 0;
    ent->n_hints = 0;
    char buf[SC_STR_MAX];
    const char *end = start + len;
    for (const char *p = start; p < end - 3; p++) {
        if (*p != '"')
            continue;
        const char *q = p + 1;
        /* "collection" */
        if ((size_t)(end - q) > 10 && strncmp(q, "collection", 10) == 0 && q[10] == '"') {
            q += 11;
            q = sc_skip_ws(q);
            if (*q != ':')
                continue;
            q++;
            if (sc_parse_string(&q, ent->collection, sizeof(ent->collection)) == 0)
                p = q;
            continue;
        }
        /* "alias" */
        if ((size_t)(end - q) > 5 && strncmp(q, "alias", 5) == 0 && q[5] == '"') {
            q += 6;
            q = sc_skip_ws(q);
            if (*q != ':')
                continue;
            q++;
            if (sc_parse_string(&q, ent->alias, sizeof(ent->alias)) == 0)
                p = q;
            continue;
        }
        /* "metadata" */
        if ((size_t)(end - q) > 8 && strncmp(q, "metadata", 8) == 0 && q[8] == '"') {
            q += 9;
            q = sc_skip_ws(q);
            if (*q != ':')
                continue;
            q++;
            q = sc_skip_ws(q);
            if (*q != '{')
                continue;
            int depth = 1;
            q++;
            const char *mb = q;
            while (mb < end && depth > 0) {
                if (*mb == '{') depth++;
                else if (*mb == '}') depth--;
                mb++;
            }
            sc_parse_metadata_block(q, (size_t)(mb > q ? mb - 1 - q : 0), ent);
            p = mb;
            continue;
        }
        /* "elk" */
        if ((size_t)(end - q) > 3 && strncmp(q, "elk", 3) == 0 && q[3] == '"') {
            q += 4;
            q = sc_skip_ws(q);
            if (*q != ':')
                continue;
            q++;
            q = sc_skip_ws(q);
            if (*q != '{')
                continue;
            int depth = 1;
            q++;
            const char *eb = q;
            while (eb < end && depth > 0) {
                if (*eb == '{')
                    depth++;
                else if (*eb == '}')
                    depth--;
                eb++;
            }
            const char *inner = q;
            size_t ilen = (size_t)((eb > inner && depth == 0) ? (eb - 1 - inner) : 0);
            for (const char *ip = inner; ilen > 0 && (size_t)(inner + ilen - ip) > 8; ip++) {
                if (*ip != '"')
                    continue;
                const char *iq = ip + 1;
                if (strncmp(iq, "allow", 5) == 0 && iq[5] == '"') {
                    iq += 6;
                    iq = sc_skip_ws(iq);
                    if (*iq != ':')
                        continue;
                    iq++;
                    sc_parse_bool(&iq, &ent->elk_allow);
                } else if (strncmp(iq, "index", 5) == 0 && iq[5] == '"') {
                    iq += 6;
                    iq = sc_skip_ws(iq);
                    if (*iq != ':')
                        continue;
                    iq++;
                    if (sc_parse_string(&iq, buf, sizeof(buf)) == 0)
                        snprintf(ent->elk_index, sizeof(ent->elk_index), "%s", buf);
                }
            }
            p = eb;
        }
    }
}

static int sc_find_collections_array(const char *json, const char **out_start, const char **out_end) {
    const char *p = json;
    const char *key = strstr(p, "\"collections\"");
    if (!key)
        return -1;
    p = key + 13;
    p = sc_skip_ws(p);
    if (*p != ':')
        return -1;
    p++;
    p = sc_skip_ws(p);
    if (*p != '[')
        return -1;
    p++;
    *out_start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '[')
            depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 0) {
                *out_end = p;
                return 0;
            }
        }
        p++;
    }
    return -1;
}

sc_registry_t *sc_registry_load_file(const char *path) {
    if (!path || !path[0])
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(8 * 1024 * 1024)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';

    sc_registry_t *reg = (sc_registry_t *)calloc(1, sizeof(*reg));
    if (!reg) {
        free(buf);
        return NULL;
    }

    const char *arr_s, *arr_e;
    if (sc_find_collections_array(buf, &arr_s, &arr_e) != 0) {
        free(buf);
        free(reg);
        return NULL;
    }

    for (const char *p = arr_s; p < arr_e; p++) {
        p = sc_skip_ws((char *)p);
        if (p >= arr_e || *p == ']')
            break;
        if (*p == ',')
            continue;
        if (*p != '{')
            continue;
        int depth = 1;
        const char *ob = p + 1;
        const char *q = ob;
        while (q < arr_e && depth > 0) {
            if (*q == '{')
                depth++;
            else if (*q == '}') {
                depth--;
                if (depth == 0) {
                    if (reg->n < SC_COL_MAX) {
                        sc_parse_collection_object(ob, (size_t)(q - ob), &reg->entries[reg->n]);
                        if (reg->entries[reg->n].collection[0])
                            reg->n++;
                    }
                    p = q;
                    break;
                }
            }
            q++;
        }
    }

    free(buf);
    return reg;
}

void sc_registry_free(sc_registry_t *reg) {
    free(reg);
}

size_t sc_registry_elk_count(const sc_registry_t *reg) {
    if (!reg)
        return 0;
    size_t c = 0;
    for (size_t i = 0; i < reg->n; i++) {
        if (reg->entries[i].elk_allow)
            c++;
    }
    return c;
}

int sc_registry_elk_index(const sc_registry_t *reg, const char *collection, char *out, size_t out_sz) {
    if (!reg || !collection || !out || out_sz == 0)
        return -1;
    for (size_t i = 0; i < reg->n; i++) {
        if (strcmp(reg->entries[i].collection, collection) != 0)
            continue;
        if (!reg->entries[i].elk_allow)
            return -1;
        const char *ix = reg->entries[i].elk_index;
        if (!ix[0] || strcmp(ix, ".") == 0)
            snprintf(out, out_sz, "idx_%s", collection);
        else
            snprintf(out, out_sz, "%s", ix);
        return 0;
    }
    return -1;
}

int sc_registry_elk_allowed(const sc_registry_t *reg, const char *collection) {
    if (!reg || !collection)
        return 0;
    for (size_t i = 0; i < reg->n; i++) {
        if (strcmp(reg->entries[i].collection, collection) == 0)
            return reg->entries[i].elk_allow ? 1 : 0;
    }
    return 0;
}

void sc_registry_foreach_elk(const sc_registry_t *reg,
                             void (*fn)(const char *collection, const char *elk_index, void *u),
                             void *u) {
    if (!reg || !fn)
        return;
    for (size_t i = 0; i < reg->n; i++) {
        if (!reg->entries[i].elk_allow)
            continue;
        char idx[SC_STR_MAX];
        if (sc_registry_elk_index(reg, reg->entries[i].collection, idx, sizeof idx) != 0)
            continue;
        fn(reg->entries[i].collection, idx, u);
    }
}

void sc_registry_foreach(const sc_registry_t *reg, sc_registry_foreach_fn fn, void *user) {
    if (!reg || !fn)
        return;
    for (size_t i = 0; i < reg->n; i++) {
        if (!reg->entries[i].collection[0])
            continue;
        fn(reg->entries[i].collection, reg->entries[i].elk_allow ? 1 : 0, user);
    }
}

/* ---------- Field existence check ---------- */

int sc_registry_has_field(const sc_registry_t *reg, const char *collection, const char *field) {
    if (!reg || !collection || !field) return 0;
    for (size_t i = 0; i < reg->n; i++) {
        if (strcmp(reg->entries[i].collection, collection) != 0) continue;
        for (size_t h = 0; h < reg->entries[i].n_hints; h++) {
            if (strcmp(reg->entries[i].field_hints[h].key, field) == 0) return 1;
        }
        return 0; /* collection found but field not in hints */
    }
    return 0;
}

/* ---------- Schema summary for LLM prompts ---------- */

size_t sc_registry_schema_summary(const sc_registry_t *reg, char *buf, size_t cap) {
    if (!reg || !buf || cap == 0) return 0;
    size_t pos = 0;
    for (size_t i = 0; i < reg->n && pos + 10 < cap; i++) {
        const sc_entry_t *e = &reg->entries[i];
        if (!e->elk_allow || !e->collection[0]) continue;
        pos += (size_t)snprintf(buf + pos, cap - pos, "- %s", e->collection);
        if (e->alias[0])
            pos += (size_t)snprintf(buf + pos, cap - pos, " (%s)", e->alias);
        pos += (size_t)snprintf(buf + pos, cap - pos, ": {");
        for (size_t h = 0; h < e->n_hints && pos + 50 < cap; h++) {
            if (h > 0) pos += (size_t)snprintf(buf + pos, cap - pos, ", ");
            pos += (size_t)snprintf(buf + pos, cap - pos, "%s", e->field_hints[h].key);
            if (e->field_hints[h].hint[0])
                pos += (size_t)snprintf(buf + pos, cap - pos, " (%s)", e->field_hints[h].hint);
        }
        pos += (size_t)snprintf(buf + pos, cap - pos, "}\n");
    }
    buf[pos] = '\0';
    return pos;
}

/* ---------- Term vocabulary for NL cue learning ---------- */

#define SC_VOCAB_MAX 512

typedef struct {
    char term[128];
    char collection[SC_STR_MAX];
    char field[64];             /* empty = collection-level match */
} sc_vocab_entry_t;

struct sc_term_vocab {
    sc_vocab_entry_t entries[SC_VOCAB_MAX];
    size_t n;
};

static void vocab_lower(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[j++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    dst[j] = '\0';
}

static void vocab_add(sc_term_vocab_t *v, const char *term, const char *collection, const char *field) {
    if (!v || !term || !term[0] || v->n >= SC_VOCAB_MAX) return;
    /* skip duplicates */
    for (size_t i = 0; i < v->n; i++) {
        if (strcmp(v->entries[i].term, term) == 0 &&
            strcmp(v->entries[i].collection, collection) == 0 &&
            strcmp(v->entries[i].field, field ? field : "") == 0)
            return;
    }
    sc_vocab_entry_t *e = &v->entries[v->n];
    vocab_lower(e->term, sizeof(e->term), term);
    snprintf(e->collection, sizeof(e->collection), "%s", collection);
    if (field)
        snprintf(e->field, sizeof(e->field), "%s", field);
    else
        e->field[0] = '\0';
    v->n++;
}

/** Try to add singular form: strip trailing 's'. */
static void vocab_add_singular(sc_term_vocab_t *v, const char *plural, const char *collection) {
    size_t len = strlen(plural);
    if (len > 2 && plural[len - 1] == 's') {
        char singular[128];
        if (len - 1 < sizeof(singular)) {
            memcpy(singular, plural, len - 1);
            singular[len - 1] = '\0';
            vocab_add(v, singular, collection, NULL);
        }
    }
}

/** Tokenize a hint value string and add each word as a vocab entry. */
static void vocab_add_hint_tokens(sc_term_vocab_t *v, const char *hint, const char *collection, const char *field) {
    char buf[SC_HINT_VAL_MAX];
    vocab_lower(buf, sizeof(buf), hint);
    /* split on comma, space, parentheses */
    char *save = NULL;
    char *tok = strtok_r(buf, " ,.()/", &save);
    while (tok) {
        if (strlen(tok) >= 2) /* skip single chars */
            vocab_add(v, tok, collection, field);
        tok = strtok_r(NULL, " ,.()/", &save);
    }
}

/** Tokenize alias string and add each word. */
static void vocab_add_alias_tokens(sc_term_vocab_t *v, const char *alias, const char *collection) {
    char buf[SC_STR_MAX];
    vocab_lower(buf, sizeof(buf), alias);
    char *save = NULL;
    char *tok = strtok_r(buf, " ,.()/", &save);
    while (tok) {
        if (strlen(tok) >= 2)
            vocab_add(v, tok, collection, NULL);
        tok = strtok_r(NULL, " ,.()/", &save);
    }
}

sc_term_vocab_t *sc_term_vocab_build(const sc_registry_t *reg) {
    if (!reg) return NULL;
    sc_term_vocab_t *v = (sc_term_vocab_t *)calloc(1, sizeof(*v));
    if (!v) return NULL;

    for (size_t i = 0; i < reg->n; i++) {
        const sc_entry_t *e = &reg->entries[i];
        if (!e->collection[0]) continue;
        const char *col = e->collection;

        /* 1. Collection name + singular */
        vocab_add(v, col, col, NULL);
        vocab_add_singular(v, col, col);

        /* 2. Alias tokens */
        if (e->alias[0])
            vocab_add_alias_tokens(v, e->alias, col);

        /* 3. Field hints: key = field name, value = description tokens */
        for (size_t h = 0; h < e->n_hints; h++) {
            const char *key = e->field_hints[h].key;
            const char *hint = e->field_hints[h].hint;
            /* field name itself */
            vocab_add(v, key, col, key);
            /* underscore split: "sold_date" → "sold", "date" */
            char kbuf[64];
            snprintf(kbuf, sizeof(kbuf), "%s", key);
            char *ksave = NULL;
            char *ktok = strtok_r(kbuf, "_", &ksave);
            while (ktok) {
                if (strlen(ktok) >= 2)
                    vocab_add(v, ktok, col, key);
                ktok = strtok_r(NULL, "_", &ksave);
            }
            /* hint value tokens → field-level vocab */
            if (hint[0])
                vocab_add_hint_tokens(v, hint, col, key);
        }
    }
    return v;
}

void sc_term_vocab_free(sc_term_vocab_t *v) {
    free(v);
}

int sc_term_vocab_lookup(const sc_term_vocab_t *v, const char *term,
                         const char **out_collection, const char **out_field) {
    if (!v || !term || !term[0]) return -1;
    char norm[128];
    vocab_lower(norm, sizeof(norm), term);
    for (size_t i = 0; i < v->n; i++) {
        if (strcmp(v->entries[i].term, norm) == 0) {
            if (out_collection) *out_collection = v->entries[i].collection;
            if (out_field) *out_field = v->entries[i].field[0] ? v->entries[i].field : NULL;
            return 0;
        }
    }
    return -1;
}

size_t sc_term_vocab_count(const sc_term_vocab_t *v) {
    return v ? v->n : 0;
}
