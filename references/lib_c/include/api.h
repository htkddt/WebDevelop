#ifndef M4_API_H
#define M4_API_H

#include <stddef.h>
#include <stdint.h>

struct smart_topic_options;
struct model_switch_options;

/**
 * Public API surface for applications (see docs/api.md). Other headers (engine, storage, ollama, …)
 * are internal unless you extend the engine directly.
 */

/* Execution mode (rule §7 A/B/C/D). Use when creating context. */
#define M4ENGINE_MODE_ONLY_MEMORY      0  /* A: RAM only, no Mongo/Redis/ELK */
#define M4ENGINE_MODE_ONLY_MONGO       1  /* B: MongoDB only */
#define M4ENGINE_MODE_MONGO_REDIS      2  /* C: Mongo + Redis */
#define M4ENGINE_MODE_MONGO_REDIS_ELK  3  /* D: Mongo + Redis + ELK */

/** Default number of history cycles (user+bot pairs) to include when building context for the next chat. */
#define API_CONTEXT_BATCH_SIZE_DEFAULT 30
/**
 * Default seconds of inactivity before dropping a per-user in-memory L1 ring (see `.cursor/chat_l1_memory.md`).
 * Override: `M4_SESSION_IDLE_SECONDS` (0 = never evict) or `api_options_t.session_idle_seconds` > 0 at `api_create`.
 */
#define API_SESSION_IDLE_DEFAULT_SEC 300
/** Set `api_options_t.inject_geo_knowledge` to this to enable [KNOWLEDGE_BASE] from geo_atlas on each prompt (Mongo read). Default is off. */
#define API_INJECT_GEO_KNOWLEDGE_ON  1
#define API_INJECT_GEO_KNOWLEDGE_OFF 0

/**
 * Tenant and user keys must stay consistent across api_load_chat_history, api_chat, and api_chat_stream:
 * Mongo always filters by `tenant_id`; optional `user_id` scopes `user` + Redis L1/L2 within that tenant.
 */
#define API_DEFAULT_TENANT_ID "default"
/** Stored Mongo `user` and RAG scope when user_id is NULL or "" on api_chat / api_chat_stream. */
#define API_DEFAULT_USER_ID   "default"

/** Message source (where the message came from); used for display e.g. "Bot [time]-[type]:". */
#define API_SOURCE_MEMORY   'M'  /* current session / in-memory */
#define API_SOURCE_REDIS    'R'  /* assistant reply from Redis (vector cache hit) */
#define API_SOURCE_MONGODB  'G'  /* loaded from MongoDB (history) */
#define API_SOURCE_OLLAMA   'O'  /* assistant reply from local Ollama HTTP only — not a generic "default LLM" */
#define API_SOURCE_CLOUD    'C'  /* assistant reply from hosted LLM (c-lib ai_agent + libcurl) */

/**
 * `metadata.llm_model_id` / in-memory slot uses `provider:model_id`. Keep in sync with
 * `python_ai/engine_ctypes.py` (`API_LLM_ROUTE_PREFIX_*`, `API_LLM_MODEL_ID_REDIS_RAG`) and `src/ai_agent.c` (`cl_write_llm_label`).
 * `API_LLM_ROUTE_PREFIX_OLLAMA` is only for turns that actually hit local Ollama — not a generic default label.
 */
#define API_LLM_ROUTE_PREFIX_GROQ      "groq"
#define API_LLM_ROUTE_PREFIX_CEREBRAS   "cerebras"
#define API_LLM_ROUTE_PREFIX_GEMINI    "gemini"
#define API_LLM_ROUTE_PREFIX_OLLAMA    "ollama"
/** When the assistant text is a Redis vector cache hit (no `provider:` in id); session / Mongo `llm_model_id`. */
#define API_LLM_MODEL_ID_REDIS_RAG     "redis_rag"

/** `M4_CHAT_BACKEND` env values consumed by `ai_agent.c` (see `.cursor/models/ai_agent.md`). */
#define API_CHAT_BACKEND_ENV_OLLAMA    "ollama"
#define API_CHAT_BACKEND_ENV_CLOUD     "cloud"

/**
 * Which HTTP/API path produced the last assistant reply — from the call site in c-lib, not from parsing model_id.
 * Use for adapter-specific prompt packing or logging. See `.cursor/ptomp.md` (classify by routing).
 */
#define API_CHAT_WIRE_NONE         0u
#define API_CHAT_WIRE_OPENAI_CHAT  1u /* Groq, Cerebras, any OpenAI-compatible chat/completions */
#define API_CHAT_WIRE_GEMINI       2u /* Google generateContent */
#define API_CHAT_WIRE_OLLAMA       3u /* local ollama_query* */
#define API_CHAT_WIRE_REDIS_RAG    4u /* vector cache hit; no LLM */
#define API_CHAT_WIRE_EXTERNAL     5u /* api_chat_external_reply (host-supplied text) */

/**
 * Supported dynamic prompt tags (.cursor/ptomp.md). Only these keys are accepted by api_set_prompt_tag().
 * Values are UTF-8; copied into the context (strdup). NULL or empty value clears the tag.
 *
 * Render order inside the LLM prompt (after Topic + optional [KNOWLEDGE_BASE]):
 *   1) system_time — [SYSTEM_TIME: …] + temporal instruction (or pass a line already starting with "[SYSTEM_TIME:")
 *   2) persona — full system persona block; if unset, the compiled-in default (Mắm) is used
 *   3) instructions — optional extra system text before chat history
 */
#define API_PROMPT_TAG_SYSTEM_TIME  "system_time"
#define API_PROMPT_TAG_PERSONA      "persona"
#define API_PROMPT_TAG_INSTRUCTIONS "instructions"
/** Max bytes per tag value (excluding NUL). */
#define API_PROMPT_TAG_VALUE_MAX    16384
/** Max bytes for `api_get_last_llm_model` / per-history-slot completion label (e.g. groq:llama-3.1-8b-instant). */
#define API_LLM_MODEL_MAX           160

/** RAG embedding: built-in hash vector (default, no Ollama). See `.cursor/vector_generate.md`. */
#define API_VECTOR_GEN_CUSTOM  0
/** RAG embedding: Ollama /api/embed; optional `vector_ollama_model`, else same chain as chat embed. */
#define API_VECTOR_GEN_OLLAMA  1

/** api_embed_migration_enqueue: OR together to queue provenance backfill and/or full re-embed for tenant. */
#define API_EMBED_MIG_FLAG_PROVENANCE 1u
#define API_EMBED_MIG_FLAG_REEMBED    2u

/** Options for api_create(). All pointer fields may be NULL; 0 ports use defaults. */
typedef struct api_options {
    int mode;                   /* M4ENGINE_MODE_* above; ELK (es_host) is used only when mode == M4ENGINE_MODE_MONGO_REDIS_ELK */
    const char *mongo_uri;      /* default "mongodb://127.0.0.1:27017" */
    const char *redis_host;     /* default "127.0.0.1" */
    int redis_port;             /* 0 = 6379 */
    const char *es_host;        /* NULL or "" = ELK disabled */
    int es_port;                /* 0 = 9200 */
    const char *log_db;         /* ai_logs DB override (validated [a-zA-Z0-9_], 1..63 chars) */
    const char *log_coll;       /* ai_logs collection override (same validation) */
    /** Limit how many history cycles (user+bot pairs) are used when building context for input.
     *  Chat history may still be kept larger (e.g. 30 for display); this caps what is sent to the LLM.
     *  0 = use API_CONTEXT_BATCH_SIZE_DEFAULT (30). */
    int context_batch_size;
    /** Optional: smart topic (mini AI) for intent-based temperature. When set and enable==true,
     *  engine_init calls initial_smart_topic and queries use TECH 0.0 / CHAT 0.8 / DEFAULT 0.5. */
    const struct smart_topic_options *smart_topic_opts;
    /** Geo atlas: 0 (default) = do not read Mongo geo_atlas when building each chat prompt (avoids blocking I/O).
     *  Non-zero = prepend [KNOWLEDGE_BASE] from geo_atlas (opt-in). Background geo_learning still runs async when enabled. */
    int inject_geo_knowledge;
    /** 0 (default): if `API_PROMPT_TAG_SYSTEM_TIME` is not set, prepend `[SYSTEM_TIME: YYYY-MM-DD HH:MM]` from local wall clock (`time`/`localtime`) on each `ctx_build_prompt`. Set non-zero to disable (model then has no clock unless the host calls `api_set_prompt_tag`). */
    int disable_auto_system_time;
    /** Non-zero: enable in-memory geo authority cache (seed provinces, CSV load, audit); see `.cursor/auth_geo.md`. */
    int geo_authority;
    /** Optional: lane-based Ollama model + inject strings; NULL = env-only / defaults (`model_switch.h`, `.cursor/model_switch.md`). */
    const struct model_switch_options *model_switch_opts;
    /**
     * `API_VECTOR_GEN_CUSTOM` (0, default): hash-based vector for Redis/Mongo RAG without Ollama embed.
     * `API_VECTOR_GEN_OLLAMA` (1): Ollama embed; `vector_ollama_model` NULL/empty → `OLLAMA_EMBED_MODEL` / tags / env chain.
     */
    int vector_gen_backend;
    const char *vector_ollama_model;
    /** Non-zero: after engine_init, enqueue provenance migration for tenant "default" (see M4_EMBED_MIGRATION_ON_START). */
    int embed_migration_autostart;
    /**
     * Per-user in-memory L1 idle eviction (`.cursor/chat_l1_memory.md`). 0 = use `M4_SESSION_IDLE_SECONDS` env or
     * API_SESSION_IDLE_DEFAULT_SEC. >0 = seconds before removing an idle session ring. Env `M4_SESSION_IDLE_SECONDS=0` disables.
     */
    int session_idle_seconds;
    /**
     * Optional: separate Mongo URI for SharedCollection (registry ELK backfill / collection validation).
     * NULL or "" = use chat `mongo_uri` only (no getenv).
     * Non-empty values must be a valid Mongo connection URI (`mongodb://` or `mongodb+srv://`); `api_create` rejects others.
     */
    const char *shared_collection_mongo_uri;
    /** Optional: SharedCollection registry JSON path (see `.cursor/shared_collection.md`). NULL or "" = no registry. */
    const char *shared_collection_json_path;
    /** Optional: Mongo database name for SC backfill / registry checks. NULL or "" = env or default `m4_ai`. */
    const char *shared_collection_backfill_db;
    /**
     * TSV/JSON path for NL routing term→intent counts (`nl_learn_terms` module). NULL or "" = disabled.
     * When **`enable_learning_terms`** is non-zero, successful chat turns (**`api_chat`**, **`api_chat_stream`** /
     * **`api_chat_stream_from_prepared`**, Redis RAG short-circuit, **`api_chat_external_reply*`) run internal
     * phrase→intent cue recording + flush (no separate record API from the app host). See `.cursor/elk_nl_routing.md` §8.
     */
    const char *learning_terms_path;
    /** Non-zero: allow internal cue recording and file updates; zero = load/read-only (e.g. `api_nl_learn_terms_score_sum` only). */
    int enable_learning_terms;
    /**
     * Non-zero: load `learning_terms_path` in a **background thread** during `api_create` (engine init returns immediately).
     * Until load finishes, internal learning hooks no-op — safe for immediate chat.
     * Zero (default): synchronous `nl_learn_terms_open` in `api_create` (failure fails context creation).
     */
    int defer_learning_terms_load;
    /** Path for LLM query plan cache. NULL or "" = in-memory only (lost on restart). See docs/api.md. */
    const char *query_cache_path;
    /** Initial persona prompt tag (API_PROMPT_TAG_PERSONA). NULL or "" = use compiled-in default. Copied at api_create. */
    const char *default_persona;
    /** Initial instructions prompt tag (API_PROMPT_TAG_INSTRUCTIONS). NULL or "" = none. Copied at api_create. */
    const char *default_instructions;
    /** Initial model lane (M4_API_MODEL_LANE_*). 0 = DEFAULT. Applied at api_create. */
    int default_model_lane;
    /** Path to geo authority CSV file. NULL or "" = disabled. Loaded into L1 cache at api_create. */
    const char *geo_authority_csv_path;
    /** Non-zero: run storage_geo_atlas_migrate_legacy at engine_init (backfill country + merger status). */
    int geo_migrate_legacy;
    /** Non-zero: enable background health checks for api_get_stats. Default 0 (disabled, saves resources). */
    int enable_stats;
    /** Non-zero: enable incremental ELK sync. Tracks last indexed _id per collection in a state file
     *  alongside learning_terms_path (or shared_collection_json_path dir). On restart, only indexes
     *  docs with _id > saved_id. First run does full backfill. See docs/api.md. */
    int schedule_refresh;
    /**
     * Debug log filter: array of module keys to enable debug logging for.
     * NULL or count 0 = no debug logs. Example: {"GEO_LEARNING", "ai_agent", "INTENT_ROUTE"}.
     * Valid keys: API, ai_agent, STORAGE, GEO_LEARNING, GEO_AUTH, OLLAMA, ELK,
     *             EMBED_MIGRATION, ENGINE, CHAT, nl_learn_terms, nl_learn_cues,
     *             LOGIC_CONFLICT, INTENT_ROUTE, SHARED_COLLECTION, SMART_TOPIC.
     * Unknown keys are logged as warnings at api_create.
     */
    const char *const *debug_modules;
    int debug_module_count;
} api_options_t;

/** Lane ids for api_set_model_lane (canonical keys: DEFAULT, EDUCATION, …). */
#define M4_API_MODEL_LANE_DEFAULT    0
#define M4_API_MODEL_LANE_EDUCATION  1
#define M4_API_MODEL_LANE_BUSINESS   2
#define M4_API_MODEL_LANE_TECH       3
#define M4_API_MODEL_LANE_CHAT       4

/** Stats snapshot filled by api_get_stats(). */
typedef struct api_stats {
    uint64_t memory_bytes;
    int mongo_connected;
    int redis_connected;
    int elk_enabled;
    int elk_connected;
    int ollama_connected;   /* 1 = Ollama running (health check GET /api/tags), 0 = not */
    uint64_t error_count;
    uint64_t warning_count;
    uint64_t processed;
    uint64_t errors;
    /** 1 if this build linked the MongoDB C driver (USE_MONGOC); 0 = stub only, mongo_connected will stay 0. */
    int mongoc_linked;
    /** Last assistant reply source (API_SOURCE_*: M/R/G/O/C). 0 if no reply yet. */
    char last_reply_source;
    /** Last assistant reply wire/API family (API_CHAT_WIRE_*). 0 if none. */
    unsigned last_chat_wire;
    /** Last assistant completion model/route (e.g. "groq:llama-3.1-8b-instant"). Empty if unknown. */
    char last_llm_model[160];
} api_stats_t;

typedef struct api_context api_context_t;

/** Token callback: token UTF-8 fragment; msg_id stable for this stream; done_flag 1 = stream finished (token may be empty). */
typedef void (*api_stream_token_cb)(const char *token, const char *msg_id, int done_flag, void *userdata);

/* Build-time Ollama defaults — moved to api_build.h; included here for backward compatibility. */
#include "api_build.h"

/* ============================================================================
 * Public API — 7 functions only. All other functionality is configured via
 * api_options_t or runs automatically inside c-lib. See docs/api.md.
 * ============================================================================ */

/**
 * 1. Create context from JSON options string. Any key not present uses default.
 * json_opts may be NULL or "{}" for full defaults.
 * Returns NULL on failure or invalid JSON.
 *
 * Example:
 *   {
 *     "mode": 3,
 *     "mongo_uri": "mongodb://127.0.0.1:27017",
 *     "redis_host": "127.0.0.1",
 *     "redis_port": 6379,
 *     "debug_modules": ["API", "ai_agent", "STORAGE"],
 *     "default_persona": "You are a helpful assistant.",
 *     "default_model_lane": 4,
 *     "lanes": [
 *       {"key": "BUSINESS", "model": "finance-llm", "api_url": "https://...", "api_key": "gsk_..."},
 *       {"key": "TECH", "model": "codellama", "inject": "You are a code expert."}
 *     ]
 *   }
 */
api_context_t *api_create(const char *json_opts);

/** 1b. Create context from C struct (for C callers who prefer struct over JSON). opts may be NULL for defaults. */
api_context_t *api_create_with_opts(const api_options_t *opts);

/** 2. Destroy context and all resources. */
void api_destroy(api_context_t *ctx);

/**
 * 3. Unified chat turn (sync + stream).
 * tenant_id: NULL or "" → "default". user_id: NULL or "" → "default".
 * user_message: user text. NULL or "" with context_json → auto-greeting.
 * context_json: optional JSON with user/session info injected as [CONTEXT] in prompt.
 *   NULL = no context. Example: {"user":{"name":"Ky","role":"Engineer"}}.
 * bot_reply_out / out_size: buffer for the full reply (null-terminated on success).
 * stream_cb: NULL → synchronous. Non-NULL → tokens via callback.
 * Returns 0 on success, -1 on failure.
 */
int api_chat(api_context_t *ctx, const char *tenant_id, const char *user_id, const char *user_message,
             const char *context_json,
             char *bot_reply_out, size_t out_size,
             api_stream_token_cb stream_cb, void *stream_userdata);

/**
 * 4. Load chat history from MongoDB and return as JSON array.
 * Primes the internal session ring buffer (for prompt context) and returns messages as JSON.
 * json_out: caller-allocated buffer filled with JSON array. null-terminated.
 * Returns: number of messages loaded (>= 0), or -1 on error. 0 if Mongo not connected or no history.
 *
 * Output format:
 *   [{"role":"user","content":"hello","source":"M","timestamp":"10:07:48","llm_model":""},
 *    {"role":"assistant","content":"hi","source":"C","timestamp":"10:07:49","llm_model":"gemini:gemini-2.5-flash"}]
 */
int api_load_chat_history(api_context_t *ctx, const char *tenant_id, const char *user_id,
                          char *json_out, size_t json_out_size);

/**
 * 5. Greeting — generate a welcome message for the user.
 * context_json: user info (e.g. {"name":"Ky","role":"ADMIN"}). NULL = generic greeting.
 * greet_opts_json: options. NULL or "{}" = defaults (condition=TODAY, response_type=CHAT).
 *   {
 *     "condition": "TODAY"|"WEEK"|"HOUR"|"SESSION"|"ALWAYS",
 *     "response_type": "CHAT"|"TEMPLATE"|"SILENT",
 *     "custom_prompt": "optional override prompt"
 *   }
 * Returns: 0 = greeting generated, 1 = no greeting needed (condition not met), -1 = error.
 */
int api_greet(api_context_t *ctx, const char *tenant_id, const char *user_id,
              const char *context_json, const char *greet_opts_json,
              char *reply_out, size_t out_size);

/** 6. Fill stats snapshot (memory, connections, counters, last reply metadata). */
void api_get_stats(api_context_t *ctx, api_stats_t *out);

/**
 * 7. Insert one geo_atlas row (runtime data enrichment from frontend).
 * vector may be NULL / vector_dim 0 to insert without embedding.
 * source defaults to "seed"; verification_status defaults to "verified"; trust_score clamped 0..1.
 * Returns 0 on success, -1 on error.
 */
int api_geo_atlas_import_row(api_context_t *ctx,
                             const char *tenant_id,
                             const char *name,
                             const char *name_normalized,
                             const char *district,
                             const char *axis,
                             const char *category,
                             const char *city,
                             const float *vector,
                             size_t vector_dim,
                             const char *embed_model_id,
                             const char *source,
                             const char *verification_status,
                             double trust_score);

#endif /* M4_API_H */
