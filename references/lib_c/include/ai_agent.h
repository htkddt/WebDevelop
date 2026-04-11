#ifndef M4_AI_AGENT_H
#define M4_AI_AGENT_H

#include <stddef.h>

/** Provider limits for prompt trimming. */
typedef struct {
    const char *name;
    int context_window_tokens;
    int prompt_max_chars;
    int daily_input_tokens;
    int save_daily_quota;
} ai_agent_provider_limits_t;

const ai_agent_provider_limits_t *ai_agent_get_provider_limits(const char *name);

/**
 * Structured prompt parts — dynamic allocation, no fixed limits.
 * Built by ctx_build_prompt_parts, trimmed per provider inside that function.
 *
 * For OPENAI_CHAT: system → {"role":"system"}, each history → {"role":"user"/"assistant"}, user → {"role":"user"}
 * For GEMINI:      system as systemInstruction, history + user as contents[].parts[]
 * For OLLAMA:      all concatenated into single prompt blob
 *
 * Lifecycle: call ai_agent_prompt_init to create, ai_agent_prompt_free to destroy.
 */
typedef struct {
    char *system;               /* heap: topic + knowledge + time + persona + instructions */
    size_t system_len;
    struct ai_agent_prompt_msg {
        char role[16];          /* "user" or "assistant" */
        char *content;          /* heap: message text */
    } *history;                 /* heap: dynamic array */
    int history_count;
    int history_cap;
    char *user;                 /* heap: current user message */
    size_t user_len;
} ai_agent_prompt_t;

/** Initialize an empty prompt (zeroed). */
void ai_agent_prompt_init(ai_agent_prompt_t *p);

/** Set system text (copies input). */
void ai_agent_prompt_set_system(ai_agent_prompt_t *p, const char *text);

/** Set user message (copies input). */
void ai_agent_prompt_set_user(ai_agent_prompt_t *p, const char *text);

/** Append a history message (copies role + content). */
void ai_agent_prompt_add_history(ai_agent_prompt_t *p, const char *role, const char *content);

/** Free all heap memory in the prompt. Safe to call multiple times. */
void ai_agent_prompt_free(ai_agent_prompt_t *p);

/**
 * AI Agent: LLM routing (cloud pool + local Ollama fallback).
 *
 * Routing logic:
 *   1. lane_api_url set → direct call to that endpoint with lane_api_key + model (no pool).
 *   2. ollama_model_lane_pin non-empty (no url) → local Ollama only with that model.
 *   3. Neither set → cloud pool (Groq → Cerebras → Gemini → Ollama) per M4_CLOUD_TRY_ORDER.
 *
 * Env: M4_CHAT_BACKEND, GROQ_API_KEY, CEREBRAS_API_KEY, GEMINI_API_KEY, M4_CLOUD_*_MODEL, M4_CLOUD_TRY_ORDER.
 * Diagnostics: M4_LOG_CLOUD_LLM=1 (stderr: curl errors, HTTP status, response prefix).
 *
 * On success writes assistant text and sets *source_out, *wire_out, llm_model_out. Returns 0 or -1.
 */
/**
 * @param prompt  Structured prompt parts (system, history, user). If NULL, uses context_prompt as blob fallback.
 * @param context_prompt  Flat prompt blob (Ollama style). Used when prompt is NULL, or as Ollama fallback.
 */
int ai_agent_complete_chat(const ai_agent_prompt_t *prompt,
                           const char *context_prompt, double temperature,
                           const char *ollama_model_lane_pin,
                           const char *lane_api_url, const char *lane_api_key,
                           char *bot_reply_out, size_t out_size,
                           char *source_out, unsigned *wire_out, char *llm_model_out, size_t llm_model_cap);

#endif /* M4_AI_AGENT_H */
