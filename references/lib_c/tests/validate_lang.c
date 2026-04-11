/*
 * Validate lang_detect() per .cursor/language.md.
 * Build: make validate-lang
 * Exit 0 if all checks pass; else exit 1 and print failures.
 */
#include "../include/lang.h"
#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *text, const char *expect_lang, double min_score, const char *name) {
    char lang[16];
    double score = 0.0;
    int ret = lang_detect(text, lang, sizeof(lang), &score);
    if (ret != 0) {
        printf("  [FAIL] %s: lang_detect returned %d\n", name, ret);
        failed = 1;
        return;
    }
    if (strcmp(lang, expect_lang) != 0) {
        printf("  [FAIL] %s: got lang=\"%s\" score=%.2f, expected lang=\"%s\" score>=%.2f\n",
               name, lang, score, expect_lang, min_score);
        failed = 1;
        return;
    }
    if (score < min_score) {
        printf("  [FAIL] %s: got lang=\"%s\" score=%.2f, expected score>=%.2f\n",
               name, lang, score, min_score);
        failed = 1;
        return;
    }
    printf("  [OK] %s: \"%s\" -> %s (%.2f)\n", name, text, lang, score);
}

int main(void) {
    printf("=== Validate lang (language.md) ===\n");

    /* language.md: "đi quận một đi đường nào?" MUST be vi, score 0.85+ */
    check("đi quận một đi đường nào?", "vi", 0.85, "vi_required");

    check("đuọc tiếng việt mất tiếng khác", "vi", 0.85, "vi_phrase");
    /* Mac Vietnamese keyboard (NFC or NFD): detection is by Unicode content only, not input method */
    check("hôm qua em đi chùa hương", "vi", 0.85, "vi_mac");

    /* Short Vietnamese */
    check("đi", "vi", 0.55, "vi_short");
    /* "xin chào" has only one VI char (à) -> can stay en; so we don't require vi here */

    /* English */
    check("hello world", "en", 0.5, "en_ascii");

    /* Empty / invalid args: lang_detect should still fill with default or return -1 */
    {
        char lang[16];
        double score = -1.0;
        int ret = lang_detect("", lang, sizeof(lang), &score);
        if (ret == 0 && strcmp(lang, "en") == 0) {
            printf("  [OK] empty -> en\n");
        } else if (ret != 0) {
            printf("  [OK] empty -> ret=%d (acceptable)\n", ret);
        } else {
            printf("  [FAIL] empty: got lang=%s ret=%d\n", lang, ret);
            failed = 1;
        }
    }

    if (failed) {
        printf("=== Validation FAILED ===\n");
        return 1;
    }
    printf("=== Validation passed ===\n");
    return 0;
}
