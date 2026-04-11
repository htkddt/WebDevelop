#include "nl_learn_cues.h"
#include "vector_generate.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *phrase;
    const char *intent;
} nl_cue_row_t;

/* Tier order and rows must stay aligned with python_ai/server/nl_learn_terms_bridge.py */

static const nl_cue_row_t tier_quant[] = {
    {"number of", "ELK_ANALYTICS"},   {"how many", "ELK_ANALYTICS"},   {"how much", "ELK_ANALYTICS"},
    {"total sales", "ELK_ANALYTICS"}, {"count of", "ELK_ANALYTICS"},   {"in total", "ELK_ANALYTICS"},
    {"per month", "ELK_ANALYTICS"},   {"per day", "ELK_ANALYTICS"},
};

static const nl_cue_row_t tier_search[] = {
    {"where can i find", "ELK_SEARCH"}, {"do you have any", "ELK_SEARCH"}, {"search for", "ELK_SEARCH"},
    {"look for", "ELK_SEARCH"},         {"show me all", "ELK_SEARCH"},     {"show me the", "ELK_SEARCH"},
    {"list all", "ELK_SEARCH"},         {"list of", "ELK_SEARCH"},        {"find me", "ELK_SEARCH"},
    {"find the", "ELK_SEARCH"},        {"find a", "ELK_SEARCH"},         {"show me", "ELK_SEARCH"},
    {"any products", "ELK_SEARCH"},    {"in stock", "ELK_SEARCH"},       {"on sale", "ELK_SEARCH"},
    {"browse", "ELK_SEARCH"},          {"catalog", "ELK_SEARCH"},        {"list", "ELK_SEARCH"},
    {"find", "ELK_SEARCH"},
};

static const nl_cue_row_t tier_time[] = {
    {"year to date", "ELK_ANALYTICS"}, {"last quarter", "ELK_ANALYTICS"}, {"this year", "ELK_ANALYTICS"},
    {"this month", "ELK_ANALYTICS"},   {"last year", "ELK_ANALYTICS"},     {"last month", "ELK_ANALYTICS"},
    {"last week", "ELK_ANALYTICS"},    {"yesterday", "ELK_ANALYTICS"},    {"today", "ELK_ANALYTICS"},
    {"ytd", "ELK_ANALYTICS"},
};

static const nl_cue_row_t tier_rag[] = {
    {"according to documentation", "RAG_VECTOR"}, {"from the documentation", "RAG_VECTOR"},
    {"knowledge base", "RAG_VECTOR"},               {"in the manual", "RAG_VECTOR"},
    {"our policy", "RAG_VECTOR"},                   {"what does the doc", "RAG_VECTOR"},
    {"from the docs", "RAG_VECTOR"},
};

static const nl_cue_row_t tier_analytics_extra[] = {
    {"year over year", "ELK_ANALYTICS"}, {"month over month", "ELK_ANALYTICS"},
    {"growth rate", "ELK_ANALYTICS"},   {"breakdown by", "ELK_ANALYTICS"},
    {"compared to", "ELK_ANALYTICS"},   {"percentage of", "ELK_ANALYTICS"},
    {"top ten", "ELK_ANALYTICS"},       {"top 10", "ELK_ANALYTICS"},
    {"ranking", "ELK_ANALYTICS"},       {"average ", "ELK_ANALYTICS"},
    {"sum of", "ELK_ANALYTICS"},        {"median", "ELK_ANALYTICS"},
};

static const nl_cue_row_t *const tier_tables[] = {
    tier_quant,
    tier_search,
    tier_time,
    tier_rag,
    tier_analytics_extra,
};

static const size_t tier_counts[] = {
    sizeof(tier_quant) / sizeof(tier_quant[0]),
    sizeof(tier_search) / sizeof(tier_search[0]),
    sizeof(tier_time) / sizeof(tier_time[0]),
    sizeof(tier_rag) / sizeof(tier_rag[0]),
    sizeof(tier_analytics_extra) / sizeof(tier_analytics_extra[0]),
};

static const char *strip_profile_suffix(const char *s) {
    static const char sep[] = "\n---\n";
    const char *last = NULL;
    const char *p = s;
    for (;;) {
        const char *f = strstr(p, sep);
        if (!f) break;
        last = f + (sizeof(sep) - 1);
        p = last;
    }
    return last ? last : s;
}

static void ascii_lower_norm(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r' || c == '\t') continue;
        if (c == '\n') {
            dst[j++] = ' ';
            continue;
        }
        dst[j++] = (char)(isupper(c) ? tolower(c) : c);
    }
    dst[j] = '\0';
    /* collapse spaces */
    size_t w = 0;
    int sp = 1;
    for (size_t i = 0; dst[i]; i++) {
        if (dst[i] == ' ') {
            if (!sp) {
                dst[w++] = ' ';
                sp = 1;
            }
        } else {
            dst[w++] = dst[i];
            sp = 0;
        }
    }
    if (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
}

static int best_in_tier(const char *u, const nl_cue_row_t *tab, size_t n, const char **out_p,
                        const char **out_i) {
    size_t best_len = 0;
    *out_p = NULL;
    *out_i = NULL;
    for (size_t k = 0; k < n; k++) {
        const char *ph = tab[k].phrase;
        if (strstr(u, ph) != NULL) {
            size_t L = strlen(ph);
            if (L > best_len) {
                best_len = L;
                *out_p = ph;
                *out_i = tab[k].intent;
            }
        }
    }
    return *out_p != NULL;
}

static int phrase_seen(const char seen[][96], int nseen, const char *ph) {
    for (int i = 0; i < nseen; i++) {
        if (strcmp(seen[i], ph) == 0) return 1;
    }
    return 0;
}

static int record_fallback_on(void) {
    const char *e = getenv("M4_NL_LEARN_RECORD_FALLBACK");
    if (!e || !e[0]) return 0;
    if (e[0] == '1' && e[1] == '\0') return 1;
    if (strcmp(e, "true") == 0 || strcmp(e, "yes") == 0 || strcmp(e, "on") == 0) return 1;
    return 0;
}

/** Try vocab lookup; if miss, try synonym → canonical → vocab lookup. */
static int vocab_or_synonym_lookup(const sc_term_vocab_t *vocab, const char *tok,
                                    const char **out_col, const char **out_field) {
    if (sc_term_vocab_lookup(vocab, tok, out_col, out_field) == 0)
        return 0;
    /* Synonym: "Sài Gòn" → "Ho Chi Minh City", then try each token of canonical. */
    m4_synonym_table_t *syn = m4_synonym_get_global();
    if (!syn) return -1;
    const char *canonical = m4_synonym_lookup(syn, tok);
    if (!canonical) return -1;
    /* Try canonical as-is first. */
    if (sc_term_vocab_lookup(vocab, canonical, out_col, out_field) == 0)
        return 0;
    /* Tokenize canonical and try each word. */
    char cbuf[256];
    snprintf(cbuf, sizeof(cbuf), "%s", canonical);
    char *csave = NULL;
    char *ctok = strtok_r(cbuf, " ,_-", &csave);
    while (ctok) {
        if (strlen(ctok) >= 2 && sc_term_vocab_lookup(vocab, ctok, out_col, out_field) == 0)
            return 0;
        ctok = strtok_r(NULL, " ,_-", &csave);
    }
    return -1;
}

/** Scan normalized utterance for SharedCollection vocab terms and record "SC:{collection}" hits. */
static void vocab_scan(nl_learn_terms_t *lt, const char *norm, const sc_term_vocab_t *vocab,
                        char seen[][96], int *nseen) {
    if (!vocab) return;
    /* Tokenize normalized utterance and look up each word in vocab (with synonym fallback). */
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", norm);
    char *save = NULL;
    char *tok = strtok_r(buf, " ,?!.;:\"'()/", &save);
    while (tok) {
        if (strlen(tok) >= 2 && !phrase_seen(seen, *nseen, tok)) {
            const char *col = NULL, *field = NULL;
            if (vocab_or_synonym_lookup(vocab, tok, &col, &field) == 0 && col) {
                char intent_label[192];
                snprintf(intent_label, sizeof(intent_label), "SC:%s", col);
                const char *argv[1] = { tok };
                (void)nl_learn_terms_record(lt, argv, 1u, intent_label, 1);
                if (*nseen < 24) {
                    strncpy(seen[*nseen], tok, 95);
                    seen[*nseen][95] = '\0';
                    (*nseen)++;
                }
            }
        }
        tok = strtok_r(NULL, " ,?!.;:\"'()/", &save);
    }
}

void nl_learn_cues_record_from_utterance(nl_learn_terms_t *lt, const char *utterance_utf8,
                                         const sc_term_vocab_t *vocab) {
    if (!lt || !utterance_utf8) return;

    const char *body = strip_profile_suffix(utterance_utf8);
    char norm[4096];
    ascii_lower_norm(norm, sizeof(norm), body);
    if (!norm[0]) return;

    char seen[24][96];
    int nseen = 0;
    int any = 0;

    /* Tier scan: intent cues (ELK_ANALYTICS, ELK_SEARCH, RAG_VECTOR). */
    for (size_t t = 0; t < sizeof(tier_tables) / sizeof(tier_tables[0]); t++) {
        const char *ph = NULL, *intent = NULL;
        if (!best_in_tier(norm, tier_tables[t], tier_counts[t], &ph, &intent)) continue;
        if (!ph || !intent) continue;
        if (phrase_seen(seen, nseen, ph)) continue;
        if (nseen < (int)(sizeof(seen) / sizeof(seen[0]))) {
            strncpy(seen[nseen], ph, sizeof(seen[0]) - 1);
            seen[nseen][sizeof(seen[0]) - 1] = '\0';
            nseen++;
        }
        {
            const char *argv[1] = { ph };
            (void)nl_learn_terms_record(lt, argv, 1u, intent, 1);
            any = 1;
        }
    }

    /* Vocab scan: entity cues from SharedCollection (SC:{collection}). */
    vocab_scan(lt, norm, vocab, seen, &nseen);

    if (!any && record_fallback_on()) {
        const char *argv[1] = { "chat" };
        (void)nl_learn_terms_record(lt, argv, 1u, "CHAT", 1);
    }
}
