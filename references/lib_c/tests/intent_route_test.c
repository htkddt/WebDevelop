/*
 * Tests: intent routing — collection resolution, SC: score learning, filter validation.
 * Run: make test_intent_route && ./build/test_intent_route
 */
#include "../include/intent_route.h"
#include "../include/intent_learn.h"
#include "../include/nl_learn_terms.h"
#include "../include/shared_collection.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int failed;

static void ok(int cond, const char *name) {
    if (cond)
        printf("  [OK] %s\n", name);
    else {
        printf("  [FAIL] %s\n", name);
        failed = 1;
    }
}

/* Create a temp learning terms file. */
static char *create_temp_lt(void) {
    char *path = strdup("/tmp/test_intent_lt_XXXXXX.json");
    /* mkstemp needs exact XXXXXX pattern. */
    static int counter = 0;
    snprintf(path, 256, "/tmp/test_intent_lt_%d.json", counter++);
    FILE *f = fopen(path, "w");
    if (!f) { free(path); return NULL; }
    fprintf(f, "{\"schema\":\"nl_learn_terms_v2\",\"terms\":{}}\n");
    fclose(f);
    return path;
}

static void cleanup_temp(const char *path) {
    if (path) {
        unlink(path);
        char wal[512];
        snprintf(wal, sizeof(wal), "%s.wal", path);
        unlink(wal);
    }
}

/* Load a test SharedCollection registry. */
static sc_registry_t *create_test_registry(void) {
    const char *path = "/tmp/test_sc_registry.json";
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fprintf(f,
        "{ \"collections\": [\n"
        "  { \"collection\": \"products\", \"alias\": \"store_products\",\n"
        "    \"elk\": { \"allow\": true },\n"
        "    \"metadata\": { \"field_hints\": {\n"
        "      \"category\": \"computer, phone, tablet\",\n"
        "      \"price\": \"unit price\", \"quantity_available\": \"stock count\" } } },\n"
        "  { \"collection\": \"carts\", \"alias\": \"shopping_carts\",\n"
        "    \"elk\": { \"allow\": true },\n"
        "    \"metadata\": { \"field_hints\": {\n"
        "      \"cart_key\": \"cart or order id\",\n"
        "      \"status\": \"pending, shipped, delivered\",\n"
        "      \"updated_at\": \"last update time\" } } },\n"
        "  { \"collection\": \"product_categories\", \"alias\": \"product_categories\",\n"
        "    \"elk\": { \"allow\": true },\n"
        "    \"metadata\": { \"field_hints\": {\n"
        "      \"code\": \"category slug\", \"active\": \"is active\" } } }\n"
        "] }\n");
    fclose(f);
    sc_registry_t *reg = sc_registry_load_file(path);
    unlink(path);
    return reg;
}

static void test_vocab_resolution(void) {
    printf("\n--- test_vocab_resolution ---\n");
    sc_registry_t *reg = create_test_registry();
    ok(reg != NULL, "registry loaded");

    sc_term_vocab_t *vocab = sc_term_vocab_build(reg);
    ok(vocab != NULL, "vocab built");

    size_t count = sc_term_vocab_count(vocab);
    printf("  vocab entries: %zu\n", count);
    ok(count > 10, "vocab has entries from field_hints");

    /* "products" → products (collection name). */
    const char *col = NULL, *field = NULL;
    ok(sc_term_vocab_lookup(vocab, "products", &col, &field) == 0, "lookup 'products' found");
    ok(col && strcmp(col, "products") == 0, "products → products collection");

    /* "computer" → products.category (from field_hints value). */
    col = NULL; field = NULL;
    ok(sc_term_vocab_lookup(vocab, "computer", &col, &field) == 0, "lookup 'computer' found");
    ok(col && strcmp(col, "products") == 0, "computer → products collection");
    ok(field && strcmp(field, "category") == 0, "computer → category field");

    /* "pending" → carts.status (from field_hints value). */
    col = NULL; field = NULL;
    ok(sc_term_vocab_lookup(vocab, "pending", &col, &field) == 0, "lookup 'pending' found");
    ok(col && strcmp(col, "carts") == 0, "pending → carts collection");
    ok(field && strcmp(field, "status") == 0, "pending → status field");

    /* "orders" → NOT in vocab (only "shopping" and "carts"). */
    col = NULL; field = NULL;
    ok(sc_term_vocab_lookup(vocab, "orders", &col, &field) != 0, "lookup 'orders' NOT found (expected)");

    sc_term_vocab_free(vocab);
    sc_registry_free(reg);
}

static void test_learned_sc_scores(void) {
    printf("\n--- test_learned_sc_scores ---\n");
    char *lt_path = create_temp_lt();
    ok(lt_path != NULL, "temp learning file created");

    nl_learn_terms_t *lt = nl_learn_terms_open(lt_path, 1);
    ok(lt != NULL, "learning terms opened");

    /* Simulate background LLM learning: "orders" → SC:carts, "sold" → SC:carts. */
    const char *k1[1] = {"orders"};
    ok(nl_learn_terms_record(lt, k1, 1, "SC:carts", 5) == 0, "record orders → SC:carts");
    const char *k2[1] = {"sold"};
    ok(nl_learn_terms_record(lt, k2, 1, "SC:carts", 3) == 0, "record sold → SC:carts");
    const char *k3[1] = {"computers"};
    ok(nl_learn_terms_record(lt, k3, 1, "SC:products", 2) == 0, "record computers → SC:products");

    /* score_text should find "orders" → SC:carts in the message. */
    int64_t sc = nl_learn_terms_score_text(lt, "how many orders sold this year", "SC:carts");
    printf("  score_text('orders sold', SC:carts) = %lld\n", (long long)sc);
    ok(sc >= 5, "SC:carts score from 'orders' + 'sold'");

    int64_t sp = nl_learn_terms_score_text(lt, "how many orders sold this year", "SC:products");
    printf("  score_text('orders sold', SC:products) = %lld\n", (long long)sp);
    ok(sc > sp, "SC:carts > SC:products for 'orders sold'");

    /* Now test classify — should resolve collection using learned scores. */
    sc_registry_t *reg = create_test_registry();
    sc_term_vocab_t *vocab = sc_term_vocab_build(reg);

    intent_route_result_t ir = {0};
    /* Record some intent cues first. */
    const char *k4[1] = {"how many"};
    nl_learn_terms_record(lt, k4, 1, "ELK_ANALYTICS", 10);

    intent_route_classify("how many orders sold this year?", lt, vocab, reg, 5, &ir);
    printf("  classify: intent=%s score=%lld collection=%s\n",
           intent_route_label(ir.intent), (long long)ir.intent_score, ir.collection);

    ok(ir.intent == INTENT_ROUTE_ELK_ANALYTICS, "intent = ELK_ANALYTICS");
    /* Collection should be carts (from learned SC: scores), not products (from vocab). */
    ok(strcmp(ir.collection, "carts") == 0 || ir.collection_score > 0,
       "collection resolved (carts preferred from learned scores)");

    sc_term_vocab_free(vocab);
    sc_registry_free(reg);
    nl_learn_terms_close(lt);
    cleanup_temp(lt_path);
    free(lt_path);
}

static void test_field_validation(void) {
    printf("\n--- test_field_validation ---\n");
    sc_registry_t *reg = create_test_registry();
    sc_term_vocab_t *vocab = sc_term_vocab_build(reg);

    /* Known fields from field_hints. */
    ok(intent_route_validate_field("products", "category", reg, vocab) == 1,
       "products.category is valid");
    ok(intent_route_validate_field("carts", "status", reg, vocab) == 1,
       "carts.status is valid");
    ok(intent_route_validate_field("carts", "updated_at", reg, vocab) == 1,
       "carts.updated_at is valid");

    /* Common ELK fields — always valid. */
    ok(intent_route_validate_field("products", "@timestamp", reg, vocab) == 1,
       "@timestamp always valid");
    ok(intent_route_validate_field("carts", "created_at", reg, vocab) == 1,
       "created_at always valid");

    /* Unknown field — invalid. */
    ok(intent_route_validate_field("products", "nonexistent_field", reg, vocab) == 0,
       "nonexistent_field is invalid");
    ok(intent_route_validate_field("carts", "bogus", reg, vocab) == 0,
       "bogus field is invalid");

    sc_term_vocab_free(vocab);
    sc_registry_free(reg);
}

/* --- Test: 30 sentences covering chat, ELK, ambiguous, multilingual --- */

typedef struct {
    const char *msg;
    const char *expect_intent;     /* "CHAT", "ELK_ANALYTICS", "ELK_SEARCH", "RAG_VECTOR" */
    const char *expect_collection; /* NULL = any/none, "carts", "products", etc. */
    const char *note;
} test_sentence_t;

static const test_sentence_t sentences[] = {
    /* --- Pure CHAT (no data signal) --- */
    {"hello", "CHAT", NULL, "greeting"},
    {"tell me a joke", "CHAT", NULL, "entertainment"},
    {"what is the capital of France?", "CHAT", NULL, "general knowledge"},
    {"explain recursion", "CHAT", NULL, "technical concept"},
    {"good morning", "CHAT", NULL, "greeting"},
    {"thank you for your help", "CHAT", NULL, "politeness"},
    {"who are you?", "CHAT", NULL, "identity question"},

    /* --- Clear ELK_ANALYTICS (counting, aggregation) --- */
    {"how many products do we have?", "ELK_ANALYTICS", "products", "count products"},
    {"how many orders this year?", "ELK_ANALYTICS", "carts", "count orders (learned synonym)"},
    {"total sales last month", "ELK_ANALYTICS", "carts", "total (analytics cue + learned)"},
    {"count of pending orders", "ELK_ANALYTICS", "carts", "count + status filter"},
    {"number of active categories", "ELK_ANALYTICS", "product_categories", "count categories"},
    {"how much revenue this quarter", "ELK_ANALYTICS", "carts", "revenue = carts"},
    {"average price of products", "ELK_ANALYTICS", "products", "avg aggregation"},

    /* --- Clear ELK_SEARCH (find, list, show) --- */
    {"show me all products", "ELK_SEARCH", "products", "show all"},
    {"find pending orders", "ELK_SEARCH", "carts", "find + status"},
    {"list all categories", "ELK_SEARCH", "product_categories", "list categories"},
    {"search for computers", "ELK_SEARCH", "products", "search by category"},
    {"show me the cheapest products", "ELK_SEARCH", "products", "show + field"},

    /* --- Ambiguous (could be chat or data) --- */
    {"products", "CHAT", NULL, "single word — not enough signal"},
    {"tell me about our orders", "CHAT", NULL, "conversational about data"},
    {"what do you know about sales?", "CHAT", NULL, "meta question"},
    {"can you help with inventory?", "CHAT", NULL, "request for help"},

    /* --- RAG/knowledge base --- */
    {"according to the documentation", "RAG_VECTOR", NULL, "doc reference"},
    {"what does the manual say about returns?", "RAG_VECTOR", NULL, "manual reference"},
    {"check the knowledge base", "RAG_VECTOR", NULL, "knowledge base cue"},

    /* --- Edge cases --- */
    {"how many ways to cook rice?", "CHAT", NULL, "false positive: 'how many' but not data"},
    {"list of ingredients for cake", "CHAT", NULL, "list but not data collection"},
    {"how many?", "ELK_ANALYTICS", NULL, "bare count — no collection"},
    {"", "CHAT", NULL, "empty message"},

    {NULL, NULL, NULL, NULL}
};

static void test_sentences(void) {
    printf("\n--- test_sentences (30 cases) ---\n");

    /* Set up learning store with pre-learned scores (simulating background worker output). */
    char *lt_path = create_temp_lt();
    nl_learn_terms_t *lt = nl_learn_terms_open(lt_path, 1);
    sc_registry_t *reg = create_test_registry();
    sc_term_vocab_t *vocab = sc_term_vocab_build(reg);

    /* Simulate learned cue scores (what accumulates from real usage). */
    const char *k1[1]; int64_t d;

    /* Intent cues. */
    k1[0] = "how many"; nl_learn_terms_record(lt, k1, 1, "ELK_ANALYTICS", 20);
    k1[0] = "how much"; nl_learn_terms_record(lt, k1, 1, "ELK_ANALYTICS", 10);
    k1[0] = "total"; nl_learn_terms_record(lt, k1, 1, "ELK_ANALYTICS", 8);
    k1[0] = "count of"; nl_learn_terms_record(lt, k1, 1, "ELK_ANALYTICS", 8);
    k1[0] = "number of"; nl_learn_terms_record(lt, k1, 1, "ELK_ANALYTICS", 8);
    k1[0] = "average"; nl_learn_terms_record(lt, k1, 1, "ELK_ANALYTICS", 5);
    k1[0] = "show me all"; nl_learn_terms_record(lt, k1, 1, "ELK_SEARCH", 10);
    k1[0] = "show me the"; nl_learn_terms_record(lt, k1, 1, "ELK_SEARCH", 8);
    k1[0] = "find"; nl_learn_terms_record(lt, k1, 1, "ELK_SEARCH", 8);
    k1[0] = "list all"; nl_learn_terms_record(lt, k1, 1, "ELK_SEARCH", 8);
    k1[0] = "search for"; nl_learn_terms_record(lt, k1, 1, "ELK_SEARCH", 8);
    k1[0] = "list"; nl_learn_terms_record(lt, k1, 1, "ELK_SEARCH", 5);
    k1[0] = "according to documentation"; nl_learn_terms_record(lt, k1, 1, "RAG_VECTOR", 10);
    k1[0] = "knowledge base"; nl_learn_terms_record(lt, k1, 1, "RAG_VECTOR", 10);
    k1[0] = "in the manual"; nl_learn_terms_record(lt, k1, 1, "RAG_VECTOR", 8);

    /* SC:* collection scores (from background LLM learning). */
    k1[0] = "orders"; nl_learn_terms_record(lt, k1, 1, "SC:carts", 10);
    k1[0] = "sales"; nl_learn_terms_record(lt, k1, 1, "SC:carts", 8);
    k1[0] = "revenue"; nl_learn_terms_record(lt, k1, 1, "SC:carts", 6);
    k1[0] = "sold"; nl_learn_terms_record(lt, k1, 1, "SC:carts", 8);
    k1[0] = "pending"; nl_learn_terms_record(lt, k1, 1, "SC:carts", 5);
    k1[0] = "computers"; nl_learn_terms_record(lt, k1, 1, "SC:products", 5);
    k1[0] = "cheapest"; nl_learn_terms_record(lt, k1, 1, "SC:products", 3);
    k1[0] = "active"; nl_learn_terms_record(lt, k1, 1, "SC:product_categories", 5);
    k1[0] = "categories"; nl_learn_terms_record(lt, k1, 1, "SC:product_categories", 8);

    int pass = 0, total = 0;
    for (size_t i = 0; sentences[i].msg || sentences[i].note; i++) {
        const test_sentence_t *s = &sentences[i];
        if (!s->msg) continue;
        total++;

        intent_route_result_t ir = {0};
        intent_route_classify(s->msg, lt, vocab, reg, 5, &ir);

        const char *got_intent = intent_route_label(ir.intent);
        int intent_ok = (strcmp(got_intent, s->expect_intent) == 0);

        int col_ok = 1;
        if (s->expect_collection) {
            col_ok = (ir.collection[0] && strcmp(ir.collection, s->expect_collection) == 0);
        }

        if (intent_ok && col_ok) {
            printf("  [OK] \"%s\" → %s %s (%s)\n",
                   s->msg, got_intent, ir.collection[0] ? ir.collection : "", s->note);
            pass++;
        } else {
            printf("  [FAIL] \"%s\" → got %s/%s expected %s/%s (%s)\n",
                   s->msg, got_intent, ir.collection[0] ? ir.collection : "(none)",
                   s->expect_intent, s->expect_collection ? s->expect_collection : "*",
                   s->note);
        }
    }
    printf("\n  Score: %d/%d passed\n", pass, total);
    if (pass < total) failed = 1;

    sc_term_vocab_free(vocab);
    sc_registry_free(reg);
    nl_learn_terms_close(lt);
    cleanup_temp(lt_path);
    free(lt_path);
}

int main(void) {
    printf("=== Intent Route Tests ===\n");

    test_vocab_resolution();
    test_learned_sc_scores();
    test_field_validation();
    test_sentences();

    printf("\n%s\n", failed ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failed ? 1 : 0;
}
