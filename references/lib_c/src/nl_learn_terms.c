#include "nl_learn_terms.h"
#include "utils.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NL_TERM_MAX 512
#define NL_INTENT_MAX 128
#define NL_LINE_MAX 4096
/** WAL compaction threshold: compact when WAL exceeds this many lines. */
#define NL_WAL_COMPACT_LINES 500

/** 0 = errors only; 1 = lifecycle; 2 = verbose (record + score_sum). Default 1 when env unset. */
static int nl_learn_log_level(void) {
    const char *e = getenv("M4_NL_LEARN_LOG");
    if (!e || !e[0]) return 1;
    if (e[0] == '0' && e[1] == '\0') return 0;
    if (strcmp(e, "false") == 0 || strcmp(e, "no") == 0 || strcmp(e, "off") == 0) return 0;
    if (strcmp(e, "verbose") == 0 || (e[0] == '2' && e[1] == '\0')) return 2;
    return 1;
}

static void nl_learn_vlog(int min_level, const char *fmt, ...) {
    if (nl_learn_log_level() < min_level) return;
    fprintf(stderr, "[nl_learn_terms] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void nl_learn_verr(const char *fmt, ...) {
    fprintf(stderr, "[nl_learn_terms] ERROR ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

struct nl_learn_terms {
    char *path;
    m4_ht_t *counts;
    int enable_write;
    int dirty;
    /** 1: JSON v2 (nl_learn_terms_v2); 0: TSV v1 (magic line + tab rows). */
    int format_json;
    /** WAL: append-only log for O(1) per-turn writes. */
    char *wal_path;     /* path + ".wal" */
    FILE *wal_fp;       /* kept open for append; NULL when read-only or unavailable */
    size_t wal_lines;   /* lines written since last compaction */
};

static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int valid_term(const char *t) {
    if (!t || !t[0]) return -1;
    size_t L = strlen(t);
    if (L > NL_TERM_MAX) return -1;
    for (size_t i = 0; i < L; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == '\t' || c == '\n' || c == '\r') return -1;
        if (c < 0x20u || c == 0x7fu) return -1;
    }
    return 0;
}

static int valid_intent(const char *t) {
    if (!t || !t[0]) return -1;
    size_t L = strlen(t);
    if (L > NL_INTENT_MAX) return -1;
    for (size_t i = 0; i < L; i++) {
        char c = t[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == ':')) return -1;
    }
    return 0;
}

/** elk_nl_routing.md §4 — keep in sync with python_ai NL_LEARN_ALLOWED_INTENTS.
 *  Also allows "SC:{collection}" prefix for SharedCollection entity learning (§5b). */
static int intent_on_closed_list(const char *t) {
    static const char *const allowed[] = {
        "CHAT",
        "ELK_ANALYTICS",
        "ELK_SEARCH",
        "RAG_VECTOR",
        NULL,
    };
    for (size_t i = 0; allowed[i]; i++) {
        if (strcmp(t, allowed[i]) == 0) return 0;
    }
    /* SC:{collection} prefix for SharedCollection entity terms. */
    if (strncmp(t, "SC:", 3) == 0 && t[3] != '\0') return 0;
    return -1;
}

static int nl_learn_relax_intent_closed_list(void) {
    const char *e = getenv("M4_NL_LEARN_RELAX_INTENT");
    if (!e || !e[0] || (e[0] == '0' && e[1] == '\0')) return 0;
    if (strcmp(e, "false") == 0 || strcmp(e, "no") == 0 || strcmp(e, "off") == 0) return 0;
    return 1;
}

/** Syntax plus §4 closed list unless M4_NL_LEARN_RELAX_INTENT is set. */
static int intent_acceptable_for_storage(const char *t) {
    if (valid_intent(t) != 0) return -1;
    if (nl_learn_relax_intent_closed_list()) return 0;
    return intent_on_closed_list(t);
}

static int make_key(char *buf, size_t cap, const char *term, const char *intent) {
    if (valid_term(term) != 0 || intent_acceptable_for_storage(intent) != 0) return -1;
    if (snprintf(buf, cap, "%s\t%s", term, intent) >= (int)cap) return -1;
    return 0;
}

static void free_long(void *p) { free(p); }

/* ---------- JSON v2 (schema nl_learn_terms_v2) ---------- */

static const char *nlj_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}

static int nlj_parse_string(const char **pp, char *out, size_t cap) {
    const char *p = nlj_skip_ws(*pp);
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (i + 1 < cap) out[i++] = *p++;
            else p++;
            continue;
        }
        if (i + 1 < cap) out[i++] = *p++;
        else p++;
    }
    if (*p != '"') return -1;
    out[i] = '\0';
    *pp = p + 1;
    return 0;
}

static int nlj_skip_value(const char **pp);

static int nlj_skip_object(const char **pp) {
    const char *p = nlj_skip_ws(*pp);
    if (*p != '{') return -1;
    p++;
    while (1) {
        p = nlj_skip_ws(p);
        if (*p == '}') {
            p++;
            *pp = p;
            return 0;
        }
        char skip_key[256];
        if (nlj_parse_string(&p, skip_key, sizeof skip_key)) return -1;
        p = nlj_skip_ws(p);
        if (*p != ':') return -1;
        p++;
        if (nlj_skip_value(&p)) return -1;
        p = nlj_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            p++;
            *pp = p;
            return 0;
        }
        return -1;
    }
}

static int nlj_skip_array(const char **pp) {
    const char *p = nlj_skip_ws(*pp);
    if (*p != '[') return -1;
    p++;
    while (1) {
        p = nlj_skip_ws(p);
        if (*p == ']') {
            p++;
            *pp = p;
            return 0;
        }
        if (nlj_skip_value(&p)) return -1;
        p = nlj_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            p++;
            *pp = p;
            return 0;
        }
        return -1;
    }
}

static int nlj_skip_value(const char **pp) {
    const char *p = nlj_skip_ws(*pp);
    if (*p == '{') return nlj_skip_object(pp);
    if (*p == '[') return nlj_skip_array(pp);
    if (*p == '"') {
        char tmp[8];
        return nlj_parse_string(pp, tmp, sizeof tmp);
    }
    if (strncmp(p, "true", 4) == 0) {
        *pp = p + 4;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *pp = p + 5;
        return 0;
    }
    if (strncmp(p, "null", 4) == 0) {
        *pp = p + 4;
        return 0;
    }
    if (*p == '-') p++;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        while (*p >= '0' && *p <= '9') p++;
    }
    *pp = p;
    return 0;
}

static int nlj_parse_int64(const char **pp, int64_t *out) {
    const char *p = nlj_skip_ws(*pp);
    errno = 0;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p || errno != 0) return -1;
    *out = (int64_t)v;
    *pp = end;
    return 0;
}

static int parse_map_entries(const char **pp, const char *term, nl_learn_terms_t *lt, size_t *ok,
                             size_t *skip) {
    while (1) {
        const char *p = nlj_skip_ws(*pp);
        if (*p == '}') {
            *pp = p + 1;
            return 0;
        }
        char intent_buf[NL_INTENT_MAX + 8];
        if (nlj_parse_string(pp, intent_buf, sizeof intent_buf)) return -1;
        p = nlj_skip_ws(*pp);
        if (*p != ':') return -1;
        *pp = p + 1;
        int64_t v;
        if (nlj_parse_int64(pp, &v)) return -1;
        if (valid_term(term) != 0 || intent_acceptable_for_storage(intent_buf) != 0) {
            (*skip)++;
        } else {
            char kbuf[NL_TERM_MAX + NL_INTENT_MAX + 8];
            if (make_key(kbuf, sizeof kbuf, term, intent_buf) != 0) {
                (*skip)++;
            } else {
                int64_t *slot = (int64_t *)malloc(sizeof(int64_t));
                if (!slot) return -1;
                *slot = v;
                if (m4_ht_set(lt->counts, kbuf, slot) != 0) {
                    free(slot);
                    return -1;
                }
                (*ok)++;
            }
        }
        p = nlj_skip_ws(*pp);
        if (*p == ',') {
            *pp = p + 1;
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return 0;
        }
        return -1;
    }
}

static int parse_term_object(const char **pp, const char *term, nl_learn_terms_t *lt, size_t *ok,
                             size_t *skip) {
    while (1) {
        const char *p = nlj_skip_ws(*pp);
        if (*p == '}') {
            *pp = p + 1;
            return 0;
        }
        char kbuf[64];
        if (nlj_parse_string(pp, kbuf, sizeof kbuf)) return -1;
        p = nlj_skip_ws(*pp);
        if (*p != ':') return -1;
        *pp = p + 1;
        if (strcmp(kbuf, "map") == 0) {
            p = nlj_skip_ws(*pp);
            if (*p != '{') return -1;
            *pp = p + 1;
            if (parse_map_entries(pp, term, lt, ok, skip)) return -1;
        } else {
            if (nlj_skip_value(pp)) return -1;
        }
        p = nlj_skip_ws(*pp);
        if (*p == ',') {
            *pp = p + 1;
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return 0;
        }
        return -1;
    }
}

static int load_json_buf(nl_learn_terms_t *lt, const char *json, size_t *data_ok, size_t *data_skip) {
    size_t ok = 0, skip = 0;
    const char *p = json;
    const char *origin = json;
    p = nlj_skip_ws(p);
    if (*p != '{') { nl_learn_verr("load_json: expected '{' at offset %zu\n", (size_t)(p - origin)); return -1; }
    p++;
    while (1) {
        p = nlj_skip_ws(p);
        if (*p == '}') {
            p++;
            break;
        }
        char rkey[80];
        if (nlj_parse_string(&p, rkey, sizeof rkey)) { nl_learn_verr("load_json: bad root key at offset %zu\n", (size_t)(p - origin)); return -1; }
        p = nlj_skip_ws(p);
        if (*p != ':') { nl_learn_verr("load_json: expected ':' after key '%s' at offset %zu\n", rkey, (size_t)(p - origin)); return -1; }
        p++;
        if (strcmp(rkey, "terms") == 0) {
            p = nlj_skip_ws(p);
            if (*p != '{') { nl_learn_verr("load_json: expected '{' for terms at offset %zu\n", (size_t)(p - origin)); return -1; }
            p++;
            while (1) {
                p = nlj_skip_ws(p);
                if (*p == '}') {
                    p++;
                    break;
                }
                char term_buf[NL_TERM_MAX + 8];
                if (nlj_parse_string(&p, term_buf, sizeof term_buf)) { nl_learn_verr("load_json: bad term key at offset %zu\n", (size_t)(p - origin)); return -1; }
                p = nlj_skip_ws(p);
                if (*p != ':') { nl_learn_verr("load_json: expected ':' after term '%s' at offset %zu\n", term_buf, (size_t)(p - origin)); return -1; }
                p++;
                p = nlj_skip_ws(p);
                if (*p != '{') { nl_learn_verr("load_json: expected '{' for term object '%s' at offset %zu\n", term_buf, (size_t)(p - origin)); return -1; }
                p++;
                if (parse_term_object(&p, term_buf, lt, &ok, &skip)) { nl_learn_verr("load_json: parse_term_object failed for '%s' at offset %zu\n", term_buf, (size_t)(p - origin)); return -1; }
                p = nlj_skip_ws(p);
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p == '}') {
                    p++;
                    break;
                }
                nl_learn_verr("load_json: unexpected char '%c' after term '%s' at offset %zu\n", *p, term_buf, (size_t)(p - origin));
                return -1;
            }
        } else {
            if (nlj_skip_value(&p)) { nl_learn_verr("load_json: skip_value failed for key '%s' at offset %zu\n", rkey, (size_t)(p - origin)); return -1; }
        }
        p = nlj_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            p++;
            break;
        }
        nl_learn_verr("load_json: unexpected char '%c' after key '%s' at offset %zu\n", *p, rkey, (size_t)(p - origin));
        return -1;
    }
    if (data_ok) *data_ok = ok;
    if (data_skip) *data_skip = skip;
    (void)nlj_skip_ws(p);
    return 0;
}

static void nlj_fprint_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c < 0x20u)
            fprintf(f, "\\u%04x", (unsigned)c);
        else
            fputc((int)c, f);
    }
    fputc('"', f);
}

static int path_looks_json(const char *path) {
    size_t L = strlen(path);
    return L > 5 && strcmp(path + L - 5, ".json") == 0;
}

static char *read_entire_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    /* No fixed cap: allow large priors files. Reject only if (sz + 1) for NUL would overflow size_t. */
    if ((unsigned long long)sz > (unsigned long long)SIZE_MAX - 2ULL) {
        fclose(f);
        errno = EFBIG;
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    if (out_sz) *out_sz = (size_t)sz;
    return buf;
}

/** Returns 1 if JSON, 0 if TSV, -1 if path ends in `.json` but body is non-JSON text. */
static int sniff_format_json(const char *path, const char *buf, int path_is_json) {
    (void)path;
    const char *p = buf;
    if (p[0] == (char)0xef && p[1] == (char)0xbb && p[2] == (char)0xbf) p += 3;
    p = nlj_skip_ws(p);
    if (*p == '{') return 1;
    if (*p == '\0' && path_is_json) return 1;
    if (path_is_json && *p != '\0') return -1;
    return 0;
}

static int parse_data_line(char *line, char **term_out, char **intent_out, char **count_out) {
    char *t1 = strchr(line, '\t');
    if (!t1) return -1;
    *t1 = '\0';
    char *t2 = strchr(t1 + 1, '\t');
    if (!t2) return -1;
    *t2 = '\0';
    *term_out = line;
    *intent_out = t1 + 1;
    *count_out = t2 + 1;
    if (!(*term_out)[0] || !(*intent_out)[0] || !(*count_out)[0]) return -1;
    if (strchr(*count_out, '\t')) return -1;
    return 0;
}

static int load_tsv_buffer(nl_learn_terms_t *lt, char *buf, size_t *data_rows_ok, size_t *data_rows_skipped) {
    size_t ok = 0, skip = 0;
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    int first = 1;
    while (line) {
        trim_crlf(line);
        if (line[0] && line[0] != '#') {
            if (first) {
                first = 0;
                if (strcmp(line, NL_LEARN_TERMS_FILE_MAGIC) == 0) {
                    line = strtok_r(NULL, "\n", &save);
                    continue;
                }
            }
            char *term, *intent, *cstr;
            char *work = line;
            if (parse_data_line(work, &term, &intent, &cstr) != 0) {
                skip++;
            } else if (valid_term(term) != 0 || intent_acceptable_for_storage(intent) != 0) {
                skip++;
            } else {
                errno = 0;
                char *end = NULL;
                long long v = strtoll(cstr, &end, 10);
                if (errno != 0 || end == cstr || *end != '\0') {
                    skip++;
                } else {
                    char kbuf[NL_TERM_MAX + NL_INTENT_MAX + 8];
                    if (make_key(kbuf, sizeof kbuf, term, intent) != 0) {
                        skip++;
                    } else {
                        int64_t *slot = (int64_t *)malloc(sizeof(int64_t));
                        if (!slot) {
                            nl_learn_verr("load: OOM allocating count slot\n");
                            return -1;
                        }
                        *slot = v;
                        if (m4_ht_set(lt->counts, kbuf, slot) != 0) {
                            free(slot);
                            nl_learn_verr("load: m4_ht_set failed (OOM?)\n");
                            return -1;
                        }
                        ok++;
                    }
                }
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if (data_rows_ok) *data_rows_ok = ok;
    if (data_rows_skipped) *data_rows_skipped = skip;
    return 0;
}

/* ---------- WAL (write-ahead log) — append-only delta log ---------- */

/** Build WAL path: snapshot path + ".wal". Caller must free. */
static char *wal_path_from_snapshot(const char *path) {
    if (!path) return NULL;
    size_t plen = strlen(path);
    char *wp = (char *)malloc(plen + 5);
    if (!wp) return NULL;
    memcpy(wp, path, plen);
    memcpy(wp + plen, ".wal", 5);
    return wp;
}

/** Replay WAL lines into hash table. Format: term\tintent\tdelta\n */
static int wal_replay(nl_learn_terms_t *lt, size_t *replayed) {
    if (!lt || !lt->wal_path) return 0;
    size_t count = 0;
    FILE *f = fopen(lt->wal_path, "r");
    if (!f) {
        if (errno == ENOENT) {
            if (replayed) *replayed = 0;
            return 0;
        }
        nl_learn_verr("wal_replay: fopen(%s): %s\n", lt->wal_path, strerror(errno));
        return -1;
    }
    char line[NL_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        trim_crlf(line);
        if (!line[0]) continue;
        /* Parse: term\tintent\tdelta */
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = '\0';
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = '\0';
        const char *term = line;
        const char *intent = t1;
        int64_t delta = strtoll(t2, NULL, 10);
        if (valid_term(term) != 0 || intent_acceptable_for_storage(intent) != 0) continue;
        char kbuf[NL_TERM_MAX + NL_INTENT_MAX + 8];
        if (make_key(kbuf, sizeof kbuf, term, intent) != 0) continue;
        int64_t *vp = (int64_t *)m4_ht_get(lt->counts, kbuf);
        if (!vp) {
            vp = (int64_t *)malloc(sizeof(int64_t));
            if (!vp) { fclose(f); return -1; }
            *vp = 0;
            if (m4_ht_set(lt->counts, kbuf, vp) != 0) { free(vp); fclose(f); return -1; }
        }
        *vp += delta;
        count++;
    }
    fclose(f);
    lt->wal_lines = count;
    if (count > 0) lt->dirty = 1;
    if (replayed) *replayed = count;
    nl_learn_vlog(1, "wal_replay: path=%s lines=%zu\n", lt->wal_path, count);
    return 0;
}

/** Append one delta line to WAL. O(1) per call, no fsync. */
static int wal_append(nl_learn_terms_t *lt, const char *term, const char *intent, int delta) {
    if (!lt || !lt->wal_fp) return -1;
    if (fprintf(lt->wal_fp, "%s\t%s\t%d\n", term, intent, delta) < 0) {
        nl_learn_verr("wal_append: write failed: %s\n", strerror(errno));
        return -1;
    }
    fflush(lt->wal_fp);
    lt->wal_lines++;
    return 0;
}

/** Open WAL file for appending (write mode only). */
static void wal_open_append(nl_learn_terms_t *lt) {
    if (!lt || !lt->wal_path || !lt->enable_write) return;
    int is_new = (access(lt->wal_path, F_OK) != 0);
    lt->wal_fp = fopen(lt->wal_path, "a");
    if (!lt->wal_fp)
        nl_learn_verr("wal_open_append: fopen(%s): %s\n", lt->wal_path, strerror(errno));
    else if (is_new)
        nl_learn_vlog(1, "wal: created %s (append-only delta log for performance)\n", lt->wal_path);
}

/** Truncate WAL after successful snapshot compaction. */
static void wal_truncate(nl_learn_terms_t *lt) {
    if (!lt || !lt->wal_path) return;
    if (lt->wal_fp) { fclose(lt->wal_fp); lt->wal_fp = NULL; }
    FILE *f = fopen(lt->wal_path, "w");
    if (f) fclose(f);
    lt->wal_lines = 0;
    wal_open_append(lt);
    nl_learn_vlog(1, "wal_truncate: done path=%s\n", lt->wal_path);
}

static int load_auto(nl_learn_terms_t *lt, size_t *data_ok, size_t *data_skip) {
    size_t sz = 0;
    char *buf = read_entire_file(lt->path, &sz);
    int pj = path_looks_json(lt->path);

    if (!buf) {
        if (errno == ENOENT) {
            lt->format_json = pj ? 1 : 0;
            nl_learn_vlog(1, "load: no existing file (ok) path=%s format=%s\n", lt->path,
                          lt->format_json ? "json" : "tsv");
            if (data_ok) *data_ok = 0;
            if (data_skip) *data_skip = 0;
            return 0;
        }
        nl_learn_verr("load: read %s: %s\n", lt->path, strerror(errno));
        return -1;
    }

    int sniff = sniff_format_json(lt->path, buf, pj);
    if (sniff < 0) {
        nl_learn_verr("load: path ends in .json but file is not JSON object: %s\n", lt->path);
        free(buf);
        return -1;
    }
    lt->format_json = sniff;

    size_t ok = 0, skip = 0;
    int err = 0;
    if (lt->format_json) {
        err = load_json_buf(lt, buf, &ok, &skip);
        nl_learn_vlog(1, "load: json path=%s cells=%zu skipped=%zu\n", lt->path, ok, skip);
    } else {
        err = load_tsv_buffer(lt, buf, &ok, &skip);
        nl_learn_vlog(1, "load: tsv path=%s data_rows=%zu skipped=%zu\n", lt->path, ok, skip);
    }
    free(buf);
    if (data_ok) *data_ok = ok;
    if (data_skip) *data_skip = skip;
    return err;
}

typedef struct {
    char **keys;
    size_t n;
    size_t cap;
} key_list_t;

static void collect_key(const char *key, void *value, void *userdata) {
    (void)value;
    key_list_t *kl = (key_list_t *)userdata;
    if (!kl) return;
    if (kl->n >= kl->cap) {
        size_t ncap = kl->cap ? kl->cap * 2 : 64;
        char **nk = (char **)realloc(kl->keys, ncap * sizeof(char *));
        if (!nk) return;
        kl->keys = nk;
        kl->cap = ncap;
    }
    char *cpy = strdup(key);
    if (!cpy) return;
    kl->keys[kl->n++] = cpy;
}

static int scmp_key(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

nl_learn_terms_t *nl_learn_terms_open(const char *path, int enable_write) {
    if (!path || !path[0]) return NULL;
    nl_learn_terms_t *lt = (nl_learn_terms_t *)calloc(1, sizeof(nl_learn_terms_t));
    if (!lt) {
        nl_learn_verr("open: calloc lt failed\n");
        return NULL;
    }
    lt->path = strdup(path);
    lt->counts = m4_ht_create(64);
    lt->enable_write = enable_write ? 1 : 0;
    lt->dirty = 0;
    lt->format_json = 0;
    lt->wal_path = wal_path_from_snapshot(path);
    lt->wal_fp = NULL;
    lt->wal_lines = 0;
    if (!lt->path || !lt->counts) {
        nl_learn_verr("open: strdup or m4_ht_create failed path=%s\n", path);
        nl_learn_terms_close(lt);
        return NULL;
    }
    /* 1. Load snapshot (JSON or TSV). */
    if (load_auto(lt, NULL, NULL) != 0) {
        nl_learn_terms_close(lt);
        return NULL;
    }
    /* 2. Replay WAL deltas on top of snapshot. */
    size_t wal_replayed = 0;
    if (lt->wal_path && wal_replay(lt, &wal_replayed) != 0)
        nl_learn_verr("open: wal_replay failed (continuing without WAL)\n");
    /* 3. Open WAL for future appends (write mode only). */
    if (lt->enable_write)
        wal_open_append(lt);
    nl_learn_vlog(1, "open: path=%s enable_write=%d format=%s unique_keys=%zu wal_replayed=%zu\n",
                  path, lt->enable_write, lt->format_json ? "json" : "tsv",
                  (size_t)m4_ht_count(lt->counts), wal_replayed);
    return lt;
}

void nl_learn_terms_close(nl_learn_terms_t *lt) {
    if (!lt) return;
    nl_learn_vlog(1, "close: path=%s dirty=%d enable_write=%d wal_lines=%zu\n",
                  lt->path ? lt->path : "(null)", lt->dirty, lt->enable_write, lt->wal_lines);
    /* Compact on close: snapshot + truncate WAL so next open is clean. */
    if (lt->dirty && lt->enable_write) {
        int sr = nl_learn_terms_save(lt);
        if (sr != 0) nl_learn_verr("close: auto-save failed (path=%s)\n", lt->path ? lt->path : "");
    }
    if (lt->wal_fp) { fclose(lt->wal_fp); lt->wal_fp = NULL; }
    if (lt->counts) {
        m4_ht_destroy(lt->counts, free_long);
        lt->counts = NULL;
    }
    free(lt->wal_path);
    free(lt->path);
    free(lt);
    nl_learn_vlog(1, "close: done\n");
}

static int save_tsv(nl_learn_terms_t *lt, FILE *f) {
    if (fprintf(f, "%s\n", NL_LEARN_TERMS_FILE_MAGIC) < 0) return -1;
    key_list_t kl = {0};
    m4_ht_foreach(lt->counts, collect_key, &kl);
    if (kl.keys && kl.n > 0) qsort(kl.keys, kl.n, sizeof(char *), scmp_key);
    for (size_t i = 0; i < kl.n; i++) {
        const char *key = kl.keys[i];
        int64_t *vp = (int64_t *)m4_ht_get(lt->counts, key);
        if (!vp) continue;
        if (fprintf(f, "%s\t%lld\n", key, (long long)*vp) < 0) {
            for (size_t j = 0; j < kl.n; j++) free(kl.keys[j]);
            free(kl.keys);
            return -1;
        }
    }
    for (size_t j = 0; j < kl.n; j++) free(kl.keys[j]);
    free(kl.keys);
    return 0;
}

/** Split composite key "term\\tintent" into term_out / intent_out (pointers into key, mutated with \\0). */
static int split_composite_key(char *key_mut, char **term_out, char **intent_out) {
    char *tab = strchr(key_mut, '\t');
    if (!tab) return -1;
    *tab = '\0';
    *term_out = key_mut;
    *intent_out = tab + 1;
    return 0;
}

static int save_json(nl_learn_terms_t *lt, FILE *f) {
    key_list_t kl = {0};
    m4_ht_foreach(lt->counts, collect_key, &kl);
    if (kl.keys && kl.n > 0) qsort(kl.keys, kl.n, sizeof(char *), scmp_key);

    if (fprintf(f, "{\n  \"schema\": \"%s\",\n  \"terms\": {\n", NL_LEARN_TERMS_JSON_SCHEMA) < 0) goto fail;

    char prev_term[NL_TERM_MAX + 8];
    prev_term[0] = '\0';
    int first_term_outer = 1;
    int in_term = 0;
    int first_map_entry = 1;
    char key_copy[NL_TERM_MAX + NL_INTENT_MAX + 16];

    for (size_t i = 0; i < kl.n; i++) {
        if (strlen(kl.keys[i]) >= sizeof(key_copy)) continue;
        memcpy(key_copy, kl.keys[i], strlen(kl.keys[i]) + 1);
        char *term_p, *intent_p;
        if (split_composite_key(key_copy, &term_p, &intent_p) != 0) continue;
        int64_t *vp = (int64_t *)m4_ht_get(lt->counts, kl.keys[i]);
        if (!vp) continue;

        if (strcmp(term_p, prev_term) != 0) {
            if (in_term) {
                if (fprintf(f, "\n      }\n    }") < 0) goto fail;
            }
            if (!first_term_outer) {
                if (fprintf(f, ",\n") < 0) goto fail;
            }
            first_term_outer = 0;
            if (fprintf(f, "    ") < 0) goto fail;
            nlj_fprint_string(f, term_p);
            if (fprintf(f, ": {\n      \"map\": {\n        ") < 0) goto fail;
            snprintf(prev_term, sizeof prev_term, "%s", term_p);
            in_term = 1;
            first_map_entry = 1;
        }
        if (!first_map_entry) {
            if (fprintf(f, ",\n        ") < 0) goto fail;
        }
        first_map_entry = 0;
        nlj_fprint_string(f, intent_p);
        if (fprintf(f, ": %lld", (long long)*vp) < 0) goto fail;
    }
    if (in_term) {
        if (fprintf(f, "\n      }\n    }") < 0) goto fail;
    }
    if (fprintf(f, "\n  }\n}\n") < 0) goto fail;
    for (size_t j = 0; j < kl.n; j++) free(kl.keys[j]);
    free(kl.keys);
    return 0;
fail:
    for (size_t j = 0; j < kl.n; j++) free(kl.keys[j]);
    free(kl.keys);
    return -1;
}

int nl_learn_terms_save(nl_learn_terms_t *lt) {
    if (!lt || !lt->path || !lt->counts) {
        nl_learn_verr("save: invalid state (lt/path/counts)\n");
        return -1;
    }
    if (!lt->dirty) {
        nl_learn_vlog(1, "save: skipped (not dirty)\n");
        return 0;
    }

    size_t plen = strlen(lt->path);
    char *tmp = (char *)malloc(plen + 8);
    if (!tmp) {
        nl_learn_verr("save: OOM tmp path\n");
        return -1;
    }
    memcpy(tmp, lt->path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    nl_learn_vlog(1, "save: writing tmp=%s -> %s format=%s\n", tmp, lt->path,
                  lt->format_json ? "json" : "tsv");

    FILE *f = fopen(tmp, "w");
    if (!f) {
        nl_learn_verr("save: fopen(%s): %s\n", tmp, strerror(errno));
        free(tmp);
        return -1;
    }
    int wr = lt->format_json ? save_json(lt, f) : save_tsv(lt, f);
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    if (fclose(f) != 0) {
        nl_learn_verr("save: fclose(tmp): %s\n", strerror(errno));
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (wr != 0) {
        nl_learn_verr("save: write body failed\n");
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (rename(tmp, lt->path) != 0) {
        nl_learn_verr("save: rename(%s -> %s): %s\n", tmp, lt->path, strerror(errno));
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    lt->dirty = 0;
    /* Snapshot written — truncate WAL (all deltas are now in the snapshot). */
    if (lt->wal_lines > 0)
        wal_truncate(lt);
    nl_learn_vlog(1, "save: ok path=%s rows=%zu\n", lt->path, (size_t)m4_ht_count(lt->counts));
    return 0;
}

int nl_learn_terms_record(nl_learn_terms_t *lt, const char *const *term_keys, size_t nkeys,
                          const char *intent, int delta) {
    if (!lt || !lt->counts || !intent) {
        nl_learn_verr("record: invalid args\n");
        return -1;
    }
    if (!lt->enable_write) {
        nl_learn_vlog(2, "record: skipped (enable_write=0) intent=%s nkeys=%zu delta=%d\n",
                      intent, nkeys, delta);
        return 0;
    }
    if (intent_acceptable_for_storage(intent) != 0) {
        nl_learn_verr("record: intent not allowed (§4 closed list or bad syntax): %s\n", intent);
        return -1;
    }
    int64_t d = (int64_t)delta;
    for (size_t i = 0; i < nkeys; i++) {
        const char *term = term_keys[i];
        if (!term || !term[0]) continue;
        if (valid_term(term) != 0) {
            nl_learn_verr("record: invalid term at index %zu\n", i);
            return -1;
        }
        char kbuf[NL_TERM_MAX + NL_INTENT_MAX + 8];
        if (make_key(kbuf, sizeof kbuf, term, intent) != 0) {
            nl_learn_verr("record: make_key failed term=%s intent=%s\n", term, intent);
            return -1;
        }
        int64_t *vp = (int64_t *)m4_ht_get(lt->counts, kbuf);
        if (!vp) {
            vp = (int64_t *)malloc(sizeof(int64_t));
            if (!vp) return -1;
            *vp = 0;
            if (m4_ht_set(lt->counts, kbuf, vp) != 0) {
                free(vp);
                nl_learn_verr("record: m4_ht_set OOM\n");
                return -1;
            }
        }
        *vp += d;
        lt->dirty = 1;
        /* WAL: append delta instead of rewriting snapshot. */
        wal_append(lt, term, intent, delta);
    }
    /* Auto-compact when WAL grows too large. */
    if (lt->wal_lines >= NL_WAL_COMPACT_LINES) {
        nl_learn_vlog(1, "record: WAL hit %zu lines — auto-compacting\n", lt->wal_lines);
        (void)nl_learn_terms_save(lt);
    }
    nl_learn_vlog(2, "record: intent=%s nkeys=%zu delta=%d\n", intent, nkeys, delta);
    return 0;
}

int64_t nl_learn_terms_score_sum(nl_learn_terms_t *lt, const char *const *term_keys, size_t nkeys,
                                 const char *intent) {
    if (!lt || !lt->counts || !intent || intent_acceptable_for_storage(intent) != 0) {
        if (intent && nl_learn_log_level() >= 2)
            nl_learn_vlog(2, "score_sum: early exit (bad args or disallowed intent %s)\n", intent);
        return 0;
    }
    int64_t sum = 0;
    for (size_t i = 0; i < nkeys; i++) {
        const char *term = term_keys[i];
        if (!term || !term[0]) continue;
        if (valid_term(term) != 0) continue;
        char kbuf[NL_TERM_MAX + NL_INTENT_MAX + 8];
        if (make_key(kbuf, sizeof kbuf, term, intent) != 0) continue;
        int64_t *vp = (int64_t *)m4_ht_get(lt->counts, kbuf);
        if (vp) sum += *vp;
    }
    nl_learn_vlog(2, "score_sum: intent=%s nkeys=%zu sum=%lld\n", intent, nkeys, (long long)sum);
    return sum;
}

/** Callback data for score_text iteration. */
typedef struct {
    const char *text;     /* normalized user input */
    const char *intent;   /* intent to match */
    size_t intent_len;
    int64_t total;
} score_text_ctx_t;

static void score_text_cb(const char *composite_key, void *value, void *userdata) {
    score_text_ctx_t *sc = (score_text_ctx_t *)userdata;
    int64_t *vp = (int64_t *)value;
    if (!vp || *vp <= 0) return;

    /* composite_key = "term\tintent" — split to check intent match. */
    const char *tab = strchr(composite_key, '\t');
    if (!tab) return;
    const char *key_intent = tab + 1;
    size_t term_len = (size_t)(tab - composite_key);

    /* Check intent matches. */
    if (strcmp(key_intent, sc->intent) != 0) return;

    /* Check if term appears in the text. */
    char term_buf[NL_TERM_MAX + 1];
    if (term_len >= sizeof(term_buf)) return;
    memcpy(term_buf, composite_key, term_len);
    term_buf[term_len] = '\0';

    if (strstr(sc->text, term_buf) != NULL)
        sc->total += *vp;
}

int64_t nl_learn_terms_score_text(nl_learn_terms_t *lt, const char *normalized_text, const char *intent) {
    if (!lt || !lt->counts || !normalized_text || !normalized_text[0] || !intent) return 0;
    score_text_ctx_t sc = {
        .text = normalized_text,
        .intent = intent,
        .intent_len = strlen(intent),
        .total = 0
    };
    m4_ht_foreach(lt->counts, score_text_cb, &sc);
    nl_learn_vlog(2, "score_text: intent=%s total=%lld\n", intent, (long long)sc.total);
    return sc.total;
}