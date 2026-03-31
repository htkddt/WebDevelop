/*
 * c_ai — C test app that uses the c-lib (M4-Hardcore AI Engine).
 * Build from repo root: make lib && make -C c_ai
 * Run: ./c_ai/c_ai_bot "Your question"
 */
#include "engine.h"
#include "ollama.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *prompt = (argc > 1) ? argv[1] : "Say hello in one sentence.";
    char out[4096];

    printf("[c_ai] Using c-lib (libm4engine)\n");
    printf("[c_ai] Prompt: %s\n", prompt);

    /* Call the C library: Ollama query */
    int r = ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT,
                         OLLAMA_DEFAULT_MODEL, prompt, out, sizeof(out));
    if (r != 0) {
        printf("[c_ai] ollama_query failed (is Ollama running?)\n");
        return 1;
    }
    printf("[c_ai] Reply: %s\n", out[0] ? out : "(empty)");

    /* Optional: exercise engine (create/destroy) */
    engine_config_t config = {
        .mode = MODE_HYBRID,
        .execution_mode = MODE_ONLY_MEMORY,
        .mongo_uri = NULL,
        .redis_host = NULL,
        .redis_port = 0,
        .es_host = NULL,
        .es_port = 0,
        .batch_size = 100,
        .vector_search_enabled = 0,
        .debug_mode = false
    };
    engine_t *e = engine_create(&config);
    if (e) {
        printf("[c_ai] engine_create OK (MODE_ONLY_MEMORY)\n");
        engine_destroy(e);
    }
    return 0;
}
