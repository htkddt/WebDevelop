# Intent Routing — Data-Aware Chat Pipeline

## Overview

Two operating modes based on data maturity:

```
EARLY STAGE (current — little learning data):
  Phase 1: GATE → Phase 2: LLM EXTRACT → Phase 3: EXECUTE → Phase 4: FORMAT + LEARN
  
MATURE STAGE (future — enough learned patterns):
  Phase 1: CLASSIFY (scores) → Phase 3: EXECUTE → Phase 4: FORMAT
  (Phase 2 LLM skipped for known patterns)
```

### Flow: parallel foreground + background learning

The response is **never blocked** by LLM extraction. Phase 2 runs in the **background** after the response is sent — same pattern as `geo_learning` (enqueue turn → async worker → save results for future turns).

```
User: "how many computers sold this year?"
  │
  ├─── FOREGROUND (immediate, user sees response fast) ──────────────┐
  │                                                                   │
  │  Phase 1: CLASSIFY — use current scores + cue detection           │
  │    scores say ELK_ANALYTICS, collection=products (best guess)     │
  │    Cost: ~0ms                                                     │
  │                                                                   │
  │  Phase 3: EXECUTE — ELK query with current best guess             │
  │    May be imperfect (wrong collection, no time filter)            │
  │    Cost: ~50-100ms                                                │
  │                                                                   │
  │  Phase 4: FORMAT — inject [DATA_RESULT] → LLM → response         │
  │    User gets answer immediately                                   │
  │                                                                   │
  └───────────────────────────────────────────────────────────────────┘
  │
  ├─── BACKGROUND (async, after response sent) ──────────────────────┐
  │                                                                   │
  │  Phase 2: LLM EXTRACT — enqueue to background worker              │
  │    Worker calls LLM with question + collection schemas            │
  │    LLM returns: {"collection":"carts","operation":"count",        │
  │                   "filters":[{"field":"created_at",               │
  │                               "op":"gte","value":"2026-01-01"}]}  │
  │                                                                   │
  │  LEARN: record LLM's decision into cue store                      │
  │    "sold" → SC:carts +1 (LLM chose carts, not products)          │
  │    "computers" → SC:carts +1                                      │
  │    "how many" → ELK_ANALYTICS +1                                  │
  │    WAL append (O(1), non-blocking)                                │
  │                                                                   │
  └───────────────────────────────────────────────────────────────────┘
```

**Result over time:**

```
Turn 1:  "how many computers sold?"
         Foreground: products (wrong) → count=0 → "No data found"
         Background: LLM says carts → learn "sold"→SC:carts

Turn 2:  "how many computers sold?"  
         Foreground: SC:carts now has score → routes to carts → count=42 → "42 sold"
         Background: LLM confirms carts → reinforce

Turn 10: "how many phones sold?"
         Foreground: "sold"→SC:carts score=10 → routes to carts immediately → accurate
         Background: LLM confirms → reinforce
         
Turn 50: patterns well established, background LLM rarely adds new info
```

**First turn may be imperfect** — that's the tradeoff for zero blocking. By turn 2-3 of the same pattern, the foreground path is already accurate from background learning.

### Mature stage (future optimization)

Once learning data is rich enough, the background LLM extraction can be **skipped** for known patterns — saving the LLM call entirely.

```
Transition criteria:
  - SC:* scores for top collections > N (e.g. 50+)
  - Intent scores for main patterns > M (e.g. 100+)
  - Configurable: M4_INTENT_ROUTE_SKIP_LLM_THRESHOLD env var

When mature:
  Foreground: scores resolve everything → accurate, ~0ms
  Background: LLM skipped (scores already high confidence)
  → Zero LLM overhead for data questions
```

### Performance comparison

| Stage | Foreground cost | Background cost | Response time |
|-------|----------------|-----------------|---------------|
| Early (few turns) | ~50-100ms (ELK) | ~0.5-2s (LLM, async) | **Same as chat** |
| Growing (10+ turns) | ~50-100ms (ELK, accurate) | ~0.5-2s (LLM, async) | **Same as chat** |
| Mature (100+ turns) | ~50-100ms (ELK, accurate) | skipped | **Same as chat** |
| Chat (no data) | ~0ms | skipped | **Same as chat** |

**Key insight:** Response time is **always the same** — the LLM extraction never blocks the user. The system starts imperfect and gets accurate in the background, just like how `geo_learning` builds place knowledge without blocking chat.

### Implementation: background worker

Same pattern as `geo_learning` (already in the codebase):

```
api_chat turn → response sent → enqueue to intent_learn worker
  │
  └─ Worker thread (background):
      1. Build prompt: user question + SharedCollection schemas
      2. Call LLM (same model as chat, small prompt ~200 tokens)
      3. Parse JSON response → {collection, operation, filters}
      4. Record into nl_learn_terms:
         - SC:{collection} for entity terms found in question
         - Intent cue for operation type
      5. WAL append (non-blocking)
```

Worker queue capacity: 16 turns (same as geo_learning). If queue full, drop oldest — learning is best-effort, never critical path.

---

## Phase 1: CLASSIFY — What does the user want?

### Intents

| Intent | Trigger examples | Route to |
|--------|-----------------|----------|
| `CHAT` | "hello", "tell me a joke", "explain recursion" | LLM directly (current behavior) |
| `DATA_QUERY` | "how many X", "list all Y", "total Z this month" | Phase 2 → ELK/Mongo |
| `DATA_LOOKUP` | "what is the price of X", "show me order #123" | Phase 2 → Mongo find |
| `GEO` | "where is X", "districts in Saigon" | geo_atlas + LLM |
| `RAG` | similar to previous question (high vector score) | Redis L2 cache |
| `ACTION` | "create a new X", "update my profile" | App layer callback (future) |

### Classification method

**Option A — Extend smart_topic (LLM micro-query):**

Current smart_topic asks a tiny model: "classify as TECH/CHAT/EDUCATION/BUSINESS/DEFAULT".

Extend the prompt to include data intents:
```
Classify this user message into exactly ONE category:
- CHAT (general conversation, opinions, explanations)
- DATA_QUERY (counting, aggregation, statistics about stored data)
- DATA_LOOKUP (find specific record, show details)
- GEO (geography, places, locations, directions)
- TECH (code, programming, technical)
- EDUCATION (learning, study)
- BUSINESS (work, company)

Message: "how many computers sold this year?"
Answer (one word):
```

→ `DATA_QUERY`

**Cost:** one micro-query per chat (already exists for smart_topic). Just change the prompt.

**Option B — Keyword pre-filter (no LLM, fast path):**

Before calling smart_topic, check for strong signal keywords:

| Pattern | Intent | Confidence |
|---------|--------|------------|
| `how many`, `bao nhiêu`, `count`, `tổng` | DATA_QUERY | High |
| `list all`, `show all`, `liệt kê` | DATA_QUERY | High |
| `price of`, `giá`, `chi tiết`, `order #` | DATA_LOOKUP | High |
| `where is`, `ở đâu`, `quận nào` | GEO | High |
| `create`, `tạo`, `update`, `xóa` | ACTION | High |

If keyword match → skip smart_topic (save one LLM call).
If no keyword match → fall through to smart_topic LLM.

**Recommended: Option B first, Option A as fallback.**

```
User input
  │
  ├── Keyword match? → intent detected (fast, no LLM)
  │
  └── No keyword? → smart_topic LLM classify → intent
```

---

## Phase 2: EXTRACT — What to query?

### Input
- User message: "how many computers sold this year?"
- Intent from Phase 1: `DATA_QUERY`
- SharedCollection schema (from registry JSON)

### Output: Query Plan (JSON)

```json
{
  "intent": "DATA_QUERY",
  "collection": "products",
  "operation": "count",
  "filters": [
    {"field": "category", "op": "eq", "value": "computer"},
    {"field": "sold_date", "op": "gte", "value": "2026-01-01"}
  ],
  "group_by": null,
  "sort": null,
  "limit": null
}
```

### How to extract: LLM with schema context

Send the user question + available schemas to the LLM:

```
You are a query planner. Given the user's question and available data collections,
output a JSON query plan. Return ONLY valid JSON, no explanation.

Available collections:
- products: {name: string, category: string, price: number, sold_date: date, quantity: number}
- employees: {name: string, gender: string, department: string, hire_date: date, salary: number}
- carts: {user_id: string, items: array, total: number, created_at: date}
- product_categories: {name: string, slug: string, parent_id: string}

Current date: 2026-04-08

User question: "how many computers sold this year?"

Query plan JSON:
```

LLM returns:
```json
{
  "collection": "products",
  "operation": "count",
  "filters": [
    {"field": "category", "op": "eq", "value": "computer"},
    {"field": "sold_date", "op": "gte", "value": "2026-01-01"}
  ]
}
```

### Schema source

The schema comes from SharedCollection registry (`shared_collection_registry.json`) which is already loaded at `api_create`. We need to:
1. Read collection names + field names from the registry
2. Format as text for the LLM prompt
3. Include field types if available (for date range, numeric aggregation)

### Operations supported

| Operation | ELK query | Mongo query |
|-----------|-----------|-------------|
| `count` | `POST /{index}/_count` | `collection.countDocuments(filter)` |
| `sum` | `POST /{index}/_search` with `aggs.sum` | `collection.aggregate([{$match}, {$group: {_id: null, total: {$sum: "$field"}}}])` |
| `avg` | Aggs with `avg` | `$avg` |
| `list` | `POST /{index}/_search` with `size` | `collection.find(filter).limit(N)` |
| `find_one` | `POST /{index}/_search` with `size:1` | `collection.findOne(filter)` |
| `group_by` | Aggs with `terms` | `$group` |

### Filter operators

| Op | ELK | Mongo |
|----|-----|-------|
| `eq` | `{"term": {"field": "value"}}` | `{"field": "value"}` |
| `neq` | `{"bool": {"must_not": {"term": ...}}}` | `{"field": {"$ne": "value"}}` |
| `gt` / `gte` / `lt` / `lte` | `{"range": {"field": {"gte": ...}}}` | `{"field": {"$gte": ...}}` |
| `contains` | `{"match": {"field": "text"}}` | `{"field": {"$regex": "text"}}` |
| `in` | `{"terms": {"field": [...]}}` | `{"field": {"$in": [...]}}` |

### Time range resolution

The LLM is told the current date. Common patterns:

| User says | Resolved filter |
|-----------|----------------|
| "this year" | `sold_date >= 2026-01-01` |
| "last month" | `sold_date >= 2026-03-01 AND < 2026-04-01` |
| "today" | `created_at >= 2026-04-08` |
| "Q1" | `>= 2026-01-01 AND < 2026-04-01` |

The LLM handles this naturally given `[SYSTEM_TIME]` in the prompt.

---

## Complete data flow diagram

### Files involved

```
nl_learn_terms.json             ← scores snapshot (term → intent → count)
nl_learn_terms.json.wal         ← score deltas (append-only, compacts into .json)
nl_learn_terms.json.querycache  ← LLM query plans (question → collection + operation + filters)
```

- `.json` + `.wal` = same data, two forms (snapshot + deltas). Phase 1 reads these.
- `.querycache` = raw LLM extraction output. Phase 3 reads these. **Not linked to scores** (see Q&A discussion).

### Per-turn flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                        USER CHAT INPUT                               │
│                "how many computers sold this year?"                   │
└────────────────────────────┬────────────────────────────────────────┘
                             │
  ┌──────────────────────────┴──────────────────────────────────────┐
  │                    FOREGROUND (blocking, fast)                    │
  │                                                                  │
  │  Phase 1: CLASSIFY                                     ~0ms      │
  │  intent_route_classify(msg, lt, vocab, threshold, &ir)           │
  │    ├─ score_text: scan .json/.wal terms against message           │
  │    │   reads: nl_learn_terms.json + .wal (in RAM)                │
  │    ├─ if score > threshold → ELK intent                          │
  │    ├─ resolve collection via vocab + SC:* scores                  │
  │    └─ if score < threshold → CHAT (normal LLM path)              │
  │                                                                  │
  │  Phase 1b: SMART_TOPIC (fallback for CHAT)                       │
  │    if intent == CHAT → smart_topic LLM micro-query               │
  │    if intent != CHAT → skip (save 200-500ms)                     │
  │                                                                  │
  │  Phase 3: EXECUTE (if ELK intent)                      ~50-100ms │
  │  intent_route_execute(&ir, msg, registry, storage, &elk)         │
  │    ├─ resolve ELK index from collection                           │
  │    ├─ check .querycache for cached plan (word similarity)         │
  │    │   reads: nl_learn_terms.json.querycache (in RAM)            │
  │    │   ├─ HIT: build query from cached filters                   │
  │    │   │   → {"query":{"bool":{"must":[{"range":{...}}]}}}       │
  │    │   └─ MISS: fallback to match_all                            │
  │    │       → {"query":{"match_all":{}}}                          │
  │    ├─ storage_elk_search → ELK HTTP                               │
  │    └─ parse result_count + snippets                               │
  │                                                                  │
  │  Phase 4: FORMAT                                                  │
  │  intent_route_format_data_result → [DATA_RESULT] block            │
  │    └─ prepend to context_buf before LLM call                      │
  │                                                                  │
  │  LLM CHAT CALL → response to user                                │
  │                                                                  │
  └──────────────────────────┬──────────────────────────────────────┘
                             │
  ┌──────────────────────────┴──────────────────────────────────────┐
  │                    AFTER RESPONSE (non-blocking)                  │
  │                                                                  │
  │  nl_learn_cues (every turn, immediate):                           │
  │    ├─ tier scan: "how many" → ELK_ANALYTICS +1                   │
  │    ├─ vocab scan: "products" → SC:products +1                     │
  │    └─ writes: .wal append (O(1))                                  │
  │                                                                  │
  │  intent_learn worker (background thread):                         │
  │    ├─ build prompt: question + SharedCollection schemas           │
  │    ├─ LLM micro-query → response JSON                             │
  │    ├─ parse: {collection, operation, filters}                     │
  │    │                                                              │
  │    ├─ WRITE SCORES: content words → SC:{collection}               │
  │    │   "sold" → SC:carts +1, "computers" → SC:carts +1           │
  │    │   writes: .wal append                                        │
  │    │   → improves Phase 1 routing for next turn                   │
  │    │                                                              │
  │    └─ WRITE PLAN: full query plan with filters                    │
  │        writes: .querycache append                                 │
  │        → improves Phase 3 query building for next turn            │
  │                                                                  │
  └─────────────────────────────────────────────────────────────────┘
                             │
                             ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │                    NEXT TURN (system is smarter)                   │
  │                                                                  │
  │  Phase 1 reads .json/.wal:                                        │
  │    SC:carts > SC:products → routes to carts (corrected)           │
  │    ELK_ANALYTICS score higher → routes more confidently           │
  │                                                                  │
  │  Phase 3 reads .querycache:                                       │
  │    cached plan has filters → builds precise ELK query             │
  │    {"range":{"created_at":{"gte":"2026-01-01"}}}                  │
  │    → accurate filtered count instead of match_all                 │
  │                                                                  │
  │  Both improve independently. No linked key between them (yet).    │
  └─────────────────────────────────────────────────────────────────┘
```

### Learning progression

```
Turn 1: fresh system
  Phase 1: score=0 → CHAT (no ELK)
  Phase 3: not reached
  Background: LLM extracts plan → saved to .wal + .querycache

Turn 2: same question
  Phase 1: score=5 → ELK_ANALYTICS, collection=carts (learned from turn 1)
  Phase 3: cache HIT → uses filters from turn 1's plan → accurate query
  Background: reinforces scores + may update plan

Turn 10: similar question
  Phase 1: score=50 → high confidence routing
  Phase 3: cache HIT → precise filters
  Background: still runs but adds little new (pattern established)

Turn 100+: mature
  Phase 1: instant accurate routing
  Phase 3: cached plans for most patterns
  Background: could be skipped (future M4_INTENT_ROUTE_SKIP_LLM_THRESHOLD)
```

---

## Exists vs Missing — status audit

### Foreground path

| Component | Status | Location | Notes |
|-----------|--------|----------|-------|
| `intent_route_classify()` — score-based routing | **DONE** | `src/intent_route.c` | `score_text` scans all learned terms |
| `nl_learn_terms` — score store + WAL | **DONE** | `src/nl_learn_terms.c` | WAL append O(1); `score_text` for classify |
| `sc_term_vocab` — word→collection.field lookup | **DONE** | `src/shared_collection.c` | Built from alias + field_hints at startup |
| `intent_route_execute()` — ELK query builder + run | **DONE** | `src/intent_route.c` | Checks `.querycache` for filters, falls back to `match_all` |
| `intent_learn_cache_lookup()` — query plan cache | **DONE** | `src/intent_learn.c` | Word similarity match against cached plans |
| Query from cached filters (`term`, `range`, `match`) | **DONE** | `src/intent_route.c` | `ir_build_query_from_filters` parses filter array |
| `intent_route_format_data_result()` — [DATA_RESULT] inject | **DONE** | `src/intent_route.c` | Prepends to context_buf |
| `storage_elk_search()` — ELK facade | **DONE** | `src/storage.c` | Wraps `elk_search` |
| Score-based routing skips smart_topic | **DONE** | `src/api.c` | Score > threshold → skip Ollama |
| Debug logging (`INTENT_ROUTE` module) | **DONE** | `src/intent_route.c` | `m4_log` via `debug_modules` |

### After-response learning

| Component | Status | Location | Notes |
|-----------|--------|----------|-------|
| `nl_learn_cues` — phrase detection + vocab scan | **DONE** | `src/nl_learn_cues.c` | Tier scan (intent) + vocab scan (SC:*) → writes .wal |
| Synonym fallback in vocab scan | **DONE** | `src/nl_learn_cues.c` | `m4_synonym_get_global()` |
| WAL append-only persistence | **DONE** | `src/nl_learn_terms.c` | O(1) per turn, compact at 500 lines |
| `intent_learn` background worker | **DONE** | `src/intent_learn.c` | LLM extraction → writes scores to .wal + plans to .querycache |
| `sc_registry_schema_summary()` — schema for LLM prompt | **DONE** | `src/shared_collection.c` | Lists collections + field_hints for extraction prompt |
| Query plan cache (`.querycache` file) | **DONE** | `src/intent_learn.c` | Append-only JSON lines, loaded at startup, searched by word similarity |
| **Vietnamese cues** | **MISSING** | — | Only English phrases in tier tables |

### ELK infrastructure

| Component | Status | Location | Notes |
|-----------|--------|----------|-------|
| BSON → plain JSON converter | **DONE** | `src/storage.c` | `elk_bson_to_plain_json` |
| Cold backfill (full + incremental) | **DONE** | `src/storage.c` | `schedule_refresh` option |
| Change stream (real-time sync) | **DONE** | `src/storage.c` | Preflight checks, graceful fallback |
| Sync state file (`.elk_sync_state`) | **DONE** | `src/storage.c` | `collection:last_id` per line |

### Phase 2: EXTRACT (collection + field resolution)

| Component | Status | Location | Notes |
|-----------|--------|----------|-------|
| Collection resolution via vocab table | **DONE** | `src/intent_route.c` `ir_resolve_collection` | Vocab lookup + SC: score boost |
| Field resolution via vocab table | **DONE** | `src/shared_collection.c` `sc_term_vocab_lookup` | Returns collection + field |
| **LLM extraction for complex queries** | **MISSING** | — | Current: broad multi_match. Future: LLM → structured query plan for complex filters |
| **Time expression parser** ("this month" → date range) | **MISSING** | — | Current: detected as ELK_ANALYTICS cue but not converted to date filter |
| **Structured filter builder** (field=value, range) | **MISSING** | — | Current: multi_match on all fields. Future: targeted term/range queries |

### Phase 3: EXECUTE

| Component | Status | Location | Notes |
|-----------|--------|----------|-------|
| `intent_route_execute()` — build + run ELK query | **DONE** | `src/intent_route.c` | ANALYTICS: size:0 multi_match. SEARCH: size:5 multi_match |
| `storage_elk_search()` — storage facade | **DONE** | `src/storage.c` | Wraps `elk_search` with ctx access |
| ELK response parser (hit count + snippets) | **DONE** | `src/intent_route.c` | Parses `total.value` and `_source` objects |
| **Mongo fallback** (when ELK unavailable) | **MISSING** | — | Could aggregate on Mongo when mode != ELK |
| **Targeted ELK queries** (term, range, aggs) | **MISSING** | — | Current: broad multi_match. Future: precise queries from Phase 2 |

### Phase 4: FORMAT

| Component | Status | Location | Notes |
|-----------|--------|----------|-------|
| `intent_route_format_data_result()` — build [DATA_RESULT] | **DONE** | `src/intent_route.c` | JSON block + instruction for LLM |
| Prepend to context_buf | **DONE** | `src/api.c` `api_apply_model_switch` | `memmove` + `memcpy` before existing prompt |
| **Result type formatting** (count vs list vs aggregation) | **PARTIAL** | `src/intent_route.c` | ANALYTICS: count. SEARCH: top hits. Missing: group_by, sum, avg |

### Learning terms — I/O and structure

| Component | Status | Location | Notes |
|-----------|--------|----------|-------|
| WAL append-only writes | **DONE** | `src/nl_learn_terms.c` | O(1) per turn; no fsync |
| Auto-compaction | **DONE** | `src/nl_learn_terms.c` | At 500 WAL lines or `close()` |
| Entity cue recording (SC:{collection}) | **DONE** | `src/nl_learn_cues.c` | Vocab scan after tier scan |
| **Redis backend** (HINCRBY, multi-process) | **MISSING** | — | Would use `redis_set_counter`; per-tenant keys |
| **Score decay** (age out stale counts) | **MISSING** | — | Old scores never shrink |
| **Per-tenant isolation** | **MISSING** | — | All tenants share one global file |

### Smart_topic ↔ NL learn integration

| Aspect | Status | Notes |
|--------|--------|-------|
| smart_topic sets temperature + lane | **DONE** | Fallback when score < threshold |
| `intent_route_classify` runs BEFORE smart_topic | **DONE** | Score > threshold → skip smart_topic LLM call |
| Unified decision point | **DONE** | `api_apply_model_switch`: score gate → smart_topic fallback |

---

## Semantic ELK query cache (Redis L2 — cross-language)

### Concept

Cache the **ELK query** (not the result) as a Redis vector entry. On semantic cache hit, **re-execute** the cached query against ELK for fresh data. Multilingual embeddings ensure the same question in any language maps to a nearby vector — one cached query serves all languages.

### Why cache the query, not the result

```
Cached = ELK query JSON       ← stable, language-independent, reusable
Result = always from ELK      ← always current, never stale

"how many computers sold this month?"   → builds ELK query → CACHE query
"có bao nhiêu máy tính bán tháng này?" → embed ≈ English → HIT → re-execute → fresh result
```

Data changes constantly (42 → 43 → 89). The query doesn't. By caching only the query, the result is **always correct at the moment it's asked**.

### What's cached (Redis vector payload)

```json
{
  "index":      "idx_products",
  "op":         "count",
  "elk_query":  "{\"query\":{\"bool\":{\"must\":[{\"term\":{\"category\":\"computer\"}},{\"range\":{\"sold_date\":{\"gte\":\"2026-04-01\"}}}]}},\"size\":0}",
  "query_plan": {"collection":"products","operation":"count","filters":[...]}
}
```

The vector key is the **embedding of the user question**. The payload is the **ELK query to re-execute**.

### Flow

```
USER INPUT (any language)
  │
  ▼
① EMBED question
   api_user_message_embedding(msg) → vec[768]
   (already happens per turn for chat RAG)
  │
  ▼
② REDIS SEMANTIC SEARCH — ELK cache lane
   redis_search_semantic(tenant|elk_cache, vec, dim, k=1, min_score=0.90)
  │
  ├── HIT (score ≥ 0.90) ──────────────────────────────────────┐
  │   Parse payload → {index, elk_query, op}                    │
  │   SKIP Phase 1 (classify) — already know it's ELK          │
  │   SKIP Phase 2 (extract)  — already have the query          │
  │   → go directly to Phase 3: elk_search(index, elk_query)    │
  │   → fresh result from ELK                                    │
  │   → Phase 4: format with [DATA_RESULT]                       │
  │                                                              │
  └── MISS ─────────────────────────────────────────────────────┐
      → Phase 1: classify (nl_learn_cues + score_sum)            │
      → Phase 2: extract (LLM + schema → query plan → ELK query)│
      → Phase 3: elk_search → result                             │
      → CACHE: redis_set_vector_ttl(tenant|elk_cache,            │
               vec, {index, elk_query, op, query_plan}, ttl=300) │
      → Phase 4: format                                          │
```

### What this skips on cache hit

| Phase | On MISS | On HIT | Saved |
|-------|---------|--------|-------|
| Embed | ~100ms | ~100ms | — (needed for semantic match) |
| Redis search | ~0.2ms | ~0.2ms | — |
| Phase 1: classify | ~0ms | **SKIP** | ~0ms (cues are fast) |
| Phase 2: extract (LLM) | **~1-3s** | **SKIP** | **~1-3s** ← biggest saving |
| Phase 3: ELK query | ~50-100ms | ~50-100ms | — (always fresh) |
| Phase 4: format (LLM) | ~LLM | ~LLM | — |

### Cross-language cache hits

Multilingual embeddings (e.g. `nomic-embed-text`) map the same meaning to similar vectors regardless of language:

```
"how many computers sold this month?"       → vec_en  ─┐
"có bao nhiêu máy tính bán tháng này?"      → vec_vi  ─┤ cosine ≈ 0.94
"combien d'ordinateurs vendus ce mois-ci?"  → vec_fr  ─┤ cosine ≈ 0.92
"今月何台のコンピュータが売れましたか？"         → vec_ja  ─┘ cosine ≈ 0.91

One cached ELK query → serves all languages.
```

### Redis lane separation

Use a lane prefix to separate ELK query cache from chat RAG cache:

| Lane | Key prefix | TTL | Purpose |
|------|-----------|-----|---------|
| Chat RAG | `tenant\|chat` | 60s | Cached LLM replies (existing) |
| ELK query cache | `tenant\|elk_cache` | 300s | Cached ELK query JSON (new) |
| Geo landmarks | `tenant\|m4geo` | permanent | Geo dedup index (existing) |

### Exists vs Missing for semantic cache

| Component | Status | Location |
|-----------|--------|----------|
| `ollama_embeddings()` | **EXISTS** | `src/ollama.c` |
| `api_user_message_embedding()` | **EXISTS** | `src/api.c:1505` — already called per turn |
| `redis_set_vector_ttl()` with custom TTL | **EXISTS** | `src/redis.c:153` |
| `redis_search_semantic()` with cosine KNN | **EXISTS** | `src/redis.c:198` |
| `cosine_similarity()` | **EXISTS** | `src/redis.c` |
| Chat RAG flow (embed → search → hit → skip) | **EXISTS** | `src/api.c:1585-1600` — same pattern to follow |
| **ELK cache lane** (`tenant\|elk_cache` prefix) | **MISSING** | Need lane routing in `redis_search_semantic` |
| **Cache-write after Phase 3** (store ELK query as payload) | **MISSING** | Insert `redis_set_vector_ttl` after successful `elk_search` |
| **Cache-read before Phase 1** (check ELK cache first) | **MISSING** | Insert `redis_search_semantic` before classify |
| **Payload parse** (extract index + elk_query from hit) | **MISSING** | Simple JSON parse of cached payload |

---

## Implementation plan

### Done

| Step | What | Location |
|------|------|----------|
| ✅ | Score-based routing: `intent_route_classify` with `score_text` | `src/intent_route.c`, `src/api.c` |
| ✅ | Keyword pre-filter skips smart_topic when score > threshold | `src/api.c` `api_apply_model_switch` |
| ✅ | SharedCollection parser: `alias`, `field_hints` → `sc_entry_t` | `src/shared_collection.c` |
| ✅ | Term vocab table: `sc_term_vocab_build/lookup` | `src/shared_collection.c` |
| ✅ | Vocab scan in cues: entity recording (`SC:{collection}`) | `src/nl_learn_cues.c` |
| ✅ | Synonym fallback in vocab scan | `src/nl_learn_cues.c` |
| ✅ | ELK query builder: `intent_route_execute` (multi_match) | `src/intent_route.c` |
| ✅ | `storage_elk_search` facade | `src/storage.c` |
| ✅ | `[DATA_RESULT]` injection: `intent_route_format_data_result` | `src/intent_route.c` |
| ✅ | WAL append-only persistence for nl_learn_terms | `src/nl_learn_terms.c` |
| ✅ | `score_text`: iterate all stored terms against message | `src/nl_learn_terms.c` |
| ✅ | Debug modules: INTENT_ROUTE, SHARED_COLLECTION, etc. | `src/debug_log.c` |
| ✅ | BSON → plain JSON for ELK indexing | `src/storage.c` |
| ✅ | Incremental backfill (`schedule_refresh` + `.elk_sync_state`) | `src/storage.c` |
| ✅ | Change stream real-time sync + preflight checks | `src/storage.c` |

### Done — background intent learning (Phase 2)

| Step | What | Location |
|------|------|----------|
| ✅ BG1 | `intent_learn` worker: background thread (queue cap=16) | `src/intent_learn.c` |
| ✅ BG2 | LLM prompt: question + `sc_registry_schema_summary` → micro-query | `src/intent_learn.c` |
| ✅ BG3 | JSON plan parser: extract `collection`, `operation` from LLM response | `src/intent_learn.c` |
| ✅ BG4 | Record decisions: content words → `SC:{collection}`, operation → intent | `src/intent_learn.c` |
| ✅ | `sc_registry_schema_summary`: builds "- collection: {field (hint)}" text | `src/shared_collection.c` |
| ✅ | Wired: init at `api_create`, enqueue after turn, shutdown at `api_destroy` | `src/api.c` |

---

## Q&A — design decisions and known limitations

### How does the system learn which collection to query?

The background worker (Phase 2) sends each user question to the LLM along with the available collection schemas. The LLM returns a JSON query plan saying, for example, "this is a count query on the `carts` collection." The worker then records that decision: every content word in the user's message (like "sold", "computers") gets associated with `SC:carts` in the learning store. Next time a similar question comes in, Phase 1 scores see that "sold" has a high `SC:carts` score and routes there directly — without needing the LLM.

### What happens on the very first turn? (cold start)

Phase 1 has no learned data yet, so scores are 0 for everything. The message goes through as normal CHAT. But in the background, the intent_learn worker calls the LLM and records the correct collection mapping. By the second or third similar question, scores have built up enough to route correctly. The first turn is imperfect — this is the tradeoff for zero-latency responses.

### Why not call the LLM in the foreground for Phase 2?

It adds 0.5-2 seconds to every data question. Since the user is waiting for a response, that delay is noticeable. By running Phase 2 in the background, the response time stays the same as a normal chat turn. The accuracy improves over time instead of being perfect on turn one.

### What if the LLM returns garbage or the wrong collection?

The parser is lenient — it looks for the first `{` in the LLM response and extracts `"collection"` and `"operation"` fields. If neither is found, the response is silently dropped and nothing is recorded. Wrong collection names still get recorded, but since the user will eventually ask questions that the LLM answers correctly, the correct mappings accumulate faster and outweigh the errors. Enable `debug_modules: ["INTENT_ROUTE"]` to see every LLM extraction decision.

### Why does "how many products sold?" route to `products` instead of `carts`?

The vocab table maps the word "products" to the `products` collection because that word appears in the collection name. The system doesn't understand that "sold" implies transaction data in `carts`. This is exactly what the background LLM learning fixes — the LLM knows "sold" means carts, records that mapping, and future queries route correctly.

### How rich do the SharedCollection field_hints need to be?

The richer the better. The LLM prompt includes field names and their hint descriptions. A collection with `"field_hints": {"status": "pending, shipped, delivered"}` gives the LLM much more context than one with no hints at all. Collections without any `field_hints` still work — the LLM infers from the collection name — but accuracy improves with better hints.

Example of a well-documented collection:
```json
{
  "collection": "carts",
  "alias": "Shopping carts and orders",
  "metadata": {
    "field_hints": {
      "user_id": "Customer who placed the order",
      "product_name": "Name of the product purchased",
      "category": "Product category (computer, phone, tablet)",
      "qty": "Quantity ordered",
      "total": "Total price in VND",
      "status": "Order status (pending, shipped, delivered, cancelled)",
      "created_at": "Date the order was created"
    }
  }
}
```

### Is the learning data per-tenant or global?

Currently global — all tenants share one learning terms file. This means tenant A's questions improve routing for tenant B. For most deployments this is fine (same schema, same collections). For strict tenant isolation, a future Redis backend (`HINCRBY nl:cues:{tenant}`) would separate the scores per tenant.

### What about thread safety?

The `intent_learn` background worker writes to the same `nl_learn_terms` store that the foreground `score_text` reads. The hash table is not mutex-protected for concurrent read/write. In practice this is safe for 64-bit counters on modern platforms (atomic loads/stores). For strict safety, the existing `nl_learn_mx` mutex could be extended to cover the background worker's writes. This is noted as a known limitation.

### How does Phase 3 build the ELK query now?

Phase 3 extracts **content keywords** from the user message by stripping stop words ("how", "many", "this", "year", "the", etc.) and question patterns. For analytics (count), if no content keywords remain, it uses `match_all` to count everything in the resolved collection. For search, it uses `multi_match` with the extracted keywords.

Example: "how many products sold this year?" → keywords = "products sold" → `multi_match` on "products sold". "how many orders?" → keywords = "orders" → `multi_match` on "orders". "how many?" → no keywords → `match_all` (count all).

This works for simple queries. For precise filters (time ranges, field-specific terms), see next question.

### When will Phase 3 use precise ELK queries instead of broad multi_match?

Currently, Phase 3 builds a `multi_match` query using keywords from the user message. This works for simple cases but misses time filters and field-specific terms. The next step is to use the LLM's query plan (which includes `filters` with field names, operators, and values) to build targeted `term` and `range` queries. This requires the background worker to also cache the filter plan, not just the collection name.

### Can the system eventually skip the background LLM call?

Yes — this is the "mature stage" described in the overview. Once scores for common patterns are high enough (e.g. "sold" → `SC:carts` score > 50), the foreground Phase 1 is confident enough to route correctly without LLM confirmation. A future `M4_INTENT_ROUTE_SKIP_LLM_THRESHOLD` setting would control when to stop enqueuing to the background worker for known patterns.

### How does this interact with smart_topic?

Smart_topic classifies messages into temperature tiers (TECH, CHAT, EDUCATION, BUSINESS). Intent routing classifies into data intents (ELK_ANALYTICS, ELK_SEARCH, RAG_VECTOR, CHAT). When intent routing decides ELK, it skips the smart_topic LLM call entirely (saves 200-500ms) and sets a low temperature (0.1) for deterministic data answers. When intent routing decides CHAT (no data signal), smart_topic runs as normal for temperature tuning.

### What happens if Elasticsearch is down?

Phase 3 (`intent_route_execute`) calls `storage_elk_search`, which fails if ELK is unreachable. The function returns -1, no `[DATA_RESULT]` is injected, and the LLM handles the question as normal chat. The user gets a conversational response instead of data. The background learning still runs — it doesn't depend on ELK being available.

### [DISCUSSION] Three files — how do they relate?

Currently the learning system produces three files:

```
nl_learn_terms.json           ← scores (term → intent → count)
nl_learn_terms.json.wal       ← score deltas (append-only, compacts into .json)
nl_learn_terms.json.querycache ← raw LLM query plans (question → collection + operation + filters)
```

**`.json` + `.wal`** are linked — WAL is deltas that compact into the snapshot. Same data, two forms.

**`.querycache`** is **not linked** to the scores. It stores raw LLM output independently. Phase 3 looks up plans by word similarity against the question text. There's no shared key with the term scores.

**The concern:** a cached plan may say "carts" but the term scores may say "products" — they can disagree. The cache has no confidence measure, no link to how many times this plan was validated.

**Current state:** it works but is fragile. The raw data is valuable — the LLM's extraction decisions are the most accurate signal we have. But without linking to scores, we can't tell if a cached plan is trustworthy.

**Ideas for future unification (no decision yet):**

| Option | How | Tradeoff |
|--------|-----|----------|
| **A: Extend nl_learn_terms** | Add plan data per term: `"sold" → {SC:carts: 8, plan: {filters:[...]}}` | Clean linkage, but changes the JSON v2 format; plans don't naturally fit as counters |
| **B: Derive at compaction** | At WAL compaction, read `.querycache`, match plans to high-score terms, embed into the snapshot | Keeps WAL simple; links happen at compaction time; more complex logic |
| **C: Confidence gate** | Keep separate files, but Phase 3 only uses a cached plan if the SC:collection score agrees with the plan's collection | Simple code change; doesn't unify the data but prevents disagreements |
| **D: Use querycache as training input** | The cached plans are raw LLM decisions — treat them as training data, not lookup cache. Process them into term scores + plan patterns at compaction. The `.querycache` becomes a staging area, not a permanent store. | Cleanest separation of concerns; cache is ephemeral, learning store is authoritative |
| **E: Redis semantic cache** | Embed the question as a vector, store the plan as payload in Redis L2 (the cross-language design from earlier). Similarity search finds plans for any language. | Best accuracy + cross-language; depends on Redis + embeddings being available |

**For now:** `.querycache` stays as raw data. Phase 3 uses it opportunistically. The design for linking/unifying is open.

### [DISCUSSION] Collection resolution should use learned SC: scores, not just vocab

**Problem observed:** User says "how many orders in pending stages?" The collection is `carts` but the word "orders" isn't in the vocab (only "carts" and "shopping" are). The background worker correctly learns `orders → SC:carts`, but `ir_resolve_collection` only checks vocab + synonyms. If both miss, the learned SC: scores are ignored — even though the answer is already in the WAL.

**Why this matters:** Collection names like "carts", "products", "employees" are common words. Users naturally use related words — "orders" for carts, "staff" for employees, "items" for products. The vocab only maps exact words from the config (`alias` + `field_hints`). Requiring the app to list every possible synonym in the config defeats the purpose of self-learning.

**Current flow (broken):**
```
"orders" → vocab lookup → MISS
         → synonym lookup → MISS
         → collection = (none) → skip ELK
         → meanwhile WAL has: orders\tSC:carts\t+5 (ignored!)
```

**Expected flow (with fix):**
```
"orders" → vocab lookup → MISS
         → synonym lookup → MISS
         → scan SC:* scores for ALL elk.allow collections
           SC:carts = 5 (from "orders" in WAL) → collection = carts
         → ELK query on idx_carts → real data
```

**The fix:** After vocab + synonym fail, iterate all elk.allow collections and call `score_text(norm, "SC:{collection}")` for each. If any has a positive score, use the highest as the resolved collection. This makes the learned data the fallback when config is incomplete.

**Impact:**
- Turn 1: "orders" → no vocab, no learned data → CHAT (correct — nothing to route to yet)
- Turn 1 background: LLM extracts → carts → records `orders → SC:carts`
- Turn 2: "orders" → vocab miss → SC:carts score = 5 → routes to carts → real data
- No config change needed. No alias. The system learned it.

**When to implement:** This is the key missing piece for the self-learning to be fully effective. Vocab + alias remain useful for day-one accuracy (before any learning). But learned SC: scores should always be the fallback.

### [DISCUSSION] Initial learning from collection names and relationships

**Problem:** On first startup with zero learning data, the system only knows collection names and metadata. It waits for user conversations to learn synonyms. But collection names themselves contain predictable signals — can we bootstrap learning from them?

**What we can learn from collection names alone:**

```
SharedCollection provides: products, carts, product_categories

From names, the engine can infer at init:
  "products" → products (exact)
  "product"  → products (singular)
  "carts"    → carts (exact)
  "cart"     → carts (singular)
  "categories" → product_categories
  "category"   → product_categories
```

This already works (vocab table does singular + name matching).

**What we can NOT infer from names alone:**

```
  "orders"   → carts?    (synonym — name doesn't imply this)
  "sales"    → carts?    (domain concept — name doesn't imply this)
  "staff"    → employees? (if that collection existed)
```

These need either metadata enrichment OR background LLM learning.

**Relationship discovery from structure:**

The SharedCollection config already has structural hints that reveal relationships between collections:

```json
{
  "collection": "carts",
  "joins": [
    {
      "from": "lines.product_id",
      "to_collection": "products",
      "to_field": "_id"
    }
  ]
}
```

This tells us: **carts references products**. At init, the engine could learn:

```
carts has product_id → carts is related to products
  → "products sold" might mean carts (where the product reference lives)
  → "product orders" might mean carts
  → "items purchased" might mean carts (lines contain products)
```

**Field names as signals:**

```
carts.lines = [{product_id, sku, name, qty, unit_price}]

Field names tell us:
  "sku"        → relates to products.sku
  "qty"        → quantity, implies transaction/order
  "unit_price" → implies purchase/sale
  "product_id" → explicitly links to products
```

If `carts` has `product_id`, `qty`, `unit_price` — it's a transaction collection. The LLM background worker could be seeded with this knowledge at init instead of waiting for user questions.

**Initial LLM seed prompt (at startup, one-time):**

```
Given these data collections and their fields, describe the relationships
and common user questions for each:

- products: {sku, name, price, quantity_available, category, published}
- carts: {cart_key, lines[{product_id, sku, name, qty, unit_price}], updated_at}
- product_categories: {code, name, active}

For each collection, list:
1. What kind of data it holds
2. Related synonyms users might use
3. Which questions would query this collection
```

LLM returns:
```
products: catalog items. Synonyms: "items", "merchandise", "inventory"
  Questions: "how many products?", "what's in stock?", "product list"

carts: purchase transactions/orders. Synonyms: "orders", "sales", "purchases", "transactions"
  Questions: "how many orders?", "pending orders", "sales this month", "revenue"

product_categories: category taxonomy. Synonyms: "categories", "types", "groups"
  Questions: "how many categories?", "active categories"
```

→ Record these synonyms as SC:* scores before any user conversation.

**When to seed:**
- At `intent_learn_init` (once, background)
- Only if learning store is empty (fresh start)
- Uses the same LLM extraction worker — just a different prompt

**What this gives us:**
- Turn 1: "how many orders?" → SC:carts already has score from seed → routes correctly
- No metadata needed for common collections
- Strange names (`tbl_001`) still need metadata — LLM can't infer from meaningless names

**Open questions:**

| Question | Notes |
|----------|-------|
| **Cost:** one LLM call at startup per fresh install | Acceptable — happens once, cached forever |
| **Accuracy:** LLM may guess wrong relationships | Seeds are low-weight; real usage quickly overwrites |
| **When to re-seed:** if collections change | On registry reload or restart with empty WAL |
| **Joins as signal:** should `ir_resolve_collection` also check join paths? | If carts.joins→products, and user says "product orders", the join tells us carts is the transaction side |

### Remaining — accuracy improvements

| Step | What | Effort | Priority |
|------|------|--------|----------|
| 2 | **Vietnamese cues**: add ~20 rows to tier tables | Small | High |
| FILTER | **Query plan → ELK query**: use LLM's filters for `term`/`range` (not broad `multi_match`) | Medium | High (after BG1-4) |
| AGG | **Aggregation queries**: `size:0` + `aggs` for sum, avg, group_by | Medium | Medium |
| NEG | **Negative cues**: "how many ways" = CHAT, not analytics | Small | Medium |
| 12 | **Co-occurrence pairs**: composite key recording | Small | Low |

### Remaining — infrastructure

| Step | What | Effort | Priority |
|------|------|--------|----------|
| 14 | **Semantic ELK query cache** (Redis L2 cross-language) | Medium | Medium |
| 5 | **Redis backend** for scores (HINCRBY, per-tenant) | Medium | Medium |
| 13 | **Score decay** (weekly multiply or Redis TTL) | Small | Low |
| 10 | **Mongo fallback** query builder (when ELK unavailable) | Medium | Low |
| MATURE | **Skip LLM threshold**: when scores high enough, skip background Phase 2 | Small | Low (future) |

---

## Files — current state

| File | Role |
|------|------|
| `include/intent_route.h` | Intent enum, classify result, elk result, execute/format functions |
| `src/intent_route.c` | Phase 1 classify + Phase 3 execute + Phase 4 format |
| `include/shared_collection.h` | Registry + `sc_term_vocab_t` API |
| `src/shared_collection.c` | JSON parser (collection, alias, field_hints, elk) + vocab builder |
| `include/nl_learn_cues.h` | Cue recording with vocab param |
| `src/nl_learn_cues.c` | 5-tier intent cues + vocab entity scan + synonym fallback |
| `include/nl_learn_terms.h` | Score store API + `score_text` |
| `src/nl_learn_terms.c` | Score store + WAL + `score_text` iterator + SC: prefix |
| `include/storage.h` | `storage_elk_search`, `storage_get_sc_registry`, `storage_set_schedule_refresh` |
| `src/storage.c` | BSON→JSON, ELK search, backfill (full/incremental), change stream, sync state |
| `include/intent_learn.h` | Background worker API: init, enqueue, shutdown |
| `src/intent_learn.c` | Phase 2 LLM extraction worker: queue, prompt, parse, record to cue store |
| `src/api.c` | Wiring: classify → execute → format + enqueue to intent_learn worker |
| `src/debug_log.c` | INTENT_ROUTE, SHARED_COLLECTION, SMART_TOPIC, nl_learn_cues modules |
| `docs/api.md` | Debug modules, WAL docs, ELK sync docs, intent routing flow |

---

## Phase 5: ADMIN — manual routing overrides (design, not implemented)

### Problem

The learning system is automatic — it accumulates patterns from LLM decisions and user behavior. But sometimes an admin needs to:
- **Force** a phrase to always route to a specific collection ("sales" → always carts, never products)
- **Block** a phrase from routing ("how many ways" → never ELK, always CHAT)
- **Adjust** scores manually (boost or reset a collection mapping)
- **View** current learned state (what phrases map where, top scores)
- **Reset** learning for a collection or entirely

### Approach: admin edits a pins file

A separate JSON file (alongside the learning terms file) holds admin overrides. These **take priority** over learned scores.

**File:** `{learning_terms_path}.pins` (auto-loaded at startup, hot-reloadable)

```json
{
  "pins": [
    {"phrase": "sales", "intent": "ELK_ANALYTICS", "collection": "carts", "note": "admin: sales always means cart orders"},
    {"phrase": "revenue", "intent": "ELK_ANALYTICS", "collection": "carts"},
    {"phrase": "how many ways", "intent": "CHAT", "note": "admin: this is conversational, not data"}
  ],
  "blocks": [
    {"phrase": "recipe", "note": "never route to ELK — cooking questions are chat"}
  ],
  "resets": []
}
```

### How pins interact with scoring

```
User: "how many sales this year?"
  │
  ├─ Phase 1: check pins FIRST
  │   "sales" → pin found: intent=ELK_ANALYTICS, collection=carts
  │   → override score-based routing → use pinned collection
  │
  ├─ Phase 3: query idx_carts (from pin, not from scores)
  │
  └─ Learning still runs in background (pin doesn't block learning)
```

```
User: "how many ways to cook rice?"
  │
  ├─ Phase 1: check pins/blocks FIRST
  │   "how many ways" → block found → force CHAT
  │   → skip Phase 3 entirely
  │
  └─ Normal LLM chat response
```

### Admin API (future)

Options for how admins manage pins:

| Approach | Description | Effort |
|----------|-------------|--------|
| **File edit** | Admin edits `.pins` JSON directly, engine reloads on restart or SIGHUP | Small |
| **API endpoint** | App layer exposes `/api/admin/intent-pins` CRUD, writes to the file | Medium |
| **Dashboard** | Kibana or custom UI showing learned scores + pin editor | Large |

### What the admin can see (diagnostic)

With `debug_modules: ["INTENT_ROUTE"]` enabled, every chat turn logs:

```
[INTENT_ROUTE][DEBUG] classify: intent=ELK_ANALYTICS score=12 collection=carts field=status
  (analytics=12 search=0 rag=0 chat=0 threshold=5)
```

A future admin endpoint could expose:
- Top learned phrases per intent (most confident ELK_ANALYTICS patterns)
- Top phrases per collection (what words route to each collection)
- Score history (is a mapping getting stronger or weaker over time?)
- Background LLM decisions log (what did Phase 2 decide for recent turns?)

### Open questions for Phase 5

| Question | Options | Notes |
|----------|---------|-------|
| **File format** | JSON `.pins` file vs Mongo collection vs Redis hash | File is simplest, Mongo allows multi-instance sync |
| **Hot reload** | SIGHUP → re-read file vs poll mtime vs admin API trigger | SIGHUP is standard for config reload |
| **Pin priority** | Pins always win vs pins add high score (e.g. +1000) | "Always win" is simpler and predictable |
| **Scope** | Global pins vs per-tenant pins | Global first; per-tenant needs Redis or Mongo |
| **Learning interaction** | Pins override but learning still records vs pins also suppress learning for that phrase | Override-only is safer — learning continues, admin can remove pin later |
| **Bulk import** | Admin uploads CSV of phrase→collection mappings at once | Useful for initial setup; convert CSV → pins JSON |
