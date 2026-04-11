/*
 * Intra-prompt logic guard: province list count vs user expectation + merger rules (.cursor/conflict_detector.md).
 */
#ifndef M4_CONFLICT_DETECTOR_H
#define M4_CONFLICT_DETECTOR_H

#include <stddef.h>

typedef struct conflict_result {
    int has_logic_conflict; /* 1 if any rule fired */
    int count_mismatch;     /* expected unique list items != inferred expectation */
    int merger_conflict;    /* merged entity listed alongside parent */
    int expected_count;     /* from user message, or -1 if not inferred */
    int observed_unique;    /* unique normalized items from numbered list */
    char correction_note[640];
} conflict_result_t;

/**
 * Analyze user + assistant messages. Uses `geo_authority` (must be initialized).
 * Fills `out`; appends suggested correction text into `correction_note` (Option A).
 * Returns 0.
 */
int conflict_detector_analyze(const char *user_msg, const char *assistant_msg, conflict_result_t *out);

#endif /* M4_CONFLICT_DETECTOR_H */
