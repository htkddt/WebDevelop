/*
 * Conflict detector — post-inference list consistency (.cursor/conflict_detector.md).
 */
#include "conflict_detector.h"
#include "geo_authority.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIST_ITEMS 160
#define ITEM_KEY_SZ 256

static void trim_in_place(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[n - 1] = '\0';
        n--;
    }
}

/** Infer e.g. "33 tỉnh" / "33 provinces" from user text. Returns -1 if none. */
static int parse_expected_province_count(const char *user) {
    if (!user || !user[0]) return -1;
    for (const char *p = user; *p;) {
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }
        const char *start = p;
        char *endp = NULL;
        long n = strtol(p, &endp, 10);
        if (endp == p || n < 20 || n > 99) {
            p++;
            continue;
        }
        char win[200];
        size_t w = 0;
        for (const char *q = start; *q && w < sizeof(win) - 1; q++) win[w++] = *q;
        win[w] = '\0';
        char low[200];
        for (w = 0; win[w] && w < sizeof(low) - 1; w++)
            low[w] = (char)tolower((unsigned char)win[w]);
        low[w] = '\0';
        if (strstr(low, "province") || strstr(low, "tinh") || strstr(win, "tỉnh"))
            return (int)n;
        p = endp ? endp : p + 1;
    }
    return -1;
}

static int add_unique(char keys[][ITEM_KEY_SZ], int *n, const char *norm) {
    if (!norm || !norm[0]) return 0;
    for (int i = 0; i < *n; i++) {
        if (strcmp(keys[i], norm) == 0) return 0;
    }
    if (*n >= MAX_LIST_ITEMS) return -1;
    strncpy(keys[*n], norm, ITEM_KEY_SZ - 1);
    keys[*n][ITEM_KEY_SZ - 1] = '\0';
    (*n)++;
    return 1;
}

/** Split `seg` by commas / semicolons; normalize each piece into keys. */
static void split_and_collect(const char *seg, char keys[][ITEM_KEY_SZ], int *n) {
    char buf[1024];
    size_t L = strlen(seg);
    if (L >= sizeof(buf)) L = sizeof(buf) - 1;
    memcpy(buf, seg, L);
    buf[L] = '\0';

    for (char *p = buf, *tok; (tok = strtok(p, ",;")); p = NULL) {
        trim_in_place(tok);
        if (!tok[0]) continue;
        char nk[ITEM_KEY_SZ];
        geo_authority_normalize_key(tok, nk, sizeof(nk));
        if (strlen(nk) >= 2)
            (void)add_unique(keys, n, nk);
    }
}

/**
 * Scan `text` for numbered lines (1. / 1)) and collect unique normalized place fragments.
 */
static void extract_numbered_list_keys(const char *text, char keys[][ITEM_KEY_SZ], int *n) {
    const char *p = text ? text : "";
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        size_t line_len = (size_t)(p - line_start);
        char line[1024];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';
        if (*p == '\r') p++;
        if (*p == '\n') p++;

        trim_in_place(line);
        if (!line[0]) continue;
        char *q = line;
        if (!isdigit((unsigned char)*q)) continue;
        while (isdigit((unsigned char)*q)) q++;
        if (*q != '.' && *q != ')') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) continue;
        split_and_collect(q, keys, n);
    }
}

static void check_merger_conflicts(const char keys[][ITEM_KEY_SZ], int n, int *merger_conflict) {
    for (int i = 0; i < n; i++) {
        const geo_authority_entry_t *e = geo_authority_lookup(keys[i]);
        if (!e || !e->merged_into_key || !e->merged_into_key[0]) continue;
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            if (strcmp(e->merged_into_key, keys[j]) == 0) {
                *merger_conflict = 1;
                fprintf(stderr,
                        "[LOGIC_CONFLICT] merger: \"%s\" is merged into \"%s\" but both appear in the list.\n",
                        keys[i], keys[j]);
                return;
            }
        }
    }
}

int conflict_detector_analyze(const char *user_msg, const char *assistant_msg, conflict_result_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->expected_count = -1;

    out->expected_count = parse_expected_province_count(user_msg);

    char keys[MAX_LIST_ITEMS][ITEM_KEY_SZ];
    int n = 0;
    extract_numbered_list_keys(assistant_msg, keys, &n);
    out->observed_unique = n;

    if (out->expected_count >= 0 && n != out->expected_count) {
        out->count_mismatch = 1;
        fprintf(stderr, "[LOGIC_CONFLICT] expected %d unique numbered items (from user), observed %d.\n",
                out->expected_count, n);
    }

    check_merger_conflicts(keys, n, &out->merger_conflict);

    out->has_logic_conflict = (out->count_mismatch || out->merger_conflict) ? 1 : 0;

    if (out->has_logic_conflict) {
        if (out->count_mismatch && out->expected_count >= 0) {
            (void)snprintf(out->correction_note, sizeof(out->correction_note),
                "\n\n[LOGIC_CONFLICT] Note: Mắm đang say — list trên có %d mục (khác %d theo chuẩn bạn nêu). "
                "Kiểm tra lại sáp nhập / đếm tỉnh.\n",
                n, out->expected_count);
        } else if (out->merger_conflict) {
            (void)snprintf(out->correction_note, sizeof(out->correction_note),
                "\n\n[LOGIC_CONFLICT] Note: Mắm đang say — list có mâu thuẫn sáp nhập hành chính (xem authority cache).\n");
        }
    }
    return 0;
}
