/*
 * Language detection — Universal Unicode range detector per .cursor/language.md.
 * Mechanism: leading-byte patterns for script blocks (CJK, Thai, Arabic, Vietnamese,
 * Japanese, Korean, Russian/Cyrillic, Hindi/Devanagari, …).
 * Mixed: multiple script blocks with >15% distribution each → "mixed", score 0.4.
 * Optimization: fast ASCII skip (0x00–0x7F). Safety: UTF-8 byte-jumping to avoid infinite loops.
 */
#include "lang.h"
#include <string.h>

#define LANG_MAX_CHARS 4096
#define MIXED_THRESHOLD 0.15

typedef enum {
    SCRIPT_ASCII,
    SCRIPT_VI,
    SCRIPT_TH,
    SCRIPT_CJK,
    SCRIPT_JA,
    SCRIPT_KO,
    SCRIPT_RU,
    SCRIPT_AR,
    SCRIPT_HI,
    SCRIPT_OTHER,
    SCRIPT_COUNT
} script_t;

static const char *script_lang(script_t s) {
    switch (s) {
        case SCRIPT_VI:  return "vi";
        case SCRIPT_TH:  return "th";
        case SCRIPT_CJK: return "zh";
        case SCRIPT_JA:  return "ja";
        case SCRIPT_KO:  return "ko";
        case SCRIPT_RU:  return "ru";
        case SCRIPT_AR:  return "ar";
        case SCRIPT_HI:  return "hi";
        case SCRIPT_ASCII:
        case SCRIPT_OTHER: return "en";
        default: return "en";
    }
}

/**
 * Advance p by one UTF-8 code point. Safe: never infinite loop; advances at least 1 byte.
 */
static const unsigned char *utf8_next(const unsigned char *p) {
    if (*p < 0x80) return p + 1;
    if (*p < 0xC2) return p + 1; /* invalid or continuation */
    if (*p <= 0xDF && p[1]) return p + 2;
    if (*p <= 0xEF && p[1] && p[2]) return p + 3;
    if (p[1] && p[2] && p[3]) return p + 4;
    return p + 1;
}

/**
 * Classify one UTF-8 code point by leading byte (and following bytes where needed).
 * Returns which script it belongs to and advances *p to next code point.
 */
static script_t classify(const unsigned char **p) {
    const unsigned char *q = *p;
    if (q[0] <= 0x7F) {
        *p = q + 1;
        return SCRIPT_ASCII;
    }
    /* Vietnamese: expanded per language.md (Urgent Fix). C3/C4/C5/C6 full blocks, E1 BA/BB, CC 80-BF (combining/NFD). */
    if (q[0] == 0xC3 && q[1] >= 0x80 && q[1] <= 0xBF) { *p = q + 2; return SCRIPT_VI; }
    if ((q[0] == 0xC4 || q[0] == 0xC5 || q[0] == 0xC6) && q[1] >= 0x80 && q[1] <= 0xBF) { *p = q + 2; return SCRIPT_VI; }
    if (q[0] == 0xCC && q[1] >= 0x80 && q[1] <= 0xBF) { *p = q + 2; return SCRIPT_VI; } /* Combining diacritics (NFD/Unikey) */
    if (q[0] == 0xE1 && q[1] >= 0xBA && q[1] <= 0xBB && q[2]) { *p = q + 3; return SCRIPT_VI; } /* E1 BA xx, E1 BB xx (Latin Extended Additional) */
    /* Korean: Hangul Jamo E1 84–87 xx; Hangul Syllables EA–ED xx xx (must be after VI so E1 BB is not KO) */
    if (q[0] == 0xE1 && q[1] && q[1] >= 0x84 && q[1] <= 0x87 && q[2]) { *p = q + 3; return SCRIPT_KO; }
    if (q[0] >= 0xEA && q[0] <= 0xED && q[1] && q[2]) { *p = q + 3; return SCRIPT_KO; }
    /* Russian / Cyrillic: D0 xx, D1 xx (U+0400–04FF) */
    if ((q[0] == 0xD0 || q[0] == 0xD1) && q[1]) { *p = q + 2; return SCRIPT_RU; }
    /* Hindi / Devanagari: E0 A4 xx, E0 A5 xx (U+0900–097F) */
    if (q[0] == 0xE0 && q[1] && (q[1] == 0xA4 || q[1] == 0xA5) && q[2]) { *p = q + 3; return SCRIPT_HI; }
    /* Thai: E0 B8 xx, E0 B9 xx */
    if (q[0] == 0xE0 && q[1] == 0xB8 && q[2]) { *p = q + 3; return SCRIPT_TH; }
    if (q[0] == 0xE0 && q[1] == 0xB9 && q[2]) { *p = q + 3; return SCRIPT_TH; }
    /* Japanese: Hiragana/Katakana E3 81 xx, E3 82 xx, E3 83 xx (U+3040–30FF) */
    if (q[0] == 0xE3 && q[1] && (q[1] == 0x81 || q[1] == 0x82 || q[1] == 0x83) && q[2]) { *p = q + 3; return SCRIPT_JA; }
    /* CJK: common 3-byte ranges E4–E9 (CJK Unified Ideographs, etc.) */
    if (q[0] >= 0xE4 && q[0] <= 0xE9 && q[1] && q[2]) { *p = q + 3; return SCRIPT_CJK; }
    /* Arabic: D8–DF xx (2-byte UTF-8) */
    if (q[0] >= 0xD8 && q[0] <= 0xDF && q[1]) { *p = q + 2; return SCRIPT_AR; }
    /* Safe UTF-8 skip for any other multi-byte */
    *p = utf8_next(q);
    return SCRIPT_OTHER;
}

int lang_detect(const char *text, char *lang_out, size_t lang_out_size, double *score_out) {
    if (!text || !lang_out || lang_out_size < 3 || !score_out) return -1;

    size_t counts[SCRIPT_COUNT];
    memset(counts, 0, sizeof(counts));

    const unsigned char *p = (const unsigned char *)text;
    size_t n = 0;
    while (*p && n < LANG_MAX_CHARS) {
        script_t s = classify(&p);
        if (s < SCRIPT_COUNT) counts[s]++;
        n++;
    }

    size_t total = 0;
    for (int i = 0; i < SCRIPT_COUNT; i++) total += counts[i];
    if (total == 0) {
        /* Empty or only invalid bytes → default en */
        if (lang_out_size >= 3) { lang_out[0] = 'e'; lang_out[1] = 'n'; lang_out[2] = '\0'; }
        *score_out = 0.5;
        return 0;
    }

    /* Mixed: only non-ASCII scripts count (language.md: ASCII+VI = Latin family; Russian+ASCII = ru) */
    int distinct_scripts_count = 0;
    script_t best = SCRIPT_ASCII;
    size_t best_count = counts[SCRIPT_ASCII];
    double best_eff = (double)best_count;
    for (int i = 0; i < SCRIPT_COUNT; i++) {
        /* Vietnamese +10% weight when choosing dominant (Local Bias, language.md) */
        double eff = (i == SCRIPT_VI && counts[i] > 0) ? (double)counts[i] * 1.1 : (double)counts[i];
        if (eff > best_eff) { best = (script_t)i; best_eff = eff; best_count = counts[i]; }
        /* Only non-ASCII scripts with >=15% count toward mixed (SCRIPT_ASCII never triggers mixed) */
        if (i != SCRIPT_ASCII && counts[i] > 0 && (double)counts[i] / (double)total >= MIXED_THRESHOLD)
            distinct_scripts_count++;
    }

    if (lang_out_size < 6) {
        *score_out = 0.0;
        return -1;
    }

    if (distinct_scripts_count >= 2) {
        memcpy(lang_out, "mixed", 6);
        lang_out[5] = '\0';
        *score_out = 0.4;
        return 0;
    }

    /* Latin family: when only VI is the distinct non-ASCII script and has enough presence, prefer vi over en (language.md validation). */
    if (distinct_scripts_count == 1 && counts[SCRIPT_VI] >= 2 && (double)counts[SCRIPT_VI] / (double)total >= MIXED_THRESHOLD)
        best = SCRIPT_VI, best_count = counts[SCRIPT_VI];

    const char *lang = script_lang(best);
    size_t len = strlen(lang);
    if (len >= lang_out_size) len = lang_out_size - 1;
    memcpy(lang_out, lang, len);
    lang_out[len] = '\0';
    if (best_count == 0) *score_out = 0.5;
    else if (best == SCRIPT_VI) *score_out = (counts[SCRIPT_VI] >= 2) ? 0.85 : 0.60;
    else if (best == SCRIPT_TH) *score_out = 0.80;
    else if (best == SCRIPT_CJK) *score_out = 0.85;
    else if (best == SCRIPT_JA) *score_out = 0.85;
    else if (best == SCRIPT_KO) *score_out = 0.85;
    else if (best == SCRIPT_RU) *score_out = 0.82;
    else if (best == SCRIPT_AR) *score_out = 0.80;
    else if (best == SCRIPT_HI) *score_out = 0.82;
    else *score_out = 0.90; /* en / ASCII */
    return 0;
}
