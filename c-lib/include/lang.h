#ifndef M4_LANG_H
#define M4_LANG_H

#include <stddef.h>

/*
 * Language detection — Universal Unicode range detector per .cursor/language.md.
 * Script blocks: Vietnamese (vi), Thai (th), CJK/Chinese (zh), Japanese (ja), Korean (ko),
 * Russian/Cyrillic (ru), Arabic (ar), Hindi/Devanagari (hi), English (en). Mixed: score 0.4.
 */

/**
 * Detect language of text. Fills lang_out (e.g. "en", "vi", "th", "zh", "ja", "ko", "ru", "ar", "hi", "mixed") and score_out [0,1].
 * If score < 0.5, storage uses "mixed" for metadata.lang (rule in storage.c).
 * Returns 0 on success, -1 on invalid args.
 */
int lang_detect(const char *text, char *lang_out, size_t lang_out_size, double *score_out);

#endif /* M4_LANG_H */
