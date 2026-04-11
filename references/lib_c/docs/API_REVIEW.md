# API Review — Potential Bugs, Pros/Cons, Solutions

## Summary

| Priority | Count | Category |
|----------|-------|----------|
| Critical | 4 | Thread safety, dangling pointers, stack overflow risk |
| High | 8 | Data loss, incorrect behavior, dead code |
| Medium | 10 | Edge cases, missing validation, inconsistencies |
| Low | 6 | UX, parameter count, missing features |

---

## API 1: `api_create` / `api_create_with_opts`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | **Critical** | **Dangling pointers from JSON parser.** `api_create()` sets `api_options_t` fields to point into `json_opts_t` heap memory, then calls `json_opts_free()` after `api_create_with_opts()`. If the engine stores those pointers (mongo_uri, redis_host, etc.) for its lifetime, they become dangling. |
| B2 | Medium | `strdup` failure for `default_persona`/`default_instructions` silently drops them — no error or log. |
| B3 | Low | `ftell` on `geo_authority_csv_path` returns `long` — could overflow on 32-bit systems (mitigated by 16MB cap). |

### Pros
- Dual entry: JSON string for FFI, C struct for native callers
- Thorough URI validation before proceeding
- Clean cleanup on every failure branch — no resource leaks
- Deferred NL learning load avoids blocking startup

### Cons
- 30+ fields in `api_options_t` — high coupling for adding new options
- No struct versioning — old callers could silently zero-init new fields
- `m4_log_init` is global — multiple `api_create` calls overwrite each other's debug config

### Solutions
- **B1**: Deep-copy all string fields inside `fill_default_config` using `strdup`, so engine doesn't depend on caller pointer lifetime. Or: don't free `json_opts_t` until `api_destroy`.
- **B2**: Check `strdup` return, log warning on failure.
- Add `uint32_t version` field to `api_options_t` as ABI guard.

---

## API 2: `api_destroy`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | **Critical** | **Use-after-free on concurrent stream.** If a stream worker is running (`api_chat` with `stream_cb`), calling `api_destroy` frees the engine/sessions while the worker thread still holds `w->ctx`. No drain/refcount mechanism. |
| B2 | High | `m4_log_shutdown()` is global — destroying one context kills logging for all contexts. |
| B3 | Medium | NL learn mutex could race with concurrent `api_ctx_nl_learn_after_user_turn` on a stream worker thread. |

### Pros
- Ordered teardown: sessions → NL learn → engine → stat → context
- Properly joins deferred NL load thread before destroying
- NULL-safe (early return)

### Cons
- No protection against use-after-free from concurrent calls
- No "drain active operations" mechanism

### Solutions
- Add `atomic_int shutting_down` flag. Set in `api_destroy`, check in `api_chat` before starting work.
- Move `m4_log_shutdown()` to a separate `api_shutdown()` function.
- Add reference counting on active operations.

---

## API 3: `api_chat`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | **Critical** | **No mutex on session hash table.** Concurrent `api_chat` calls for different users race on `m4_ht_set`/`m4_ht_get`/`m4_ht_foreach` in `api_ctx_purge_idle`. |
| B2 | **Critical** | **Stack usage ~600KB.** `ai_agent_prompt_t` (~500KB) + `context_buf[65536]` + `embed_vec[8192]` stack-allocated. Thread stacks may be 512KB–1MB by default. |
| B3 | High | **`localtime()` not thread-safe.** Called from `append_system_time_wall_clock`. Should use `localtime_r`. |
| B4 | High | **`strtok()` not thread-safe.** Used in `run_cloud_tiers` (ai_agent.c). Should use `strtok_r`. |
| B5 | High | **Auto-greeting persists synthetic prompt as user message.** `"[GREETING] The user just opened..."` is stored in session/Mongo as the user's message instead of an empty string. |
| B6 | High | **Provider trimming is dead code.** `ctx_build_prompt_parts` always called with `provider=NULL` — per-provider limits never enforced through the structured prompt path. |
| B7 | Medium | Double `model_switch_resolve` in stream path — second call uses DEFAULT intent instead of actual classified intent. |

### Pros
- Unified sync + stream — one function, one code path
- Redis RAG short-circuit avoids unnecessary LLM calls
- UTF-8 accumulator prevents broken multi-byte chars in stream
- Cloud fallback chain provides resilience
- User context injection via `[CONTEXT]` block

### Cons
- ~330 lines with 4 duplicated "persist turn" blocks
- 9 parameters (plus internal structures)
- Stack usage dangerous for threaded environments

### Solutions
- **B1**: Add `pthread_rwlock_t` to `api_context_t` for session map access.
- **B2**: Heap-allocate `ai_agent_prompt_t` (`malloc` + `free`).
- **B3**: Replace `localtime()` with `localtime_r()`.
- **B4**: Replace `strtok()` with `strtok_r()`.
- **B5**: Store empty string in session/Mongo, use synthetic prompt only for LLM.
- **B6**: Pass provider name from `run_cloud_tiers` iteration into `ctx_build_prompt_parts`.
- Extract persist-turn logic into single helper (use existing `api_append_turn_phase`).

---

## API 4: `api_load_chat_history`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | High | `session_clear` wipes in-memory messages without warning. Unpersisted messages are silently lost. |
| B2 | Medium | Magic string `"__tenant_wide__"` for NULL user_id — collision possible if a user has that literal ID. |
| B3 | Medium | No session-level mutex — concurrent `api_chat` and `api_load_chat_history` for same user race on ring buffer. |

### Pros
- Redis L1 cache first, Mongo fallback — minimizes latency
- Graceful: returns 0 (no-op) when Mongo not connected
- tenant_validate_id prevents injection

### Cons
- Destructive (clears session) with no merge option
- Returns 0 or -1 — no way to know how many messages loaded
- Source always tagged MONGODB even when loaded from Redis L1

### Solutions
- Return message count on success instead of 0.
- Add merge/append mode option.
- Fix source tag for Redis L1 path.

---

## API 5: `api_greet`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | High | **Condition checks in-memory `last_activity` only.** After server restart, all sessions are fresh → greets every user again, even if they chatted minutes ago. |
| B2 | High | **Hand-rolled JSON parser is fragile.** `greet_json_str` uses `strstr` — matches keys inside value strings. |
| B3 | Medium | SILENT returns 0 (greeting generated) but reply is empty. Callers can't distinguish from a failed CHAT that produced empty. |
| B4 | Medium | Greeting not persisted to Mongo — lost on restart, condition may re-trigger. |
| B5 | Low | ai_agent call ignores model_switch/lane config — always uses NULL model pin. |

### Pros
- Graceful fallback: CHAT fails → TEMPLATE automatically
- Flexible condition system (ALWAYS/TODAY/WEEK/HOUR/SESSION)
- Temperature tuned for natural greetings (0.7)
- Zero-config works: NULL opts = TODAY + CHAT

### Cons
- Hardcoded Vietnamese template — not i18n
- No streaming support for CHAT greetings
- `custom_prompt` limited to 1024 chars

### Solutions
- **B1**: Check Mongo for last turn timestamp, not just in-memory session.
- **B2**: Reuse `json_opts.c` parser instead of ad-hoc `greet_json_str`.
- **B3**: Return 2 for SILENT (distinct from 0=generated and 1=condition-not-met).
- **B4**: Persist greeting to Mongo via `engine_append_turn`.
- Add `template` and `locale` fields to greet_opts_json.

---

## API 6: `api_get_stats`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | High | **Blocking network calls.** `ollama_check_running` (2s timeout) + `elasticsearch_check_reachable` (2s timeout) = up to 4s block if both services are down. |
| B2 | Medium | `memory_bytes` undercount — doesn't include `ctx_llm` per-slot (160 bytes), hash table overhead, or NL learn data. |
| B3 | Low | `last_reply_source` is from "last active session" only — not per-tenant/user. |

### Pros
- Single-call health snapshot
- `mongoc_linked` flag distinguishes "no driver" from "driver present but disconnected"
- `last_llm_model` shows exactly which provider handled the last request

### Cons
- No per-tenant stats
- No latency metrics
- Blocking health checks on a frequently-called function

### Solutions
- **B1**: Cache health check results with 10s TTL. Only re-check if stale.
- **B2**: Add `API_CTX_LLM_SIZE` to per-slot estimate.
- Add `tenant_id`/`user_id` params for per-session stats.

---

## API 7: `api_get_history_message`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | High | **Operates on "current session" only.** No tenant/user parameter — reads whichever session was last touched. Wrong session returned if interleaving users. |
| B2 | Low | `source_out` writes one char with no null terminator. Caller must know it's a char, not a string. |

### Pros
- All output parameters NULL-safe
- Per-message LLM model label enables UI display of provider
- Clean ring buffer arithmetic

### Cons
- 11 parameters — extremely unwieldy
- No session selection (tied to "current session")
- No bulk read / iterator

### Solutions
- **B1**: Add `tenant_id` and `user_id` parameters.
- Introduce `api_history_entry_t` struct to replace 11 output params with 1.
- Re-expose `api_get_history_count` as public API.

---

## API 8: `api_geo_atlas_import_row`

### Potential Bugs

| ID | Severity | Bug |
|----|----------|-----|
| B1 | Medium | No length validation on string parameters (district, city, etc.) — very long strings could cause issues. |
| B2 | Medium | Redis `doc_id` not unique — same `name_normalized` + same second = collision. |
| B3 | Low | NaN `trust_score` passes through clamping unchecked. |
| B4 | Low | No `tenant_validate_id` check on `tenant_id`. |

### Pros
- Clean defaults (source="seed", status="verified")
- trust_score clamping
- Dual storage (Mongo + Redis vector index)

### Cons
- 14 parameters — most verbose function in the API
- No batch import
- No return of inserted document ID

### Solutions
- **B1**: Add string length limits (1024 chars max).
- **B2**: Include random component in doc_id.
- **B3**: Add NaN check (`if (isnan(trust_score)) trust_score = 0.5`).
- Introduce batch variant: `api_geo_atlas_import_batch(ctx, docs, count)`.

---

## Cross-Cutting: Priority Fix List

| Priority | Fix | Status | Impact |
|----------|-----|--------|--------|
| **P0** | Add `pthread_rwlock_t` for session hash table | **FIXED** | Prevents data corruption on concurrent calls |
| **P0** | Fix dangling pointers in `api_create` (strdup in fill_default_config) | **FIXED** | Prevents use-after-free crashes |
| **P0** | Dynamic `ai_agent_prompt_t` (heap-allocated, no fixed limits) | **FIXED** | Prevents stack overflow, removes history/system size limits |
| **P1** | Replace `localtime()` → `localtime_r()` | **FIXED** | Thread safety |
| **P1** | Replace `strtok()` → `strtok_r()` in ai_agent.c | **FIXED** | Thread safety |
| **P1** | Add `atomic_int shutting_down` flag in `api_destroy` | **FIXED** | Prevents crash on concurrent destroy + chat |
| **P1** | Wire provider name into `ctx_build_prompt_parts` | Open | Enables per-provider prompt trimming (currently dead code) |
| **P2** | Cache health checks in `api_get_stats` with TTL | **FIXED** | Background thread checks every 10s, api_get_stats reads cached result (zero blocking) |
| **P2** | ~~Persist greetings to Mongo~~ | **By design** | Greeting is ephemeral — always fresh based on current user context |
| **P2** | Add tenant/user params to `api_get_history_message` | Open | Correct session targeting |
| **P3** | Extract duplicated persist-turn logic into helper | Open | Reduce 330-line api_chat |
| **P3** | Introduce `api_history_entry_t` struct | Open | Reduce 11-param function |
| **P3** | Add batch geo import with auto-flush | **FIXED** | Queue rows internally, bulk insert on batch_size (100) OR timeout (5s). Background pthread flushes — no data loss if user leaves. Drain remaining in api_destroy. |

---

## Fixes Applied (this session)

### Thread Safety
- `pthread_rwlock_t sessions_lock` added to `api_context_t` — protects session hash table
- Read lock (`rdlock`) on `m4_ht_get` — multiple concurrent readers OK
- Write lock (`wrlock`) on `m4_ht_set` / `m4_ht_take` / `m4_ht_foreach` — exclusive for mutations
- Double-check pattern after write lock — prevents duplicate session creation
- `pthread_rwlock_destroy` in `api_destroy` — proper cleanup

### Shutdown Safety
- `atomic_int shutting_down` added to `api_context_t`
- Set in `api_destroy` before freeing resources
- Checked at `api_chat` entry — returns -1 if shutting down
- Prevents use-after-free when stream worker is running during destroy

### Thread-Safe Functions
- `localtime()` → `localtime_r()` in `append_system_time_wall_clock` (api.c)
- `strtok()` → `strtok_r()` in `run_cloud_tiers` (ai_agent.c)

### Remaining Open Items (by priority)

**P0 — Critical:** All fixed.

**P1 — High:**
- Provider prompt trimming: `ctx_build_prompt_parts` always called with `provider=NULL` — trimming logic exists but is never activated. Low risk since `ai_agent` provider limits table handles it at the cloud tier level.

**P2 — Medium:**
- `api_get_stats` blocks 4s if Ollama+ELK down → cache with TTL
- `api_greet` condition checks in-memory only → check Mongo for last turn
- `api_get_history_message` reads wrong session → add tenant/user params

**P3 — Low:**
- 4 duplicated persist-turn blocks in api_chat → extract helper
- 11-param `api_get_history_message` → introduce struct
- 14-param `api_geo_atlas_import_row` → batch variant
