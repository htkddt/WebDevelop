### Language strategy
- **Task**: Implement a Universal Unicode Range Detector.
- **Mechanism**: Use leading-byte patterns to identify script blocks:
  - **Vietnamese (vi)**: C3/C4/C6, E1 BB xx
  - **Thai (th)**: E0 B8 xx, E0 B9 xx
  - **CJK / Chinese (zh)**: E4–E9 xx xx
  - **Japanese (ja)**: E3 81/82/83 xx (Hiragana/Katakana)
  - **Korean (ko)**: E1 84–87 xx (Hangul Jamo), EA–ED xx xx (Hangul Syllables)
  - **Russian (ru)**: D0/D1 xx (Cyrillic)
  - **Arabic (ar)**: D8–DF xx
  - **Hindi (hi)**: E0 A4/A5 xx (Devanagari)
  - **English (en)**: ASCII 0x00–0x7F default
- **Input method**: Detection depends only on UTF-8 content, not keyboard or locale. Mac Vietnamese input often produces NFD (base + combining marks); combining marks `0xCC 80–BF` are counted as VI, so both NFC and NFD work.
- **Mixed Lang**: If **two or more distinct non-ASCII** script blocks each have ≥15% distribution, force `metadata.lang = "mixed"` and `score = 0.4`. (See distinct_scripts_count below; ASCII does not count.)
- **Optimization**: Fast ASCII skip (0x00–0x7F). **Safety**: Default UTF-8 byte-jumping to prevent infinite loops on corrupted strings.

- **Priority**: If `vi_count > 0`, give Vietnamese a +10% weight when choosing the dominant script (Local Bias for Saigon Engine).
- **Korean/Vietnamese Conflict**: When the 1st byte is `0xE1`, strictly check the 2nd byte: `vi` = 0xBB, `ko` = 0x84–0x87 (Hangul Jamo). Never treat E1 BB as Korean.
- **Normalization**: Storage uses `metadata.lang` = detected lang when `score >= 0.5`; otherwise storage forces `"mixed"`. For high-confidence display, treat `score > 0.8` as dominant lang.
- **ASCII Skip**: Use `while (*p && *p < 0x80) p++;` (0x80 hex) for the fastest English/ASCII skip.
- **Logic Philosophy**: Treat `SCRIPT_ASCII` and `SCRIPT_VI` as part of the same Latin-based family. They must NOT trigger `mixed` against each other.
- **Mixed (distinct_scripts_count)**:
  - Increment only for **non-ASCII** scripts (VI, TH, CJK, JA, KO, RU, AR, HI, OTHER) that have ≥15% of code points.
  - `mixed` is triggered only when `distinct_scripts_count >= 2`. So: ASCII+VI → one distinct script (VI) → not mixed; Russian+ASCII → one distinct (RU) → `ru`.
- **Dominant Selection**: When calculating `best_eff`, keep the +10% boost for `SCRIPT_VI` (Local Bias for Saigon).
- **Refactoring**: Same logic for any language: e.g. Russian + ASCII → `ru`, not `mixed`.

- **Urgent Fix**: Language detection is returning 'en' for clear Vietnamese input like "đi quận một".
- **Reason**: The current byte-matching for VI is too specific and misses many UTF-8 accented characters and NFD (composed) marks.
- **Action**: 
  - Expand `SCRIPT_VI` detection in `classify()` to cover entire UTF-8 ranges: 
    - `0xC3 0x80-0xBF` (Latin-1 supplement)
    - `0xC4, 0xC5, 0xC6` entire blocks (Latin Extended-A; e.g. đ = C4 91, ư/ơ = C6 xx)
    - `0xE1 0xBA xx`, `0xE1 0xBB xx` (Latin Extended Additional — e.g. ậ, ộ, ế)
    - **Crucial**: `0xCC 0x80-0xBF` (Combining Diacritics) for NFD/Unikey marks.
- **Dominant logic**: Keep the +10% boost for VI.
- **Validation**: Test with "đi quận một đi đường nào?". Result MUST be `lang: "vi"`, `score: 0.85+`.

