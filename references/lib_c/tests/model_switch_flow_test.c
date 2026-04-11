/*
 * Unit tests: model_switch_resolve — lane table, merge flag, session override, getenv fallback.
 * Pure C; no Ollama, no Mongo. Build: make test-model-switch-flow
 */
#include "../include/model_switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void ok(int cond, const char *name) {
    if (cond)
        printf("  [OK] %s\n", name);
    else {
        printf("  [FAIL] %s\n", name);
        failed = 1;
    }
}

static void test_intent_keys(void) {
    printf("\n--- smart_topic_intent_lane_key ---\n");
    ok(strcmp(smart_topic_intent_lane_key(SMART_TOPIC_INTENT_EDUCATION), "EDUCATION") == 0,
       "EDUCATION intent -> EDUCATION");
    ok(strcmp(smart_topic_intent_lane_key(SMART_TOPIC_INTENT_BUSINESS), "BUSINESS") == 0,
       "BUSINESS intent -> BUSINESS");
    ok(strcmp(smart_topic_intent_lane_key(SMART_TOPIC_INTENT_TECH), "TECH") == 0, "TECH");
    ok(strcmp(smart_topic_intent_lane_key(SMART_TOPIC_INTENT_CHAT), "CHAT") == 0, "CHAT");
    ok(strcmp(smart_topic_intent_lane_key(SMART_TOPIC_INTENT_DEFAULT), "DEFAULT") == 0, "DEFAULT");
}

static void test_merge_and_session(void) {
    printf("\n--- table lookup + MERGE_SMART_TOPIC_INTENT ---\n");
    static const model_switch_lane_entry_t lanes[] = {
        { "EDUCATION", "model-edu", "inj-edu" },
        { "TECH", "model-tech", NULL },
        { "DEFAULT", "model-default", NULL },
    };
    model_switch_options_t opts = {
        .lanes = lanes,
        .lane_count = sizeof(lanes) / sizeof(lanes[0]),
        .fallback_model = "model-fallback",
        .flags = MODEL_SWITCH_FLAG_MERGE_SMART_TOPIC_INTENT,
        .adaptive_profile_id = NULL,
    };
    model_switch_profile_t p;
    memset(&p, 0, sizeof(p));

    /* Empty session + EDUCATION intent -> merge to EDUCATION row */
    model_switch_resolve(&opts, "", SMART_TOPIC_INTENT_EDUCATION, &p);
    ok(strcmp(p.lane_key, "EDUCATION") == 0, "merge: lane_key EDUCATION");
    ok(strcmp(p.model, "model-edu") == 0, "merge: model from EDUCATION row");
    ok(strcmp(p.inject, "inj-edu") == 0, "merge: inject from row");

    /* Session TECH wins over EDUCATION intent */
    memset(&p, 0, sizeof(p));
    model_switch_resolve(&opts, "TECH", SMART_TOPIC_INTENT_EDUCATION, &p);
    ok(strcmp(p.lane_key, "TECH") == 0, "session TECH overrides intent");
    ok(strcmp(p.model, "model-tech") == 0, "TECH row model");

    /* No merge: DEFAULT intent stays DEFAULT row */
    opts.flags = 0;
    memset(&p, 0, sizeof(p));
    model_switch_resolve(&opts, "", SMART_TOPIC_INTENT_EDUCATION, &p);
    ok(strcmp(p.lane_key, "DEFAULT") == 0, "no merge: forced DEFAULT key");
    ok(strcmp(p.model, "model-default") == 0, "no merge: DEFAULT row model");
}

static void test_fallback_and_env(void) {
    printf("\n--- unknown key -> fallback_model; getenv M4_MODEL_* ---\n");
    static const model_switch_lane_entry_t lanes[] = {
        { "DEFAULT", "model-default", NULL },
    };
    model_switch_options_t opts = {
        .lanes = lanes,
        .lane_count = 1,
        .fallback_model = "model-fallback",
        .flags = 0,
        .adaptive_profile_id = NULL,
    };
    model_switch_profile_t p;
    memset(&p, 0, sizeof(p));
    model_switch_resolve(&opts, "UNKNOWN_LANE", SMART_TOPIC_INTENT_DEFAULT, &p);
    ok(strcmp(p.lane_key, "UNKNOWN_LANE") == 0, "session UNKNOWN_LANE preserved");
    ok(strcmp(p.model, "model-fallback") == 0, "no table row -> fallback_model");

#ifdef _WIN32
    printf("  [SKIP] getenv M4_MODEL_ORPHAN (setenv not portable here)\n");
#else
    if (setenv("M4_MODEL_ORPHAN", "model-from-env", 1) != 0) {
        printf("  [SKIP] setenv failed\n");
        return;
    }
    memset(&p, 0, sizeof(p));
    model_switch_resolve(&opts, "ORPHAN", SMART_TOPIC_INTENT_DEFAULT, &p);
    ok(strcmp(p.model, "model-from-env") == 0, "getenv M4_MODEL_ORPHAN");
    unsetenv("M4_MODEL_ORPHAN");
#endif
}

int main(void) {
    printf("=== model_switch input flow (unit) ===\n");
    test_intent_keys();
    test_merge_and_session();
    test_fallback_and_env();
    if (failed) {
        printf("\n=== FAILED ===\n");
        return 1;
    }
    printf("\n=== OK ===\n");
    printf("\nTip: full path (smart_topic micro-call + api_chat) needs Ollama; run with\n");
    printf("  smart_topic enable + model_switch_opts + api_set_model_lane_key, then api_chat.\n");
    return 0;
}
