#include "api.h"
#include "engine.h"
#include "conflict_detector.h"
#include "geo_authority.h"
#include "model_switch.h"
#include "smart_topic.h"
#include "storage.h"
#include "stat.h"
#include "ollama.h"
#include "embed.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdint.h>

static const char *DEFAULT_MONGO_URI = "mongodb://127.0.0.1:27017";
static const char *DEFAULT_REDIS_HOST = "127.0.0.1";
static const int DEFAULT_REDIS_PORT = 6379;
static const int DEFAULT_ES_PORT = 9200;
static const char *DEFAULT_TENANT_ID = "default";

/* Step 4: In-memory context circular buffer. Rule: sliding window, head = (head+1)%N. */
#define API_CTX_CAPACITY_MAX  64
#define API_CTX_MSG_SIZE      2048
#define API_CONTEXT_BUFFER_SIZE 65536  /* 64KB for reconstructed prompt; rule §13 */
#define API_CTX_ACTIVE_REASONING_MAX 5  /* last N messages in prompt (strip-5: last 5 full) */
#define API_CTX_STRIP_5                 5  /* rule strip-5: if batch > 5, earlier = user-only; last 5 = full input/assistant */
#define API_CTX_TOPIC_MAX 256           /* pre-query: compose topic line (internal, not public API) */
#define API_CTX_EARLIER_MAX 2048        /* max chars for "Earlier (user only): ..." block */
#define API_CTX_TS_SIZE 32               /* display timestamp per message (e.g. "10:07:48.117") */
#define API_CTX_ROLE_SIZE 16             /* role string per slot; must fit "assistant" (9 chars) for You/Bot display */
#define API_RAG_PREFIX_MAX 4096         /* RAG context prepended to prompt when Redis L2 has hits */
#define API_PROMPT_TAG_SLOTS 3          /* system_time, persona, instructions — keys in api.h */

/* System guard: Mắm persona per .cursor/ptomp.md — identity, anti-AI, tone, Scale 3, commands, conciseness. */
static const char API_CTX_SYSTEM_GUARD[] =
    "You are Mắm: a gritty Saigon local, Senior C Engineer (tông Lào). "
    "Never say \"I am an AI\", \"I am a C program\", or \"I apologize\". "
    "Be concise, salty, street-smart (Saigon slang). "
    "If lang is mixed, reply only in Saigon-style Vietnamese. "
    "If you have no context from Memory/ELK, say you don't know—do not invent places. "
    "If user says \"Speak English\", switch to English at once; do not explain the switch. "
    "Get straight to the point.\n\n";

/* Appended after [SYSTEM_TIME: …] for explicit tag and auto wall-clock paths (.cursor/ptomp.md). */
static const char API_SYSTEM_TIME_INSTRUCTION[] =
    "Use this [SYSTEM_TIME] as your absolute temporal truth; do not rely on your training cut-off for \"today\".\n"
    "For questions about the current calendar year, month, date, weekday, or phrases like \"năm nay\" / \"this year\", "
    "answer using ONLY the date/time in [SYSTEM_TIME] above—do not invent or recall a year from training data.\n\n";

#define API_RAG_REPLY_MIN_SCORE 0.85  /* if Redis L2 hit score >= this, use as reply and skip Ollama */
#define API_RAG_FIRST_SNIPPET_SIZE 2048
typedef struct {
    char buf[API_RAG_PREFIX_MAX];
    size_t used;
    char first_snippet[API_RAG_FIRST_SNIPPET_SIZE];  /* top hit for cache-reply path */
    size_t first_len;
    double best_score;
} rag_accum_t;
static void rag_accum_cb(const char *snippet, double score, void *userdata) {
    rag_accum_t *a = (rag_accum_t *)userdata;
    if (!a) return;
    if (a->first_len == 0 && snippet) {
        size_t slen = strnlen(snippet, API_RAG_FIRST_SNIPPET_SIZE - 1);
        if (slen > 0) {
            memcpy(a->first_snippet, snippet, slen);
            a->first_snippet[slen] = '\0';
            a->first_len = slen;
            a->best_score = score;
        }
    }
    if (a->used >= sizeof(a->buf) - 1) return;
    size_t rem = sizeof(a->buf) - a->used;
    int n = snprintf(a->buf + a->used, rem, "%s\n", snippet);
    if (n > 0 && (size_t)n < rem) a->used += (size_t)n;
    else if (n > 0) a->used = sizeof(a->buf) - 1;
}

struct api_context {
    engine_t *engine;
    stat_ctx_t *stat;
    int context_batch_size;
    /* Circular buffer: last N messages (user/assistant). head = oldest slot. */
    int ctx_head;
    int ctx_count;
    int ctx_capacity;
    char *ctx_roles;     /* ctx_capacity * API_CTX_ROLE_SIZE */
    char *ctx_messages;  /* ctx_capacity * API_CTX_MSG_SIZE */
    char *ctx_sources;   /* ctx_capacity: API_SOURCE_MEMORY/REDIS/MONGODB/OLLAMA per slot */
    char *ctx_timestamps; /* ctx_capacity * API_CTX_TS_SIZE: display time per message */
    char last_reply_source; /* API_SOURCE_REDIS or API_SOURCE_OLLAMA after last assistant push; 0 if none */
    int inject_geo_knowledge; /* 0 = skip Mongo read in prompt build (default, non-blocking) */
    char *prompt_tag_slots[API_PROMPT_TAG_SLOTS]; /* strdup; see API_PROMPT_TAG_* */
    int disable_auto_system_time; /* 1 = skip local clock [SYSTEM_TIME] when tag unset */
    /** Forced lane key for model_switch (e.g. "EDUCATION"); empty = DEFAULT / merge from smart_topic. */
    char model_lane_key[MODEL_SWITCH_LANE_KEY_MAX];
};

/* Chat debug (stderr): on by default; set M4_DEBUG_CHAT=0|false|no|off to disable.
 * M4_DEBUG_CHAT=1|true|yes|2|verbose; M4_DEBUG_CHAT_TOKENS=1 for per-token logs. */
static int api_debug_chat_on(void) {
    const char *e = getenv("M4_DEBUG_CHAT");
    if (!e || !e[0]) return 1;
    if (e[0] == '0' && e[1] == '\0') return 0;
    if (strcmp(e, "false") == 0) return 0;
    if (strcmp(e, "no") == 0) return 0;
    if (strcmp(e, "off") == 0) return 0;
    if (e[0] == '1' && e[1] == '\0') return 1;
    if (e[0] == '2' && e[1] == '\0') return 1;
    if (strcmp(e, "verbose") == 0) return 1;
    if (strcmp(e, "true") == 0) return 1;
    if (strcmp(e, "yes") == 0) return 1;
    return 0;
}

static int api_debug_chat_verbose(void) {
    const char *e = getenv("M4_DEBUG_CHAT");
    if (!e) return 0;
    if (e[0] == '2' && e[1] == '\0') return 1;
    if (strcmp(e, "verbose") == 0) return 1;
    return 0;
}

static int api_debug_chat_tokens(void) {
    const char *e = getenv("M4_DEBUG_CHAT_TOKENS");
    return e && e[0] == '1' && e[1] == '\0';
}

static void api_debug_utf8_preview(const char *tag, const char *s, size_t max_show) {
    if (!s) s = "";
    size_t L = strlen(s);
    size_t n = L < max_show ? L : max_show;
    fprintf(stderr, "[API]%s len=%zu\n", tag, L);
    fprintf(stderr, "%.*s%s\n", (int)n, s, L > n ? "..." : "");
}

static void api_debug_storage(const api_context_t *ctx, const char *phase) {
    if (!ctx || !api_debug_chat_on()) return;
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    const engine_config_t *cfg = engine_get_config(ctx->engine);
    int mc = (st && storage_mongo_connected(st)) ? 1 : 0;
    int rc = (st && storage_redis_connected(st)) ? 1 : 0;
    int elk = (cfg && cfg->es_host && cfg->es_host[0]) ? 1 : 0;
    int elk_ok = 0;
    if (elk && cfg)
        elk_ok = (elasticsearch_check_reachable(cfg->es_host, cfg->es_port) == 0) ? 1 : 0;
    int vs = engine_vector_search_enabled(ctx->engine) ? 1 : 0;
#ifdef USE_MONGOC
    int mlink = 1;
#else
    int mlink = 0;
#endif
    fprintf(stderr,
            "[API][%s] storage: mongoc_linked=%d mongo_connected=%d redis_connected=%d elk_configured=%d "
            "elk_reachable=%d vector_search=%d execution_mode=%d\n",
            phase, mlink, mc, rc, elk, elk_ok, vs,
            cfg ? (int)cfg->execution_mode : -1);
}

static execution_mode_t api_mode_to_engine(int mode) {
    switch (mode) {
        case M4ENGINE_MODE_ONLY_MEMORY:     return MODE_ONLY_MEMORY;
        case M4ENGINE_MODE_ONLY_MONGO:      return MODE_ONLY_MONGO;
        case M4ENGINE_MODE_MONGO_REDIS:     return MODE_MONGO_REDIS;
        case M4ENGINE_MODE_MONGO_REDIS_ELK: return MODE_MONGO_REDIS_ELK;
        default:                            return MODE_ONLY_MONGO;
    }
}

/* Clear context buffer (e.g. before loading history from DB). */
static void ctx_clear(api_context_t *ctx) {
    if (!ctx) return;
    ctx->ctx_head = 0;
    ctx->ctx_count = 0;
}

/* Push one message with source and optional display timestamp. ts NULL or "" stored as empty. */
static void ctx_push_message_with_source(api_context_t *ctx, const char *role, const char *content, char source, const char *ts) {
    if (!ctx || !ctx->ctx_roles || !ctx->ctx_messages || !ctx->ctx_sources || ctx->ctx_capacity <= 0) return;
    const char *r = role ? role : "user";
    size_t rlen = strlen(r);
    if (rlen >= API_CTX_ROLE_SIZE) rlen = API_CTX_ROLE_SIZE - 1;
    size_t clen = content ? strnlen(content, API_CTX_MSG_SIZE - 1) : 0;
    if (!source) source = (r[0] == 'u') ? API_SOURCE_MEMORY : API_SOURCE_OLLAMA;
    int idx;
    if (ctx->ctx_count < ctx->ctx_capacity) {
        idx = (ctx->ctx_head + ctx->ctx_count) % ctx->ctx_capacity;
        ctx->ctx_count++;
    } else {
        idx = ctx->ctx_head;
        ctx->ctx_head = (ctx->ctx_head + 1) % ctx->ctx_capacity;
    }
    memcpy(ctx->ctx_roles + idx * API_CTX_ROLE_SIZE, r, rlen);
    ctx->ctx_roles[idx * API_CTX_ROLE_SIZE + rlen] = '\0';
    if (clen > 0)
        memcpy(ctx->ctx_messages + idx * API_CTX_MSG_SIZE, content, clen);
    ctx->ctx_messages[idx * API_CTX_MSG_SIZE + clen] = '\0';
    ctx->ctx_sources[idx] = source;
    if (ctx->ctx_timestamps) {
        size_t tlen = ts && ts[0] ? strnlen(ts, API_CTX_TS_SIZE - 1) : 0;
        char *slot = ctx->ctx_timestamps + idx * API_CTX_TS_SIZE;
        if (tlen > 0) memcpy(slot, ts, tlen);
        slot[tlen] = '\0';
    }
    if (r[0] == 'a' || strcmp(r, "assistant") == 0) ctx->last_reply_source = source;
}
#define ctx_push_message(ctx, role, content) ctx_push_message_with_source((ctx), (role), (content), 0, NULL)

/* Pre-query: compose topic from current user message (internal only, not a public API). */
static size_t ctx_compose_topic(const char *current_user_msg, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    const char *topic = (current_user_msg && current_user_msg[0]) ? current_user_msg : "General";
    size_t tlen = strnlen(topic, API_CTX_TOPIC_MAX - 16);
    if (tlen > (size_t)(API_CTX_TOPIC_MAX - 16)) tlen = API_CTX_TOPIC_MAX - 16;
    size_t n = (size_t)snprintf(out, out_size, "Topic: %.*s\n\n", (int)tlen, topic);
    return n < out_size ? n : out_size - 1;
}

#define API_CTX_KNOWLEDGE_BASE_MAX 2048

static int prompt_tag_slot_for_key(const char *key) {
    if (!key || !key[0]) return -1;
    if (strcmp(key, API_PROMPT_TAG_SYSTEM_TIME) == 0) return 0;
    if (strcmp(key, API_PROMPT_TAG_PERSONA) == 0) return 1;
    if (strcmp(key, API_PROMPT_TAG_INSTRUCTIONS) == 0) return 2;
    return -1;
}

static void prompt_tags_free(api_context_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < API_PROMPT_TAG_SLOTS; i++) {
        free(ctx->prompt_tag_slots[i]);
        ctx->prompt_tag_slots[i] = NULL;
    }
}

static void append_system_time_wall_clock(char *out, size_t out_size, size_t *n) {
    if (!out || !n || *n >= out_size) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char tb[64];
    if (!tm || strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", tm) == 0) return;
    size_t rem = out_size - *n;
    if (rem <= 1) return;
    int w = snprintf(out + *n, rem, "[SYSTEM_TIME: %s]\n\n%s", tb, API_SYSTEM_TIME_INSTRUCTION);
    if (w > 0 && (size_t)w < rem) *n += (size_t)w;
    else if (w > 0) *n = out_size - 1;
}

static void ensure_double_newline_suffix(char *out, size_t cap, size_t *n) {
    if (!out || !n || *n >= cap) return;
    if (*n >= 2 && out[*n - 2] == '\n' && out[*n - 1] == '\n') return;
    if (*n >= 1 && out[*n - 1] == '\n') {
        if (*n < cap - 1) out[(*n)++] = '\n';
        return;
    }
    if (*n < cap - 1) out[(*n)++] = '\n';
    if (*n < cap - 1) out[(*n)++] = '\n';
}

/* Append UTF-8 block; ensure paragraph ends with \n\n before next section. */
static void append_block_paragraph(char *out, size_t cap, size_t *n, const char *s) {
    if (!out || !n || !s || !s[0] || cap <= *n) return;
    size_t rem = cap - *n;
    size_t sl = strlen(s);
    if (sl >= rem) sl = rem - 1;
    memcpy(out + *n, s, sl);
    *n += sl;
    ensure_double_newline_suffix(out, cap, n);
}

/* Build prompt: topic + optional [KNOWLEDGE_BASE] + system guard + last N messages. Internal only. */
static size_t ctx_build_prompt(const api_context_t *ctx, const char *current_user_msg,
                               char *out, size_t out_size) {
    if (!ctx || !out || out_size == 0) return 0;
    size_t n = 0;
    n = ctx_compose_topic(current_user_msg, out, out_size);
    if (n >= out_size) { out[out_size - 1] = '\0'; return out_size - 1; }
    /* [KNOWLEDGE_BASE] only when opt-in: avoids blocking Mongo read on every chat (geo_atlas is optional). */
    if (ctx->inject_geo_knowledge) {
        storage_ctx_t *st = engine_get_storage(ctx->engine);
        if (st) {
            char kb_buf[API_CTX_KNOWLEDGE_BASE_MAX];
            size_t kb_len = storage_geo_atlas_get_landmarks_for_prompt(st, kb_buf, sizeof(kb_buf));
            if (kb_len > 0 && n + 20 + kb_len < out_size) {
                int w = snprintf(out + n, out_size - n, "[KNOWLEDGE_BASE]\n%s\n\n", kb_buf);
                if (w > 0) n += (size_t)w;
            }
        }
    }
    {
        const engine_config_t *ecfg = engine_get_config(ctx->engine);
        if (ecfg && ecfg->geo_authority_enabled) {
            char gh[1024];
            geo_authority_prompt_hint(gh, sizeof(gh));
            if (gh[0]) append_block_paragraph(out, out_size, &n, gh);
        }
    }
    /* Dynamic prompt tags (API_PROMPT_TAG_*): system_time → persona | default → instructions */
    if (ctx->prompt_tag_slots[0] && ctx->prompt_tag_slots[0][0]) {
        const char *v = ctx->prompt_tag_slots[0];
        size_t rem = out_size > n ? out_size - n : 0;
        if (rem > 1) {
            int w;
            if (strncmp(v, "[SYSTEM_TIME:", 14) == 0) {
                w = snprintf(out + n, rem, "%s", v);
                if (w > 0 && (size_t)w < rem) n += (size_t)w;
                else if (w > 0) n = out_size - 1;
                ensure_double_newline_suffix(out, out_size, &n);
            } else {
                w = snprintf(out + n, rem, "[SYSTEM_TIME: %s]\n\n%s", v, API_SYSTEM_TIME_INSTRUCTION);
                if (w > 0 && (size_t)w < rem) n += (size_t)w;
                else if (w > 0) n = out_size - 1;
            }
        }
    } else if (!ctx->disable_auto_system_time) {
        append_system_time_wall_clock(out, out_size, &n);
    }
    if (ctx->prompt_tag_slots[1] && ctx->prompt_tag_slots[1][0])
        append_block_paragraph(out, out_size, &n, ctx->prompt_tag_slots[1]);
    else if (n + sizeof(API_CTX_SYSTEM_GUARD) - 1 < out_size) {
        memcpy(out + n, API_CTX_SYSTEM_GUARD, sizeof(API_CTX_SYSTEM_GUARD) - 1);
        n += sizeof(API_CTX_SYSTEM_GUARD) - 1;
    }
    if (ctx->prompt_tag_slots[2] && ctx->prompt_tag_slots[2][0])
        append_block_paragraph(out, out_size, &n, ctx->prompt_tag_slots[2]);
    /* Rule strip-5: if batch > 5, "Earlier" = user-only inputs; last 5 = full input/assistant. Else all full. */
    int start = ctx->ctx_count > API_CTX_STRIP_5 ? ctx->ctx_count - API_CTX_STRIP_5 : 0;
    int num = ctx->ctx_count - start;
    if (ctx->ctx_count > API_CTX_STRIP_5 && (out_size - n) > 32) {
        size_t earlier_start = n;
        n += (size_t)snprintf(out + n, out_size - n, "Earlier (user only): ");
        for (int i = 0; i < start && n < earlier_start + API_CTX_EARLIER_MAX && n < out_size - 4; i++) {
            int idx = (ctx->ctx_head + i) % ctx->ctx_capacity;
            const char *r = ctx->ctx_roles + idx * API_CTX_ROLE_SIZE;
            if (strcmp(r, "user") != 0) continue;
            const char *m = ctx->ctx_messages + idx * API_CTX_MSG_SIZE;
            size_t mlen = strnlen(m, API_CTX_MSG_SIZE - 1);
            if (mlen > out_size - n - 4) mlen = out_size - n - 4;
            if (n > (size_t)(earlier_start + 21)) { memcpy(out + n, " | ", 3); n += 3; }
            if (mlen > 0) { memcpy(out + n, m, mlen); n += mlen; }
        }
        if (n < out_size) { out[n++] = '\n'; out[n++] = '\n'; }
    }
    for (int i = 0; i < num && n < out_size - 64; i++) {
        int idx = (ctx->ctx_head + start + i) % ctx->ctx_capacity;
        const char *r = ctx->ctx_roles + idx * API_CTX_ROLE_SIZE;
        const char *m = ctx->ctx_messages + idx * API_CTX_MSG_SIZE;
        const char *label = (strcmp(r, "user") == 0) ? "User: " : "Assistant: ";
        size_t label_len = strlen(label);
        if (n + label_len >= out_size) break;
        memcpy(out + n, label, label_len);
        n += label_len;
        size_t mlen = strnlen(m, API_CTX_MSG_SIZE - 1);
        if (mlen > out_size - n - 2) mlen = out_size - n - 2;
        if (mlen > 0) {
            memcpy(out + n, m, mlen);
            n += mlen;
        }
        if (n < out_size) { out[n++] = '\n'; }
    }
    if (n < out_size) out[n] = '\0';
    else out[out_size - 1] = '\0';
    return n;
}

/* Epoch milliseconds string for storage and ordering; display converts in UI. */
/** After assistant text is final: conflict check + optional correction note + geo audit. Returns has_logic_conflict for storage. */
static int run_geo_authority_post_chat(api_context_t *ctx, const char *user_msg, char *reply, size_t reply_cap) {
    if (!ctx || !reply || reply_cap == 0) return 0;
    const engine_config_t *cfg = engine_get_config(ctx->engine);
    if (!cfg || !cfg->geo_authority_enabled) return 0;
    conflict_result_t cr;
    memset(&cr, 0, sizeof(cr));
    (void)conflict_detector_analyze(user_msg, reply, &cr);
    if (cr.has_logic_conflict && cr.correction_note[0]) {
        size_t L = strlen(reply);
        size_t rem = reply_cap > L ? reply_cap - L - 1 : 0;
        if (rem > 1)
            strncat(reply, cr.correction_note, rem);
    }
    geo_authority_audit_response_text(reply);
    return cr.has_logic_conflict ? 1 : 0;
}

static void epoch_ms_string(char *buf, size_t size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + (int64_t)(tv.tv_usec / 1000);
    (void)snprintf(buf, size, "%lld", (long long)ms);
}

static void fill_default_config(execution_mode_t mode, const api_options_t *opts,
                                engine_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->mode = MODE_HYBRID;
    config->execution_mode = mode;
    config->batch_size = 100;
    config->vector_search_enabled = (mode == MODE_MONGO_REDIS || mode == MODE_MONGO_REDIS_ELK) ? 1 : 0;
    config->debug_mode = true;
    config->smart_topic_opts = (opts && opts->smart_topic_opts) ? opts->smart_topic_opts : NULL;
    config->model_switch_opts = (opts && opts->model_switch_opts) ? opts->model_switch_opts : NULL;
    config->geo_learning_enabled = (mode == MODE_MONGO_REDIS || mode == MODE_MONGO_REDIS_ELK);
    config->geo_authority_enabled = (opts && opts->geo_authority != 0);
    config->vector_gen_backend = API_VECTOR_GEN_CUSTOM;
    if (opts && opts->vector_gen_backend == API_VECTOR_GEN_OLLAMA)
        config->vector_gen_backend = API_VECTOR_GEN_OLLAMA;
    config->vector_ollama_model =
        (opts && opts->vector_ollama_model && opts->vector_ollama_model[0]) ? opts->vector_ollama_model : NULL;
    config->embed_migration_autostart = (opts && opts->embed_migration_autostart != 0);

    switch (mode) {
        case MODE_ONLY_MEMORY:
            config->mongo_uri = NULL;
            config->redis_host = NULL;
            config->redis_port = 0;
            config->es_host = NULL;
            config->es_port = 0;
            break;
        case MODE_ONLY_MONGO:
            config->mongo_uri = (opts && opts->mongo_uri && opts->mongo_uri[0]) ? opts->mongo_uri : DEFAULT_MONGO_URI;
            config->redis_host = NULL;
            config->redis_port = 0;
            config->es_host = NULL;
            config->es_port = 0;
            break;
        case MODE_MONGO_REDIS:
            config->mongo_uri = (opts && opts->mongo_uri && opts->mongo_uri[0]) ? opts->mongo_uri : DEFAULT_MONGO_URI;
            config->redis_host = (opts && opts->redis_host && opts->redis_host[0]) ? opts->redis_host : DEFAULT_REDIS_HOST;
            config->redis_port = (opts && opts->redis_port != 0) ? opts->redis_port : DEFAULT_REDIS_PORT;
            config->es_host = NULL;
            config->es_port = 0;
            break;
        case MODE_MONGO_REDIS_ELK:
            config->mongo_uri = (opts && opts->mongo_uri && opts->mongo_uri[0]) ? opts->mongo_uri : DEFAULT_MONGO_URI;
            config->redis_host = (opts && opts->redis_host && opts->redis_host[0]) ? opts->redis_host : DEFAULT_REDIS_HOST;
            config->redis_port = (opts && opts->redis_port != 0) ? opts->redis_port : DEFAULT_REDIS_PORT;
            config->es_host = (opts && opts->es_host && opts->es_host[0]) ? opts->es_host : "";
            config->es_port = (opts && opts->es_port != 0) ? opts->es_port : DEFAULT_ES_PORT;
            break;
    }
}

api_context_t *api_create(const api_options_t *opts) {
    int mode_int = (opts && opts->mode >= 0 && opts->mode <= 3) ? opts->mode : M4ENGINE_MODE_ONLY_MONGO;
    execution_mode_t mode = api_mode_to_engine(mode_int);
    engine_config_t config;
    fill_default_config(mode, opts, &config);

    engine_t *engine = engine_create(&config);
    if (!engine) return NULL;

    storage_ctx_t *storage = engine_get_storage(engine);
    if (opts && opts->log_db && opts->log_db[0] && opts->log_coll && opts->log_coll[0]) {
        if (storage_set_ai_logs(storage, opts->log_db, opts->log_coll) != 0) {
            engine_destroy(engine);
            return NULL;
        }
    }

    stat_ctx_t *stat = stat_create();
    if (!stat) {
        engine_destroy(engine);
        return NULL;
    }
    stat_set_execution_mode(stat, mode);

    if (engine_init(engine) != 0) {
        stat_destroy(stat);
        engine_destroy(engine);
        return NULL;
    }
    stat_set_mongo_connected(stat, storage_mongo_connected(storage));
    stat_set_redis_connected(stat, storage_redis_connected(storage));
    stat_set_elk_enabled(stat, (config.es_host && config.es_host[0]) ? 1 : 0);

    api_context_t *ctx = (api_context_t *)malloc(sizeof(api_context_t));
    if (!ctx) {
        stat_destroy(stat);
        engine_destroy(engine);
        return NULL;
    }
    ctx->engine = engine;
    ctx->stat = stat;
    ctx->context_batch_size = (opts && opts->context_batch_size > 0)
        ? opts->context_batch_size
        : API_CONTEXT_BATCH_SIZE_DEFAULT;
    ctx->ctx_capacity = ctx->context_batch_size;
    if (ctx->ctx_capacity > API_CTX_CAPACITY_MAX) ctx->ctx_capacity = API_CTX_CAPACITY_MAX;
    if (ctx->ctx_capacity < 1) ctx->ctx_capacity = 1;
    ctx->ctx_head = 0;
    ctx->ctx_count = 0;
    ctx->ctx_roles = (char *)malloc((size_t)ctx->ctx_capacity * API_CTX_ROLE_SIZE);
    ctx->ctx_messages = (char *)malloc((size_t)ctx->ctx_capacity * API_CTX_MSG_SIZE);
    ctx->ctx_sources = (char *)malloc((size_t)ctx->ctx_capacity);
    ctx->ctx_timestamps = (char *)malloc((size_t)ctx->ctx_capacity * API_CTX_TS_SIZE);
    if (!ctx->ctx_roles || !ctx->ctx_messages || !ctx->ctx_sources || !ctx->ctx_timestamps) {
        free(ctx->ctx_roles);
        free(ctx->ctx_messages);
        free(ctx->ctx_sources);
        free(ctx->ctx_timestamps);
        stat_destroy(stat);
        engine_destroy(engine);
        free(ctx);
        return NULL;
    }
    memset(ctx->ctx_roles, 0, (size_t)ctx->ctx_capacity * API_CTX_ROLE_SIZE);
    memset(ctx->ctx_messages, 0, (size_t)ctx->ctx_capacity * API_CTX_MSG_SIZE);
    memset(ctx->ctx_sources, 0, (size_t)ctx->ctx_capacity);
    memset(ctx->ctx_timestamps, 0, (size_t)ctx->ctx_capacity * API_CTX_TS_SIZE);
    ctx->last_reply_source = 0;
    ctx->inject_geo_knowledge = (opts && opts->inject_geo_knowledge) ? 1 : 0;
    ctx->disable_auto_system_time = (opts && opts->disable_auto_system_time) ? 1 : 0;
    ctx->model_lane_key[0] = '\0';
    memset(ctx->prompt_tag_slots, 0, sizeof(ctx->prompt_tag_slots));
    return ctx;
}

int api_set_model_lane(api_context_t *ctx, int lane) {
    if (!ctx) return -1;
    const char *k = NULL;
    switch (lane) {
        case M4_API_MODEL_LANE_DEFAULT:   k = "DEFAULT"; break;
        case M4_API_MODEL_LANE_EDUCATION: k = "EDUCATION"; break;
        case M4_API_MODEL_LANE_BUSINESS:  k = "BUSINESS"; break;
        case M4_API_MODEL_LANE_TECH:      k = "TECH"; break;
        case M4_API_MODEL_LANE_CHAT:      k = "CHAT"; break;
        default: return -1;
    }
    size_t ln = strlen(k);
    if (ln >= sizeof(ctx->model_lane_key)) return -1;
    memcpy(ctx->model_lane_key, k, ln + 1);
    return 0;
}

int api_set_model_lane_key(api_context_t *ctx, const char *lane_key) {
    if (!ctx) return -1;
    if (!lane_key || !lane_key[0]) {
        ctx->model_lane_key[0] = '\0';
        return 0;
    }
    size_t n = strlen(lane_key);
    if (n >= sizeof(ctx->model_lane_key)) return -1;
    memcpy(ctx->model_lane_key, lane_key, n + 1);
    return 0;
}

int api_set_prompt_tag(api_context_t *ctx, const char *key, const char *value) {
    if (!ctx || !key) return -1;
    int slot = prompt_tag_slot_for_key(key);
    if (slot < 0) return -1;
    free(ctx->prompt_tag_slots[slot]);
    ctx->prompt_tag_slots[slot] = NULL;
    if (!value || !value[0]) return 0;
    size_t len = strlen(value);
    if (len > (size_t)API_PROMPT_TAG_VALUE_MAX) return -1;
    ctx->prompt_tag_slots[slot] = strdup(value);
    return ctx->prompt_tag_slots[slot] ? 0 : -1;
}

void api_clear_prompt_tags(api_context_t *ctx) {
    prompt_tags_free(ctx);
}

void api_destroy(api_context_t *ctx) {
    if (!ctx) return;
    prompt_tags_free(ctx);
    free(ctx->ctx_roles);
    free(ctx->ctx_messages);
    free(ctx->ctx_sources);
    free(ctx->ctx_timestamps);
    stat_destroy(ctx->stat);
    engine_destroy(ctx->engine);
    free(ctx);
}

static char *api_gen_temp_message_id(void) {
    unsigned char b[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, 16, f) != 16) {
        if (f) fclose(f);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        for (int i = 0; i < 16; i++)
            b[i] = (unsigned char)((tv.tv_usec ^ (unsigned)(i * 131) ^ tv.tv_sec) & 0xff);
    } else
        fclose(f);
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    char buf[40];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return strdup(buf);
}

/** User-message embedding — see **`m4_embed_for_engine`** / **`include/embed.h`**. */
static int api_user_message_embedding(engine_t *eng, const char *msg, float *embed_vec, size_t max_dim,
                                      size_t *embed_dim, char *embed_model, size_t embed_model_sz) {
    if (!eng || !embed_vec || !embed_dim || max_dim == 0 || !embed_model || embed_model_sz == 0)
        return -1;
    return m4_embed_for_engine(eng, msg, embed_vec, max_dim, embed_dim, embed_model, embed_model_sz);
}

/** After ctx_build_prompt + RAG prepend: smart_topic classify, model_switch resolve, optional inject. */
static void api_apply_model_switch(api_context_t *ctx, const char *user_msg,
                                   char *context_buf, size_t ctx_sz,
                                   char *ollama_model, size_t ollama_model_sz,
                                   double *temperature_io) {
    smart_topic_intent_t st = SMART_TOPIC_INTENT_DEFAULT;
    double temp = -1.0;
    char st_buf[64];
    if (get_smart_topic(st_buf, sizeof(st_buf)) == 0 && strstr(st_buf, "disabled") == NULL)
        smart_topic_classify_for_query(user_msg, &temp, &st);
    if (temperature_io)
        *temperature_io = temp;

    const engine_config_t *cfg = engine_get_config(ctx->engine);
    model_switch_profile_t prof;
    model_switch_resolve(cfg ? cfg->model_switch_opts : NULL, ctx->model_lane_key, st, &prof);

    if (ollama_model && ollama_model_sz > 0) {
        size_t cpy = strlen(prof.model);
        if (cpy >= ollama_model_sz) cpy = ollama_model_sz - 1;
        memcpy(ollama_model, prof.model, cpy);
        ollama_model[cpy] = '\0';
    }
    if (prof.inject[0] && context_buf && ctx_sz > 1) {
        char tmp[API_CONTEXT_BUFFER_SIZE];
        int n = snprintf(tmp, sizeof(tmp), "[Lane context]\n%s\n\n", prof.inject);
        if (n > 0 && (size_t)n < sizeof(tmp)) {
            size_t prefix = (size_t)n;
            size_t body = strnlen(context_buf, ctx_sz - 1);
            if (prefix + body + 1 < sizeof(tmp) && prefix + body + 1 <= ctx_sz) {
                memcpy(tmp + prefix, context_buf, body + 1);
                memcpy(context_buf, tmp, prefix + body + 1);
            }
        }
    }
}

/* 0 = Ollama path (context_buf filled), 1 = Redis hit (redis_reply filled), -1 = error */
static int api_chat_prepare_for_stream(api_context_t *ctx, const char *tid, const char *msg,
                                       char *user_ts_out, size_t user_ts_sz,
                                       char *context_buf, size_t ctx_sz,
                                       char *redis_reply, size_t redis_sz,
                                       char *ollama_model, size_t ollama_model_sz,
                                       double *temperature_out) {
    if (!ctx || !user_ts_out || user_ts_sz == 0 || !context_buf || ctx_sz == 0
        || !redis_reply || redis_sz == 0)
        return -1;

    epoch_ms_string(user_ts_out, user_ts_sz);
    ctx_push_message_with_source(ctx, "user", msg, API_SOURCE_MEMORY, user_ts_out);

    rag_accum_t rag = { {0}, 0, {0}, 0, 0.0 };
    if (engine_vector_search_enabled(ctx->engine)) {
        storage_ctx_t *st = engine_get_storage(ctx->engine);
        if (st && storage_redis_connected(st)) {
            float embed_vec[OLLAMA_EMBED_MAX_DIM];
            size_t embed_dim = 0;
            char embed_model_rag_stream[128];
            if (api_user_message_embedding(ctx->engine, msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                           embed_model_rag_stream, sizeof(embed_model_rag_stream)) == 0
                && embed_dim > 0) {
                storage_rag_search(st, tid, "default", embed_vec, embed_dim, 5, 0.0, rag_accum_cb, &rag);
                if (rag.first_len > 0 && rag.best_score >= API_RAG_REPLY_MIN_SCORE) {
                    const char *reply = rag.first_snippet;
                    size_t reply_len = rag.first_len;
                    const char *nl = memchr(rag.first_snippet, '\n', rag.first_len);
                    if (nl) {
                        reply = nl + 1;
                        reply_len = rag.first_len - (size_t)(nl + 1 - rag.first_snippet);
                    }
                    size_t copy_len = reply_len;
                    if (copy_len >= redis_sz) copy_len = redis_sz - 1;
                    memcpy(redis_reply, reply, copy_len);
                    redis_reply[copy_len] = '\0';
                    return 1;
                }
            }
        }
    }

    memset(context_buf, 0, ctx_sz);
    ctx_build_prompt(ctx, msg, context_buf, ctx_sz);
    if (rag.used > 0) {
        const char *header = "Context from past turns:\n";
        size_t hlen = strlen(header);
        size_t total_prepend = hlen + rag.used + 2;
        size_t existing = strnlen(context_buf, ctx_sz - 1);
        if (total_prepend + existing < ctx_sz) {
            memmove(context_buf + total_prepend, context_buf, existing + 1);
            memcpy(context_buf, header, hlen);
            memcpy(context_buf + hlen, rag.buf, rag.used);
            context_buf[hlen + rag.used] = '\n';
            context_buf[hlen + rag.used + 1] = '\n';
        }
    }
    if (temperature_out)
        *temperature_out = -1.0;
    if (ollama_model && ollama_model_sz)
        ollama_model[0] = '\0';
    api_apply_model_switch(ctx, msg, context_buf, ctx_sz, ollama_model, ollama_model_sz, temperature_out);
    return 0;
}

typedef struct {
    api_context_t *ctx;
    char *tid;
    char *uid;
    char *user_msg;
    char *msg_id;
    char user_ts[API_CTX_TS_SIZE];
    char prompt[API_CONTEXT_BUFFER_SIZE];
    char ollama_model[MODEL_SWITCH_MODEL_MAX];
    double temperature;
    api_stream_token_cb cb;
    void *cb_ud;
    size_t dbg_tok_count;
    size_t dbg_tok_bytes;
} stream_work_t;

static void stream_forward_token(const char *token, void *userdata) {
    stream_work_t *w = (stream_work_t *)userdata;
    if (!w || !w->cb || !token || !token[0]) return;
    w->dbg_tok_count++;
    w->dbg_tok_bytes += strlen(token);
    if (api_debug_chat_tokens()) {
        size_t tl = strlen(token);
        size_t show = tl < 160 ? tl : 160;
        fprintf(stderr, "[API][stream] token #%zu +%zuB: %.*s%s\n", w->dbg_tok_count, tl, (int)show, token,
                tl > show ? "..." : "");
    }
    /* Per-token guard is deferred: full-response audit runs after stream completes. */
    w->cb(token, w->msg_id, 0, w->cb_ud);
}

static void *api_chat_stream_worker(void *arg) {
    stream_work_t *w = (stream_work_t *)arg;
    char full[OL_BUF_SIZE];
    full[0] = '\0';

    if (api_debug_chat_on()) {
        api_debug_storage(w->ctx, "stream_worker");
        fprintf(stderr,
                "[API][stream] temp_message_id=%s tenant=%s user=%s model=%s temperature=%.4g verbose_ndjson=%d "
                "log_each_token=%d\n",
                w->msg_id ? w->msg_id : "?", w->tid ? w->tid : "?", w->uid ? w->uid : "?",
                (w->ollama_model[0] != '\0') ? w->ollama_model : "(default/env)", w->temperature,
                api_debug_chat_verbose() ? 1 : 0, api_debug_chat_tokens() ? 1 : 0);
        api_debug_utf8_preview("[stream] user_input", w->user_msg, 500);
        api_debug_utf8_preview("[stream] prompt_ptomp", w->prompt, 800);
    }

    const char *om = (w->ollama_model[0] != '\0') ? w->ollama_model : NULL;
    int orv = ollama_query_stream(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, om, w->prompt,
                                  w->temperature, stream_forward_token, w, full, sizeof(full));
    /* libcurl can report failure after bytes arrived (reset, timeout). If we already streamed
     * text into `full`, still persist to ctx + Mongo like a successful turn. */
    if (orv != 0 && (!full[0])) {
        if (w->cb) w->cb("", w->msg_id, 1, w->cb_ud);
        free(w->msg_id);
        free(w->tid);
        free(w->uid);
        free(w->user_msg);
        free(w);
        return (void *)(intptr_t)-1;
    }
    if (orv != 0 && full[0])
        fprintf(stderr, "[API] ollama_query_stream error but partial reply captured; persisting turn.\n");

    if (!full[0]) {
        fprintf(stderr,
                "[API] chat stream: empty assistant (Ollama error line, missing/wrong model, or unparsed NDJSON); "
                "not persisting turn.\n");
        if (w->cb) w->cb("", w->msg_id, 1, w->cb_ud);
        free(w->msg_id);
        free(w->tid);
        free(w->uid);
        free(w->user_msg);
        free(w);
        return (void *)(intptr_t)-1;
    }

    if (api_debug_chat_on()) {
        fprintf(stderr,
                "[API][stream] pump_done: ollama_query_stream_rc=%d token_callbacks=%zu token_bytes=%zu "
                "assembled_full_len=%zu\n",
                orv, w->dbg_tok_count, w->dbg_tok_bytes, strlen(full));
        api_debug_utf8_preview("[stream] assembled_full", full, 1200);
    }

    int logic_conflict = run_geo_authority_post_chat(w->ctx, w->user_msg, full, sizeof(full));
    char bot_ts[24];
    epoch_ms_string(bot_ts, sizeof(bot_ts));
    ctx_push_message_with_source(w->ctx, "assistant", full, API_SOURCE_OLLAMA, bot_ts);

    {
        float embed_vec[OLLAMA_EMBED_MAX_DIM];
        size_t embed_dim = 0;
        char lang_buf[16];
        double lang_score = 0.0;
        char embed_model[128];
        (void)api_user_message_embedding(w->ctx->engine, w->user_msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                         embed_model, sizeof(embed_model));
        (void)lang_detect(w->user_msg, lang_buf, sizeof(lang_buf), &lang_score);
        {
            int ar = engine_append_turn(w->ctx->engine, w->tid, w->uid, w->user_msg, full, w->user_ts,
                                        (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                        lang_buf[0] ? lang_buf : NULL, lang_score, embed_model, w->msg_id,
                                        logic_conflict);
            if (ar != 0)
                fprintf(stderr,
                        "[API] engine_append_turn failed rc=%d (MEMORY mode, storage offline, or Mongo error). "
                        "tenant=%s user=%s\n",
                        ar, w->tid ? w->tid : "?", w->uid ? w->uid : "?");
            else if (api_debug_chat_on())
                fprintf(stderr,
                        "[API][stream] engine_append_turn rc=0 (turn persisted; mongoc_linked/mongo_connected "
                        "are in the storage lines above)\n");
        }
    }
    engine_inc_processed(w->ctx->engine, 1);

    if (w->cb) w->cb("", w->msg_id, 1, w->cb_ud);
    free(w->msg_id);
    free(w->tid);
    free(w->uid);
    free(w->user_msg);
    free(w);
    return (void *)(intptr_t)0;
}

int api_chat_stream(api_context_t *ctx,
                    const char *tenant_id,
                    const char *user_id,
                    const char *user_message,
                    const char *temp_message_id,
                    api_stream_token_cb cb,
                    void *userdata) {
    if (!ctx || !cb) return -1;
    const char *tid0 = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *uid0 = (user_id && user_id[0]) ? user_id : "default";
    const char *msg = user_message ? user_message : "";

    char *msg_id = NULL;
    if (temp_message_id && temp_message_id[0])
        msg_id = strdup(temp_message_id);
    else
        msg_id = api_gen_temp_message_id();
    if (!msg_id) return -1;

    char user_ts[API_CTX_TS_SIZE];
    char context_buf[API_CONTEXT_BUFFER_SIZE];
    char redis_reply[API_CTX_MSG_SIZE];
    double stream_temp = -1.0;
    char stream_model[MODEL_SWITCH_MODEL_MAX];
    stream_model[0] = '\0';
    int prep = api_chat_prepare_for_stream(ctx, tid0, msg, user_ts, sizeof(user_ts),
                                           context_buf, sizeof(context_buf),
                                           redis_reply, sizeof(redis_reply),
                                           stream_model, sizeof(stream_model),
                                           &stream_temp);
    if (prep < 0) {
        free(msg_id);
        return -1;
    }

    if (prep == 1) {
        if (api_debug_chat_on()) {
            api_debug_storage(ctx, "stream_redis_rag_hit");
            fprintf(stderr, "[API][stream] path=redis_rag temp_message_id=%s tenant=%s user=%s\n", msg_id, tid0,
                    uid0);
            api_debug_utf8_preview("[stream] user_input", msg, 500);
            api_debug_utf8_preview("[stream] redis_reply_cached", redis_reply, 800);
        }
        int logic_conflict = run_geo_authority_post_chat(ctx, msg, redis_reply, sizeof(redis_reply));
        char bot_ts[24];
        epoch_ms_string(bot_ts, sizeof(bot_ts));
        ctx_push_message_with_source(ctx, "assistant", redis_reply, API_SOURCE_REDIS, bot_ts);
        {
            float embed_vec[OLLAMA_EMBED_MAX_DIM];
            size_t embed_dim = 0;
            char lang_buf[16];
            double lang_score = 0.0;
            char embed_model[128];
            (void)api_user_message_embedding(ctx->engine, msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                             embed_model, sizeof(embed_model));
            (void)lang_detect(msg, lang_buf, sizeof(lang_buf), &lang_score);
            {
                int ar = engine_append_turn(ctx->engine, tid0, uid0, msg, redis_reply, user_ts,
                                            (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                            lang_buf[0] ? lang_buf : NULL, lang_score, embed_model, msg_id,
                                            logic_conflict);
                if (ar != 0)
                    fprintf(stderr,
                            "[API] engine_append_turn failed rc=%d (Redis cache hit path). tenant=%s\n", ar, tid0);
            }
        }
        engine_inc_processed(ctx->engine, 1);
        cb(redis_reply, msg_id, 0, userdata);
        cb("", msg_id, 1, userdata);
        free(msg_id);
        return 0;
    }

    stream_work_t *w = (stream_work_t *)calloc(1, sizeof(stream_work_t));
    if (!w) {
        free(msg_id);
        return -1;
    }
    w->ctx = ctx;
    w->tid = strdup(tid0);
    w->uid = strdup(uid0);
    w->user_msg = strdup(msg);
    w->msg_id = msg_id;
    memcpy(w->user_ts, user_ts, sizeof(w->user_ts));
    memcpy(w->prompt, context_buf, sizeof(w->prompt));
    memcpy(w->ollama_model, stream_model, sizeof(w->ollama_model));
    w->cb = cb;
    w->cb_ud = userdata;
    w->temperature = stream_temp;

    if (api_debug_chat_on()) {
        api_debug_storage(ctx, "stream_ollama_path");
        fprintf(stderr,
                "[API][stream] path=ollama temp_message_id=%s tenant=%s user=%s model=%s temperature=%.4g "
                "(user_input + ptomp prompt logged in worker)\n",
                msg_id, tid0, uid0, stream_model[0] ? stream_model : "(default)", stream_temp);
    }

    pthread_t th;
    if (pthread_create(&th, NULL, api_chat_stream_worker, w) != 0) {
        free(w->tid);
        free(w->uid);
        free(w->user_msg);
        free(w->msg_id);
        free(w);
        return -1;
    }
    void *rv = NULL;
    pthread_join(th, &rv);
    return (rv == (void *)(intptr_t)0) ? 0 : -1;
}

int api_chat(api_context_t *ctx, const char *tenant_id, const char *user_message,
             char *bot_reply_out, size_t out_size) {
    if (!ctx || !bot_reply_out || out_size == 0) return -1;
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *msg = user_message ? user_message : "";
    int logic_conflict = 0;

    char user_ts[24];
    epoch_ms_string(user_ts, sizeof(user_ts));

    /* Step 4: push user into circular buffer (source = MEMORY). */
    ctx_push_message_with_source(ctx, "user", msg, API_SOURCE_MEMORY, user_ts);

    /* Generate vector then Redis: if option enabled and Redis returns a high-score hit, return it and skip Ollama (check TTL when Redis impl has it). */
    rag_accum_t rag = { {0}, 0, {0}, 0, 0.0 };
    if (engine_vector_search_enabled(ctx->engine)) {
        storage_ctx_t *st = engine_get_storage(ctx->engine);
        if (st && storage_redis_connected(st)) {
            float embed_vec[OLLAMA_EMBED_MAX_DIM];
            size_t embed_dim = 0;
            char embed_model_rag2[128];
            if (api_user_message_embedding(ctx->engine, msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                           embed_model_rag2, sizeof(embed_model_rag2)) == 0
                && embed_dim > 0) {
                storage_rag_search(st, tid, "default", embed_vec, embed_dim, 5, 0.0, rag_accum_cb, &rag);
                /* If Redis found a record with score >= threshold, return it as chat and skip AI agent (Ollama). */
                if (rag.first_len > 0 && rag.best_score >= API_RAG_REPLY_MIN_SCORE) {
                    /* Payload from storage is "input\nassistant"; use only the assistant part for display. */
                    const char *reply = rag.first_snippet;
                    size_t reply_len = rag.first_len;
                    const char *nl = memchr(rag.first_snippet, '\n', rag.first_len);
                    if (nl) {
                        reply = nl + 1;
                        reply_len = rag.first_len - (size_t)(nl + 1 - rag.first_snippet);
                    }
                    size_t copy_len = reply_len;
                    if (copy_len >= out_size) copy_len = out_size - 1;
                    memcpy(bot_reply_out, reply, copy_len);
                    bot_reply_out[copy_len] = '\0';
                    logic_conflict = run_geo_authority_post_chat(ctx, msg, bot_reply_out, out_size);
                    char bot_ts[24];
                    epoch_ms_string(bot_ts, sizeof(bot_ts));
                    ctx_push_message_with_source(ctx, "assistant", bot_reply_out, API_SOURCE_REDIS, bot_ts);
                    /* Still append turn to Mongo (vector/lang from Phase 1 below) for consistency. */
                    goto append_turn;
                }
            }
        }
    }

    char context_buf[API_CONTEXT_BUFFER_SIZE];
    memset(context_buf, 0, sizeof(context_buf));
    ctx_build_prompt(ctx, msg, context_buf, sizeof(context_buf));
    if (rag.used > 0) {
        const char *header = "Context from past turns:\n";
        size_t hlen = strlen(header);
        size_t total_prepend = hlen + rag.used + 2; /* + "\n\n" */
        size_t existing = strnlen(context_buf, sizeof(context_buf) - 1);
        if (total_prepend + existing < sizeof(context_buf)) {
            memmove(context_buf + total_prepend, context_buf, existing + 1);
            memcpy(context_buf, header, hlen);
            memcpy(context_buf + hlen, rag.buf, rag.used);
            context_buf[hlen + rag.used] = '\n';
            context_buf[hlen + rag.used + 1] = '\n';
        }
    }

    double temperature = -1.0;
    char ollama_model[MODEL_SWITCH_MODEL_MAX];
    ollama_model[0] = '\0';
    api_apply_model_switch(ctx, msg, context_buf, sizeof(context_buf), ollama_model, sizeof(ollama_model), &temperature);
    const char *om = (ollama_model[0] != '\0') ? ollama_model : NULL;
    int ollama_ret = (temperature >= 0.0)
        ? ollama_query_with_options(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, om,
                                   context_buf, temperature, bot_reply_out, out_size)
        : ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, om,
                      context_buf, bot_reply_out, out_size);
    if (ollama_ret != 0) {
        return -1;
    }
    logic_conflict = run_geo_authority_post_chat(ctx, msg, bot_reply_out, out_size);
    char bot_ts[24];
    epoch_ms_string(bot_ts, sizeof(bot_ts));
    ctx_push_message_with_source(ctx, "assistant", bot_reply_out, API_SOURCE_OLLAMA, bot_ts);

append_turn:;
    /* Phase 1: vector (Ollama embed) + lang (LangDetector) before storage — .cursor/lang_vector_phase1.md */
    {
    float embed_vec[OLLAMA_EMBED_MAX_DIM];
    size_t embed_dim = 0;
    char lang_buf[16];
    double lang_score = 0.0;
    char embed_model[128];
    (void)api_user_message_embedding(ctx->engine, msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                     embed_model, sizeof(embed_model));
    (void)lang_detect(msg, lang_buf, sizeof(lang_buf), &lang_score);

    {
        int ar = engine_append_turn(ctx->engine, tid, "default", msg, bot_reply_out, user_ts,
                                    (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                    lang_buf[0] ? lang_buf : NULL, lang_score, embed_model, NULL,
                                    logic_conflict);
        if (ar != 0)
            fprintf(stderr,
                    "[API] engine_append_turn failed rc=%d — turn not in Mongo (MEMORY mode, no mongoc client, "
                    "or insert error). tenant=%s\n",
                    ar, tid);
    }
    }
    engine_inc_processed(ctx->engine, 1);
    return 0;
}

int api_set_log_collection(api_context_t *ctx, const char *db, const char *coll) {
    if (!ctx || !db || !coll) return -1;
    storage_ctx_t *storage = engine_get_storage(ctx->engine);
    return storage_set_ai_logs(storage, db, coll);
}

static void load_history_cb(const char *role, const char *content, const char *ts, void *userdata) {
    api_context_t *ctx = (api_context_t *)userdata;
    ctx_push_message_with_source(ctx, role, content, API_SOURCE_MONGODB, ts);
}

int api_load_chat_history(api_context_t *ctx, const char *tenant_id) {
    if (!ctx) return -1;
    storage_ctx_t *storage = engine_get_storage(ctx->engine);
    if (!storage_mongo_connected(storage)) return 0;
    ctx_clear(ctx);
    /* Prefer cache-before-Mongo: try Redis L1 first, then Mongo on miss (see .cursor/redis.md, FLOW_DIGRAM §7). */
    if (storage_redis_connected(storage))
        return storage_get_chat_history_cached(storage, tenant_id ? tenant_id : DEFAULT_TENANT_ID,
                                              ctx->context_batch_size, load_history_cb, ctx);
    return storage_get_chat_history(storage, tenant_id ? tenant_id : DEFAULT_TENANT_ID,
                                   ctx->context_batch_size, load_history_cb, ctx);
}

int api_get_history_count(api_context_t *ctx) {
    return ctx ? ctx->ctx_count : 0;
}

int api_get_history_message(api_context_t *ctx, int index,
                            char *role_buf, size_t role_size,
                            char *content_buf, size_t content_size,
                            char *source_out, char *ts_buf, size_t ts_size) {
    if (!ctx || index < 0 || index >= ctx->ctx_count) return -1;
    int idx = (ctx->ctx_head + index) % ctx->ctx_capacity;
    if (role_buf && role_size > 0) {
        size_t n = strnlen(ctx->ctx_roles + idx * API_CTX_ROLE_SIZE, API_CTX_ROLE_SIZE - 1);
        if (n >= role_size) n = role_size - 1;
        memcpy(role_buf, ctx->ctx_roles + idx * API_CTX_ROLE_SIZE, n);
        role_buf[n] = '\0';
    }
    if (content_buf && content_size > 0) {
        size_t n = strnlen(ctx->ctx_messages + idx * API_CTX_MSG_SIZE, API_CTX_MSG_SIZE - 1);
        if (n >= content_size) n = content_size - 1;
        memcpy(content_buf, ctx->ctx_messages + idx * API_CTX_MSG_SIZE, n);
        content_buf[n] = '\0';
    }
    if (source_out && ctx->ctx_sources) *source_out = ctx->ctx_sources[idx];
    if (ts_buf && ts_size > 0 && ctx->ctx_timestamps) {
        size_t n = strnlen(ctx->ctx_timestamps + idx * API_CTX_TS_SIZE, API_CTX_TS_SIZE - 1);
        if (n >= ts_size) n = ts_size - 1;
        memcpy(ts_buf, ctx->ctx_timestamps + idx * API_CTX_TS_SIZE, n);
        ts_buf[n] = '\0';
    }
    return 0;
}

char api_get_last_reply_source(api_context_t *ctx) {
    return ctx && ctx->last_reply_source ? ctx->last_reply_source : 0;
}

size_t api_get_geo_atlas_landmarks(api_context_t *ctx, char *out, size_t out_size) {
    if (!ctx || !out || out_size == 0) return 0;
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    if (!st) return 0;
    return storage_geo_atlas_get_landmarks_for_prompt(st, out, out_size);
}

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
                             double trust_score) {
    if (!ctx || !name || !name[0]) return -1;
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    if (!st) return -1;
    storage_geo_atlas_doc_t d;
    memset(&d, 0, sizeof(d));
    d.tenant_id = (tenant_id && tenant_id[0]) ? tenant_id : "default";
    d.name = name;
    d.name_normalized = (name_normalized && name_normalized[0]) ? name_normalized : name;
    d.district = district ? district : "";
    d.axis = axis ? axis : "";
    d.category = category ? category : "";
    d.city = city ? city : "";
    d.vector = vector;
    d.vector_dim = vector_dim;
    d.embed_model_id = embed_model_id && embed_model_id[0] ? embed_model_id : "";
    d.source = (source && source[0]) ? source : STORAGE_GEO_SOURCE_SEED;
    d.verification_status = (verification_status && verification_status[0]) ? verification_status
                                                                            : STORAGE_GEO_STATUS_VERIFIED;
    d.trust_score = trust_score;
    if (d.trust_score < 0.0) d.trust_score = 0.0;
    if (d.trust_score > 1.0) d.trust_score = 1.0;
    int ret = storage_geo_atlas_insert_doc(st, &d);
    if (ret == 0 && vector && vector_dim > 0 && storage_redis_connected(st)) {
        const char *tid = d.tenant_id;
        const char *nn = d.name_normalized ? d.name_normalized : name;
        char doc_id[192];
        (void)snprintf(doc_id, sizeof(doc_id), "geo_csv_%s_%lld", nn, (long long)(time(NULL) * 1000LL));
        (void)storage_geo_redis_index_landmark(st, tid, doc_id, vector, vector_dim, name);
    }
    return ret;
}

int api_geo_authority_load_csv(api_context_t *ctx, const char *csv_utf8) {
    if (!ctx || !csv_utf8) return -1;
    const engine_config_t *cfg = engine_get_config(ctx->engine);
    if (!cfg || !cfg->geo_authority_enabled) return -1;
    return geo_authority_load_buffer(csv_utf8);
}

int api_geo_atlas_migrate_legacy(api_context_t *ctx, unsigned long long *modified_out) {
    if (!ctx)
        return -1;
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    if (!st)
        return -1;
    return storage_geo_atlas_migrate_legacy(st, modified_out);
}

int api_embed_migration_enqueue(api_context_t *ctx, const char *tenant_id, unsigned flags) {
    if (!ctx || !ctx->engine || flags == 0) return -1;
    return engine_embed_migration_enqueue(ctx->engine, tenant_id, flags);
}

int api_query(api_context_t *ctx, const char *prompt, char *out, size_t out_size) {
    if (!ctx || !out || out_size == 0) return -1;
    const char *p = prompt ? prompt : "";
    double temperature = -1.0;
    char st_buf[64];
    if (get_smart_topic(st_buf, sizeof(st_buf)) == 0 && strstr(st_buf, "disabled") == NULL) {
        smart_topic_temperature_for_query(p, &temperature);
    }
    /* NULL model: use OLLAMA_MODEL env else OLLAMA_DEFAULT_MODEL */
    int r = (temperature >= 0.0)
        ? ollama_query_with_options(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, NULL,
                                    p, temperature, out, out_size)
        : ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, NULL,
                      p, out, out_size);
    if (r == 0) engine_inc_processed(ctx->engine, 1);
    return r;
}

void api_get_stats(api_context_t *ctx, api_stats_t *out) {
    if (!ctx || !out) return;
    storage_ctx_t *storage = engine_get_storage(ctx->engine);
    const engine_config_t *cfg = engine_get_config(ctx->engine);

    stat_set_mongo_connected(ctx->stat, storage_mongo_connected(storage));
    stat_set_redis_connected(ctx->stat, storage_redis_connected(storage));

    int elk_on = (cfg && cfg->es_host && cfg->es_host[0]) ? 1 : 0;
    stat_set_elk_enabled(ctx->stat, elk_on);
    if (elk_on)
        stat_set_elk_connected(ctx->stat, elasticsearch_check_reachable(cfg->es_host, cfg->es_port) == 0 ? 1 : 0);
    else
        stat_set_elk_connected(ctx->stat, 0);

    uint64_t processed = 0, errors = 0;
    engine_get_stats(ctx->engine, &processed, &errors);
    stat_set_processed(ctx->stat, processed);
    stat_set_errors(ctx->stat, errors);

    /* Rough in-process context buffer footprint (was always 0 before). */
    uint64_t mem_est = (uint64_t)ctx->ctx_capacity *
                       (uint64_t)(API_CTX_MSG_SIZE + API_CTX_ROLE_SIZE + API_CTX_TS_SIZE + 4u);
    stat_set_memory_bytes(ctx->stat, mem_est);

    stat_snapshot_t snap;
    stat_get_snapshot(ctx->stat, &snap);
    out->memory_bytes = snap.memory_bytes;
    out->mongo_connected = snap.mongo_connected;
    out->redis_connected = snap.redis_connected;
    out->elk_enabled = snap.elk_enabled;
    out->elk_connected = snap.elk_connected;
    out->ollama_connected = (ollama_check_running(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT) == 0) ? 1 : 0;
    out->error_count = snap.error_count;
    out->warning_count = snap.warning_count;
    out->processed = snap.processed;
    out->errors = snap.errors;
#ifdef USE_MONGOC
    out->mongoc_linked = 1;
#else
    out->mongoc_linked = 0;
#endif
}
