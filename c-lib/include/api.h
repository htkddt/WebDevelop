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
#define API_SOURCE_OLLAMA   'O'  /* assistant reply from Ollama (AI) */

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

/** RAG embedding: built-in hash vector (default, no Ollama). See `.cursor/vector_generate.md`. */
#define API_VECTOR_GEN_CUSTOM  0
/** RAG embedding: Ollama /api/embed; optional `vector_ollama_model`, else same chain as chat embed. */
#define API_VECTOR_GEN_OLLAMA  1

/** api_embed_migration_enqueue: OR together to queue provenance backfill and/or full re-embed for tenant. */
#define API_EMBED_MIG_FLAG_PROVENANCE 1u
#define API_EMBED_MIG_FLAG_REEMBED    2u

/** Options for api_create(). All pointer fields may be NULL; 0 ports use defaults. */
typedef struct api_options {
    int mode;                   /* M4ENGINE_MODE_* above; default MONGO if 0 */
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
} api_stats_t;

typedef struct api_context api_context_t;

/**
 * Per-stream identifiers carried through the stream worker pthread (.cursor/streaming.md).
 * temp_message_id is heap-owned when allocated by the library (api_chat_stream with NULL id).
 */
typedef struct api_stream_context {
    const char *tenant_id;
    const char *user_id;
    char *temp_message_id;
} api_stream_context_t;

/** Token callback: token UTF-8 fragment; msg_id stable for this stream; done_flag 1 = stream finished (token may be empty). */
typedef void (*api_stream_token_cb)(const char *token, const char *msg_id, int done_flag, void *userdata);

/* --- 6 public APIs --- */

/**
 * 1. Create context from options. opts may be NULL for defaults (MONGO mode).
 * Returns NULL on failure.
 */
api_context_t *api_create(const api_options_t *opts);

/** 2. Destroy context and all resources. */
void api_destroy(api_context_t *ctx);

/**
 * Set chat model lane for subsequent api_chat / api_chat_stream (education, business, …).
 * lane: M4_API_MODEL_LANE_* . Returns 0 on success, -1 on invalid ctx or lane.
 */
int api_set_model_lane(api_context_t *ctx, int lane);

/**
 * Set lane by arbitrary key (must match a row in `model_switch_options_t.lanes` or getenv `M4_MODEL_<KEY>`).
 * NULL or "" clears session override (use DEFAULT + smart_topic merge). Key length < 64.
 */
int api_set_model_lane_key(api_context_t *ctx, const char *lane_key);

/**
 * Set or clear a supported prompt tag (see API_PROMPT_TAG_*). Unknown key → -1.
 * value NULL or "" removes the tag. On success returns 0. Not thread-safe vs concurrent api_chat on same ctx.
 * Used when building the chat prompt (api_chat / api_chat_stream via ctx_build_prompt); not applied to api_query.
 */
int api_set_prompt_tag(api_context_t *ctx, const char *key, const char *value);

/** Remove all prompt tags (revert to default persona-only system block). */
void api_clear_prompt_tags(api_context_t *ctx);

/**
 * 3. One chat turn: append user message, call Ollama, append bot reply, return bot text.
 * tenant_id: NULL or "" → API_DEFAULT_TENANT_ID; must satisfy tenant_id charset rules when non-empty.
 * user_id: NULL or "" → API_DEFAULT_USER_ID on writes/RAG; for history+prompt alignment, pass the same tenant_id
 * and user_id to api_load_chat_history (use NULL user_id there only for legacy tenant-wide history).
 * out_size is buffer size; out is null-terminated on success. Returns 0 on success, -1 on failure.
 */
int api_chat(api_context_t *ctx, const char *tenant_id, const char *user_id, const char *user_message,
             char *bot_reply_out, size_t out_size);

/**
 * Streaming chat turn: same context/RAG/smart_topic as api_chat, but Ollama tokens arrive via cb.
 * Runs the HTTP stream on an internal pthread; cb is invoked from that thread (serialize access to ctx).
 * temp_message_id: NULL → generate UUID (heap); else copy used as correlation id for Mongo (temp_message_id field).
 * user_id: stored on the turn document (default "default" if NULL/empty).
 * On success or Ollama failure, cb(..., done_flag==1) is called exactly once after stream ends; then msg_id is freed.
 * Returns 0 on success, -1 on allocation / thread / prepare error.
 */
int api_chat_stream(api_context_t *ctx,
                    const char *tenant_id,
                    const char *user_id,
                    const char *user_message,
                    const char *temp_message_id,
                    api_stream_token_cb cb,
                    void *userdata);

/**
 * 4. Raw query to Ollama (no chat history append). Returns 0 on success, -1 on failure.
 * out is null-terminated on success.
 */
int api_query(api_context_t *ctx, const char *prompt, char *out, size_t out_size);

/** 5. Fill stats snapshot (memory, connections, error/warning counts). */
void api_get_stats(api_context_t *ctx, api_stats_t *out);

/**
 * 6. Override ai_logs DB/collection (e.g. for scale). db and coll must pass validation.
 * Returns 0 on success, -1 on invalid names.
 */
int api_set_log_collection(api_context_t *ctx, const char *db, const char *coll);

/**
 * 7. Load chat history from MongoDB (when Mongo in options), scoped by tenant_id and optionally user_id.
 * tenant_id: NULL or "" → API_DEFAULT_TENANT_ID; validated like other tenant ids.
 * Fetches up to context_batch_size **documents** (newest by createdAt), then keeps at most
 * context_batch_size **messages** (user/assistant lines, most recent batch). Fills internal buffer oldest-first.
 * user_id: NULL or "" → **tenant-wide** (no `user` filter; may mix users in prompt — legacy).
 * Non-empty → only documents with Mongo `user` == user_id; use with the same tenant_id/user_id as api_chat for a correct prompt.
 * Invalid tenant_id or user_id → -1.
 * Returns 0 on success; 0 if Mongo not connected (no-op). -1 on error.
 */
int api_load_chat_history(api_context_t *ctx, const char *tenant_id, const char *user_id);

/**
 * 8. Number of messages in the **last active** session ring (after load or chat for that tenant/user).
 * See `.cursor/chat_l1_memory.md` if the session was evicted by idle timeout.
 */
int api_get_history_count(api_context_t *ctx);

/**
 * 9. Copy history message at index (0 = oldest). role_buf/content_buf null-terminated.
 * If source_out is non-NULL, writes API_SOURCE_* char (M/R/G/O) for display (e.g. "Bot [time]-[type]:").
 * If ts_buf is non-NULL and ts_size > 0, writes display timestamp (e.g. "10:07:48.117") for "You [ts]:" / "Bot [ts]:".
 * Returns 0 on success, -1 if index out of range.
 */
int api_get_history_message(api_context_t *ctx, int index,
                            char *role_buf, size_t role_size,
                            char *content_buf, size_t content_size,
                            char *source_out, char *ts_buf, size_t ts_size);

/** 10. Source of last assistant reply (API_SOURCE_REDIS or API_SOURCE_OLLAMA). 0 if none yet. For display "Bot [time]-[type]:". */
char api_get_last_reply_source(api_context_t *ctx);

/**
 * 11. Get geo_atlas landmarks for testing/debug (geo_learning module). Fills out with lines like "name (district, city)\n".
 * Returns number of bytes written (0 if no Mongo or no landmarks). Used by python_ai to verify geo data generation.
 */
size_t api_get_geo_atlas_landmarks(api_context_t *ctx, char *out, size_t out_size);

/**
 * 12. Insert one geo_atlas row (CSV / bulk import). Mirrors storage_geo_atlas_insert_doc; optional Redis geo index when vector set.
 * vector may be NULL and vector_dim 0 to insert without embedding (not recommended for dedup).
 * source defaults to "seed" if NULL/empty; verification_status defaults to "verified"; trust_score clamped 0..1.
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

/**
 * 13. Load CSV text into the geo authority L1 cache (first column = place name). No-op if `geo_authority` was not enabled at `api_create`.
 * Returns number of rows inserted, or -1 on error. See `.cursor/auth_geo.md`.
 */
int api_geo_authority_load_csv(api_context_t *ctx, const char *csv_utf8);

/**
 * 14. Backfill legacy `geo_atlas` rows (missing `country`, or `merged_into` without `merged` status).
 * Returns 0 on success, -1 on error. Writes modified document count to *modified_out when non-NULL.
 * See `.cursor/geo_leanring.md` §17.
 */
int api_geo_atlas_migrate_legacy(api_context_t *ctx, unsigned long long *modified_out);

/**
 * Queue async embed migration for tenant_id (NULL → "default"). flags: API_EMBED_MIG_FLAG_*.
 * Returns 0 on success, -1 if no engine/worker or invalid tenant.
 */
int api_embed_migration_enqueue(api_context_t *ctx, const char *tenant_id, unsigned flags);

#endif /* M4_API_H */
