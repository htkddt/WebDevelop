/*
 * Unit tests: message source (API_SOURCE_*), Redis L2 in-memory reply cache,
 * and api_load_chat_history JSON output.
 * Build: make test-api-source-redis
 * Run: ./build/api_source_and_redis_test
 */
#include "../include/api.h"
#include "../include/redis.h"
#include "../include/ollama.h"
#include <stdio.h>
#include <string.h>

static int failed;

static void ok(int cond, const char *name) {
    if (cond) {
        printf("  [OK] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
        failed = 1;
    }
}

/* Skip (no failure) when Ollama not running. */
static int skip_if_no_ollama(void) {
    return ollama_check_running(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT) != 0;
}

static void test_source_constants(void) {
    printf("\n--- source constants (API_SOURCE_*) ---\n");
    ok(API_SOURCE_MEMORY == 'M', "API_SOURCE_MEMORY");
    ok(API_SOURCE_REDIS == 'R', "API_SOURCE_REDIS");
    ok(API_SOURCE_MONGODB == 'G', "API_SOURCE_MONGODB");
    ok(API_SOURCE_OLLAMA == 'O', "API_SOURCE_OLLAMA");
    ok(strcmp(API_DEFAULT_TENANT_ID, "default") == 0, "API_DEFAULT_TENANT_ID");
}

static void test_redis_ttl_constants(void) {
    printf("\n--- Redis TTL constants ---\n");
    ok(REDIS_CACHE_TTL_SECONDS == 300, "REDIS_CACHE_TTL_SECONDS 300");
    ok(REDIS_REPLY_CACHE_TTL_SECONDS == 60, "REDIS_REPLY_CACHE_TTL_SECONDS 60");
}

static void test_api_fresh_context(void) {
    printf("\n--- api: fresh context (ONLY_MEMORY) ---\n");
    api_context_t *ctx = api_create("{\"mode\": 0}");
    ok(ctx != NULL, "api_create(ONLY_MEMORY)");

    if (!ctx) return;

    /* No history — JSON should be empty array */
    char json[4096];
    int count = api_load_chat_history(ctx, API_DEFAULT_TENANT_ID, NULL, json, sizeof(json));
    ok(count == 0, "load_chat_history returns 0 (MEMORY mode)");
    ok(strcmp(json, "[]") == 0, "json output is empty array");

    api_stats_t st;
    api_get_stats(ctx, &st);
    ok(st.last_reply_source == 0, "last_reply_source 0 (no reply yet)");

    api_destroy(ctx);
    printf("  [OK] api_destroy\n");
}

static void test_api_load_history_no_mongo(void) {
    printf("\n--- api: load_chat_history (no Mongo) ---\n");
    api_context_t *ctx = api_create("{\"mode\": 0}");
    ok(ctx != NULL, "api_create");

    if (!ctx) return;

    char json[4096];
    int r = api_load_chat_history(ctx, API_DEFAULT_TENANT_ID, NULL, json, sizeof(json));
    ok(r == 0, "load_chat_history (no Mongo) returns 0");
    ok(strcmp(json, "[]") == 0, "json output is empty array");

    api_destroy(ctx);
}

/* Static callback for redis_search_semantic (C has no lambdas). */
static int s_callback_invoked;
static void semantic_cb(const char *payload, size_t payload_len, double score, void *userdata) {
    (void)payload;
    (void)payload_len;
    (void)score;
    (void)userdata;
    s_callback_invoked = 1;
}

static void test_redis_l2_inmemory_cache(void) {
    printf("\n--- Redis L2 in-memory cache: set_vector then search_semantic invokes callback ---\n");
    redis_ctx_t *rctx = redis_create(REDIS_DEFAULT_HOST, REDIS_DEFAULT_PORT);
    ok(rctx != NULL, "redis_create");

    if (!rctx) return;

    ok(redis_initial(rctx) == 0, "redis_initial");
    ok(redis_connected(rctx) == 1, "redis_connected");

    float vec[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    ok(redis_set_vector(rctx, "default", "doc1", vec, 4, "payload", 7) == 0, "redis_set_vector");

    s_callback_invoked = 0;
    int ret = redis_search_semantic(rctx, "default", vec, 4, 5, REDIS_SEMANTIC_MIN_SCORE_DEFAULT, semantic_cb, NULL);
    ok(ret == 0, "redis_search_semantic returns 0");
    ok(s_callback_invoked == 1, "callback invoked (in-memory L2 returns hit for same vector)");

    redis_destroy(rctx);
}

/* Send the same input twice via api_chat; both must succeed. Second reply from in-memory L2 (REDIS). Skipped if Ollama not running. */
static void test_api_chat_same_input_twice(void) {
    printf("\n--- api_chat: same input twice (is it right?) ---\n");
    if (skip_if_no_ollama()) {
        printf("  [SKIP] Ollama not running\n");
        return;
    }
    api_context_t *ctx = api_create("{\"mode\": 3}");  /* MONGO_REDIS_ELK: vector_search_enabled, L2 cache */
    ok(ctx != NULL, "api_create");

    if (!ctx) return;

    static const char same_input[] = "is it right?";
    char reply1[2048];
    char reply2[2048];
    reply1[0] = reply2[0] = '\0';

    int r1 = api_chat(ctx, API_DEFAULT_TENANT_ID, API_DEFAULT_USER_ID, same_input, NULL, reply1, sizeof(reply1), NULL, NULL);
    ok(r1 == 0, "api_chat first call returns 0");
    ok(reply1[0] != '\0', "first reply non-empty");

    int r2 = api_chat(ctx, API_DEFAULT_TENANT_ID, API_DEFAULT_USER_ID, same_input, NULL, reply2, sizeof(reply2), NULL, NULL);
    ok(r2 == 0, "api_chat second call (same input) returns 0");
    ok(reply2[0] != '\0', "second reply non-empty");

    api_stats_t st;
    api_get_stats(ctx, &st);
    ok(st.last_reply_source == API_SOURCE_REDIS, "last reply source REDIS (in-memory L2 cache hit on second same input)");

    api_destroy(ctx);
}

int main(void) {
    printf("=== api source + Redis stub unit tests ===\n");

    test_source_constants();
    test_redis_ttl_constants();
    test_api_fresh_context();
    test_api_load_history_no_mongo();
    test_redis_l2_inmemory_cache();
    test_api_chat_same_input_twice();

    printf("\n=== %s ===\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}
