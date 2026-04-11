#include "api.h"
#include "nl_learn_terms.h"
#include "nl_learn_cues.h"
#include "intent_route.h"
#include "intent_learn.h"
#include "engine.h"
#include "tenant.h"
#include "utils.h"
#include "conflict_detector.h"
#include "geo_authority.h"
#include "model_switch.h"
#include "smart_topic.h"
#include "storage.h"
#include "stat.h"
#include "validate.h"
#include "ollama.h"
#include "ai_agent.h"

#include "embed.h"
#include "lang.h"
#include "debug_log.h"
#include "json_opts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>

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
#define API_CTX_LLM_SIZE 160             /* completion label per slot (e.g. groq:llama-3.1-8b-instant); see metadata.llm_model_id */
#define API_RAG_PREFIX_MAX 4096         /* RAG context prepended to prompt when Redis L2 has hits */
#define API_PROMPT_TAG_SLOTS 3          /* system_time, persona, instructions — keys in api.h */
#define API_SESSION_UID_TENANT_WIDE "__tenant_wide__"
#define API_SESSION_KEY_MAX 160

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

typedef struct api_chat_session {
    int ctx_head;
    int ctx_count;
    int ctx_capacity;
    char *ctx_roles;
    char *ctx_messages;
    char *ctx_sources;
    char *ctx_timestamps;
    char *ctx_llm; /* per-slot completion label; parallel to ctx_messages */
    char last_reply_source;
    unsigned last_chat_wire; /* API_CHAT_WIRE_*; 0 = none / not set this turn */
    char last_llm_model[API_CTX_LLM_SIZE];
    time_t last_activity;
} api_chat_session_t;

struct api_context {
    engine_t *engine;
    stat_ctx_t *stat;
    int context_batch_size;
    int ring_capacity;
    m4_ht_t *sessions;
    pthread_rwlock_t sessions_lock; /* protects sessions hash table + last_session_key */
    atomic_int shutting_down;       /* set by api_destroy to prevent use-after-free */
    int session_idle_sec;
    char last_session_key[API_SESSION_KEY_MAX];
    int last_session_valid;
    int inject_geo_knowledge;
    char *prompt_tag_slots[API_PROMPT_TAG_SLOTS];
    int disable_auto_system_time;
    char model_lane_key[MODEL_SWITCH_LANE_KEY_MAX];
    struct nl_learn_terms *nl_learn;
    const sc_term_vocab_t *nl_learn_vocab; /* from SharedCollection registry; NULL if no registry */
    /** Set when `learning_terms_path` was non-empty (sync or defer). */
    int nl_learn_mx_inited;
    pthread_mutex_t nl_learn_mx;
    pthread_t nl_learn_load_tid;
    int nl_learn_load_started;
    atomic_int nl_learn_async_abandon;
    char *nl_learn_async_path;
    int nl_learn_async_enable_write;
    /** Mirrors `enable_learning_terms` when `learning_terms_path` set; internal cue recording uses this. */
    int nl_learn_writes_enabled;

    /* Geo import batch queue */
    struct {
        storage_geo_atlas_doc_t *items;   /* heap array of queued docs */
        char **string_bufs;               /* heap: one strdup'd block per item for all string fields */
        float **vector_bufs;              /* heap: copied vectors */
        int count;
        int cap;
        int batch_size;                   /* flush when count reaches this (default 100) */
        int flush_timeout_sec;            /* flush if idle for this many seconds (default 5) */
        time_t last_push;                 /* timestamp of last push */
        pthread_mutex_t lock;
        pthread_cond_t cond;
        pthread_t thread;
        int thread_started;
        atomic_int stop;
    } geo_batch;

    /* Background health check cache */
    struct {
        int ollama_connected;
        int elk_connected;
        time_t last_check;
        int interval_sec;           /* default 10s */
        pthread_t thread;
        int thread_started;
        atomic_int stop;
    } health;
};

/* Forward declarations */
static int api_get_history_message(api_context_t *ctx, int index,
                            char *role_buf, size_t role_size,
                            char *content_buf, size_t content_size,
                            char *source_out, char *ts_buf, size_t ts_size,
                            char *llm_model_out, size_t llm_model_cap);

/* Forward declarations for geo batch import */
static void geo_batch_init(api_context_t *ctx);
static void geo_batch_destroy(api_context_t *ctx);

/* Forward declarations for background health check */
static void health_check_init(api_context_t *ctx);
static void health_check_destroy(api_context_t *ctx);

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

/* stderr: options as received by c-lib (string lengths only, no secrets). Unset = on; empty = off. */
static int api_log_api_create_opts_enabled(void) {
    const char *e = getenv("M4_LOG_API_CREATE_OPTS");
    if (!e) return 1;
    if (!e[0]) return 0;
    if (e[0] == '0' && e[1] == '\0') return 0;
    if (strcmp(e, "false") == 0 || strcmp(e, "no") == 0 || strcmp(e, "off") == 0) return 0;
    return 1;
}

/* stderr: api_chat argument capacities. Unset = on; empty = off. */
static int api_log_api_chat_enabled(void) {
    const char *e = getenv("M4_LOG_API_CHAT");
    if (!e) return 1;
    if (!e[0]) return 0;
    if (e[0] == '0' && e[1] == '\0') return 0;
    if (strcmp(e, "false") == 0 || strcmp(e, "no") == 0 || strcmp(e, "off") == 0) return 0;
    return 1;
}

static size_t api_opts_cstr_len(const char *s) {
    return (s && s[0]) ? strlen(s) : (size_t)0;
}

/**
 * Sync + stream chat entry points share this line so hosts can `grep '[API] api_chat'`.
 * kind: sync | stream | stream_prepared. buf_or_prompt_cap: reply buffer size (sync) or prompt strlen (stream_prepared); 0 for stream.
 */
static void api_log_api_chat_turn_caps(const char *kind,
                                       api_context_t *ctx,
                                       const char *tid,
                                       const char *uid,
                                       const char *msg,
                                       size_t buf_or_prompt_cap,
                                       const char *temp_message_id) {
    if (!api_log_api_chat_enabled()) return;
    fprintf(stderr,
            "[API] api_chat: kind=%s ctx=%p tenant_strlen=%zu user_strlen=%zu user_message_strlen=%zu "
            "reply_buf_cap_or_prompt_cap=%zu temp_message_id_strlen=%zu\n",
            kind, (void *)ctx, strlen(tid), strlen(uid), strlen(msg), buf_or_prompt_cap,
            api_opts_cstr_len(temp_message_id));
}

static void api_log_api_create_options_received(const api_options_t *opts) {
    int mode = 0, ctx_bs = 0, rp = 0, ep = 0;
    int inj_geo = 0, dis_st = 0, geo_auth = 0, vec_b = 0, emb_auto = 0, sess_idle = 0;
    int learn_en = 0, defer_lt = 0;
    size_t lm = 0, lr = 0, le = 0, ldb = 0, lc = 0, vm = 0, scm = 0, scj = 0, scb = 0, ltp = 0;
    int st_en = 0, st_lib = 0, st_ex = 0;
    size_t st_coll = 0, st_tiny = 0, st_b2 = 0;
    int st_present = 0;
    size_t ms_lanes = 0, ms_fb = 0, ms_adapt = 0;
    uint32_t ms_flags = 0;
    int ms_present = 0;
    int ctx_eff = API_CONTEXT_BATCH_SIZE_DEFAULT;

    if (opts) {
        mode = opts->mode;
        ctx_bs = opts->context_batch_size;
        ctx_eff = (opts->context_batch_size > 0) ? opts->context_batch_size : API_CONTEXT_BATCH_SIZE_DEFAULT;
        rp = opts->redis_port;
        ep = opts->es_port;
        inj_geo = opts->inject_geo_knowledge;
        dis_st = opts->disable_auto_system_time;
        geo_auth = opts->geo_authority;
        vec_b = opts->vector_gen_backend;
        emb_auto = opts->embed_migration_autostart;
        sess_idle = opts->session_idle_seconds;
        learn_en = opts->enable_learning_terms;
        defer_lt = opts->defer_learning_terms_load;
        lm = api_opts_cstr_len(opts->mongo_uri);
        lr = api_opts_cstr_len(opts->redis_host);
        le = api_opts_cstr_len(opts->es_host);
        ldb = api_opts_cstr_len(opts->log_db);
        lc = api_opts_cstr_len(opts->log_coll);
        vm = api_opts_cstr_len(opts->vector_ollama_model);
        scm = api_opts_cstr_len(opts->shared_collection_mongo_uri);
        scj = api_opts_cstr_len(opts->shared_collection_json_path);
        scb = api_opts_cstr_len(opts->shared_collection_backfill_db);
        ltp = api_opts_cstr_len(opts->learning_terms_path);
        if (opts->smart_topic_opts) {
            const smart_topic_options_t *st = opts->smart_topic_opts;
            st_present = 1;
            st_en = st->enable ? 1 : 0;
            st_lib = (int)st->library_type;
            st_ex = (int)st->execution_mode;
            st_coll = api_opts_cstr_len(st->mini_ai_collection);
            st_tiny = api_opts_cstr_len(st->model_tiny);
            st_b2 = api_opts_cstr_len(st->model_b2);
        }
        if (opts->model_switch_opts) {
            const model_switch_options_t *ms = opts->model_switch_opts;
            ms_present = 1;
            ms_lanes = ms->lane_count;
            ms_flags = ms->flags;
            ms_fb = api_opts_cstr_len(ms->fallback_model);
            ms_adapt = api_opts_cstr_len(ms->adaptive_profile_id);
        }
    }

    fprintf(stderr,
            "[API] api_create: api_options_t_size=%zu opts=%p mode=%d context_batch_size=%d context_batch_effective=%d "
            "redis_port=%d es_port=%d inject_geo=%d disable_auto_system_time=%d geo_authority=%d "
            "vector_gen_backend=%d embed_migration_autostart=%d session_idle_seconds=%d "
            "enable_learning_terms=%d defer_learning_terms_load=%d "
            "strlen mongo_uri=%zu redis_host=%zu es_host=%zu log_db=%zu log_coll=%zu vector_ollama_model=%zu "
            "shared_collection_mongo_uri=%zu shared_collection_json_path=%zu shared_collection_backfill_db=%zu "
            "learning_terms_path=%zu "
            "smart_topic_ptr=%s enable=%d library_type=%d execution_mode=%d st_strlen coll=%zu tiny=%zu b2=%zu "
            "model_switch_ptr=%s lanes=%zu flags=%u fb_strlen=%zu adaptive_strlen=%zu\n",
            sizeof(api_options_t), (void *)opts, mode, ctx_bs, ctx_eff, rp, ep, inj_geo, dis_st, geo_auth, vec_b,
            emb_auto, sess_idle, learn_en, defer_lt, lm, lr, le, ldb, lc, vm, scm, scj, scb, ltp,
            st_present ? "set" : "null", st_en, st_lib, st_ex, st_coll, st_tiny, st_b2,
            ms_present ? "set" : "null", (size_t)ms_lanes, ms_flags, ms_fb, ms_adapt);
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

static int api_resolve_session_idle(const api_options_t *opts) {
    if (opts && opts->session_idle_seconds > 0)
        return opts->session_idle_seconds;
    const char *e = getenv("M4_SESSION_IDLE_SECONDS");
    if (e && e[0] == '0' && e[1] == '\0')
        return 0;
    if (e && e[0]) {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end != e && v >= 0)
            return (v > 86400000) ? 86400000 : (int)v;
    }
    return API_SESSION_IDLE_DEFAULT_SEC;
}

static void api_chat_session_destroy(void *p) {
    api_chat_session_t *s = (api_chat_session_t *)p;
    if (!s) return;
    free(s->ctx_roles);
    free(s->ctx_messages);
    free(s->ctx_sources);
    free(s->ctx_timestamps);
    free(s->ctx_llm);
    free(s);
}

static api_chat_session_t *api_chat_session_create(int capacity) {
    if (capacity < 1) capacity = 1;
    api_chat_session_t *s = (api_chat_session_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx_capacity = capacity;
    s->last_activity = time(NULL);
    s->ctx_roles = (char *)malloc((size_t)capacity * API_CTX_ROLE_SIZE);
    s->ctx_messages = (char *)malloc((size_t)capacity * API_CTX_MSG_SIZE);
    s->ctx_sources = (char *)malloc((size_t)capacity);
    s->ctx_timestamps = (char *)malloc((size_t)capacity * API_CTX_TS_SIZE);
    s->ctx_llm = (char *)malloc((size_t)capacity * API_CTX_LLM_SIZE);
    if (!s->ctx_roles || !s->ctx_messages || !s->ctx_sources || !s->ctx_timestamps || !s->ctx_llm) {
        api_chat_session_destroy(s);
        return NULL;
    }
    memset(s->ctx_roles, 0, (size_t)capacity * API_CTX_ROLE_SIZE);
    memset(s->ctx_messages, 0, (size_t)capacity * API_CTX_MSG_SIZE);
    memset(s->ctx_sources, 0, (size_t)capacity);
    memset(s->ctx_timestamps, 0, (size_t)capacity * API_CTX_TS_SIZE);
    memset(s->ctx_llm, 0, (size_t)capacity * API_CTX_LLM_SIZE);
    return s;
}

static void session_clear(api_chat_session_t *s) {
    if (!s) return;
    s->ctx_head = 0;
    s->ctx_count = 0;
    s->last_reply_source = 0;
    s->last_chat_wire = API_CHAT_WIRE_NONE;
    s->last_llm_model[0] = '\0';
    if (s->ctx_llm && s->ctx_capacity > 0)
        memset(s->ctx_llm, 0, (size_t)s->ctx_capacity * API_CTX_LLM_SIZE);
}

static void session_touch(api_chat_session_t *s) {
    if (s) s->last_activity = time(NULL);
}

static int api_make_session_key(char *out, size_t olen, const char *tid, const char *uid_slot) {
    if (!out || olen == 0 || !tid || !uid_slot) return -1;
    int n = snprintf(out, olen, "%s:%s", tid, uid_slot);
    if (n < 0 || (size_t)n >= olen) return -1;
    return 0;
}

typedef struct {
    char **keys;
    size_t n, cap;
    time_t now;
    int idle_sec;
} purge_collect_t;

static void purge_visit(const char *key, void *value, void *userdata) {
    purge_collect_t *c = (purge_collect_t *)userdata;
    api_chat_session_t *s = (api_chat_session_t *)value;
    if (!c || !s || c->idle_sec <= 0) return;
    if (difftime(c->now, s->last_activity) <= (double)c->idle_sec) return;
    if (c->n >= c->cap) {
        size_t nc = c->cap ? c->cap * 2u : 16u;
        char **nk = (char **)realloc(c->keys, nc * sizeof(char *));
        if (!nk) return;
        c->keys = nk;
        c->cap = nc;
    }
    c->keys[c->n] = strdup(key);
    if (c->keys[c->n]) c->n++;
}

static void api_ctx_purge_idle(api_context_t *ctx) {
    if (!ctx || !ctx->sessions || ctx->session_idle_sec <= 0) return;
    pthread_rwlock_wrlock(&ctx->sessions_lock);
    purge_collect_t col = {NULL, 0, 0, time(NULL), ctx->session_idle_sec};
    m4_ht_foreach(ctx->sessions, purge_visit, &col);
    for (size_t i = 0; i < col.n; i++) {
        void *v = m4_ht_take(ctx->sessions, col.keys[i]);
        if (v) api_chat_session_destroy(v);
        free(col.keys[i]);
    }
    free(col.keys);
    if (ctx->last_session_valid && m4_ht_get(ctx->sessions, ctx->last_session_key) == NULL)
        ctx->last_session_valid = 0;
    pthread_rwlock_unlock(&ctx->sessions_lock);
}

static void api_ctx_set_last_session(api_context_t *ctx, const char *key) {
    if (!ctx || !key) return;
    snprintf(ctx->last_session_key, sizeof(ctx->last_session_key), "%s", key);
    ctx->last_session_valid = 1;
}

static api_chat_session_t *api_ctx_current_session(api_context_t *ctx) {
    if (!ctx || !ctx->sessions || !ctx->last_session_valid) return NULL;
    pthread_rwlock_rdlock(&ctx->sessions_lock);
    api_chat_session_t *s = (api_chat_session_t *)m4_ht_get(ctx->sessions, ctx->last_session_key);
    pthread_rwlock_unlock(&ctx->sessions_lock);
    return s;
}

static api_chat_session_t *api_ctx_get_session(api_context_t *ctx, const char *tid, const char *uid_slot, int create) {
    if (!ctx || !ctx->sessions || !tid || !uid_slot) return NULL;
    api_ctx_purge_idle(ctx); /* purge takes write lock internally */
    char key[API_SESSION_KEY_MAX];
    if (api_make_session_key(key, sizeof(key), tid, uid_slot) != 0) return NULL;

    /* Try read-lock first (fast path: session exists) */
    pthread_rwlock_rdlock(&ctx->sessions_lock);
    api_chat_session_t *s = (api_chat_session_t *)m4_ht_get(ctx->sessions, key);
    pthread_rwlock_unlock(&ctx->sessions_lock);
    if (s) {
        session_touch(s);
        api_ctx_set_last_session(ctx, key);
        return s;
    }
    if (!create) return NULL;

    /* Write-lock for creation */
    pthread_rwlock_wrlock(&ctx->sessions_lock);
    /* Double-check (another thread may have created it) */
    s = (api_chat_session_t *)m4_ht_get(ctx->sessions, key);
    if (s) {
        pthread_rwlock_unlock(&ctx->sessions_lock);
        session_touch(s);
        api_ctx_set_last_session(ctx, key);
        return s;
    }
    s = api_chat_session_create(ctx->ring_capacity);
    if (!s) { pthread_rwlock_unlock(&ctx->sessions_lock); return NULL; }
    if (m4_ht_set(ctx->sessions, key, s) != 0) {
        pthread_rwlock_unlock(&ctx->sessions_lock);
        api_chat_session_destroy(s);
        return NULL;
    }
    pthread_rwlock_unlock(&ctx->sessions_lock);
    session_touch(s);
    api_ctx_set_last_session(ctx, key);
    return s;
}

static api_chat_session_t *api_ctx_require_session_by_key(api_context_t *ctx, const char *key) {
    if (!ctx || !ctx->sessions || !key || !key[0]) return NULL;
    api_ctx_purge_idle(ctx);

    pthread_rwlock_rdlock(&ctx->sessions_lock);
    api_chat_session_t *s = (api_chat_session_t *)m4_ht_get(ctx->sessions, key);
    pthread_rwlock_unlock(&ctx->sessions_lock);
    if (s) {
        session_touch(s);
        api_ctx_set_last_session(ctx, key);
        return s;
    }

    pthread_rwlock_wrlock(&ctx->sessions_lock);
    s = (api_chat_session_t *)m4_ht_get(ctx->sessions, key);
    if (s) {
        pthread_rwlock_unlock(&ctx->sessions_lock);
        session_touch(s);
        api_ctx_set_last_session(ctx, key);
        return s;
    }
    s = api_chat_session_create(ctx->ring_capacity);
    if (!s) { pthread_rwlock_unlock(&ctx->sessions_lock); return NULL; }
    if (m4_ht_set(ctx->sessions, key, s) != 0) {
        pthread_rwlock_unlock(&ctx->sessions_lock);
        api_chat_session_destroy(s);
        return NULL;
    }
    pthread_rwlock_unlock(&ctx->sessions_lock);
    session_touch(s);
    api_ctx_set_last_session(ctx, key);
    return s;
}

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
    struct tm tm_buf;
    struct tm *tm = localtime_r(&t, &tm_buf);
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

/* When caller omits source byte: infer from completion label + wire (ai_agent routing — not "default Ollama"). */
static char api_infer_assistant_source(unsigned chat_wire, const char *llm_model_id) {
    if (llm_model_id && llm_model_id[0]) {
        size_t lo = strlen(API_LLM_ROUTE_PREFIX_OLLAMA);
        if (strncmp(llm_model_id, API_LLM_ROUTE_PREFIX_OLLAMA, lo) == 0 && llm_model_id[lo] == ':')
            return API_SOURCE_OLLAMA;
        lo = strlen(API_LLM_ROUTE_PREFIX_GROQ);
        if (strncmp(llm_model_id, API_LLM_ROUTE_PREFIX_GROQ, lo) == 0 && llm_model_id[lo] == ':')
            return API_SOURCE_CLOUD;
        lo = strlen(API_LLM_ROUTE_PREFIX_CEREBRAS);
        if (strncmp(llm_model_id, API_LLM_ROUTE_PREFIX_CEREBRAS, lo) == 0 && llm_model_id[lo] == ':')
            return API_SOURCE_CLOUD;
        lo = strlen(API_LLM_ROUTE_PREFIX_GEMINI);
        if (strncmp(llm_model_id, API_LLM_ROUTE_PREFIX_GEMINI, lo) == 0 && llm_model_id[lo] == ':')
            return API_SOURCE_CLOUD;
    }
    if (chat_wire == API_CHAT_WIRE_OLLAMA)
        return API_SOURCE_OLLAMA;
    if (chat_wire == API_CHAT_WIRE_OPENAI_CHAT || chat_wire == API_CHAT_WIRE_GEMINI)
        return API_SOURCE_CLOUD;
    if (chat_wire == API_CHAT_WIRE_REDIS_RAG)
        return API_SOURCE_REDIS;
    if (chat_wire == API_CHAT_WIRE_EXTERNAL)
        return API_SOURCE_CLOUD;
    /* Unknown label+wire: align with default chat policy (hosted pool first), not assumed local Ollama. */
    return API_SOURCE_CLOUD;
}

static void session_push_message_with_source(api_chat_session_t *s, const char *role, const char *content, char source,
                                            const char *ts, unsigned chat_wire, const char *llm_model_id) {
    if (!s || !s->ctx_roles || !s->ctx_messages || !s->ctx_sources || s->ctx_capacity <= 0) return;
    const char *r = role ? role : "user";
    size_t rlen = strlen(r);
    if (rlen >= API_CTX_ROLE_SIZE) rlen = API_CTX_ROLE_SIZE - 1;
    size_t clen = content ? strnlen(content, API_CTX_MSG_SIZE - 1) : 0;
    if (!source) {
        if (r[0] == 'u' || strcmp(r, "user") == 0)
            source = API_SOURCE_MEMORY;
        else if (r[0] == 'a' || strcmp(r, "assistant") == 0)
            source = api_infer_assistant_source(chat_wire, llm_model_id);
        else
            source = API_SOURCE_MEMORY;
    }
    int idx;
    if (s->ctx_count < s->ctx_capacity) {
        idx = (s->ctx_head + s->ctx_count) % s->ctx_capacity;
        s->ctx_count++;
    } else {
        idx = s->ctx_head;
        s->ctx_head = (s->ctx_head + 1) % s->ctx_capacity;
    }
    memcpy(s->ctx_roles + idx * API_CTX_ROLE_SIZE, r, rlen);
    s->ctx_roles[idx * API_CTX_ROLE_SIZE + rlen] = '\0';
    if (clen > 0)
        memcpy(s->ctx_messages + idx * API_CTX_MSG_SIZE, content, clen);
    s->ctx_messages[idx * API_CTX_MSG_SIZE + clen] = '\0';
    s->ctx_sources[idx] = source;
    if (s->ctx_timestamps) {
        size_t tlen = ts && ts[0] ? strnlen(ts, API_CTX_TS_SIZE - 1) : 0;
        char *slot = s->ctx_timestamps + idx * API_CTX_TS_SIZE;
        if (tlen > 0) memcpy(slot, ts, tlen);
        slot[tlen] = '\0';
    }
    if (s->ctx_llm) {
        char *lslot = s->ctx_llm + idx * API_CTX_LLM_SIZE;
        lslot[0] = '\0';
        if (r[0] == 'a' || strcmp(r, "assistant") == 0) {
            if (llm_model_id && llm_model_id[0]) {
                size_t L = strnlen(llm_model_id, API_CTX_LLM_SIZE - 1);
                memcpy(lslot, llm_model_id, L);
                lslot[L] = '\0';
            } else if (source == API_SOURCE_REDIS) {
                memcpy(lslot, API_LLM_MODEL_ID_REDIS_RAG, sizeof(API_LLM_MODEL_ID_REDIS_RAG));
            } else if (source == API_SOURCE_CLOUD && chat_wire == API_CHAT_WIRE_EXTERNAL) {
                memcpy(lslot, "external", sizeof("external"));
            }
        }
    }
    if (r[0] == 'a' || strcmp(r, "assistant") == 0) {
        s->last_reply_source = source;
        if (chat_wire != API_CHAT_WIRE_NONE)
            s->last_chat_wire = chat_wire;
        if (llm_model_id && llm_model_id[0]) {
            size_t L = strnlen(llm_model_id, API_CTX_LLM_SIZE - 1);
            memcpy(s->last_llm_model, llm_model_id, L);
            s->last_llm_model[L] = '\0';
        } else if (source == API_SOURCE_REDIS) {
            memcpy(s->last_llm_model, API_LLM_MODEL_ID_REDIS_RAG, sizeof(API_LLM_MODEL_ID_REDIS_RAG));
        } else if (source == API_SOURCE_CLOUD && chat_wire == API_CHAT_WIRE_EXTERNAL) {
            memcpy(s->last_llm_model, "external", sizeof("external"));
        } else
            s->last_llm_model[0] = '\0';
    }
    session_touch(s);
}

/* Build prompt: topic + optional [KNOWLEDGE_BASE] + system guard + last N messages. Internal only. */
static size_t ctx_build_prompt(const api_context_t *ctx, api_chat_session_t *session, const char *current_user_msg,
                               const char *context_json, char *out, size_t out_size) {
    if (!ctx || !out || out_size == 0) return 0;
    int ctx_count = session ? session->ctx_count : 0;
    int ctx_head = session ? session->ctx_head : 0;
    int ctx_cap = session ? session->ctx_capacity : 1;
    char *ctx_roles = session ? session->ctx_roles : NULL;
    char *ctx_messages = session ? session->ctx_messages : NULL;
    size_t n = 0;
    n = ctx_compose_topic(current_user_msg, out, out_size);
    if (n >= out_size) { out[out_size - 1] = '\0'; return out_size - 1; }
    /* [CONTEXT] from caller — user info, session data, etc. Injected as-is (JSON). */
    if (context_json && context_json[0] && n + 20 + strlen(context_json) < out_size) {
        int w = snprintf(out + n, out_size - n,
                         "[USER_CONTEXT] You are talking to this user. Use this information when they ask about themselves:\n%s\n\n",
                         context_json);
        if (w > 0) n += (size_t)w;
        m4_log("API", M4_LOG_DEBUG, "prompt: [CONTEXT] injected (%zu chars)", strlen(context_json));
    } else {
        m4_log("API", M4_LOG_DEBUG, "prompt: no [CONTEXT] (context_json=%s)",
               context_json ? "empty" : "NULL");
    }
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
    int start = ctx_count > API_CTX_STRIP_5 ? ctx_count - API_CTX_STRIP_5 : 0;
    int num = ctx_count - start;
    if (ctx_roles && ctx_messages && ctx_count > API_CTX_STRIP_5 && (out_size - n) > 32) {
        size_t earlier_start = n;
        n += (size_t)snprintf(out + n, out_size - n, "Earlier (user only): ");
        for (int i = 0; i < start && n < earlier_start + API_CTX_EARLIER_MAX && n < out_size - 4; i++) {
            int idx = (ctx_head + i) % ctx_cap;
            const char *r = ctx_roles + idx * API_CTX_ROLE_SIZE;
            if (strcmp(r, "user") != 0) continue;
            const char *m = ctx_messages + idx * API_CTX_MSG_SIZE;
            size_t mlen = strnlen(m, API_CTX_MSG_SIZE - 1);
            if (mlen > out_size - n - 4) mlen = out_size - n - 4;
            if (n > (size_t)(earlier_start + 21)) { memcpy(out + n, " | ", 3); n += 3; }
            if (mlen > 0) { memcpy(out + n, m, mlen); n += mlen; }
        }
        if (n < out_size) { out[n++] = '\n'; out[n++] = '\n'; }
    }
    for (int i = 0; i < num && n < out_size - 64 && ctx_roles && ctx_messages; i++) {
        int idx = (ctx_head + start + i) % ctx_cap;
        const char *r = ctx_roles + idx * API_CTX_ROLE_SIZE;
        const char *m = ctx_messages + idx * API_CTX_MSG_SIZE;
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

/**
 * Transform the master prompt into structured parts for a specific provider.
 * Reads system text from the already-built flat blob (ctx_build_prompt output).
 * Reads history from session ring buffer as individual role+content pairs.
 * Trims to provider limits if provider is specified.
 *
 * @param full_prompt  The flat blob from ctx_build_prompt (system + history + user all in one)
 * @param session      Session ring buffer for structured history
 * @param user_msg     Current user message
 * @param provider     Provider name for trimming: "groq", "cerebras", "gemini", "ollama", or NULL (no trim)
 * @param parts        Output: structured prompt parts
 */
static void ctx_build_prompt_parts(const char *full_prompt, api_chat_session_t *session,
                                   const char *user_msg, const char *provider,
                                   ai_agent_prompt_t *parts) {
    if (!parts) return;
    ai_agent_prompt_init(parts);

    /* System: extract from flat blob — everything before the history section */
    if (full_prompt) {
        const char *hist_start = NULL;
        const char *markers[] = {"Earlier (user only):", "User: ", "Assistant: "};
        for (int m = 0; m < 3; m++) {
            const char *p = strstr(full_prompt, markers[m]);
            if (p && (!hist_start || p < hist_start)) hist_start = p;
        }
        if (hist_start) {
            /* Copy system portion only */
            size_t sys_len = (size_t)(hist_start - full_prompt);
            char *sys = (char *)malloc(sys_len + 1);
            if (sys) { memcpy(sys, full_prompt, sys_len); sys[sys_len] = '\0'; }
            parts->system = sys;
            parts->system_len = sys ? sys_len : 0;
        } else {
            ai_agent_prompt_set_system(parts, full_prompt);
        }
    }

    /* History: individual role + content from session ring buffer */
    if (session && session->ctx_count > 0 && session->ctx_roles && session->ctx_messages) {
        int cnt = session->ctx_count;
        int head = session->ctx_head;
        int cap = session->ctx_capacity;
        for (int i = 0; i < cnt; i++) {
            int idx = (head + i) % cap;
            const char *r = session->ctx_roles + idx * API_CTX_ROLE_SIZE;
            const char *m = session->ctx_messages + idx * API_CTX_MSG_SIZE;
            ai_agent_prompt_add_history(parts, r, m);
        }
    }

    /* Current user message */
    ai_agent_prompt_set_user(parts, user_msg);

    /* Trim to provider limits if specified */
    if (provider && provider[0]) {
        const ai_agent_provider_limits_t *limits = ai_agent_get_provider_limits(provider);
        if (limits && limits->prompt_max_chars > 0) {
            int max_chars = limits->prompt_max_chars;
            size_t est = (parts->system ? parts->system_len : 0) + (parts->user ? parts->user_len : 0);
            for (int i = 0; i < parts->history_count; i++)
                est += (parts->history[i].content ? strlen(parts->history[i].content) : 0) + 16;

            if ((int)est > max_chars) {
                m4_log("ai_agent", M4_LOG_DEBUG, "prompt_parts[%s]: %zu chars > limit %d, trimming",
                       provider, est, max_chars);

                int system_budget = max_chars * 2 / 5;
                int user_budget = max_chars / 5;
                int history_budget = max_chars - user_budget - system_budget;

                /* Trim user (rarely needed) */
                if (parts->user && (int)parts->user_len > user_budget && user_budget > 100) {
                    parts->user[user_budget] = '\0';
                    parts->user_len = (size_t)user_budget;
                }

                /* Trim system */
                if (parts->system && (int)parts->system_len > system_budget && system_budget > 100) {
                    parts->system[system_budget] = '\0';
                    parts->system_len = (size_t)system_budget;
                }

                /* Drop oldest history messages */
                int used = 0;
                for (int i = parts->history_count - 1; i >= 0; i--) {
                    int cl = (parts->history[i].content ? (int)strlen(parts->history[i].content) : 0) + 16;
                    if (used + cl > history_budget) {
                        /* Free dropped messages */
                        for (int j = 0; j <= i; j++)
                            free(parts->history[j].content);
                        int keep = parts->history_count - i - 1;
                        if (keep > 0)
                            memmove(parts->history, &parts->history[i + 1],
                                    (size_t)keep * sizeof(parts->history[0]));
                        parts->history_count = keep;
                        m4_log("ai_agent", M4_LOG_DEBUG, "prompt_parts[%s]: dropped %d history, keeping %d",
                               provider, i + 1, keep);
                        break;
                    }
                    used += cl;
                }
            }
        }
    }
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

/* strdup that returns NULL for NULL/empty — owns the copy */
static char *safe_strdup(const char *s) {
    return (s && s[0]) ? strdup(s) : NULL;
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
    /* Deep-copy all string fields — engine owns these for its lifetime */
    config->vector_ollama_model = (opts) ? safe_strdup(opts->vector_ollama_model) : NULL;
    config->embed_migration_autostart = (opts && opts->embed_migration_autostart != 0);
    config->shared_collection_mongo_uri = (opts) ? safe_strdup(opts->shared_collection_mongo_uri) : NULL;
    config->shared_collection_json_path = (opts) ? safe_strdup(opts->shared_collection_json_path) : NULL;
    config->shared_collection_backfill_db = (opts) ? safe_strdup(opts->shared_collection_backfill_db) : NULL;

    switch (mode) {
        case MODE_ONLY_MEMORY:
            config->mongo_uri = NULL;
            config->redis_host = NULL;
            config->redis_port = 0;
            config->es_host = NULL;
            config->es_port = 0;
            break;
        case MODE_ONLY_MONGO:
            config->mongo_uri = (opts) ? safe_strdup(opts->mongo_uri) : NULL;
            if (!config->mongo_uri) config->mongo_uri = strdup(DEFAULT_MONGO_URI);
            config->redis_host = NULL;
            config->redis_port = 0;
            config->es_host = NULL;
            config->es_port = 0;
            break;
        case MODE_MONGO_REDIS:
            config->mongo_uri = (opts) ? safe_strdup(opts->mongo_uri) : NULL;
            if (!config->mongo_uri) config->mongo_uri = strdup(DEFAULT_MONGO_URI);
            config->redis_host = (opts) ? safe_strdup(opts->redis_host) : NULL;
            if (!config->redis_host) config->redis_host = strdup(DEFAULT_REDIS_HOST);
            config->redis_port = (opts && opts->redis_port != 0) ? opts->redis_port : DEFAULT_REDIS_PORT;
            config->es_host = NULL;
            config->es_port = 0;
            break;
        case MODE_MONGO_REDIS_ELK:
            config->mongo_uri = (opts) ? safe_strdup(opts->mongo_uri) : NULL;
            if (!config->mongo_uri) config->mongo_uri = strdup(DEFAULT_MONGO_URI);
            config->redis_host = (opts) ? safe_strdup(opts->redis_host) : NULL;
            if (!config->redis_host) config->redis_host = strdup(DEFAULT_REDIS_HOST);
            config->redis_port = (opts && opts->redis_port != 0) ? opts->redis_port : DEFAULT_REDIS_PORT;
            config->es_host = (opts) ? safe_strdup(opts->es_host) : NULL;
            if (!config->es_host) config->es_host = strdup("");
            config->es_port = (opts && opts->es_port != 0) ? opts->es_port : DEFAULT_ES_PORT;
            break;
    }
    /* Inform when chat Mongo URI was omitted but this mode uses Mongo (same default as fill above). */
    if (mode != MODE_ONLY_MEMORY) {
        int explicit_mongo_uri = opts && opts->mongo_uri && opts->mongo_uri[0];
        if (!explicit_mongo_uri) {
            fprintf(stderr,
                    "[API] mongo_uri not set in config; using default %s "
                    "(connect when MongoDB is available on 127.0.0.1:27017)\n",
                    DEFAULT_MONGO_URI);
        }
    }
}

static void *api_nl_learn_load_thread_fn(void *arg) {
    api_context_t *ctx = (api_context_t *)arg;
    fprintf(stderr, "[API] nl_learn_terms: deferred thread starting open path=%s\n", ctx->nl_learn_async_path);
    nl_learn_terms_t *lt =
        nl_learn_terms_open(ctx->nl_learn_async_path, ctx->nl_learn_async_enable_write ? 1 : 0);
    fprintf(stderr, "[API] nl_learn_terms: deferred thread open returned %s\n", lt ? "OK" : "NULL");
    pthread_mutex_lock(&ctx->nl_learn_mx);
    int abandoned = atomic_load_explicit(&ctx->nl_learn_async_abandon, memory_order_acquire);
    if (abandoned) {
        fprintf(stderr, "[API] nl_learn_terms: deferred thread abandon=1 — closing\n");
        if (lt) nl_learn_terms_close(lt);
    } else {
        ctx->nl_learn = lt;
        if (lt) {
            fprintf(stderr, "[API] nl_learn_terms: deferred load ready path=%s\n", ctx->nl_learn_async_path);
            /* Start intent_learn worker now that nl_learn is ready. */
            storage_ctx_t *st = engine_get_storage(ctx->engine);
            sc_registry_t *screg = st ? storage_get_sc_registry(st) : NULL;
            if (screg) {
                if (intent_learn_init(screg, lt) == 0)
                    m4_log("INTENT_ROUTE", M4_LOG_INFO, "intent_learn: background worker started (deferred)");
                else
                    fprintf(stderr, "[API] intent_learn_init failed (deferred path)\n");
            } else {
                fprintf(stderr, "[API] intent_learn: skipped — no registry (st=%p screg=%p)\n",
                        (void*)st, (void*)screg);
            }
        } else
            fprintf(stderr,
                    "[API] nl_learn_terms: deferred load failed (NULL) path=%s — see [nl_learn_terms] stderr\n",
                    ctx->nl_learn_async_path);
    }
    pthread_mutex_unlock(&ctx->nl_learn_mx);
    return NULL;
}

api_context_t *api_create_with_opts(const api_options_t *opts) {
    if (api_log_api_create_opts_enabled())
        api_log_api_create_options_received(opts);
    int mode_int = (opts && opts->mode >= 0 && opts->mode <= 3) ? opts->mode : M4ENGINE_MODE_ONLY_MONGO;
    execution_mode_t mode = api_mode_to_engine(mode_int);
    if (opts) {
        if (opts->shared_collection_mongo_uri && opts->shared_collection_mongo_uri[0]) {
            if (m4_validate_mongo_connection_uri(opts->shared_collection_mongo_uri) != 0) {
                fprintf(stderr,
                        "[API] invalid shared_collection_mongo_uri (use mongodb:// or mongodb+srv://)\n");
                return NULL;
            }
        }
        if (opts->shared_collection_json_path && opts->shared_collection_json_path[0]) {
            if (m4_validate_optional_path_string(opts->shared_collection_json_path, 4096) != 0) {
                fprintf(stderr, "[API] invalid shared_collection_json_path (too long)\n");
                return NULL;
            }
        }
        if (opts->learning_terms_path && opts->learning_terms_path[0]) {
            if (m4_validate_optional_path_string(opts->learning_terms_path, 4096) != 0) {
                fprintf(stderr, "[API] invalid learning_terms_path (too long) — nl_learn_terms disabled\n");
                return NULL;
            }
        }
        if (mode != MODE_ONLY_MEMORY && opts->mongo_uri && opts->mongo_uri[0]) {
            if (m4_validate_mongo_connection_uri(opts->mongo_uri) != 0) {
                fprintf(stderr, "[API] invalid mongo_uri (use mongodb:// or mongodb+srv://)\n");
                return NULL;
            }
        }
    }
    /* Debug log filter from options */
    if (opts)
        m4_log_init(opts->debug_modules, opts->debug_module_count);

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

    /* Set schedule_refresh before engine_init (which triggers cold backfill). */
    if (opts && opts->schedule_refresh)
        storage_set_schedule_refresh(storage, 1);

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
    ctx->ring_capacity = ctx->context_batch_size;
    if (ctx->ring_capacity > API_CTX_CAPACITY_MAX) ctx->ring_capacity = API_CTX_CAPACITY_MAX;
    if (ctx->ring_capacity < 1) ctx->ring_capacity = 1;
    ctx->sessions = m4_ht_create(64);
    if (!ctx->sessions) {
        stat_destroy(stat);
        engine_destroy(engine);
        free(ctx);
        return NULL;
    }
    if (pthread_rwlock_init(&ctx->sessions_lock, NULL) != 0) {
        m4_ht_destroy(ctx->sessions, api_chat_session_destroy);
        stat_destroy(stat);
        engine_destroy(engine);
        free(ctx);
        return NULL;
    }
    atomic_init(&ctx->shutting_down, 0);
    ctx->session_idle_sec = api_resolve_session_idle(opts);
    ctx->last_session_key[0] = '\0';
    ctx->last_session_valid = 0;
    ctx->inject_geo_knowledge = (opts && opts->inject_geo_knowledge) ? 1 : 0;
    ctx->disable_auto_system_time = (opts && opts->disable_auto_system_time) ? 1 : 0;
    ctx->model_lane_key[0] = '\0';
    memset(ctx->prompt_tag_slots, 0, sizeof(ctx->prompt_tag_slots));
    ctx->nl_learn = NULL;
    ctx->nl_learn_mx_inited = 0;
    ctx->nl_learn_load_started = 0;
    ctx->nl_learn_async_path = NULL;
    ctx->nl_learn_async_enable_write = 0;
    ctx->nl_learn_writes_enabled = 0;
    atomic_init(&ctx->nl_learn_async_abandon, 0);
    if (opts && opts->learning_terms_path && opts->learning_terms_path[0]) {
        ctx->nl_learn_writes_enabled = opts->enable_learning_terms ? 1 : 0;
        if (pthread_mutex_init(&ctx->nl_learn_mx, NULL) != 0) {
            fprintf(stderr, "[API] pthread_mutex_init(nl_learn_mx) failed\n");
            m4_ht_destroy(ctx->sessions, api_chat_session_destroy);
            free(ctx);
            stat_destroy(stat);
            engine_destroy(engine);
            return NULL;
        }
        ctx->nl_learn_mx_inited = 1;
        int defer = opts->defer_learning_terms_load != 0;
        if (defer) {
            ctx->nl_learn_async_path = strdup(opts->learning_terms_path);
            if (!ctx->nl_learn_async_path) {
                pthread_mutex_destroy(&ctx->nl_learn_mx);
                ctx->nl_learn_mx_inited = 0;
                m4_ht_destroy(ctx->sessions, api_chat_session_destroy);
                free(ctx);
                stat_destroy(stat);
                engine_destroy(engine);
                return NULL;
            }
            ctx->nl_learn_async_enable_write = opts->enable_learning_terms ? 1 : 0;
            fprintf(stderr, "[API] nl_learn_terms: deferred load started path=%s\n", opts->learning_terms_path);
            if (pthread_create(&ctx->nl_learn_load_tid, NULL, api_nl_learn_load_thread_fn, ctx) != 0) {
                fprintf(stderr, "[API] pthread_create(nl_learn load) failed\n");
                free(ctx->nl_learn_async_path);
                ctx->nl_learn_async_path = NULL;
                pthread_mutex_destroy(&ctx->nl_learn_mx);
                ctx->nl_learn_mx_inited = 0;
                m4_ht_destroy(ctx->sessions, api_chat_session_destroy);
                free(ctx);
                stat_destroy(stat);
                engine_destroy(engine);
                return NULL;
            }
            ctx->nl_learn_load_started = 1;
        } else {
            ctx->nl_learn = nl_learn_terms_open(opts->learning_terms_path, opts->enable_learning_terms ? 1 : 0);
            if (!ctx->nl_learn) {
                fprintf(stderr, "[API] nl_learn_terms_open failed — see [nl_learn_terms] ERROR on stderr\n");
                pthread_mutex_destroy(&ctx->nl_learn_mx);
                ctx->nl_learn_mx_inited = 0;
                m4_ht_destroy(ctx->sessions, api_chat_session_destroy);
                free(ctx);
                stat_destroy(stat);
                engine_destroy(engine);
                return NULL;
            }
        }
    }

    /* --- New api_options_t fields (v2): persona, instructions, lane, geo csv, geo migrate --- */
    if (opts) {
        /* Default persona prompt tag */
        if (opts->default_persona && opts->default_persona[0])
            ctx->prompt_tag_slots[1] = strdup(opts->default_persona);
        /* Default instructions prompt tag */
        if (opts->default_instructions && opts->default_instructions[0])
            ctx->prompt_tag_slots[2] = strdup(opts->default_instructions);
        /* Default model lane */
        if (opts->default_model_lane > 0) {
            const char *k = NULL;
            switch (opts->default_model_lane) {
                case M4_API_MODEL_LANE_EDUCATION: k = "EDUCATION"; break;
                case M4_API_MODEL_LANE_BUSINESS:  k = "BUSINESS"; break;
                case M4_API_MODEL_LANE_TECH:      k = "TECH"; break;
                case M4_API_MODEL_LANE_CHAT:      k = "CHAT"; break;
                default: break;
            }
            if (k) {
                size_t ln = strlen(k);
                if (ln < sizeof(ctx->model_lane_key))
                    memcpy(ctx->model_lane_key, k, ln + 1);
            }
        }
        /* Geo authority CSV: read file and load into L1 cache */
        if (opts->geo_authority_csv_path && opts->geo_authority_csv_path[0]) {
            FILE *fp = fopen(opts->geo_authority_csv_path, "r");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (sz > 0 && sz < 16 * 1024 * 1024) { /* cap at 16 MB */
                    char *buf = (char *)malloc((size_t)sz + 1);
                    if (buf) {
                        size_t rd = fread(buf, 1, (size_t)sz, fp);
                        buf[rd] = '\0';
                        int rows = geo_authority_load_buffer(buf);
                        fprintf(stderr, "[API] geo_authority_csv_path: loaded %d rows from %s\n",
                                rows, opts->geo_authority_csv_path);
                        free(buf);
                    }
                }
                fclose(fp);
            } else {
                fprintf(stderr, "[API] geo_authority_csv_path: cannot open %s\n",
                        opts->geo_authority_csv_path);
            }
        }
        /* Geo atlas legacy migration */
        if (opts->geo_migrate_legacy) {
            storage_ctx_t *st = engine_get_storage(engine);
            unsigned long long modified = 0;
            int mr = storage_geo_atlas_migrate_legacy(st, &modified);
            fprintf(stderr, "[API] geo_migrate_legacy: %s (modified %llu)\n",
                    mr == 0 ? "ok" : "failed", modified);
        }
    }

    /* Build term vocab from SharedCollection registry for NL cue entity learning. */
    {
        storage_ctx_t *st = engine_get_storage(engine);
        sc_registry_t *screg = st ? storage_get_sc_registry(st) : NULL;
        if (screg) {
            ctx->nl_learn_vocab = sc_term_vocab_build(screg);
            if (ctx->nl_learn_vocab)
                m4_log("SHARED_COLLECTION", M4_LOG_INFO,
                       "term vocab built: %zu entries from registry",
                       sc_term_vocab_count(ctx->nl_learn_vocab));
        }
    }

    /* Start background intent learning worker (Phase 2 LLM extraction). */
    {
        storage_ctx_t *st = engine_get_storage(engine);
        sc_registry_t *screg = st ? storage_get_sc_registry(st) : NULL;
        /* Set query cache path if provided by app layer. */
        if (opts && opts->query_cache_path && opts->query_cache_path[0])
            intent_learn_set_cache_path(opts->query_cache_path);
        /* Start worker if nl_learn already available (non-deferred). Deferred path starts in thread. */
        if (screg && ctx->nl_learn) {
            if (intent_learn_init(screg, ctx->nl_learn) == 0)
                m4_log("INTENT_ROUTE", M4_LOG_INFO, "intent_learn: background worker started");
        }
    }

    /* Start background workers */
    geo_batch_init(ctx);
    /* Health check worker: only when user opts in AND mode has external services */
    if (opts && opts->enable_stats && mode != MODE_ONLY_MEMORY)
        health_check_init(ctx);
    else
        ctx->health.thread_started = 0;

    return ctx;
}

api_context_t *api_create(const char *json_opts) {
    json_opts_t jo;
    if (json_opts_parse(json_opts, &jo) != 0) {
        fprintf(stderr, "[API] api_create: invalid JSON options\n");
        return NULL;
    }

    /* Build api_options_t from parsed JSON */
    api_options_t opts = {0};
    if (jo.has_mode) opts.mode = jo.mode;
    else opts.mode = M4ENGINE_MODE_ONLY_MONGO; /* default */
    opts.mongo_uri = jo.mongo_uri;
    opts.redis_host = jo.redis_host;
    opts.redis_port = jo.redis_port;
    opts.es_host = jo.es_host;
    opts.es_port = jo.es_port;
    opts.log_db = jo.log_db;
    opts.log_coll = jo.log_coll;
    opts.context_batch_size = jo.context_batch_size;
    opts.inject_geo_knowledge = jo.inject_geo_knowledge;
    opts.disable_auto_system_time = jo.disable_auto_system_time;
    opts.geo_authority = jo.geo_authority;
    opts.vector_gen_backend = jo.vector_gen_backend;
    opts.vector_ollama_model = jo.vector_ollama_model;
    opts.embed_migration_autostart = jo.embed_migration_autostart;
    opts.session_idle_seconds = jo.session_idle_seconds;
    opts.shared_collection_mongo_uri = jo.shared_collection_mongo_uri;
    opts.shared_collection_json_path = jo.shared_collection_json_path;
    opts.shared_collection_backfill_db = jo.shared_collection_backfill_db;
    opts.learning_terms_path = jo.learning_terms_path;
    opts.enable_learning_terms = jo.enable_learning_terms;
    opts.defer_learning_terms_load = jo.defer_learning_terms_load;
    opts.default_persona = jo.default_persona;
    opts.default_instructions = jo.default_instructions;
    opts.default_model_lane = jo.default_model_lane;
    opts.geo_authority_csv_path = jo.geo_authority_csv_path;
    opts.geo_migrate_legacy = jo.geo_migrate_legacy;
    opts.enable_stats = jo.enable_stats;
    opts.schedule_refresh = jo.schedule_refresh;
    opts.query_cache_path = jo.query_cache_path;
    opts.debug_modules = (const char *const *)jo.debug_modules;
    opts.debug_module_count = jo.debug_module_count;

    /* Build model_switch lanes from JSON */
    model_switch_lane_entry_t *lane_entries = NULL;
    model_switch_options_t ms_opts = {0};
    if (jo.lanes && jo.lane_count > 0) {
        lane_entries = (model_switch_lane_entry_t *)calloc((size_t)jo.lane_count, sizeof(model_switch_lane_entry_t));
        if (lane_entries) {
            for (int i = 0; i < jo.lane_count; i++) {
                lane_entries[i].key = jo.lanes[i].key;
                lane_entries[i].model = jo.lanes[i].model;
                lane_entries[i].inject = jo.lanes[i].inject;
                lane_entries[i].api_url = jo.lanes[i].api_url;
                lane_entries[i].api_key = jo.lanes[i].api_key;
            }
            ms_opts.lanes = lane_entries;
            ms_opts.lane_count = (size_t)jo.lane_count;
            ms_opts.flags = MODEL_SWITCH_FLAG_MERGE_SMART_TOPIC_INTENT;
            opts.model_switch_opts = &ms_opts;
        }
    }

    api_context_t *ctx = api_create_with_opts(&opts);

    free(lane_entries);
    json_opts_free(&jo);
    return ctx;
}

static int api_set_model_lane(api_context_t *ctx, int lane) {
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

static int api_set_model_lane_key(api_context_t *ctx, const char *lane_key) {
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

static int api_set_prompt_tag(api_context_t *ctx, const char *key, const char *value) {
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

static void api_clear_prompt_tags(api_context_t *ctx) {
    prompt_tags_free(ctx);
}

void api_destroy(api_context_t *ctx) {
    if (!ctx) return;
    atomic_store(&ctx->shutting_down, 1); /* prevent new api_chat calls */
    health_check_destroy(ctx); /* stop health check thread */
    geo_batch_destroy(ctx); /* flush remaining rows + stop worker before tearing down engine */
    m4_log_shutdown();
    prompt_tags_free(ctx);
    if (ctx->sessions) {
        pthread_rwlock_wrlock(&ctx->sessions_lock);
        m4_ht_destroy(ctx->sessions, api_chat_session_destroy);
        ctx->sessions = NULL;
        pthread_rwlock_unlock(&ctx->sessions_lock);
        pthread_rwlock_destroy(&ctx->sessions_lock);
    }
    if (ctx->nl_learn_mx_inited) {
        atomic_store_explicit(&ctx->nl_learn_async_abandon, 1, memory_order_release);
        if (ctx->nl_learn_load_started) {
            (void)pthread_join(ctx->nl_learn_load_tid, NULL);
            ctx->nl_learn_load_started = 0;
        }
        pthread_mutex_lock(&ctx->nl_learn_mx);
        nl_learn_terms_close(ctx->nl_learn);
        ctx->nl_learn = NULL;
        free(ctx->nl_learn_async_path);
        ctx->nl_learn_async_path = NULL;
        pthread_mutex_unlock(&ctx->nl_learn_mx);
        pthread_mutex_destroy(&ctx->nl_learn_mx);
        ctx->nl_learn_mx_inited = 0;
    } else {
        nl_learn_terms_close(ctx->nl_learn);
        ctx->nl_learn = NULL;
    }
    if (ctx->nl_learn_vocab) {
        sc_term_vocab_free((sc_term_vocab_t *)ctx->nl_learn_vocab);
        ctx->nl_learn_vocab = NULL;
    }
    intent_learn_shutdown();
    stat_destroy(ctx->stat);
    engine_destroy(ctx->engine);
    free(ctx);
}

/** After a persisted user turn: internal NL cue enrichment (no public record API).
 *  With WAL: record() appends to WAL (O(1)); full snapshot save only at compaction/shutdown. */
static void api_ctx_nl_learn_after_user_turn(api_context_t *ctx, const char *user_utf8, int turn_persisted_ok) {
    if (!ctx || !turn_persisted_ok || !user_utf8 || !user_utf8[0]) return;
    if (!ctx->nl_learn_writes_enabled) return;
    if (!ctx->nl_learn_mx_inited) {
        if (!ctx->nl_learn) return;
        nl_learn_cues_record_from_utterance(ctx->nl_learn, user_utf8, ctx->nl_learn_vocab);
        /* WAL append happens inside record(); no full save needed per turn. */
        return;
    }
    pthread_mutex_lock(&ctx->nl_learn_mx);
    struct nl_learn_terms *p = ctx->nl_learn;
    if (p) {
        nl_learn_cues_record_from_utterance(p, user_utf8, ctx->nl_learn_vocab);
        /* WAL append happens inside record(); no full save needed per turn. */
    }
    pthread_mutex_unlock(&ctx->nl_learn_mx);
    /* Enqueue for background LLM extraction (Phase 2 learning). */
    intent_learn_enqueue(user_utf8);
}

static int64_t api_nl_learn_terms_score_sum(api_context_t *ctx, const char *const *term_keys, size_t nkeys,
                                     const char *intent) {
    if (!ctx) return 0;
    if (!ctx->nl_learn_mx_inited) {
        if (!ctx->nl_learn) return 0;
        return nl_learn_terms_score_sum(ctx->nl_learn, term_keys, nkeys, intent);
    }
    pthread_mutex_lock(&ctx->nl_learn_mx);
    struct nl_learn_terms *p = ctx->nl_learn;
    int64_t s = (p) ? nl_learn_terms_score_sum(p, term_keys, nkeys, intent) : 0;
    pthread_mutex_unlock(&ctx->nl_learn_mx);
    return s;
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
                                   double *temperature_io,
                                   char *lane_api_url, size_t url_sz,
                                   char *lane_api_key, size_t key_sz) {
    /* Phase 1: intent routing from NL learning scores (before smart_topic LLM call). */
    intent_route_result_t ir = {0};
    {
        nl_learn_terms_t *lt_snap = ctx->nl_learn;
        storage_ctx_t *st_classify = engine_get_storage(ctx->engine);
        sc_registry_t *reg_classify = st_classify ? storage_get_sc_registry(st_classify) : NULL;
        intent_route_classify(user_msg, lt_snap, ctx->nl_learn_vocab, reg_classify,
                              5 /* min_score_threshold */, &ir);
    }

    smart_topic_intent_t st = SMART_TOPIC_INTENT_DEFAULT;
    double temp = -1.0;
    /* If intent routing already decided ELK/RAG, skip smart_topic LLM call. */
    if (ir.intent != INTENT_ROUTE_CHAT) {
        /* ELK routes: use low temperature for deterministic answers. */
        temp = (ir.intent == INTENT_ROUTE_RAG_VECTOR) ? 0.3 : 0.1;
    } else {
        char st_buf[64];
        if (get_smart_topic(st_buf, sizeof(st_buf)) == 0 && strstr(st_buf, "disabled") == NULL)
            smart_topic_classify_for_query(user_msg, &temp, &st);
    }
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
    /* Pass lane endpoint override to caller */
    if (lane_api_url && url_sz > 0) {
        size_t cpy = strlen(prof.api_url);
        if (cpy >= url_sz) cpy = url_sz - 1;
        memcpy(lane_api_url, prof.api_url, cpy);
        lane_api_url[cpy] = '\0';
    }
    if (lane_api_key && key_sz > 0) {
        size_t cpy = strlen(prof.api_key);
        if (cpy >= key_sz) cpy = key_sz - 1;
        memcpy(lane_api_key, prof.api_key, cpy);
        lane_api_key[cpy] = '\0';
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

    /* Phase 3 + 4: if intent is ELK, execute query and inject [DATA_RESULT] into prompt. */
    if ((ir.intent == INTENT_ROUTE_ELK_ANALYTICS || ir.intent == INTENT_ROUTE_ELK_SEARCH)
        && context_buf && ctx_sz > 1) {
        storage_ctx_t *st = engine_get_storage(ctx->engine);
        sc_registry_t *screg = st ? storage_get_sc_registry(st) : NULL;
        if (st && screg) {
            intent_route_elk_result_t elk = {0};
            if (intent_route_execute(&ir, user_msg, screg, st, &elk) == 0 && elk.executed) {
                /* Phase 4: format [DATA_RESULT] and prepend to context_buf. */
                char data_block[4096];
                size_t dlen = intent_route_format_data_result(&ir, &elk, user_msg,
                                                              data_block, sizeof(data_block));
                if (dlen > 0) {
                    size_t existing = strnlen(context_buf, ctx_sz - 1);
                    if (dlen + existing + 1 <= ctx_sz) {
                        memmove(context_buf + dlen, context_buf, existing + 1);
                        memcpy(context_buf, data_block, dlen);
                    }
                }
            }
        }
    }
}

/* 0 = Ollama path (context_buf filled), 1 = Redis hit (redis_reply filled), -1 = error */
static int api_chat_prepare_for_stream(api_context_t *ctx, const char *tid, const char *uid, const char *msg,
                                       const char *context_json,
                                       char *user_ts_out, size_t user_ts_sz,
                                       char *context_buf, size_t ctx_sz,
                                       char *redis_reply, size_t redis_sz,
                                       char *ollama_model, size_t ollama_model_sz,
                                       double *temperature_out) {
    if (!ctx || !user_ts_out || user_ts_sz == 0 || !context_buf || ctx_sz == 0
        || !redis_reply || redis_sz == 0)
        return -1;

    const char *tid_r = (tid && tid[0]) ? tid : DEFAULT_TENANT_ID;
    const char *uid_r = (uid && uid[0]) ? uid : API_DEFAULT_USER_ID;
    api_chat_session_t *sess = api_ctx_get_session(ctx, tid_r, uid_r, 1);
    if (!sess) return -1;

    epoch_ms_string(user_ts_out, user_ts_sz);
    session_push_message_with_source(sess, "user", msg, API_SOURCE_MEMORY, user_ts_out, API_CHAT_WIRE_NONE, NULL);

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
                storage_rag_search(st, tid_r, uid_r, embed_vec, embed_dim, 5, 0.0, rag_accum_cb, &rag);
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
    ctx_build_prompt(ctx, sess, msg, context_json, context_buf, ctx_sz);
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
    api_apply_model_switch(ctx, msg, context_buf, ctx_sz, ollama_model, ollama_model_sz, temperature_out,
                           NULL, 0, NULL, 0);
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
    /* UTF-8 accumulator: hold trailing incomplete bytes between token callbacks */
    unsigned char utf8_hold[4]; /* max UTF-8 sequence is 4 bytes */
    int utf8_hold_len;
} stream_work_t;

/*
 * Return how many bytes at the END of buf[0..len-1] form an incomplete UTF-8 sequence.
 * 0 = all bytes are complete codepoints (safe to emit).
 * 1..3 = that many trailing bytes are a partial leading sequence — hold them.
 *
 * UTF-8 leading byte patterns:
 *   0xxxxxxx  (0x00-0x7F) → 1 byte, always complete
 *   110xxxxx  (0xC0-0xDF) → 2-byte sequence, needs 1 continuation
 *   1110xxxx  (0xE0-0xEF) → 3-byte sequence, needs 2 continuations
 *   11110xxx  (0xF0-0xF7) → 4-byte sequence, needs 3 continuations
 *   10xxxxxx  (0x80-0xBF) → continuation byte
 */
static int utf8_incomplete_tail(const unsigned char *buf, size_t len) {
    if (len == 0) return 0;
    /* Scan backwards from end for the last leading byte */
    size_t i = len;
    while (i > 0 && i > len - 4) {
        i--;
        unsigned char c = buf[i];
        if ((c & 0x80) == 0) {
            /* ASCII byte — everything up to and including this is complete */
            return 0;
        }
        if ((c & 0xC0) != 0x80) {
            /* Found a leading byte at position i */
            int expected;
            if ((c & 0xE0) == 0xC0)      expected = 2;
            else if ((c & 0xF0) == 0xE0) expected = 3;
            else if ((c & 0xF8) == 0xF0) expected = 4;
            else return 0; /* invalid byte, don't hold */

            int available = (int)(len - i);
            if (available < expected)
                return available; /* incomplete — hold these bytes */
            else
                return 0; /* sequence is complete */
        }
        /* continuation byte — keep scanning backwards */
    }
    /* All scanned bytes are continuation bytes with no leading byte in range — corrupt; flush as-is */
    return 0;
}

static void stream_forward_token(const char *token, void *userdata) {
    stream_work_t *w = (stream_work_t *)userdata;
    if (!w || !w->cb || !token || !token[0]) return;

    size_t tlen = strlen(token);

    /* Build a merged buffer: held bytes from previous call + new token */
    size_t merged_len = (size_t)w->utf8_hold_len + tlen;
    char merged_stack[512];
    char *merged = (merged_len < sizeof(merged_stack)) ? merged_stack : (char *)malloc(merged_len + 1);
    if (!merged) return;

    if (w->utf8_hold_len > 0) {
        memcpy(merged, w->utf8_hold, (size_t)w->utf8_hold_len);
    }
    memcpy(merged + w->utf8_hold_len, token, tlen);
    merged[merged_len] = '\0';
    w->utf8_hold_len = 0;

    /* Check if the merged buffer ends with an incomplete UTF-8 sequence */
    int tail = utf8_incomplete_tail((const unsigned char *)merged, merged_len);
    if (tail > 0 && (size_t)tail <= merged_len) {
        /* Hold the incomplete trailing bytes for next call */
        memcpy(w->utf8_hold, merged + merged_len - tail, (size_t)tail);
        w->utf8_hold_len = tail;
        merged_len -= (size_t)tail;
        merged[merged_len] = '\0';
    }

    /* Emit the safe portion (if any) */
    if (merged_len > 0 && merged[0]) {
        w->dbg_tok_count++;
        w->dbg_tok_bytes += merged_len;
        if (api_debug_chat_tokens()) {
            size_t show = merged_len < 160 ? merged_len : 160;
            fprintf(stderr, "[API][stream] token #%zu +%zuB: %.*s%s\n", w->dbg_tok_count, merged_len, (int)show,
                    merged, merged_len > show ? "..." : "");
        }
        w->cb(merged, w->msg_id, 0, w->cb_ud);
    }

    if (merged != merged_stack) free(merged);
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

    /* Same label persisted on the turn / session (ollama:<tag>); OLLAMA_MODEL / default when lane pin empty. */
    char stream_llm[API_CTX_LLM_SIZE];
    if (w->ollama_model[0])
        snprintf(stream_llm, sizeof(stream_llm), "%s:%s", API_LLM_ROUTE_PREFIX_OLLAMA, w->ollama_model);
    else {
        const char *oe = getenv("OLLAMA_MODEL");
        snprintf(stream_llm, sizeof(stream_llm), "%s:%s", API_LLM_ROUTE_PREFIX_OLLAMA,
                 (oe && oe[0]) ? oe : OLLAMA_DEFAULT_MODEL);
    }

    if (api_debug_chat_on()) {
        fprintf(stderr,
                "[API][stream] pump_done: llm_model=%s ollama_query_stream_rc=%d token_callbacks=%zu token_bytes=%zu "
                "assembled_full_len=%zu\n",
                stream_llm, orv, w->dbg_tok_count, w->dbg_tok_bytes, strlen(full));
        api_debug_utf8_preview("[stream] assembled_full", full, 1200);
    }

    int logic_conflict = run_geo_authority_post_chat(w->ctx, w->user_msg, full, sizeof(full));
    char bot_ts[24];
    epoch_ms_string(bot_ts, sizeof(bot_ts));
    char sk[API_SESSION_KEY_MAX];
    const char *uslot = (w->uid && w->uid[0]) ? w->uid : API_DEFAULT_USER_ID;
    if (api_make_session_key(sk, sizeof(sk), w->tid, uslot) == 0) {
        api_chat_session_t *sess = api_ctx_require_session_by_key(w->ctx, sk);
        if (sess)
            session_push_message_with_source(sess, "assistant", full, API_SOURCE_OLLAMA, bot_ts, API_CHAT_WIRE_OLLAMA,
                                             stream_llm);
    }

    {
        float embed_vec[OLLAMA_EMBED_MAX_DIM];
        size_t embed_dim = 0;
        char lang_buf[16];
        double lang_score = 0.0;
        char embed_model[128];
        (void)api_user_message_embedding(w->ctx->engine, w->user_msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                         embed_model, sizeof(embed_model));
        (void)lang_detect(w->user_msg, lang_buf, sizeof(lang_buf), &lang_score);
        if (api_debug_chat_on()) {
            fprintf(stderr,
                    "[API][stream] user_message_embed: dim=%zu model=%s (RAG/Mongo vector for this turn; not the "
                    "stream llm)\n",
                    embed_dim, (embed_dim > 0 && embed_model[0]) ? embed_model : "(none)");
        }
        {
            int ar = engine_append_turn(w->ctx->engine, w->tid, w->uid, w->user_msg, full, w->user_ts,
                                        (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                        lang_buf[0] ? lang_buf : NULL, lang_score, embed_model, stream_llm, w->msg_id,
                                        logic_conflict);
            if (ar != 0)
                fprintf(stderr,
                        "[API] engine_append_turn failed rc=%d (MEMORY mode, storage offline, or Mongo error). "
                        "tenant=%s user=%s\n",
                        ar, w->tid ? w->tid : "?", w->uid ? w->uid : "?");
            else if (api_debug_chat_on())
                fprintf(stderr,
                        "[API][stream] engine_append_turn rc=0 llm_model=%s (turn persisted; mongoc/mongo_connected "
                        "lines above)\n",
                        stream_llm);
            engine_inc_processed(w->ctx->engine, 1);
            api_ctx_nl_learn_after_user_turn(w->ctx, w->user_msg, ar == 0);
        }
    }

    /* Flush any held incomplete UTF-8 bytes before done signal */
    if (w->utf8_hold_len > 0 && w->cb) {
        char flush[5];
        memcpy(flush, w->utf8_hold, (size_t)w->utf8_hold_len);
        flush[w->utf8_hold_len] = '\0';
        w->utf8_hold_len = 0;
        w->cb(flush, w->msg_id, 0, w->cb_ud);
    }

    if (w->cb) w->cb("", w->msg_id, 1, w->cb_ud);
    free(w->msg_id);
    free(w->tid);
    free(w->uid);
    free(w->user_msg);
    free(w);
    return (void *)(intptr_t)0;
}

static int api_chat_stream_legacy(api_context_t *ctx,
                    const char *tenant_id,
                    const char *user_id,
                    const char *user_message,
                    const char *temp_message_id,
                    api_stream_token_cb cb,
                    void *userdata) {
    if (!ctx || !cb) return -1;
    const char *tid0 = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *uid0 = (user_id && user_id[0]) ? user_id : API_DEFAULT_USER_ID;
    if (!tenant_validate_id(tid0) || !tenant_validate_id(uid0)) return -1;
    const char *msg = user_message ? user_message : "";

    char *msg_id = NULL;
    if (temp_message_id && temp_message_id[0])
        msg_id = strdup(temp_message_id);
    else
        msg_id = api_gen_temp_message_id();
    if (!msg_id) return -1;

    api_log_api_chat_turn_caps("stream", ctx, tid0, uid0, msg, 0, msg_id);

    char user_ts[API_CTX_TS_SIZE];
    char context_buf[API_CONTEXT_BUFFER_SIZE];
    char redis_reply[API_CTX_MSG_SIZE];
    double stream_temp = -1.0;
    char stream_model[MODEL_SWITCH_MODEL_MAX];
    stream_model[0] = '\0';
    int prep = api_chat_prepare_for_stream(ctx, tid0, uid0, msg, NULL,
                                           user_ts, sizeof(user_ts),
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
            fprintf(stderr, "[API][stream] path=%s temp_message_id=%s tenant=%s user=%s\n", API_LLM_MODEL_ID_REDIS_RAG,
                    msg_id, tid0,
                    uid0);
            api_debug_utf8_preview("[stream] user_input", msg, 500);
            api_debug_utf8_preview("[stream] redis_reply_cached", redis_reply, 800);
        }
        int logic_conflict = run_geo_authority_post_chat(ctx, msg, redis_reply, sizeof(redis_reply));
        char bot_ts[24];
        epoch_ms_string(bot_ts, sizeof(bot_ts));
        api_chat_session_t *sess_r = api_ctx_get_session(ctx, tid0, uid0, 1);
        if (sess_r)
            session_push_message_with_source(sess_r, "assistant", redis_reply, API_SOURCE_REDIS, bot_ts,
                                             API_CHAT_WIRE_REDIS_RAG, API_LLM_MODEL_ID_REDIS_RAG);
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
                                            lang_buf[0] ? lang_buf : NULL, lang_score, embed_model,
                                            API_LLM_MODEL_ID_REDIS_RAG, msg_id, logic_conflict);
                if (ar != 0)
                    fprintf(stderr,
                            "[API] engine_append_turn failed rc=%d (Redis cache hit path). tenant=%s\n", ar, tid0);
                engine_inc_processed(ctx->engine, 1);
                api_ctx_nl_learn_after_user_turn(ctx, msg, ar == 0);
            }
        }
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

static int api_chat_stream_from_prepared(api_context_t *ctx,
                                  const char *tenant_id,
                                  const char *user_id,
                                  const char *user_message,
                                  const char *user_turn_ts,
                                  const char *prompt,
                                  const char *ollama_model,
                                  double temperature,
                                  const char *temp_message_id,
                                  api_stream_token_cb cb,
                                  void *userdata) {
    if (!ctx || !cb || !user_message || !user_turn_ts || !user_turn_ts[0] || !prompt || !prompt[0]) return -1;
    const char *tid0 = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *uid0 = (user_id && user_id[0]) ? user_id : API_DEFAULT_USER_ID;
    if (!tenant_validate_id(tid0) || !tenant_validate_id(uid0)) return -1;

    size_t plen = strlen(prompt);
    if (plen + 1u > API_CONTEXT_BUFFER_SIZE) return -1;

    char *msg_id = NULL;
    if (temp_message_id && temp_message_id[0])
        msg_id = strdup(temp_message_id);
    else
        msg_id = api_gen_temp_message_id();
    if (!msg_id) return -1;

    api_log_api_chat_turn_caps("stream_prepared", ctx, tid0, uid0, user_message, plen, msg_id);

    stream_work_t *w = (stream_work_t *)calloc(1, sizeof(stream_work_t));
    if (!w) {
        free(msg_id);
        return -1;
    }
    w->ctx = ctx;
    w->tid = strdup(tid0);
    w->uid = strdup(uid0);
    w->user_msg = strdup(user_message);
    w->msg_id = msg_id;
    if (!w->tid || !w->uid || !w->user_msg) {
        free(w->msg_id);
        free(w->tid);
        free(w->uid);
        free(w->user_msg);
        free(w);
        return -1;
    }
    memcpy(w->user_ts, user_turn_ts, sizeof(w->user_ts));
    memcpy(w->prompt, prompt, plen + 1);
    memset(w->ollama_model, 0, sizeof(w->ollama_model));
    if (ollama_model && ollama_model[0]) {
        size_t ml = strlen(ollama_model);
        if (ml >= sizeof(w->ollama_model)) ml = sizeof(w->ollama_model) - 1u;
        memcpy(w->ollama_model, ollama_model, ml);
        w->ollama_model[ml] = '\0';
    }
    w->temperature = temperature;
    w->cb = cb;
    w->cb_ud = userdata;

    if (api_debug_chat_on()) {
        api_debug_storage(ctx, "stream_from_prepared");
        fprintf(stderr,
                "[API][stream] path=from_prepared temp_message_id=%s tenant=%s user=%s model=%s temperature=%.4g\n",
                msg_id, tid0, uid0, w->ollama_model[0] ? w->ollama_model : "(default)", temperature);
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

int api_chat(api_context_t *ctx, const char *tenant_id, const char *user_id, const char *user_message,
             const char *context_json,
             char *bot_reply_out, size_t out_size,
             api_stream_token_cb stream_cb, void *stream_userdata) {
    /* Must have at least one output: buffer for sync, callback for stream */
    if (!ctx) return -1;
    if (atomic_load(&ctx->shutting_down)) return -1;
    if (!stream_cb && (!bot_reply_out || out_size == 0)) return -1;

    m4_log("API", M4_LOG_DEBUG, "api_chat: context_json=%s msg_len=%zu",
           (context_json && context_json[0]) ? "set" : "NULL",
           user_message ? strlen(user_message) : 0);

    /* Auto-greeting: if no user message but context is provided, generate a greeting prompt */
    char greeting_buf[512];
    if ((!user_message || !user_message[0]) && context_json && context_json[0]) {
        snprintf(greeting_buf, sizeof(greeting_buf),
                 "[GREETING] The user just opened the chat for the first time today. "
                 "Greet them warmly based on their context. Keep it short and natural.");
        user_message = greeting_buf;
    }

    /* --- Stream path: delegate to existing stream machinery --- */
    if (stream_cb) {
        char *msg_id = api_gen_temp_message_id();
        if (!msg_id) return -1;

        const char *tid0 = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
        const char *uid0 = (user_id && user_id[0]) ? user_id : API_DEFAULT_USER_ID;
        if (!tenant_validate_id(tid0) || !tenant_validate_id(uid0)) { free(msg_id); return -1; }
        const char *msg = user_message ? user_message : "";

        api_log_api_chat_turn_caps("stream", ctx, tid0, uid0, msg, 0, msg_id);

        char user_ts[API_CTX_TS_SIZE];
        char context_buf[API_CONTEXT_BUFFER_SIZE];
        char redis_reply[API_CTX_MSG_SIZE];
        double stream_temp = -1.0;
        char stream_model[MODEL_SWITCH_MODEL_MAX];
        stream_model[0] = '\0';
        int prep = api_chat_prepare_for_stream(ctx, tid0, uid0, msg, context_json,
                                               user_ts, sizeof(user_ts),
                                               context_buf, sizeof(context_buf),
                                               redis_reply, sizeof(redis_reply),
                                               stream_model, sizeof(stream_model),
                                               &stream_temp);
        if (prep < 0) { free(msg_id); return -1; }

        if (prep == 1) {
            /* Redis RAG hit — deliver cached reply via callback */
            int logic_conflict = run_geo_authority_post_chat(ctx, msg, redis_reply, sizeof(redis_reply));
            char bot_ts[24];
            epoch_ms_string(bot_ts, sizeof(bot_ts));
            api_chat_session_t *sess_r = api_ctx_get_session(ctx, tid0, uid0, 1);
            if (sess_r)
                session_push_message_with_source(sess_r, "assistant", redis_reply, API_SOURCE_REDIS, bot_ts,
                                                 API_CHAT_WIRE_REDIS_RAG, API_LLM_MODEL_ID_REDIS_RAG);
            {
                float embed_vec[OLLAMA_EMBED_MAX_DIM];
                size_t embed_dim = 0;
                char lang_buf[16]; double lang_score = 0.0; char embed_model[128];
                (void)api_user_message_embedding(ctx->engine, msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                                 embed_model, sizeof(embed_model));
                (void)lang_detect(msg, lang_buf, sizeof(lang_buf), &lang_score);
                int ar = engine_append_turn(ctx->engine, tid0, uid0, msg, redis_reply, user_ts,
                                            (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                            lang_buf[0] ? lang_buf : NULL, lang_score, embed_model,
                                            API_LLM_MODEL_ID_REDIS_RAG, msg_id, logic_conflict);
                if (ar != 0)
                    fprintf(stderr, "[API] engine_append_turn failed rc=%d (stream Redis path). tenant=%s\n", ar, tid0);
                engine_inc_processed(ctx->engine, 1);
                api_ctx_nl_learn_after_user_turn(ctx, msg, ar == 0);
            }
            if (bot_reply_out && out_size > 0) {
                size_t rl = strlen(redis_reply);
                if (rl >= out_size) rl = out_size - 1;
                memcpy(bot_reply_out, redis_reply, rl);
                bot_reply_out[rl] = '\0';
            }
            stream_cb(redis_reply, msg_id, 0, stream_userdata);
            stream_cb("", msg_id, 1, stream_userdata);
            free(msg_id);
            return 0;
        }

        /* Stream path: route through ai_agent (cloud pool + Ollama fallback).
         * If ai_agent picks Ollama AND M4_CHAT_BACKEND allows it → use real token streaming.
         * If ai_agent picks cloud → sync completion, deliver as synthetic stream. */

        /* Resolve lane endpoint override */
        char lane_url[MODEL_SWITCH_URL_MAX];
        char lane_key_buf[MODEL_SWITCH_KEY_MAX];
        lane_url[0] = lane_key_buf[0] = '\0';
        {
            /* Re-run model_switch to get lane url/key (prepare_for_stream only got model+temp) */
            const engine_config_t *cfg = engine_get_config(ctx->engine);
            model_switch_profile_t prof;
            memset(&prof, 0, sizeof(prof));
            smart_topic_intent_t st = SMART_TOPIC_INTENT_DEFAULT;
            model_switch_resolve(cfg ? cfg->model_switch_opts : NULL, ctx->model_lane_key, st, &prof);
            if (prof.api_url[0]) {
                memcpy(lane_url, prof.api_url, sizeof(lane_url));
                memcpy(lane_key_buf, prof.api_key, sizeof(lane_key_buf));
            }
        }

        const char *om = (stream_model[0] != '\0') ? stream_model : NULL;
        const char *lu = (lane_url[0] != '\0') ? lane_url : NULL;
        const char *lk = (lane_key_buf[0] != '\0') ? lane_key_buf : NULL;

        /* Always try ai_agent first (routes: lane → cloud pool → Ollama fallback).
         * Model tag from model_switch is a preference, not a "skip cloud" pin. */
        m4_log("API", M4_LOG_DEBUG, "stream: trying ai_agent sync model=%s lane_url=%s",
               om ? om : "(none)", lu ? lu : "(none)");
        {
            char sync_reply[OL_BUF_SIZE];
            sync_reply[0] = '\0';
            char assistant_source = 0;
            unsigned chat_wire = API_CHAT_WIRE_NONE;
            char llm_label[API_CTX_LLM_SIZE];
            llm_label[0] = '\0';

            /* Build structured prompt from master blob (dynamic, heap-allocated) */
            ai_agent_prompt_t stream_prompt_parts;
            api_chat_session_t *sess_stream = api_ctx_get_session(ctx, tid0, uid0, 1);
            ctx_build_prompt_parts(context_buf, sess_stream, msg, NULL, &stream_prompt_parts);

            if (ai_agent_complete_chat(&stream_prompt_parts, context_buf, stream_temp, om, lu, lk,
                                       sync_reply, sizeof(sync_reply),
                                       &assistant_source, &chat_wire,
                                       llm_label, sizeof(llm_label)) == 0 && sync_reply[0]) {
                m4_log("API", M4_LOG_INFO, "stream: ai_agent SUCCESS source=%c wire=%u model=%s reply_len=%zu",
                       assistant_source, chat_wire, llm_label, strlen(sync_reply));
                /* Synthetic stream: deliver full reply via callback */
                int logic_conflict = run_geo_authority_post_chat(ctx, msg, sync_reply, sizeof(sync_reply));
                char bot_ts[24];
                epoch_ms_string(bot_ts, sizeof(bot_ts));
                api_chat_session_t *sess_s = api_ctx_get_session(ctx, tid0, uid0, 1);
                if (sess_s)
                    session_push_message_with_source(sess_s, "assistant", sync_reply, assistant_source, bot_ts,
                                                     chat_wire, llm_label[0] ? llm_label : NULL);
                {
                    float embed_vec[OLLAMA_EMBED_MAX_DIM];
                    size_t embed_dim = 0;
                    char lang_buf[16]; double lang_score = 0.0; char embed_model[128];
                    (void)api_user_message_embedding(ctx->engine, msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                                     embed_model, sizeof(embed_model));
                    (void)lang_detect(msg, lang_buf, sizeof(lang_buf), &lang_score);
                    int ar = engine_append_turn(ctx->engine, tid0, uid0, msg, sync_reply, user_ts,
                                                (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                                lang_buf[0] ? lang_buf : NULL, lang_score, embed_model,
                                                llm_label[0] ? llm_label : NULL, msg_id, logic_conflict);
                    if (ar != 0)
                        fprintf(stderr, "[API] engine_append_turn failed rc=%d (stream ai_agent path). tenant=%s\n", ar, tid0);
                    engine_inc_processed(ctx->engine, 1);
                    api_ctx_nl_learn_after_user_turn(ctx, msg, ar == 0);
                }
                if (bot_reply_out && out_size > 0) {
                    size_t rl = strlen(sync_reply);
                    if (rl >= out_size) rl = out_size - 1;
                    memcpy(bot_reply_out, sync_reply, rl);
                    bot_reply_out[rl] = '\0';
                }
                stream_cb(sync_reply, msg_id, 0, stream_userdata);
                stream_cb("", msg_id, 1, stream_userdata);
                ai_agent_prompt_free(&stream_prompt_parts);
                free(msg_id);
                return 0;
            }
            ai_agent_prompt_free(&stream_prompt_parts);
            m4_log("API", M4_LOG_WARN, "stream: ai_agent failed — falling through to Ollama real stream");
        }

        /* Real token streaming via Ollama (M4_CHAT_BACKEND=ollama, or cloud failed, or model pinned) */
        m4_log("API", M4_LOG_INFO, "stream: Ollama real token stream model=%s", stream_model[0] ? stream_model : "(default)");
        stream_work_t *w = (stream_work_t *)calloc(1, sizeof(stream_work_t));
        if (!w) { free(msg_id); return -1; }
        w->ctx = ctx;
        w->tid = strdup(tid0);
        w->uid = strdup(uid0);
        w->user_msg = strdup(msg);
        w->msg_id = msg_id;
        memcpy(w->user_ts, user_ts, sizeof(w->user_ts));
        memcpy(w->prompt, context_buf, sizeof(w->prompt));
        memcpy(w->ollama_model, stream_model, sizeof(w->ollama_model));
        w->cb = stream_cb;
        w->cb_ud = stream_userdata;
        w->temperature = stream_temp;

        pthread_t th;
        if (pthread_create(&th, NULL, api_chat_stream_worker, w) != 0) {
            free(w->tid); free(w->uid); free(w->user_msg); free(w->msg_id); free(w);
            return -1;
        }
        void *rv = NULL;
        pthread_join(th, &rv);
        return (rv == (void *)(intptr_t)0) ? 0 : -1;
    }

    /* --- Sync path (stream_cb == NULL): original api_chat logic --- */
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *uid = (user_id && user_id[0]) ? user_id : API_DEFAULT_USER_ID;
    const char *msg = user_message ? user_message : "";
    api_log_api_chat_turn_caps("sync", ctx, tid, uid, msg, out_size, NULL);
    if (!tenant_validate_id(tid) || !tenant_validate_id(uid)) return -1;
    int logic_conflict = 0;

    char user_ts[24];
    epoch_ms_string(user_ts, sizeof(user_ts));

    api_chat_session_t *sess = api_ctx_get_session(ctx, tid, uid, 1);
    if (!sess) return -1;

    char llm_for_turn[API_CTX_LLM_SIZE];
    llm_for_turn[0] = '\0';

    /* Step 4: push user into circular buffer (source = MEMORY). */
    session_push_message_with_source(sess, "user", msg, API_SOURCE_MEMORY, user_ts, API_CHAT_WIRE_NONE, NULL);

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
                storage_rag_search(st, tid, uid, embed_vec, embed_dim, 5, 0.0, rag_accum_cb, &rag);
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
                    snprintf(llm_for_turn, sizeof(llm_for_turn), "%s", API_LLM_MODEL_ID_REDIS_RAG);
                    session_push_message_with_source(sess, "assistant", bot_reply_out, API_SOURCE_REDIS, bot_ts,
                                                     API_CHAT_WIRE_REDIS_RAG, llm_for_turn);
                    /* Still append turn to Mongo (vector/lang from Phase 1 below) for consistency. */
                    goto append_turn;
                }
            }
        }
    }

    char context_buf[API_CONTEXT_BUFFER_SIZE];
    memset(context_buf, 0, sizeof(context_buf));
    ctx_build_prompt(ctx, sess, msg, context_json, context_buf, sizeof(context_buf));
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
    char lane_url[MODEL_SWITCH_URL_MAX];
    char lane_key[MODEL_SWITCH_KEY_MAX];
    ollama_model[0] = lane_url[0] = lane_key[0] = '\0';
    api_apply_model_switch(ctx, msg, context_buf, sizeof(context_buf), ollama_model, sizeof(ollama_model), &temperature,
                           lane_url, sizeof(lane_url), lane_key, sizeof(lane_key));
    const char *om = (ollama_model[0] != '\0') ? ollama_model : NULL;
    const char *lu = (lane_url[0] != '\0') ? lane_url : NULL;
    const char *lk = (lane_key[0] != '\0') ? lane_key : NULL;

    /* Build structured prompt from master blob (provider trimming handled inside) */
    ai_agent_prompt_t prompt_parts;
    ctx_build_prompt_parts(context_buf, sess, msg, NULL, &prompt_parts);

    char assistant_source = 0;
    unsigned chat_wire = API_CHAT_WIRE_NONE;
    if (ai_agent_complete_chat(&prompt_parts, context_buf, temperature, om, lu, lk, bot_reply_out, out_size, &assistant_source,
                                 &chat_wire, llm_for_turn, sizeof(llm_for_turn)) != 0) {
        ai_agent_prompt_free(&prompt_parts);
        return -1;
    }
    logic_conflict = run_geo_authority_post_chat(ctx, msg, bot_reply_out, out_size);
    char bot_ts[24];
    epoch_ms_string(bot_ts, sizeof(bot_ts));
    session_push_message_with_source(sess, "assistant", bot_reply_out, assistant_source, bot_ts, chat_wire,
                                     llm_for_turn[0] ? llm_for_turn : NULL);

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

    int ar_turn = engine_append_turn(ctx->engine, tid, uid, msg, bot_reply_out, user_ts,
                                     (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                     lang_buf[0] ? lang_buf : NULL, lang_score, embed_model,
                                     llm_for_turn[0] ? llm_for_turn : NULL, NULL, logic_conflict);
    if (ar_turn != 0)
        fprintf(stderr,
                "[API] engine_append_turn failed rc=%d — turn not in Mongo (MEMORY mode, no mongoc client, "
                "or insert error). tenant=%s\n",
                ar_turn, tid);
    engine_inc_processed(ctx->engine, 1);
    api_ctx_nl_learn_after_user_turn(ctx, msg, ar_turn == 0);
    }
    ai_agent_prompt_free(&prompt_parts);
    return 0;
}

static int api_append_turn_phase(api_context_t *ctx, const char *tid, const char *uid, const char *msg,
                                 char *bot_reply_out, const char *user_ts, int logic_conflict,
                                 const char *llm_model_id) {
    float embed_vec[OLLAMA_EMBED_MAX_DIM];
    size_t embed_dim = 0;
    char lang_buf[16];
    double lang_score = 0.0;
    char embed_model[128];
    (void)api_user_message_embedding(ctx->engine, msg, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim,
                                     embed_model, sizeof(embed_model));
    (void)lang_detect(msg, lang_buf, sizeof(lang_buf), &lang_score);

    int ar = engine_append_turn(ctx->engine, tid, uid, msg, bot_reply_out, user_ts,
                                (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                lang_buf[0] ? lang_buf : NULL, lang_score, embed_model, llm_model_id, NULL,
                                logic_conflict);
    if (ar != 0)
        fprintf(stderr,
                "[API] engine_append_turn failed rc=%d — turn not in Mongo (MEMORY mode, no mongoc client, "
                "or insert error). tenant=%s\n",
                ar, tid);
    return ar;
}

static int api_chat_prepare_external_llm(api_context_t *ctx,
                                  const char *tenant_id, const char *user_id, const char *user_message,
                                  char *prompt_out, size_t prompt_cap,
                                  char *ollama_model_out, size_t model_cap,
                                  double *temperature_out,
                                  char *assistant_if_redis, size_t assistant_redis_cap,
                                  char *user_turn_ts_out, size_t user_turn_ts_cap) {
    if (!ctx || !prompt_out || prompt_cap == 0) return -1;
    if (user_turn_ts_out && user_turn_ts_cap < 24) return -1;
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *uid = (user_id && user_id[0]) ? user_id : API_DEFAULT_USER_ID;
    if (!tenant_validate_id(tid) || !tenant_validate_id(uid)) return -1;
    const char *msg = user_message ? user_message : "";

    char user_ts[24];
    epoch_ms_string(user_ts, sizeof(user_ts));
    if (user_turn_ts_out && user_turn_ts_cap >= sizeof(user_ts)) {
        memcpy(user_turn_ts_out, user_ts, sizeof(user_ts));
    }

    api_chat_session_t *sess = api_ctx_get_session(ctx, tid, uid, 1);
    if (!sess) return -1;

    session_push_message_with_source(sess, "user", msg, API_SOURCE_MEMORY, user_ts, API_CHAT_WIRE_NONE, NULL);

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
                storage_rag_search(st, tid, uid, embed_vec, embed_dim, 5, 0.0, rag_accum_cb, &rag);
                if (rag.first_len > 0 && rag.best_score >= API_RAG_REPLY_MIN_SCORE) {
                    char bot_local[OL_BUF_SIZE];
                    const char *reply = rag.first_snippet;
                    size_t reply_len = rag.first_len;
                    const char *nl = memchr(rag.first_snippet, '\n', rag.first_len);
                    if (nl) {
                        reply = nl + 1;
                        reply_len = rag.first_len - (size_t)(nl + 1 - rag.first_snippet);
                    }
                    size_t copy_len = reply_len;
                    if (copy_len >= sizeof(bot_local)) copy_len = sizeof(bot_local) - 1;
                    memcpy(bot_local, reply, copy_len);
                    bot_local[copy_len] = '\0';
                    int logic_conflict = run_geo_authority_post_chat(ctx, msg, bot_local, sizeof(bot_local));
                    char bot_ts[24];
                    epoch_ms_string(bot_ts, sizeof(bot_ts));
                    session_push_message_with_source(sess, "assistant", bot_local, API_SOURCE_REDIS, bot_ts,
                                                     API_CHAT_WIRE_REDIS_RAG, API_LLM_MODEL_ID_REDIS_RAG);
                    {
                        int ar_r = api_append_turn_phase(ctx, tid, uid, msg, bot_local, user_ts, logic_conflict,
                                                         API_LLM_MODEL_ID_REDIS_RAG);
                        engine_inc_processed(ctx->engine, 1);
                        api_ctx_nl_learn_after_user_turn(ctx, msg, ar_r == 0);
                    }
                    if (assistant_if_redis && assistant_redis_cap > 0) {
                        size_t n = strlen(bot_local);
                        if (n >= assistant_redis_cap) n = assistant_redis_cap - 1;
                        memcpy(assistant_if_redis, bot_local, n);
                        assistant_if_redis[n] = '\0';
                    }
                    return 1;
                }
            }
        }
    }

    char context_buf[API_CONTEXT_BUFFER_SIZE];
    memset(context_buf, 0, sizeof(context_buf));
    ctx_build_prompt(ctx, sess, msg, NULL, context_buf, sizeof(context_buf));
    if (rag.used > 0) {
        const char *header = "Context from past turns:\n";
        size_t hlen = strlen(header);
        size_t total_prepend = hlen + rag.used + 2;
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
    api_apply_model_switch(ctx, msg, context_buf, sizeof(context_buf), ollama_model, sizeof(ollama_model), &temperature,
                           NULL, 0, NULL, 0);

    size_t plen = strnlen(context_buf, sizeof(context_buf));
    if (plen >= prompt_cap) return -1;
    memcpy(prompt_out, context_buf, plen + 1);

    if (ollama_model_out && model_cap > 0) {
        size_t ml = strlen(ollama_model);
        if (ml >= model_cap) ml = model_cap - 1;
        memcpy(ollama_model_out, ollama_model, ml);
        ollama_model_out[ml] = '\0';
    }
    if (temperature_out) *temperature_out = temperature;
    return 0;
}

static int api_chat_external_reply_with_meta(api_context_t *ctx,
                                      const char *tenant_id, const char *user_id,
                                      const char *user_message, const char *assistant_message,
                                      const char *user_turn_ts, const char *llm_model_id, unsigned chat_wire) {
    if (!ctx || !assistant_message || !assistant_message[0]) return -1;
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *uid = (user_id && user_id[0]) ? user_id : API_DEFAULT_USER_ID;
    if (!tenant_validate_id(tid) || !tenant_validate_id(uid)) return -1;
    const char *msg = user_message ? user_message : "";

    char user_ts_use[24];
    if (user_turn_ts && user_turn_ts[0]) {
        strncpy(user_ts_use, user_turn_ts, sizeof(user_ts_use) - 1);
        user_ts_use[sizeof(user_ts_use) - 1] = '\0';
    } else {
        epoch_ms_string(user_ts_use, sizeof(user_ts_use));
    }

    api_chat_session_t *sess = api_ctx_get_session(ctx, tid, uid, 0);
    if (!sess) return -1;

    char bot_buf[OL_BUF_SIZE];
    size_t alen = strlen(assistant_message);
    if (alen >= sizeof(bot_buf)) alen = sizeof(bot_buf) - 1;
    memcpy(bot_buf, assistant_message, alen);
    bot_buf[alen] = '\0';

    int logic_conflict = run_geo_authority_post_chat(ctx, msg, bot_buf, sizeof(bot_buf));
    char bot_ts[24];
    epoch_ms_string(bot_ts, sizeof(bot_ts));
    unsigned wire = (chat_wire != 0u) ? chat_wire : API_CHAT_WIRE_EXTERNAL;
    const char *llm = (llm_model_id && llm_model_id[0]) ? llm_model_id : "external";
    session_push_message_with_source(sess, "assistant", bot_buf, API_SOURCE_CLOUD, bot_ts, wire, llm);

    {
        int ar_e = api_append_turn_phase(ctx, tid, uid, msg, bot_buf, user_ts_use, logic_conflict, llm);
        engine_inc_processed(ctx->engine, 1);
        api_ctx_nl_learn_after_user_turn(ctx, msg, ar_e == 0);
    }
    return 0;
}

static int api_chat_external_reply(api_context_t *ctx,
                            const char *tenant_id, const char *user_id,
                            const char *user_message, const char *assistant_message,
                            const char *user_turn_ts) {
    return api_chat_external_reply_with_meta(ctx, tenant_id, user_id, user_message, assistant_message, user_turn_ts,
                                             NULL, 0u);
}

static int api_set_log_collection(api_context_t *ctx, const char *db, const char *coll) {
    if (!ctx || !db || !coll) return -1;
    storage_ctx_t *storage = engine_get_storage(ctx->engine);
    return storage_set_ai_logs(storage, db, coll);
}

typedef struct {
    api_chat_session_t *session;
} load_history_ud_t;

static void load_history_cb(const char *role, const char *content, const char *ts, void *userdata) {
    load_history_ud_t *ud = (load_history_ud_t *)userdata;
    if (!ud || !ud->session) return;
    session_push_message_with_source(ud->session, role, content, API_SOURCE_MONGODB, ts, API_CHAT_WIRE_NONE, NULL);
}

/* ---- api_greet ---- */

static int greet_parse_condition(const char *s) {
    if (!s || !s[0]) return 0; /* TODAY */
    if (strcasecmp(s, "ALWAYS") == 0) return 1;
    if (strcasecmp(s, "TODAY") == 0) return 0;
    if (strcasecmp(s, "WEEK") == 0) return 2;
    if (strcasecmp(s, "HOUR") == 0) return 3;
    if (strcasecmp(s, "SESSION") == 0) return 4;
    return 0;
}

static int greet_parse_response_type(const char *s) {
    if (!s || !s[0]) return 0; /* CHAT */
    if (strcasecmp(s, "CHAT") == 0) return 0;
    if (strcasecmp(s, "TEMPLATE") == 0) return 1;
    if (strcasecmp(s, "SILENT") == 0) return 2;
    return 0;
}

/* Simple JSON string extraction for greet opts */
static const char *greet_json_str(const char *json, const char *key, char *buf, size_t bufsz) {
    if (!json || !key || !buf || bufsz == 0) return NULL;
    buf[0] = '\0';
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < bufsz - 1) buf[i++] = *p++;
        buf[i] = '\0';
        return buf;
    }
    if (strncmp(p, "null", 4) == 0) return NULL;
    return NULL;
}

/* Extract display_name from context_json for template greeting */
static void greet_extract_name(const char *context_json, char *name, size_t name_sz) {
    if (!name || name_sz == 0) return;
    name[0] = '\0';
    if (!context_json) return;
    char buf[256];
    if (greet_json_str(context_json, "display_name", buf, sizeof(buf)) && buf[0]) {
        snprintf(name, name_sz, "%s", buf);
        return;
    }
    if (greet_json_str(context_json, "name", buf, sizeof(buf)) && buf[0]) {
        snprintf(name, name_sz, "%s", buf);
        return;
    }
    if (greet_json_str(context_json, "email", buf, sizeof(buf)) && buf[0]) {
        snprintf(name, name_sz, "%s", buf);
    }
}

int api_greet(api_context_t *ctx, const char *tenant_id, const char *user_id,
              const char *context_json, const char *greet_opts_json,
              char *reply_out, size_t out_size) {
    if (!ctx || !reply_out || out_size == 0) return -1;
    reply_out[0] = '\0';

    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    const char *uid = (user_id && user_id[0]) ? user_id : API_DEFAULT_USER_ID;

    /* Parse options */
    char cond_str[32], rtype_str[32], custom_prompt[1024];
    cond_str[0] = rtype_str[0] = custom_prompt[0] = '\0';
    if (greet_opts_json && greet_opts_json[0]) {
        greet_json_str(greet_opts_json, "condition", cond_str, sizeof(cond_str));
        greet_json_str(greet_opts_json, "response_type", rtype_str, sizeof(rtype_str));
        greet_json_str(greet_opts_json, "custom_prompt", custom_prompt, sizeof(custom_prompt));
    }
    int condition = greet_parse_condition(cond_str);
    int rtype = greet_parse_response_type(rtype_str);

    m4_log("API", M4_LOG_DEBUG, "api_greet: tenant=%s user=%s condition=%s(%d) response_type=%s(%d) context=%s",
           tid, uid, cond_str[0] ? cond_str : "TODAY", condition,
           rtype_str[0] ? rtype_str : "CHAT", rtype,
           (context_json && context_json[0]) ? "set" : "NULL");

    /* Check condition: should we greet? */
    api_chat_session_t *sess = api_ctx_get_session(ctx, tid, uid, 0); /* don't create */
    if (condition != 1 /* not ALWAYS */) {
        if (condition == 4) { /* SESSION: greet if no session or empty */
            if (sess && sess->ctx_count > 0) {
                m4_log("API", M4_LOG_DEBUG, "api_greet: SESSION condition — session has %d messages, skip", sess->ctx_count);
                return 1;
            }
        } else {
            /* TODAY / WEEK / HOUR: check last activity */
            if (sess && sess->last_activity > 0) {
                time_t now = time(NULL);
                double diff = difftime(now, sess->last_activity);
                int skip = 0;
                if (condition == 0 && diff < 86400) skip = 1;      /* TODAY: < 24h */
                else if (condition == 2 && diff < 604800) skip = 1; /* WEEK: < 7d */
                else if (condition == 3 && diff < 3600) skip = 1;   /* HOUR: < 1h */
                if (skip) {
                    m4_log("API", M4_LOG_DEBUG, "api_greet: condition not met (last_activity %.0fs ago), skip", diff);
                    return 1;
                }
            }
        }
    }

    /* Generate greeting based on response_type */
    if (rtype == 2) { /* SILENT */
        m4_log("API", M4_LOG_DEBUG, "api_greet: SILENT — returning empty");
        return 0;
    }

    if (rtype == 1) { /* TEMPLATE — no LLM */
        char name[256];
        greet_extract_name(context_json, name, sizeof(name));
        if (name[0])
            snprintf(reply_out, out_size, "Chào %s! Hôm nay cần gì?", name);
        else
            snprintf(reply_out, out_size, "Chào bạn! Hôm nay cần gì?");
        m4_log("API", M4_LOG_INFO, "api_greet: TEMPLATE greeting for %s", name[0] ? name : "(unknown)");
        return 0;
    }

    /* rtype == 0: CHAT — use ai_agent with greeting prompt */
    char greet_msg[512];
    if (custom_prompt[0]) {
        snprintf(greet_msg, sizeof(greet_msg), "%s", custom_prompt);
    } else {
        snprintf(greet_msg, sizeof(greet_msg),
                 "[GREETING] The user just opened the chat. "
                 "Greet them warmly based on their context. Keep it short, natural, and in character.");
    }

    /* Build prompt with context */
    char context_buf[API_CONTEXT_BUFFER_SIZE];
    memset(context_buf, 0, sizeof(context_buf));
    sess = api_ctx_get_session(ctx, tid, uid, 1); /* create if needed */
    if (!sess) return -1;
    ctx_build_prompt(ctx, sess, greet_msg, context_json, context_buf, sizeof(context_buf));

    /* Route through ai_agent */
    ai_agent_prompt_t parts;
    ctx_build_prompt_parts(context_buf, sess, greet_msg, NULL, &parts);

    double temperature = 0.7; /* slightly creative for greetings */
    char assistant_source = 0;
    unsigned chat_wire = API_CHAT_WIRE_NONE;
    char llm_label[API_CTX_LLM_SIZE];
    llm_label[0] = '\0';

    if (ai_agent_complete_chat(&parts, context_buf, temperature, NULL, NULL, NULL,
                                reply_out, out_size, &assistant_source, &chat_wire,
                                llm_label, sizeof(llm_label)) != 0) {
        ai_agent_prompt_free(&parts);
        m4_log("API", M4_LOG_WARN, "api_greet: ai_agent failed — falling back to template");
        char name[256];
        greet_extract_name(context_json, name, sizeof(name));
        if (name[0])
            snprintf(reply_out, out_size, "Chào %s! Hôm nay cần gì?", name);
        else
            snprintf(reply_out, out_size, "Chào bạn! Hôm nay cần gì?");
        return 0;
    }
    ai_agent_prompt_free(&parts);

    m4_log("API", M4_LOG_INFO, "api_greet: CHAT greeting via %s reply_len=%zu",
           llm_label[0] ? llm_label : "unknown", strlen(reply_out));

    /* Greeting is ephemeral — not pushed to session or Mongo.
     * Fresh greeting every time based on current user context. */
    return 0;
}

int api_load_chat_history(api_context_t *ctx, const char *tenant_id, const char *user_id,
                          char *json_out, size_t json_out_size) {
    if (!ctx) return -1;
    const char *tid = (tenant_id && tenant_id[0]) ? tenant_id : DEFAULT_TENANT_ID;
    if (!tenant_validate_id(tid)) return -1;
    if (user_id && user_id[0] && !tenant_validate_id(user_id)) return -1;
    const char *uid_for_query = (user_id && user_id[0]) ? user_id : NULL;
    const char *uid_slot = uid_for_query ? uid_for_query : API_SESSION_UID_TENANT_WIDE;

    /* Initialize JSON output */
    if (json_out && json_out_size > 0) json_out[0] = '\0';

    storage_ctx_t *storage = engine_get_storage(ctx->engine);
    if (!storage_mongo_connected(storage)) {
        if (json_out && json_out_size > 2) { json_out[0] = '['; json_out[1] = ']'; json_out[2] = '\0'; }
        return 0;
    }
    api_chat_session_t *s = api_ctx_get_session(ctx, tid, uid_slot, 1);
    if (!s) return -1;
    session_clear(s);
    load_history_ud_t ud = {s};

    int rc;
    if (storage_redis_connected(storage))
        rc = storage_get_chat_history_cached(storage, tid, uid_for_query, ctx->context_batch_size, load_history_cb, &ud);
    else
        rc = storage_get_chat_history(storage, tid, uid_for_query, ctx->context_batch_size, load_history_cb, &ud);

    if (rc != 0) return -1;

    /* Build JSON array from session ring buffer */
    int count = s->ctx_count;
    if (json_out && json_out_size > 2) {
        size_t n = 0;
        json_out[n++] = '[';

        char role[32], content[API_CTX_MSG_SIZE], ts[API_CTX_TS_SIZE], llm[API_CTX_LLM_SIZE];
        char src;

        for (int i = 0; i < count && n < json_out_size - 256; i++) {
            if (api_get_history_message(ctx, i, role, sizeof(role), content, sizeof(content),
                                        &src, ts, sizeof(ts), llm, sizeof(llm)) != 0)
                break;

            if (i > 0 && n < json_out_size - 1) json_out[n++] = ',';

            /* Escape content for JSON */
            size_t clen = strlen(content);
            size_t esc_cap = clen * 4 + 64;
            char *esc = (char *)malloc(esc_cap);
            if (!esc) break;
            /* Simple JSON escape */
            size_t j = 0;
            for (size_t c = 0; c < clen && j < esc_cap - 2; c++) {
                if (content[c] == '"' || content[c] == '\\') { esc[j++] = '\\'; esc[j++] = content[c]; }
                else if (content[c] == '\n') { esc[j++] = '\\'; esc[j++] = 'n'; }
                else if (content[c] == '\r') { esc[j++] = '\\'; esc[j++] = 'r'; }
                else esc[j++] = content[c];
            }
            esc[j] = '\0';

            int w = snprintf(json_out + n, json_out_size - n,
                             "{\"role\":\"%s\",\"content\":\"%s\",\"source\":\"%c\",\"timestamp\":\"%s\",\"llm_model\":\"%s\"}",
                             role, esc, src ? src : '?', ts, llm);
            free(esc);
            if (w > 0 && n + (size_t)w < json_out_size)
                n += (size_t)w;
            else
                break;
        }

        if (n < json_out_size - 1) json_out[n++] = ']';
        json_out[n] = '\0';
    }

    return count;
}

static int api_get_history_count(api_context_t *ctx) {
    api_chat_session_t *s = api_ctx_current_session(ctx);
    return s ? s->ctx_count : 0;
}

static int api_get_history_message(api_context_t *ctx, int index,
                            char *role_buf, size_t role_size,
                            char *content_buf, size_t content_size,
                            char *source_out, char *ts_buf, size_t ts_size,
                            char *llm_model_out, size_t llm_model_cap) {
    api_chat_session_t *s = api_ctx_current_session(ctx);
    if (!s || index < 0 || index >= s->ctx_count) return -1;
    int idx = (s->ctx_head + index) % s->ctx_capacity;
    if (role_buf && role_size > 0) {
        size_t n = strnlen(s->ctx_roles + idx * API_CTX_ROLE_SIZE, API_CTX_ROLE_SIZE - 1);
        if (n >= role_size) n = role_size - 1;
        memcpy(role_buf, s->ctx_roles + idx * API_CTX_ROLE_SIZE, n);
        role_buf[n] = '\0';
    }
    if (content_buf && content_size > 0) {
        size_t n = strnlen(s->ctx_messages + idx * API_CTX_MSG_SIZE, API_CTX_MSG_SIZE - 1);
        if (n >= content_size) n = content_size - 1;
        memcpy(content_buf, s->ctx_messages + idx * API_CTX_MSG_SIZE, n);
        content_buf[n] = '\0';
    }
    if (source_out && s->ctx_sources) *source_out = s->ctx_sources[idx];
    if (ts_buf && ts_size > 0 && s->ctx_timestamps) {
        size_t n = strnlen(s->ctx_timestamps + idx * API_CTX_TS_SIZE, API_CTX_TS_SIZE - 1);
        if (n >= ts_size) n = ts_size - 1;
        memcpy(ts_buf, s->ctx_timestamps + idx * API_CTX_TS_SIZE, n);
        ts_buf[n] = '\0';
    }
    if (llm_model_out && llm_model_cap > 0) {
        llm_model_out[0] = '\0';
        if (s->ctx_llm) {
            const char *slot = s->ctx_llm + idx * API_CTX_LLM_SIZE;
            size_t n = strnlen(slot, API_CTX_LLM_SIZE - 1);
            if (n >= llm_model_cap) n = llm_model_cap - 1;
            if (n > 0) memcpy(llm_model_out, slot, n);
            llm_model_out[n] = '\0';
        }
    }
    return 0;
}

static char api_get_last_reply_source(api_context_t *ctx) {
    api_chat_session_t *s = api_ctx_current_session(ctx);
    return (s && s->last_reply_source) ? s->last_reply_source : 0;
}

static unsigned api_get_last_chat_wire(api_context_t *ctx) {
    api_chat_session_t *s = api_ctx_current_session(ctx);
    return s ? s->last_chat_wire : API_CHAT_WIRE_NONE;
}

static size_t api_get_last_llm_model(api_context_t *ctx, char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    api_chat_session_t *s = api_ctx_current_session(ctx);
    if (!s || !s->last_llm_model[0])
        return 0;
    size_t n = strnlen(s->last_llm_model, sizeof(s->last_llm_model) - 1);
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, s->last_llm_model, n);
    out[n] = '\0';
    return n + 1;
}

static int api_embed_text(api_context_t *ctx, const char *text, const char *model_pref,
                   float *vec_out, size_t vec_max_dim, size_t *dim_out,
                   char *model_id_out, size_t model_id_out_sz) {
    if (!ctx || !ctx->engine || !vec_out || !dim_out || vec_max_dim == 0 || !model_id_out || model_id_out_sz == 0)
        return -1;
    m4_embed_options_t o;
    m4_embed_options_from_engine(engine_get_config(ctx->engine), &o);
    if (model_pref && model_pref[0])
        o.ollama_model_pref = model_pref;
    return m4_embed_text(&o, text ? text : "", vec_out, vec_max_dim, dim_out, model_id_out, model_id_out_sz);
}

static size_t api_get_geo_atlas_landmarks(api_context_t *ctx, char *out, size_t out_size) {
    if (!ctx || !out || out_size == 0) return 0;
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    if (!st) return 0;
    return storage_geo_atlas_get_landmarks_for_prompt(st, out, out_size);
}

/* ---- Geo batch import ---- */

#define GEO_BATCH_DEFAULT_SIZE    100
#define GEO_BATCH_DEFAULT_TIMEOUT 5

static void geo_batch_flush_locked(api_context_t *ctx) {
    /* Caller holds ctx->geo_batch.lock */
    if (ctx->geo_batch.count == 0) return;
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    if (!st) return;

    int flushed = 0;
    for (int i = 0; i < ctx->geo_batch.count; i++) {
        storage_geo_atlas_doc_t *d = &ctx->geo_batch.items[i];
        int ret = storage_geo_atlas_insert_doc(st, d);
        if (ret == 0) {
            flushed++;
            if (d->vector && d->vector_dim > 0 && storage_redis_connected(st)) {
                const char *nn = d->name_normalized ? d->name_normalized : d->name;
                char doc_id[192];
                snprintf(doc_id, sizeof(doc_id), "geo_csv_%s_%lld_%d", nn,
                         (long long)(time(NULL) * 1000LL), i);
                (void)storage_geo_redis_index_landmark(st, d->tenant_id, doc_id,
                                                       d->vector, d->vector_dim, d->name);
            }
        }
        /* Free copied data */
        free(ctx->geo_batch.string_bufs[i]);
        free(ctx->geo_batch.vector_bufs[i]);
    }
    m4_log("API", M4_LOG_INFO, "geo_batch: flushed %d/%d rows to Mongo", flushed, ctx->geo_batch.count);
    ctx->geo_batch.count = 0;
}

static void *geo_batch_worker(void *arg) {
    api_context_t *ctx = (api_context_t *)arg;
    m4_log("API", M4_LOG_DEBUG, "geo_batch: worker started (batch_size=%d timeout=%ds)",
           ctx->geo_batch.batch_size, ctx->geo_batch.flush_timeout_sec);

    while (!atomic_load(&ctx->geo_batch.stop)) {
        pthread_mutex_lock(&ctx->geo_batch.lock);

        /* Wait for: batch full OR timeout OR stop signal */
        while (ctx->geo_batch.count < ctx->geo_batch.batch_size &&
               !atomic_load(&ctx->geo_batch.stop)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ctx->geo_batch.flush_timeout_sec;
            int rc = pthread_cond_timedwait(&ctx->geo_batch.cond, &ctx->geo_batch.lock, &ts);
            if (rc != 0) break; /* timeout or error → check and flush */
        }

        if (ctx->geo_batch.count > 0)
            geo_batch_flush_locked(ctx);

        pthread_mutex_unlock(&ctx->geo_batch.lock);
    }

    /* Final drain on shutdown */
    pthread_mutex_lock(&ctx->geo_batch.lock);
    if (ctx->geo_batch.count > 0) {
        m4_log("API", M4_LOG_INFO, "geo_batch: draining %d remaining rows on shutdown", ctx->geo_batch.count);
        geo_batch_flush_locked(ctx);
    }
    pthread_mutex_unlock(&ctx->geo_batch.lock);

    m4_log("API", M4_LOG_DEBUG, "geo_batch: worker stopped");
    return NULL;
}

static void geo_batch_init(api_context_t *ctx) {
    ctx->geo_batch.batch_size = GEO_BATCH_DEFAULT_SIZE;
    ctx->geo_batch.flush_timeout_sec = GEO_BATCH_DEFAULT_TIMEOUT;
    ctx->geo_batch.cap = GEO_BATCH_DEFAULT_SIZE * 2;
    ctx->geo_batch.items = (storage_geo_atlas_doc_t *)calloc((size_t)ctx->geo_batch.cap, sizeof(storage_geo_atlas_doc_t));
    ctx->geo_batch.string_bufs = (char **)calloc((size_t)ctx->geo_batch.cap, sizeof(char *));
    ctx->geo_batch.vector_bufs = (float **)calloc((size_t)ctx->geo_batch.cap, sizeof(float *));
    ctx->geo_batch.count = 0;
    ctx->geo_batch.last_push = 0;
    atomic_init(&ctx->geo_batch.stop, 0);
    pthread_mutex_init(&ctx->geo_batch.lock, NULL);
    pthread_cond_init(&ctx->geo_batch.cond, NULL);

    if (pthread_create(&ctx->geo_batch.thread, NULL, geo_batch_worker, ctx) == 0) {
        ctx->geo_batch.thread_started = 1;
    } else {
        m4_log("API", M4_LOG_WARN, "geo_batch: worker thread failed to start — falling back to sync insert");
    }
}

static void geo_batch_destroy(api_context_t *ctx) {
    if (ctx->geo_batch.thread_started) {
        atomic_store(&ctx->geo_batch.stop, 1);
        pthread_cond_signal(&ctx->geo_batch.cond);
        pthread_join(ctx->geo_batch.thread, NULL);
        ctx->geo_batch.thread_started = 0;
    }
    /* Free any remaining items (shouldn't have any after drain) */
    for (int i = 0; i < ctx->geo_batch.count; i++) {
        free(ctx->geo_batch.string_bufs[i]);
        free(ctx->geo_batch.vector_bufs[i]);
    }
    free(ctx->geo_batch.items);
    free(ctx->geo_batch.string_bufs);
    free(ctx->geo_batch.vector_bufs);
    pthread_mutex_destroy(&ctx->geo_batch.lock);
    pthread_cond_destroy(&ctx->geo_batch.cond);
}

/* Deep-copy a geo doc row into the batch queue (all strings strdup'd, vector copied) */
static int geo_batch_push(api_context_t *ctx, const storage_geo_atlas_doc_t *doc) {
    pthread_mutex_lock(&ctx->geo_batch.lock);

    /* Grow if needed */
    if (ctx->geo_batch.count >= ctx->geo_batch.cap) {
        int new_cap = ctx->geo_batch.cap * 2;
        storage_geo_atlas_doc_t *ni = (storage_geo_atlas_doc_t *)realloc(
            ctx->geo_batch.items, (size_t)new_cap * sizeof(storage_geo_atlas_doc_t));
        char **ns = (char **)realloc(ctx->geo_batch.string_bufs, (size_t)new_cap * sizeof(char *));
        float **nv = (float **)realloc(ctx->geo_batch.vector_bufs, (size_t)new_cap * sizeof(float *));
        if (!ni || !ns || !nv) {
            pthread_mutex_unlock(&ctx->geo_batch.lock);
            return -1;
        }
        ctx->geo_batch.items = ni;
        ctx->geo_batch.string_bufs = ns;
        ctx->geo_batch.vector_bufs = nv;
        ctx->geo_batch.cap = new_cap;
    }

    int idx = ctx->geo_batch.count;

    /* Deep-copy all strings into one block for easy cleanup */
    size_t total = 0;
    const char *fields[] = {
        doc->tenant_id, doc->name, doc->name_normalized, doc->district,
        doc->axis, doc->category, doc->city, doc->embed_model_id,
        doc->source, doc->verification_status
    };
    for (int f = 0; f < 10; f++)
        total += (fields[f] ? strlen(fields[f]) : 0) + 1;

    char *buf = (char *)malloc(total);
    if (!buf) { pthread_mutex_unlock(&ctx->geo_batch.lock); return -1; }
    ctx->geo_batch.string_bufs[idx] = buf;

    /* Copy each field into the block */
    storage_geo_atlas_doc_t *d = &ctx->geo_batch.items[idx];
    *d = *doc; /* copy struct (pointers will be overwritten below) */

#define COPY_FIELD(field) do { \
    if (doc->field && doc->field[0]) { \
        size_t l = strlen(doc->field); \
        memcpy(buf, doc->field, l + 1); \
        d->field = buf; buf += l + 1; \
    } else { d->field = ""; } \
} while(0)
    COPY_FIELD(tenant_id);
    COPY_FIELD(name);
    COPY_FIELD(name_normalized);
    COPY_FIELD(district);
    COPY_FIELD(axis);
    COPY_FIELD(category);
    COPY_FIELD(city);
    COPY_FIELD(embed_model_id);
    COPY_FIELD(source);
    COPY_FIELD(verification_status);
#undef COPY_FIELD

    /* Copy vector */
    if (doc->vector && doc->vector_dim > 0) {
        float *vc = (float *)malloc(doc->vector_dim * sizeof(float));
        if (vc) memcpy(vc, doc->vector, doc->vector_dim * sizeof(float));
        d->vector = vc;
        ctx->geo_batch.vector_bufs[idx] = vc;
    } else {
        d->vector = NULL;
        ctx->geo_batch.vector_bufs[idx] = NULL;
    }

    ctx->geo_batch.count++;
    ctx->geo_batch.last_push = time(NULL);

    /* Signal worker if batch is full */
    if (ctx->geo_batch.count >= ctx->geo_batch.batch_size)
        pthread_cond_signal(&ctx->geo_batch.cond);

    pthread_mutex_unlock(&ctx->geo_batch.lock);
    return 0;
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

    /* Use batch queue if worker is running, otherwise sync insert */
    if (ctx->geo_batch.thread_started)
        return geo_batch_push(ctx, &d);

    /* Fallback: sync insert (worker failed to start) */
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    if (!st) return -1;
    return storage_geo_atlas_insert_doc(st, &d);
}

static int api_geo_authority_load_csv(api_context_t *ctx, const char *csv_utf8) {
    if (!ctx || !csv_utf8) return -1;
    const engine_config_t *cfg = engine_get_config(ctx->engine);
    if (!cfg || !cfg->geo_authority_enabled) return -1;
    return geo_authority_load_buffer(csv_utf8);
}

static int api_geo_atlas_migrate_legacy(api_context_t *ctx, unsigned long long *modified_out) {
    if (!ctx)
        return -1;
    storage_ctx_t *st = engine_get_storage(ctx->engine);
    if (!st)
        return -1;
    return storage_geo_atlas_migrate_legacy(st, modified_out);
}

static int api_embed_migration_enqueue(api_context_t *ctx, const char *tenant_id, unsigned flags) {
    if (!ctx || !ctx->engine || flags == 0) return -1;
    return engine_embed_migration_enqueue(ctx->engine, tenant_id, flags);
}

static int api_query(api_context_t *ctx, const char *prompt, char *out, size_t out_size) {
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

/* ---- Background health check ---- */

#define HEALTH_CHECK_DEFAULT_INTERVAL 10

static void *health_check_worker(void *arg) {
    api_context_t *ctx = (api_context_t *)arg;
    m4_log("API", M4_LOG_DEBUG, "health_check: worker started (interval=%ds)", ctx->health.interval_sec);

    while (!atomic_load(&ctx->health.stop)) {
        const engine_config_t *cfg = engine_get_config(ctx->engine);

        /* Ollama check */
        ctx->health.ollama_connected =
            (ollama_check_running(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT) == 0) ? 1 : 0;

        /* ELK check */
        int elk_on = (cfg && cfg->es_host && cfg->es_host[0]) ? 1 : 0;
        if (elk_on)
            ctx->health.elk_connected =
                (elasticsearch_check_reachable(cfg->es_host, cfg->es_port) == 0) ? 1 : 0;
        else
            ctx->health.elk_connected = 0;

        ctx->health.last_check = time(NULL);

        /* Sleep in 1s increments so we can stop quickly */
        for (int s = 0; s < ctx->health.interval_sec && !atomic_load(&ctx->health.stop); s++)
            sleep(1);
    }

    m4_log("API", M4_LOG_DEBUG, "health_check: worker stopped");
    return NULL;
}

static void health_check_init(api_context_t *ctx) {
    ctx->health.interval_sec = HEALTH_CHECK_DEFAULT_INTERVAL;
    ctx->health.ollama_connected = 0;
    ctx->health.elk_connected = 0;
    ctx->health.last_check = 0;
    atomic_init(&ctx->health.stop, 0);

    if (pthread_create(&ctx->health.thread, NULL, health_check_worker, ctx) == 0) {
        ctx->health.thread_started = 1;
    } else {
        m4_log("API", M4_LOG_WARN, "health_check: worker failed to start — stats will use sync checks");
    }
}

static void health_check_destroy(api_context_t *ctx) {
    if (ctx->health.thread_started) {
        atomic_store(&ctx->health.stop, 1);
        pthread_join(ctx->health.thread, NULL);
        ctx->health.thread_started = 0;
    }
}

void api_get_stats(api_context_t *ctx, api_stats_t *out) {
    if (!ctx || !out) return;
    memset(out, 0, sizeof(*out));

    storage_ctx_t *storage = engine_get_storage(ctx->engine);
    const engine_config_t *cfg = engine_get_config(ctx->engine);

    stat_set_mongo_connected(ctx->stat, storage_mongo_connected(storage));
    stat_set_redis_connected(ctx->stat, storage_redis_connected(storage));

    int elk_on = (cfg && cfg->es_host && cfg->es_host[0]) ? 1 : 0;
    stat_set_elk_enabled(ctx->stat, elk_on);
    /* Use cached health check results (non-blocking) */
    stat_set_elk_connected(ctx->stat, ctx->health.elk_connected);

    uint64_t processed = 0, errors = 0;
    engine_get_stats(ctx->engine, &processed, &errors);
    stat_set_processed(ctx->stat, processed);
    stat_set_errors(ctx->stat, errors);

    size_t n_sess = ctx->sessions ? m4_ht_count(ctx->sessions) : 0;
    uint64_t mem_est = (uint64_t)n_sess * (uint64_t)ctx->ring_capacity *
                       (uint64_t)(API_CTX_MSG_SIZE + API_CTX_ROLE_SIZE + API_CTX_TS_SIZE + API_CTX_LLM_SIZE + 4u);
    stat_set_memory_bytes(ctx->stat, mem_est);

    stat_snapshot_t snap;
    stat_get_snapshot(ctx->stat, &snap);
    out->memory_bytes = snap.memory_bytes;
    out->mongo_connected = snap.mongo_connected;
    out->redis_connected = snap.redis_connected;
    out->elk_enabled = snap.elk_enabled;
    if (ctx->health.thread_started) {
        out->elk_connected = ctx->health.elk_connected;
        out->ollama_connected = ctx->health.ollama_connected;
    } else {
        /* MEMORY mode — no external services */
        out->elk_connected = 0;
        out->ollama_connected = 0;
    }
    out->error_count = snap.error_count;
    out->warning_count = snap.warning_count;
    out->processed = snap.processed;
    out->errors = snap.errors;
#ifdef USE_MONGOC
    out->mongoc_linked = 1;
#else
    out->mongoc_linked = 0;
#endif

    /* Reply metadata from last active session */
    api_chat_session_t *last_s = api_ctx_current_session(ctx);
    out->last_reply_source = (last_s && last_s->last_reply_source) ? last_s->last_reply_source : 0;
    out->last_chat_wire = last_s ? last_s->last_chat_wire : API_CHAT_WIRE_NONE;
    out->last_llm_model[0] = '\0';
    if (last_s && last_s->last_llm_model[0]) {
        size_t n = strlen(last_s->last_llm_model);
        if (n >= sizeof(out->last_llm_model)) n = sizeof(out->last_llm_model) - 1;
        memcpy(out->last_llm_model, last_s->last_llm_model, n);
        out->last_llm_model[n] = '\0';
    }
}

const char *api_build_ollama_default_host(void) {
    return OLLAMA_DEFAULT_HOST;
}

int api_build_ollama_default_port(void) {
    return OLLAMA_DEFAULT_PORT;
}

const char *api_build_ollama_default_chat_model(void) {
    return OLLAMA_DEFAULT_MODEL;
}

const char *api_build_ollama_default_embed_model(void) {
    return OLLAMA_DEFAULT_EMBED_MODEL;
}

int api_build_ollama_embed_max_dim(void) {
    return (int)OLLAMA_EMBED_MAX_DIM;
}
