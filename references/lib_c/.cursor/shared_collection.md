> **[DESIGN - Partially implemented]** Current code does basic JSON registry + cold backfill. Next: parse `alias` + `field_hints` for NL term vocab. Future (RBAC/ABAC): `public`/`sensitive` field filtering, `sanitize_for_elk`, joins.

# SharedCollection — hybrid RAG, ELK, and prompt-safe fields

**Purpose:** A single **declarative idea** for how the engine treats a logical dataset: what is allowed in memory and prompts, what syncs to Elasticsearch, and how vectors attach to retrieval. Use this doc to align **schema design**, **C ingestion**, and **Cursor** work before hardcoding collection-specific logic in many places.

**Status:** Design / target shape. Parts overlap today’s **Mongo `bot.records` / `geo_atlas`**, **Redis RAG**, and **ELK** wiring; the full **SharedCollection** object is not necessarily one BSON document yet—treat the sections below as the **contract** you want implementations to converge on.

**Related:** [embed-vector-metadata.mdc](rules/embed-vector-metadata.mdc) (always pair **vector** with **metadata**), [.cursor/mongo.md](mongo.md), [.cursor/redis.md](redis.md), [.cursor/elk.md](elk.md), **[elk_index_data.md](elk_index_data.md)** (ELK **index compose**, joins vs flat, publish + RAG diagrams), **[elk_nl_routing.md](elk_nl_routing.md)** (chat **NL → ELK vs RAG vs analytics** routing), [.cursor/PRE_QUERY_RAG_FLOW.md](PRE_QUERY_RAG_FLOW.md), [docs/TUTORIAL_BINDINGS.md](../docs/TUTORIAL_BINDINGS.md) (engine integration), **section 8** (tutorial guidelines), **section 10** (ELK pool + change stream + cold start, design).

---

## Module shape: a separate `shared_collection` module?

**Short answer:** Treat **SharedCollection** as its own **logical module** (clear API + data struct + loader), even if it ships inside the same repo as `api` / `storage`. That keeps “what may enter prompts and search” in one place instead of duplicating rules across RAG, ELK, and `ctx_build_prompt`.

### What a `shared_collection` module would own

| Responsibility | Examples |
|------------------|----------|
| **Config model** | Parse/validate the schema in section 2 (from JSON, Mongo `config` collection, or env path). |
| **Policy helpers** | Now: `sc_term_vocab_build`, `sc_term_vocab_lookup`. Future (RBAC/ABAC): `is_public_field`, `sanitize_for_elk`, `sanitize_for_cloud`. |
| **Registry** | Map internal `collection` name → loaded config; optional per-tenant overrides. |
| **Contracts** | Document and enforce: **public** ⊆ fields that may appear in prompts; **sensitive** never raw on egress. |

### What it should not own

| Keep elsewhere | Why |
|----------------|-----|
| **HTTP to Ollama / ai_agent** | Chat transport stays in `ollama.c` / `ai_agent.c`. |
| **Mongo wire protocol** | `storage.c` (or equivalent) does inserts; the module *consumes* docs and *returns* filtered views. |
| **Low-level Redis ELK clients** | Module outputs “safe payload” + index name; existing pumps send bytes. |
| **Full prompt assembly** | `api.c` / `ctx_build_prompt` still *calls* the module: “give me allowed fields + hints for this collection.” |

### Physical layout options (implementation, not required today)

1. **C library slice:** `include/shared_collection.h` + `src/shared_collection.c` — load JSON at `engine_init` or `api_create`, hang a `shared_collection_registry_t*` off `engine_t` or `api_context_t`.  
2. **Sidecar process:** Config service (Python/Go) that the engine queries rarely; heavier ops, easier hot-reload.  
3. **Mongo as source of truth:** One collection e.g. `shared_collections` with documents matching §2; module is read-mostly cache + validators.

### When a separate *repo* is worth it

Split into another package only if **multiple products** need the same policy engine without linking the full `m4engine`, or non-C runtimes must edit configs with shared validation. Until then, a **folder/module inside c-lib** is enough separation.

### Summary

| Approach | Fit |
|----------|-----|
| **Separate logical module in c-lib** | Recommended: one header, one implementation file (or small directory), called from storage ingest, ELK pump, and prompt build. |
| **Separate shared repo** | Only if you need reuse outside this engine or independent release cadence. |
| **No module (flags scattered in api.c)** | Works only while collection count and rules stay tiny. |

---

## 1. Problem this solves

| Pain | SharedCollection answer |
|------|-------------------------|
| LLM sees too much or wrong columns | **public** whitelist + **field_hints** + optional **sample_injection** |
| PII in logs / ELK / cloud | **sensitive** list + sanitize before ELK or external APIs |
| Same collection, many names | **collection** (internal) vs **alias** (LLM-facing label) |
| ELK vs vectors disagree | One config drives **elk** pump and **vector_engine** sources |
| “Thanh” vs “Province” style bugs | **metadata.description** + **allow_prompt** + **field_hints** |
| Cart has `product_id` but model needs **category** / **item** / **price** | **joins** + **async** **flat_snapshot** so one row is ELK/RAG/prompt-ready |

---

## 2. Schema (conceptual)

### 2.1 Core and security (gatekeeper)

Defines what exists in RAM and what may enter prompts or leave the trust boundary.

| Field | Type | Role | Status |
|--------|------|------|--------|
| **collection** | string | Internal name (Mongo collection, SQL table id, etc.). | **NOW** — parsed |
| **alias** | string | User- or LLM-friendly label (e.g. internal `tbl_001` → `customer_orders`). | **NOW** — needed for term vocab |
| **public** | array of strings | **Whitelist:** only these fields may be cached for RAG or injected into prompts. | **FUTURE** — RBAC/ABAC |
| **sensitive** | array of strings | **PII / secrets:** mask, hash, or drop before ELK export and before cloud LLM payloads. | **FUTURE** — RBAC/ABAC |

**Rule of thumb:** If a field is not in **public**, the prompt builder should not see it unless a separate, audited code path explicitly allows it. *(Enforced when RBAC/ABAC is implemented.)*

### 2.2 Metadata and AI context (“brain”)

Instructional layer to steer the model and document semantics without trusting the model to infer schema.

| Field | Type | Role | Status |
|--------|------|------|--------|
| **metadata** | object | Container for the fields below. | **NOW** |
| **metadata.description** | string | High-level context for this collection. | **NOW** — useful for term vocab |
| **metadata.allow_prompt** | boolean | If true, inject **description** into the system or context block. | **FUTURE** — prompt assembly |
| **metadata.field_hints** | map string → string | Human meanings for opaque keys (e.g. `dc` → physical delivery address). | **NOW** — needed for term vocab |
| **metadata.sample_injection** | integer | Max number of **anonymized** example rows to add as few-shot context; `0` = off. | **FUTURE** — prompt assembly |

### 2.3 ELK integration (data pump)

How enriched rows are pushed toward Elasticsearch (or compatible).

| Field | Type | Role | Status |
|--------|------|------|--------|
| **elk** | object | ELK subsection. | **NOW** |
| **elk.allow** | boolean | Enable background indexing for this collection. | **NOW** — parsed |
| **elk.index** | string | Target index. `”.”` or empty → `idx_{collection}`. | **NOW** — parsed |
| **elk.transform** | boolean | If true, flatten nested objects before index. | **FUTURE** |
| **elk.compose** | object (optional) | Index compose policy from **public** + **sensitive** rules. | **FUTURE** — RBAC/ABAC |

**Today:** cold backfill indexes the **full Mongo document** (+ `@timestamp`). The app layer is responsible for denormalizing documents at write time (embedding related fields like `category`, `product_name` directly). The engine does not do cross-collection joins.

**Future (RBAC/ABAC):** Apply **sensitive** masking and **public** field filtering before ELK when access control is implemented. Full compose spec: [elk_index_data.md](elk_index_data.md) §3–§6.

### 2.4 Vector engine (semantic side of hybrid search)

Aligns with the engine’s real embed/RAG paths: dimensions and family must match stored **metadata** (see embed-vector-metadata rule).

| Field | Type | Role |
|--------|------|------|
| **vector_engine** | object | Vector subsection. |
| **vector_engine.allow** | boolean | Enable semantic index for this collection (Redis L2 / Mongo-side vectors, etc.—exact store is implementation-specific). |
| **vector_engine.source_fields** | array of strings | Subset of **public** fields (or agreed derivations) used to build the text that gets embedded. |
| **vector_engine.dim** | integer | Expected vector length; must match **metadata.vector_dim** on stored documents. |
| **vector_engine.metric** | string | e.g. `cosine`, `dot_product`, `l2`—must match the scorer used at query time. |
| **vector_engine.l1_cache** | boolean | If true, keep a hot in-process index for lowest latency on small working sets (**optional** optimization). |
| **vector_engine.fallback_only** | boolean | If true, use this path only when hosted chat/embed tiers are unavailable or rate-limited—**policy hook**, not a requirement to implement N-gram SIMD unless you add it. |

**Reality check:** Today’s c-lib uses **Ollama embed** and/or **custom hash vectors** per `embed.h` / engine options—not necessarily N-gram SIMD. Treat **vector_engine** as the **configuration slot** that should map onto **`m4_embed_options_t`** and stored **embed_family** / **model_id**, not as a promise of a specific algorithm.

### 2.5 Nested collections: figure out relations, async → flat rows

> **[FUTURE — not for current implementation]** App layer should denormalize at write time. Engine-side joins are only needed if the app cannot control the document shape. Keep this section as design reference for that edge case.

Many useful prompts need **more than one Mongo collection**: e.g. **`carts`** lines have `product_id`, but the model should see **`category`**, a human **`item`** name, and **`price`** on the line—without dumping the whole **`products`** document.

**Preferred approach (now):** The app embeds related fields at write time. No engine joins needed.

**Fallback (future):** Declare **how to resolve foreign keys** into a **single flat, prompt-safe row**. Resolution can run **asynchronously** (worker after write, or batch backfill) so the hot path stays fast; the **flat snapshot** is what ELK, RAG embed text, and prompts consume.

| Field | Type | Role |
|--------|------|------|
| **joins** | array of **join** objects | Optional. Each entry describes one hop from this collection to another. |
| **join.local_field** | string | Field on **this** document whose value is the lookup key (e.g. `product_id`). |
| **join.target_collection** | string | Other collection name (e.g. `products`). |
| **join.target_key** | string | Field on the target document to match (e.g. `_id`, `sku`). Default `_id` if omitted. |
| **join.import_as** | object (map) | Map **target field name → flat output name**. Example: `{ "category": "category", "name": "item" }` yields flat keys `category` and `item` on the row. Names must land in **public** (or a dedicated **import_public** list—see below). |
| **join.optional** | boolean | If true and the target doc is missing, emit empty string / omit keys instead of failing the pipeline. |
| **join.async** | boolean | If true, primary write does **not** block on join; a **worker** (or queue) loads the target doc later and writes **`flat_snapshot`** (or a side collection). If false, resolve synchronously in the ingest path (small catalogs only). |
| **flat_snapshot** | string (path) | Where the engine stores the denormalized result—not a config field on disk, but a **convention**: e.g. subdocument `sc_flat` on the same doc, or collection `{collection}_flat`, or only ELK document body. Pick one per product and document it in section 7 (changelog). |
| **flat_public** | array of strings | Optional explicit list of keys allowed on the **flattened** row (local **public** keys + all values from **import_as**). If omitted, validate: every **import_as** value must appear in root **public**. |

**Difference from `elk.transform`:** **`elk.transform`** flattens **nested objects inside one document** (dots or brackets). **`joins`** pull **additional fields from other collections** and merge them into one **logical row** for search and LLM context.

**Carts example (intent):**

- **Collection:** `carts`
- **public:** `["cart_id", "user_id", "product_id", "qty", "line_price", "category", "item"]`  
  (last two are **filled by join**, not necessarily stored on the raw cart doc.)
- **joins:** one entry — `local_field: "product_id"`, `target_collection: "products"`, `target_key: "_id"`, `import_as: { "category": "category", "name": "item" }`, `optional: true`, `async: true`
- **Prompt / embed row:** `item`, `category`, `line_price` (and any other **public** locals like `qty`). Sensitive fields on `products` (e.g. supplier cost) never appear if they are not in **import_as** and **`products`** SharedCollection **public** is enforced on the fetched doc before merge.

**Async flow (recommended):**

1. Insert/update **cart** document (authoritative).
2. Enqueue **`sc_flatten_job`** `{ collection: "carts", _id }`.
3. Worker: load cart → for each **join**, fetch target by `target_key` == `local_field` value → build **flat object** F (local public fields + renamed imports) → apply **sensitive** on F as a whole → write **flat_snapshot** and/or re-index ELK / re-embed.

---

## 3. Data flow

### 3.1 Write path (ingestion)

1. **Validate** row against **public** / **sensitive** rules.
2. **Primary store** — persist authoritative document (e.g. Mongo) with **vector + metadata bundle** if vectors are enabled.
3. **Joins / flat row** — if **joins** present:
   - **Sync:** resolve targets, build **flat_snapshot**, then continue.
   - **Async:** enqueue flatten job; skip blocking ELK/RAG for this doc until worker completes (or use stale snapshot policy—document in changelog).
4. **ELK branch** — if **elk.allow**: sanitize (**sensitive**), optionally flatten (**elk.transform**), then prefer **flat_snapshot** body if present so ELK sees **`category`**, **`item`**, **`line_price`** on one document. Enqueue or POST to **elk.index**.
5. **Vector branch** — if **vector_engine.allow**: build text from **source_fields** using **flat row** if configured (`source_mode: "flat"` in future field) or raw doc; embed and write vector + **metadata** to the RAG store.

### 3.2 Read path (hybrid retrieval)

1. **Keyword / BM25** — query ELK (or Redis text index if that’s what you use) for candidates.
2. **Semantic** — embed query with the **same** family/dim as stored vectors; cosine (or **metric**) against L2/L1 index.
3. **Fuse** — combine scores (weighted sum, RRF, or tiered: vector gate then ELK rerank—**choose one policy** and document it next to this file when implemented).
4. **Prompt** — inject only **public** fields (and **field_hints**-expanded labels) plus **metadata** snippets allowed by **allow_prompt** and **sample_injection** caps. Prefer **flat_snapshot** when **joins** exist so nested relations do not require the LLM to “guess” `product_id` semantics.

---

## 4. Implementation notes for Cursor / C

- **Single source of truth:** Prefer one **SharedCollection** (or equivalent) config loaded at startup or per-tenant, not scattered `if (collection == "x")` branches.
- **Security:** **sensitive** must be enforced on **every** egress path (ELK, cloud LLM, logs)—same spirit as masking notes in [.cursor/ptomp.md](ptomp.md).
- **No raw-vector-only RAG:** When reading stored embeddings, use the document’s **metadata** bundle; see [embed-vector-metadata.mdc](rules/embed-vector-metadata.mdc).
- **Evolution:** Add a **version** or **embed_schema** field at the SharedCollection root when you need migrations between prompt or index layouts.

---

## 5. Quick reference checklist

### Now (NL cue learning + ELK query path)

- [ ] Parse `alias` and `field_hints` from SharedCollection JSON into `sc_entry_t`.
- [ ] **sc_term_vocab** built from registry at startup for NL cue→entity mapping.
- [ ] `alias` + `field_hints` updated when app renames columns.
- [ ] **vector_engine.dim** / **metric** consistent with stored **metadata** and query code.

### Future (RBAC / ABAC — do not implement yet)

- [ ] **public** whitelist defined and used by prompt assembly.
- [ ] **sensitive** applied before ELK and cloud.
- [ ] `sc_sanitize_for_elk` — filter fields per role before indexing.
- [ ] Hybrid **read path** score fusion documented where the code lives.
- [ ] **joins** defined for cross-collection fields; **async** flatten worker + **flat_snapshot** location documented.
- [ ] **import_as** targets vetted so no **sensitive** data leaks from joined collections.

---

## 5b. Learning cues integration — term vocabulary from SharedCollection

### Problem

The NL learning cues system (`nl_learn_cues.c`) records **intent** (ELK_ANALYTICS, ELK_SEARCH) from phrase patterns, but has no knowledge of **which collection or field** the user is asking about. The SharedCollection registry already holds the metadata needed to bridge this gap (`public`, `alias`, `field_hints`), but the C parser (`shared_collection.c`) only reads `collection`, `elk.allow`, and `elk.index` today.

### What the C parser ignores today

```
sc_entry_t (current):
  collection[160]    ← PARSED
  elk_allow          ← PARSED
  elk_index[160]     ← PARSED
  
  alias              ← IGNORED (not in struct)
  public[]           ← IGNORED (not in struct)
  field_hints{}      ← IGNORED (not in struct)
  sensitive[]        ← IGNORED (not in struct)
  metadata.*         ← IGNORED (not in struct)
```

### Proposed: build term vocabulary at startup

At `storage_connect` time, after `sc_registry_load_file` succeeds, build a **read-only vocabulary table** from the registry. This table maps user-facing words to collections and fields.

**Enhanced sc_entry_t** (parse additional fields from JSON):

```c
typedef struct {
    char collection[SC_STR_MAX];
    int elk_allow;
    char elk_index[SC_STR_MAX];
    /* NEW — for term vocab + prompt assembly */
    char alias[SC_STR_MAX];                        /* "Customer orders" */
    char public_fields[SC_FIELD_MAX][SC_STR_MAX];  /* ["order_id","status","dc"] */
    size_t n_public;
    struct { char key[64]; char hint[SC_STR_MAX]; } field_hints[SC_FIELD_MAX];
    size_t n_hints;
} sc_entry_t;
```

**Vocabulary table** (built once, lives in memory):

```c
typedef struct {
    char term[128];          /* lowercase normalized word */
    char collection[160];    /* target collection */
    char field[128];         /* target field (empty = collection-level match) */
    enum { VOCAB_COL, VOCAB_FIELD, VOCAB_HINT_VALUE } source;
} sc_term_vocab_entry_t;
```

**Build rules** (iterate all entries at startup):

| Source | Example input | Generated vocab entries |
|--------|--------------|----------------------|
| `collection` name | `"products"` | `"products"→products`, `"product"→products` (strip trailing 's') |
| `alias` (tokenized) | `"Customer orders"` | `"customer"→orders`, `"order"→orders` |
| `public[]` field names | `["status","dc","sold_date"]` | `"status"→orders.status`, `"dc"→orders.dc`, `"sold"→products.sold_date` |
| `field_hints` values (tokenized) | `{"dc":"Delivery address"}` | `"delivery"→orders.dc`, `"address"→orders.dc` |
| `field_hints` enum values | `{"category":"computer, phone, tablet"}` | `"computer"→products.category`, `"phone"→products.category` |

### Integration with nl_learn_cues

Update `nl_learn_cues_record_from_utterance` to accept an optional vocab pointer:

```c
void nl_learn_cues_record_from_utterance(
    nl_learn_terms_t *lt,
    const char *utterance_utf8,
    const sc_term_vocab_t *vocab      /* nullable — NULL = intent-only (today's behavior) */
);
```

When `vocab` is provided, after the existing 5-tier intent scan, also scan for vocab matches:

```
"how many computers sold this month?"
     │
     ├─ TIER SCAN (existing):
     │   "how many"   → record("how many", "ELK_ANALYTICS", +1)
     │   "this month" → record("this month", "ELK_ANALYTICS", +1)
     │
     └─ VOCAB SCAN (new):
         "computers"  → vocab hit: products.category
                        record("computers", "SC:products", +1)
         "sold"       → vocab hit: products.sold_date
                        record("sold", "SC:products", +1)
```

The `SC:` prefix on intent labels separates entity counts from routing intents. Requires relaxing `intent_on_closed_list` to accept `SC:*` prefix (or a separate allowlist check: `strncmp(t, "SC:", 3) == 0`).

Over time, `score_sum` can answer both:
- **Which intent?** `score_sum(["how many"], "ELK_ANALYTICS") = 1000`
- **Which collection?** `score_sum(["computers","sold"], "SC:products") = 380`

### Exists vs Missing

| Component | Status | Notes |
|-----------|--------|-------|
| `sc_registry_load_file` parses `collection`, `elk.allow`, `elk.index` | **EXISTS** | `src/shared_collection.c` |
| Parse `alias`, `public[]`, `field_hints` from JSON | **MISSING** | Need to extend `sc_entry_t` and parser |
| `sc_term_vocab_t` build from registry | **MISSING** | New function at startup |
| Vocab scan in `nl_learn_cues_record_from_utterance` | **MISSING** | Add after tier scan |
| `SC:` prefix allowlist in `nl_learn_terms.c` | **MISSING** | Small change to `intent_on_closed_list` |

---

## 5c. Denormalization strategy — app layer vs engine

### Principle

The **app layer** writes documents to Mongo already denormalized. If an order needs `category` and `product_name`, the app includes them at write time — it already has that data when creating the order. The engine does **not** do cross-collection joins.

```
APP LAYER responsibility:
  Order doc = { product_id, product_name, category, qty, price, status, ... }
                            ^^^^^^^^^^^^  ^^^^^^^^
                            app embeds these from product at write time

ENGINE responsibility:
  Read doc from Mongo → add @timestamp → index to ELK as-is
  No joins. No lookups. No knowledge of "orders have products."
```

This keeps the engine **generic** — it works with any collection structure the app defines.

### What the engine needs from SharedCollection (now)

Only the metadata for **vocab building** and **ELK indexing**:

```json
{
  "collection": "orders",
  "alias": "Customer orders",
  "elk": { "allow": true, "index": "." },
  "metadata": {
    "field_hints": {
      "category": "Product type (computer, phone, tablet)",
      "status": "Fulfillment state (pending, shipped, delivered)",
      "price": "Unit price in VND"
    }
  }
}
```

The engine uses `field_hints` keys to discover **what fields exist** and their hint text to build the **term vocabulary** for NL cue learning (§5b).

### Future: public / sensitive (RBAC / ABAC)

`public[]` and `sensitive[]` are reserved for a future access-control layer:

```json
{
  "public": ["order_id", "status", "category", "qty", "price"],
  "sensitive": ["email", "phone", "payment_token"]
}
```

These will control **who sees what** based on role or attribute — not needed for the current ELK query pipeline. The engine will filter fields before prompt injection, ELK export, or API response when RBAC/ABAC is implemented.

### Cold backfill today

```
storage_elk_backfill_collection (storage.c:580):

  for each doc in Mongo collection:
    bson_as_relaxed_extended_json(doc)     ← raw BSON → JSON (app already denormalized)
    elk_sync_pool_enqueue(index, id, json) ← push to ELK as-is + @timestamp

  Works because app layer already embedded all needed fields.
```

### Exists vs Missing

| Component | Status | Notes |
|-----------|--------|-------|
| Cold backfill (index raw docs to ELK) | **EXISTS** | `storage.c:580` — works if app denormalizes |
| Parse `alias`, `field_hints` from JSON | **MISSING** | Needed for term vocab (§5b) |
| **public/sensitive field filtering** | **FUTURE** | RBAC/ABAC — not needed for current ELK query path |
| **joins config / engine-side flatten** | **FUTURE** | Only if app layer cannot denormalize (rare edge cases) |

---

## 6. Implementation logic (markdown spec)

This section is the **executable specification in prose**: implement it in C (or another language) by following the steps and signatures below. It does not replace code; it defines **what** each function must do so multiple contributors stay aligned.

### 6.1 Config sources (priority)

1. **Primary:** **`api_options_t.shared_collection_json_path`** — absolute path to a JSON file (`{ "collections": [ ... ] }`). The C library does **not** read `M4_SHARED_COLLECTION_JSON` or other SharedCollection env vars; Python (`training/full_options`) may default the path from **`server/shared_collection_catalog`** when mode is ELK and `es_host` is set.
2. **Optional Mongo (future):** collection `shared_collections`, one document per config, keyed by `collection` field.
3. **Empty registry:** If the path is unset or empty, ELK has no SharedCollection registry (no policy for SC-backed indexing).

**Required shape (file registry):** Top level must include a **`collections`** array; each object should have a **`collection`** string (Mongo collection name). Optional **`elk`** / **`vector_engine`** blocks per section 6.2 (`elk.allow`, `elk.index`, …). The file is **untrusted** from the engine’s perspective.

**Mongo existence check (integration safety):** When the registry loads and **`USE_MONGOC`** is enabled, **`storage_connect`** checks each **`collection`** name against the backfill database (**`api_options_t.shared_collection_backfill_db`**, else default **`m4_ai`** from `STORAGE_MONGO_DB_NAME`) via **`mongoc_database_has_collection`**. Missing names log **`[STORAGE][SharedCollection] WARNING: ...`**.

**Reload:** On `SIGHUP` or admin API (future), re-parse and swap registry pointer atomically (read-copy-update) so readers never see a half-loaded table.

### 6.2 Minimal JSON example

For **nested collections** (`product_id` → `products.category`, `name` as `item`, …), see **section 6.7c**.

```json
{
  "collections": [
    {
      "collection": "orders",
      "alias": "Customer orders",
      "public": ["order_id", "status", "dc", "line_items"],
      "sensitive": ["email", "phone", "payment_token"],
      "metadata": {
        "description": "Retail order headers and lines. Province codes are ISO-3166-2.",
        "allow_prompt": true,
        "field_hints": { "dc": "Delivery address (street/city)", "status": "Fulfillment state" },
        "sample_injection": 2
      },
      "elk": { "allow": true, "index": ".", "transform": true },
      "vector_engine": {
        "allow": true,
        "source_fields": ["status", "line_items"],
        "dim": 384,
        "metric": "cosine",
        "l1_cache": false,
        "fallback_only": false
      }
    }
  ]
}
```

**Index default:** If `elk.index` is `"."` or missing, resolved name is `idx_{collection}` with safe character normalization (alphanumeric + `_`).

### 6.3 In-memory model (conceptual struct)

Document in markdown what the loader materializes (exact C types are up to implementation):

| Logical member | Notes |
|----------------|--------|
| `collection` | Normalized key for lookup (UTF-8, case policy: recommend **case-sensitive** internal). |
| `alias`, `public[]`, `sensitive[]` | Copied strings or interned in an arena for stable pointers. |
| `metadata.*` | Copied; `field_hints` as flat key/value array or hash map. |
| `elk.*` | Booleans + resolved index string. |
| `vector_engine.*` | Booleans + `source_fields[]` + numeric dim + enum for metric. |
| `schema_version` | Optional root field for migrations. |

### 6.4 Public API (pseudocode)

Use names like `sc_*` for **shared_collection** to avoid collisions with `storage_*`, `api_*`.

```
/* Lifecycle */
int  sc_registry_init(sc_registry_t **out);
void sc_registry_destroy(sc_registry_t *reg);
int  sc_registry_load_json_file(sc_registry_t *reg, const char *path);
int  sc_registry_load_mongo(sc_registry_t *reg, storage_ctx_t *st);  /* optional */
const sc_config_t *sc_lookup(const sc_registry_t *reg, const char *collection);

/* Field policy */
int  sc_is_public(const sc_config_t *cfg, const char *field_name);     /* 1 if in public[] */
int  sc_is_sensitive(const sc_config_t *cfg, const char *field_name); /* 1 if in sensitive[] */

/* Document transforms (output buffers sized by caller; return -1 on overflow) */
int  sc_filter_document_for_prompt(const sc_config_t *cfg,
       const char *json_or_bson_doc_in, char *out_json, size_t out_cap);
       /* Keep only keys in public[]; expand keys using field_hints in a "_hints" sidecar or inline comments — choose one format and document it. */

int  sc_sanitize_for_elk(const sc_config_t *cfg,
       const char *doc_in, char *out_json, size_t out_cap);
       /* Drop or replace sensitive[] values with literal "[REDACTED]" (or hash); then flatten if elk.transform. */

int  sc_build_embed_text(const sc_config_t *cfg,
       const char *doc_in, char *text_out, size_t text_cap);
       /* Concatenate values of vector_engine.source_fields in stable order; UTF-8; skip missing keys. */

int  sc_append_prompt_context_block(const sc_config_t *cfg,
       char *prompt_buf, size_t prompt_cap, size_t *inout_len);
       /* If metadata.allow_prompt: append a fixed header + alias + description + serialized field_hints (bounded). */

int  sc_validate_config(const sc_config_t *cfg, char *errbuf, size_t errcap);
       /* Ensure source_fields ⊆ public (or explicit override flag); dim > 0 if vector_engine.allow; etc. */

/* Nested collections → flat row (see section 2.5) */
int  sc_apply_joins_sync(storage_ctx_t *st, const sc_registry_t *reg,
       const sc_config_t *cfg, const char *root_doc_json, char *flat_json_out, size_t flat_cap);
       /* Resolve each join: fetch target doc, take only keys listed in import_as (source side), rename to flat keys.
          Merge with root doc keys that are in cfg->public. Apply target collection's SharedCollection filter if reg has one. */

void sc_flatten_enqueue(const sc_registry_t *reg, const char *collection, const char *doc_id_utf8);
       /* Push job for async worker (Mongo change stream, queue, or periodic sweep). */

int  sc_flatten_worker_once(storage_ctx_t *st, const sc_registry_t *reg, sc_flatten_job_t *job);
       /* Load root doc by id, run sc_apply_joins_sync, write flat_snapshot to agreed path, trigger ELK/RAG hooks. */
```

**`sc_flatten_job_t` (conceptual):** at minimum `{ collection, document_id }` (UTF-8); optional `tenant_id` if multi-tenant.

### 6.5 Algorithm: `sc_filter_document_for_prompt`

**Input:** `cfg`, document `D` (parsed object). **Output:** JSON object string `P`.

1. If `cfg == NULL`, return **legacy passthrough** (copy `D` truncated to `out_cap`) or **empty object** — product policy; document the choice in code comments.
2. Initialize empty object `P`.
3. For each key `k` in `D`:
   - If `sc_is_public(cfg, k)`: copy value of `k` into `P`.
   - Else: omit `k`.
4. If `metadata.sample_injection > 0`: this function does **not** add samples; samples come from a **separate** store query capped by that integer (see 6.8).
5. Serialize `P` to `out_json`. Return `0` or `-1` on error.

### 6.6 Algorithm: `sc_sanitize_for_elk`

1. Parse `doc_in` to object `D`.
2. Deep-copy to working object `E`.
3. For each key `k` in `sensitive[]`: if `k` exists in `E`, set value to `"[REDACTED]"` or remove key (choose one; **redact** is safer for ELK mapping stability).
4. If `elk.transform`: flatten nested objects to dotted keys (e.g. `a.b` → single level), array handling policy documented (join, or skip, or index suffix).
5. Serialize `E` to `out_json`.

### 6.7 Algorithm: `sc_build_embed_text`

1. If `!vector_engine.allow`, return `-1` or empty string per convention.
2. Let **D′** = **flat_snapshot** merged over `D` if flat exists and policy says **prefer_flat** (future flag); else `D`.
3. For each `f` in `source_fields[]` in order:
   - Look up `f` in **D′**; append UTF-8 value plus separator (e.g. `\n` or ` | `) to `text_out`.
4. Ensure resulting text length `< text_cap`. No sensitive field should appear if **source_fields** validated ⊆ **public** (and ⊆ **flat_public** when using flat rows).

### 6.7b Algorithm: `sc_apply_joins_sync` (figure out nested collection → flat)

**Input:** storage handle, registry, `cfg` for **root** collection (e.g. `carts`), `root_doc_json`.

**Output:** `flat_json_out` — single-level JSON object.

1. Parse root document `R`.
2. Initialize flat object `F` empty.
3. For each key `k` in `R` where `sc_is_public(cfg, k)` and `k` is not purely a foreign-key placeholder you plan to hide (optional **hide_local_fk** list): `F[k] = R[k]`.  
   - Product choice: either keep **`product_id`** in **public** for debugging or omit from **public** so the LLM only sees **`item`** / **`category`**.
4. For each **join** `J` in `cfg->joins`:
   - Read `v = R[J.local_field]`. If missing and `J.optional`, skip join.
   - `target_cfg = sc_lookup(reg, J.target_collection)`.
   - `T = storage_find_one(J.target_collection, J.target_key, v)`.
   - If `T` missing: if `J.optional`, skip; else return error.
   - For each pair `(src_name, dst_name)` in `J.import_as`:
     - If `target_cfg`: allow only if `sc_is_public(target_cfg, src_name)` (enforce joined collection policy).
     - `F[dst_name] = T[src_name]`.
5. Run **sensitive** masking on `F` using **root** `cfg` **sensitive** list; also mask any key in `F` that appears in `target_cfg->sensitive` if you merge policies (recommended: union of sensitive names from root + imported keys only).
6. Serialize `F` to `flat_json_out`.

**Async:** `sc_flatten_enqueue` stores `{collection, _id}`; worker runs step 1–6 and writes e.g. `R.sc_flat = F` or updates **`carts_flat`** collection, then calls ELK/RAG refresh for that id.

### 6.7c JSON: `carts` + `products` (nested → flat)

**`products` SharedCollection** (target):

```json
{
  "collection": "products",
  "alias": "Product catalog",
  "public": ["_id", "sku", "name", "category", "list_price"],
  "sensitive": ["supplier_id", "internal_cost"],
  "metadata": { "description": "Sellable SKU master.", "allow_prompt": false },
  "elk": { "allow": true, "index": ".", "transform": true },
  "vector_engine": { "allow": true, "source_fields": ["name", "category"], "dim": 384, "metric": "cosine" }
}
```

**`carts` SharedCollection** (root line item — nested resolution):

```json
{
  "collection": "carts",
  "alias": "Cart lines",
  "public": ["cart_id", "user_id", "product_id", "qty", "line_price", "category", "item"],
  "sensitive": ["user_id"],
  "joins": [
    {
      "local_field": "product_id",
      "target_collection": "products",
      "target_key": "_id",
      "import_as": { "category": "category", "name": "item" },
      "optional": true,
      "async": true
    }
  ],
  "metadata": {
    "description": "Shopping cart lines; item/category come from products.",
    "allow_prompt": true,
    "field_hints": { "line_price": "Price charged for this line", "qty": "Quantity" }
  },
  "elk": { "allow": true, "index": ".", "transform": false },
  "vector_engine": {
    "allow": true,
    "source_fields": ["item", "category", "line_price", "qty"],
    "dim": 384,
    "metric": "cosine"
  }
}
```

**Example flat row** (what ELK / prompt / embed see after join):

```json
{
  "cart_id": "c1",
  "product_id": "p9",
  "qty": 2,
  "line_price": 19.99,
  "category": "Beverages",
  "item": "Cold brew 12pk"
}
```

Omit **`user_id`** from prompt if it stays in **sensitive**; **`product_id`** can be removed from **public** if you want the model to rely only on **`item`** / **`category`**.

### 6.8 Algorithm: `sc_append_prompt_context_block`

1. If `cfg == NULL` or `!metadata.allow_prompt`, return `0` without writing.
2. Append bounded block, e.g.:

   ```
   [SHARED_COLLECTION: {alias}]
   {metadata.description}
   Field hints: dc → … ; status → …
   ```

3. Respect `prompt_cap - *inout_len`; if truncated, return `-1` and log.

4. **Few-shot samples:** If `sample_injection > 0`, caller (not this function) runs a **read-only** query on the primary store, takes up to **N** rows, runs each through `sc_filter_document_for_prompt`, then appends as `[EXAMPLE]` blocks. Never attach raw rows without filtering.

### 6.9 Algorithm: hybrid read path (orchestrator)

**Caller:** RAG layer before `ctx_build_prompt` or dedicated hybrid search API.

```
function hybrid_search(query_text, collection_name):
  cfg = sc_lookup(reg, collection_name)
  if cfg == NULL:
    fall back to existing RAG-only or ELK-only behavior

  candidates_elk = []
  if cfg.elk.allow:
    candidates_elk = elk_search(resolved_index(cfg), query_text, K_elk)

  candidates_vec = []
  if cfg.vector_engine.allow:
    qvec = embed_query(query_text)   /* must match embed_family + dim on stored docs */
    candidates_vec = redis_or_mongo_knn(qvec, K_vec, collection_name)

  merged = fuse(candidates_elk, candidates_vec, policy="document_here")  /* e.g. RRF or 0.6*vec + 0.4*elk */

  context_parts = []
  for doc in merged_top_M:
    buf = ...
    sc_filter_document_for_prompt(cfg, doc, buf, sizeof buf)
    context_parts.append(buf)

  return join(context_parts)
```

Record the chosen **fuse** policy in a one-line comment above the implementation and in this file’s changelog when it changes.

### 6.10 Validation rules (`sc_validate_config`)

| Check | On failure |
|--------|------------|
| `collection` non-empty | Error |
| `public` non-empty for new strict mode | Warning or error (product choice) |
| Each `source_fields[i]` appears in `public` | Error |
| `vector_engine.allow` && `dim` matches engine default or `m4_embed_options_t` | Error |
| `sample_injection` ≤ hard cap (e.g. 10) | Error |
| Each **import_as** value (flat name) ∈ **public** on root config (or listed in **flat_public**) | Error |
| Each **import_as** key (source field) ∈ **public** on **target** SharedCollection | Error |
| **joins** cycle (A→B→A) | Error at load time |

### 6.11 Hook points in c-lib (where to call later)

| Phase | Suggested location | Call |
|--------|-------------------|------|
| Engine / API startup | After `engine_init` or `api_create` | `sc_registry_load_*` |
| ELK ingest | Before bulk index in storage/ELK bridge | `sc_sanitize_for_elk` |
| RAG write | When building text for `m4_embed_*` for a named business collection | `sc_build_embed_text` + store **metadata** bundle per embed rule |
| After cart/line write | Storage insert hook or change stream | `sc_flatten_enqueue` when **joins[].async** |
| Background worker | Dedicated thread / queue consumer | `sc_flatten_worker_once` → write **flat_snapshot** → ELK/RAG; future: **section 10** pool bulk ELK |
| Prompt build | `ctx_build_prompt` or tag injection site | `sc_append_prompt_context_block`; prefer **flat_snapshot** in `sc_filter_document_for_prompt` when present |
| Cloud LLM egress | Before JSON body build in `ai_agent.c` if attaching extra JSON | Re-run **sensitive** policy on any structured attachment |

### 6.11b Current c-lib reality (audit — bulk ELK / pthreads on init)

**SharedCollection → ELK:** There is **no** implemented `shared_collection` module and **no** code path that **on engine init** bulk-syncs Mongo `shared_collections` (or arbitrary business collections) into Elasticsearch.

**ELK in tree today:**

- **`storage_elk_ingest`** in `src/storage.c` is a **stub** (TODO comment; body is effectively no-op). It is **called** from **`storage_append_ai_log`** when `es_host` is set, but it does **not** perform a real HTTP ingest yet.
- **`.cursor/elk.md`** describes the intended pipeline (`ai_index`, `auto_lang_processor`); implementation is not complete in `storage_elk_ingest`.

**Pthreads / async workers that already exist (for reuse as patterns, not for SharedCollection ELK today):**

| Mechanism | File | Role |
|-----------|------|------|
| **Embed migration worker** | `src/embed_worker.c` | `pthread` + mutex/cond + queue; started from **`engine_init`** (`engine.c`) for provenance / re-embed jobs — **Mongo/embed**, not ELK bulk. |
| **Geo learning worker** | `src/geo_learning.c` | Background **`pthread`** processes enqueued chat turns (Ollama extract → Mongo `geo_atlas` / Redis). **ELK audit** for geo is documented as **future** in `.cursor/geo_leanring.md`. |
| **Stream worker** | `src/api.c` | **`pthread_create`** per **`api_chat`** stream call; joined before return — **Ollama stream only**, not startup bulk. |

**Mongo bulk:** **`storage_upsert_batch`** is also a **TODO** stub (no mongoc bulk insert yet).

**Implication for “initial bulk sync to ELK”:** You would **add** something new — e.g. enqueue jobs from `engine_init` (or a dedicated admin API) and drain them in a worker modeled on **`embed_worker.c`** (queue + background thread + libcurl `_bulk` to Elasticsearch), applying **`sc_sanitize_for_elk`** per document. Until then, **do not assume** any automatic sync of **shared_collections** or flattened rows to ELK on startup.

### 6.12 Testing matrix (markdown-driven QA)

| Case | Expected |
|------|----------|
| Doc with extra keys | Prompt filter drops non-**public** |
| Doc with **sensitive** only in ELK path | ELK payload redacted |
| `source_fields` contains private key | `sc_validate_config` fails at load |
| `cfg == NULL` | Documented fallback behavior |
| Huge description | `sc_append_prompt_context_block` truncates safely |
| Join target missing, **optional: true** | Flat row has no imported keys; no crash |
| Join target has **sensitive** field not in **import_as** | Must not appear in **F** |
| Async job runs twice | Idempotent overwrite of **flat_snapshot** |

---

## 7. Changelog (edit when behavior changes)

| Date | Change |
|------|--------|
| 2026-03-29 | Added **joins**, async **flat_snapshot**, **carts/products** example, `sc_apply_joins_sync` / worker hooks. |
| 2026-03-29 | **6.11b** — audit: no SharedCollection ELK bulk on init; `storage_elk_ingest` stub; pthread patterns (`embed_worker`, `geo_learning`, stream). |
| 2026-03-29 | **Section 10** — pthread ELK bulk pool, `shared_collections` change stream → rescan/flat → pool, cold-start backfill. **Section 11** — index. |
| (add rows) | e.g. fuse policy = RRF, ELK redaction = `[REDACTED]`, **flat_snapshot** stored at `sc_flat` subdoc |

---

## 8. Tutorial guidelines (how to read and how to write docs)

Use this section so **onboarding tutorials** stay consistent with the spec and **new authors** do not fork ad-hoc rules.

### 8.1 Recommended reading order (learners)

| Step | Section | Outcome |
|------|---------|---------|
| 1 | **Section 1** (problem table) | Know which pain each knob fixes. |
| 2 | **Section 2.1–2.4** | Understand **public** / **sensitive** / **metadata** / **elk** / **vector_engine**. |
| 3 | **Section 2.5** | Add **joins** only after single-collection rules are clear. |
| 4 | **Section 3** | See write vs read path end-to-end. |
| 5 | **Section 6.2** then **6.7c** | Copy JSON patterns: simple collection, then **carts/products** nested example. |
| 6 | **Section 6.5–6.7b** | Implement or review code against algorithms. |
| 7 | **Section 5** checklist + **6.12** tests | Verify before merge. |

**Integration tutorial (whole engine):** after this doc, read **[docs/TUTORIAL_BINDINGS.md](../docs/TUTORIAL_BINDINGS.md)** for `make lib`, Python, HTTP, and env — it links back here for **SharedCollection**.

### 8.2 Hands-on lab order (tutorials you ship to app teams)

1. **One collection, no joins** — define `public` / `sensitive`, optional **elk** + **vector_engine**, load JSON via `M4_SHARED_COLLECTION_JSON`.
2. **Prompt block** — toggle **metadata.allow_prompt** and show before/after in `api_chat` context.
3. **Add a join** — duplicate **section 6.7c**: `products` first, then `carts` with `import_as`; run **async** flatten and show **flat_snapshot** in ELK or prompt.
4. **Hybrid search** — keyword + vector using the same **public** / **flat** row (section 6.9).

Do not start labs with **auto-discovery** (section 9); it is optional and not required for **explicit joins**.

### 8.3 Style rules for contributors editing this file

| Rule | Detail |
|------|--------|
| **Spec vs story** | Normative behavior lives in **tables** and **numbered algorithms**; marketing language belongs outside this file. |
| **One example per concept** | Extend **6.7c** or add `6.7d` with a new JSON block; avoid duplicating full carts JSON in prose. |
| **Changelog** | Any change to fuse policy, redaction string, or **flat_snapshot** storage location gets a row in **section 7**. |
| **Cross-links** | Link to [embed-vector-metadata.mdc](rules/embed-vector-metadata.mdc) when mentioning vectors; link **TUTORIAL_BINDINGS** for “how do I run the server?”. |
| **Future / unimplemented** | Label optional features (sections **9**, **10**) as **design only** until code exists. |

### 8.4 What integration tutorials should say about SharedCollection

When you write or update **[TUTORIAL_BINDINGS.md](../docs/TUTORIAL_BINDINGS.md)** (or internal wiki):

- Mention **SharedCollection** as **declarative policy** (prompt whitelist, ELK, vectors, joins), not as a mandatory shipped module.
- Point to **`M4_SHARED_COLLECTION_JSON`** and (when implemented) Mongo `shared_collections` — same as **section 6.1**.
- State clearly: **without config**, the registry is empty and behavior is **legacy / product default** — do not imply automatic PII stripping unless `sc_*` is wired.

---

## 9. Optional design: join-key discovery (L1 / L2 / L3)

**Status:** Design only — not required when **joins** are **explicit** in JSON (section 2.5). Use this when you want faster “does this `product_id` exist?” checks before hitting Mongo for the full document.

### 9.1 Goal

Cheaply test whether `collection_A.field_x` likely references `collection_B`’s primary key, using a **tiered cache**; **never** treat a cache miss as “key does not exist” until **Mongo** confirms.

### 9.2 Tiered flow

| Tier | Mechanism | On hit | On miss |
|------|-----------|--------|---------|
| **L1** | In-process set or Bloom filter of known primary keys per `target_collection` (bounded size). | Skip Redis/Mongo for existence probe only. | Fall through to L2. |
| **L2** | Redis `SISMEMBER` on set e.g. `{collection}:pk` populated from ingest. | Confirm id likely exists. | Fall through to L3. |
| **L3** | Mongo indexed lookup (`findOne` on `target_key`). | Authoritative; **async** refresh L2 + L1 for that id. | Join fails (or **optional** join skips). |

**Reliability:** If L1/L2 miss, **always** query L3 before failing a non-optional join. If L3 finds the document, **write-back** to L2 (e.g. `SADD`) and update L1 so the next request is cheaper.

### 9.3 Optional config fields (future)

| Field | Type | Role |
|--------|------|------|
| **join.discovery** | object | Per-join overrides; omit if all lookups go straight to Mongo. |
| **discovery.auto_join** | boolean | If true, allow heuristics to **propose** `local_field` → `target_collection` (offline job); human must still commit to **joins** in JSON. |
| **discovery.sample_depth** | integer | How many documents to sample when suggesting relations (e.g. `5`). |
| **discovery.lookup_order** | array of strings | Default `["L1", "L2", "L3"]` — sequential probe order. |

### 9.4 Learned relations (optional)

A **`relation_registry`** collection (or file) can store **confirmed** mappings such as `carts.product_id → products._id` after a human or job approves them. **Do not** auto-trust guessed relations for **sensitive** paths without review.

**Tutorial note:** Teach **explicit joins** (section 2.5 / 6.7c) first; introduce section 9 only in an **“optimization / scale”** advanced module.

---

## 10. Design: background ELK sync pool + change stream + cold start

**Status:** Design only — not implemented in c-lib today (see **section 6.11b**). This section specifies **markdown logic** for a **pthread worker pool** that **bulk-indexes** flattened documents to Elasticsearch **without blocking** `api_chat` or `engine_init` beyond enqueueing work.

### 10.1 Goals

| Goal | Mechanism |
|------|-----------|
| ELK writes never block the hot path | All HTTP bulk work runs in **background worker threads** fed by a **bounded queue**. |
| Policy changes propagate | **Watch** the **`shared_collections`** source (Mongo **change stream** or file reload); when **joins** / **elk** / **public** change, **re-flatten** affected data and enqueue **ELK bulk** jobs. |
| Cold start is consistent | On **initial** startup (or first pool start), **scan** each registered business collection, **apply current join policy** → **flat row** → enqueue **index** jobs so ELK catches up with Mongo. |

### 10.2 ELK sync thread pool (bulk, background)

**Pattern:** Reuse the spirit of **`embed_worker.c`** (mutex + cond + queue + dedicated `pthread`s), but with **multiple consumer threads** (`pool_size` ≥ 1) draining one **job queue**.

| Component | Responsibility |
|-----------|----------------|
| **`elk_sync_queue_t`** | Bounded FIFO of **`elk_sync_job_t`** (see 10.5). Drop-oldest or block-with-timeout on overflow — document policy; prefer **block** with back-pressure so Mongo scan does not flood memory. |
| **Worker threads** | `N` × `pthread` loop: dequeue job → build Elasticsearch **Bulk** API body (NDJSON) → `libcurl` POST to `/_bulk` (or index-per-job if bulk disabled) → on failure: `fprintf(stderr, …)` + optional retry counter; **never** `pthread_join` from `api_chat`. |
| **Main / engine thread** | **`elk_sync_pool_start(pool, n_workers)`** after Mongo + registry ready; **`elk_sync_pool_stop`** on shutdown (broadcast + join workers). |
| **ELK payload** | Each job carries **resolved index name** (from SharedCollection **elk.index**), **sanitized JSON** from **`sc_sanitize_for_elk`**, optional `_id` for idempotent updates. |

**Bulk batching:** Workers may **coalesce** multiple jobs up to `max_bulk_bytes` / `max_bulk_docs` before one HTTP request to reduce QPS.

### 10.3 Change stream: watch `shared_collections` → detect join policy → enqueue flat work

**When the registry is backed by Mongo** (collection name e.g. `shared_collections` per section 6.1):

1. **Open** a Mongo **change stream** on that collection (`insert`, `update`, `replace`, `delete`).
2. On each event:
   - **Parse** the new/removed SharedCollection document.
   - **`sc_validate_config`**; on success **swap** registry entry (RCU / lock).
   - **Diff** previous vs new **`joins`**, **`public`**, **`elk.allow`**, **`import_as`**.
   - If join or ELK-relevant fields changed:
     - Enqueue **`JOB_RESCAN_COLLECTION`** for the affected **`collection`** name (the **root** collection whose policy changed, e.g. `carts`).
     - Optionally enqueue **`JOB_RESCAN_TARGET`** for **`target_collection`** values that appear in new joins (e.g. `products`) so denorm keys stay consistent — product policy.

3. A **scheduler thread** (or one pool worker) expands **`JOB_RESCAN_COLLECTION`** into many **`JOB_FLATTEN_AND_INDEX`** jobs (batched cursor over Mongo `carts`).

**File-backed JSON registry:** If only **`M4_SHARED_COLLECTION_JSON`** is used, replace change stream with **`SIGHUP`** or periodic **`mtime`** poll → reload file → same **diff → enqueue rescan** logic.

### 10.4 Initial startup: fetch → detect join policy → flat → pool

**Runs once** when the ELK sync pool starts (or on explicit admin “reindex”):

```
function cold_start_elk_backfill():
  reg = load_registry()   /* JSON or Mongo */
  if reg empty: return

  for each cfg in reg.collections where cfg.elk.allow == true:
    coll = cfg.collection
    cursor = mongo_find_all(coll)   /* or tenant-scoped; respect indexes */
    for each doc in cursor (batched):
      if cfg.joins non-empty:
        F = sc_apply_joins_sync(storage, reg, cfg, doc)   /* section 6.7b */
      else:
        F = sc_filter_document_for_prompt(cfg, doc, ...)   /* public-only slice */
      sanitized = sc_sanitize_for_elk(cfg, F, ...)
      enqueue JOB_ELK_INDEX { index, sanitized, _id: doc._id }

  /* Do not block: cold start only enqueues; workers drain in background. */
```

**Ordering:** Load **registry first** (so **target** SharedCollections exist for **`sc_apply_joins_sync`** public checks). If **`products`** config is missing, flatten step should **optional**-skip join fields per **join.optional**.

**Throttling:** Use **cursor batch size** (e.g. 500) and **queue high-water** mark to avoid OOM on huge collections.

### 10.5 Job types (queue payload sketch)

| Job kind | Fields (conceptual) | Producer |
|----------|---------------------|----------|
| **`JOB_ELK_INDEX`** | `index`, `body_json`, `doc_id` | Flatten path, cold start, rescan worker |
| **`JOB_FLATTEN_AND_INDEX`** | `collection`, `doc_id` | Change stream expansion, single-doc retry |
| **`JOB_RESCAN_COLLECTION`** | `collection` | Policy change from change stream |
| **`JOB_BULK_FLUSH`** | (internal) | Worker coalescer timer — optional |

### 10.6 Interaction with flatten async (section 2.5)

| Path | Role |
|------|------|
| **Per-write `sc_flatten_enqueue`** | Fast path after cart insert — enqueue **`JOB_FLATTEN_AND_INDEX`** instead of a separate single-thread worker if the **pool** is the only async sink. |
| **Pool workers** | Execute flatten + sanitize + bulk ELK; same threads can run **`sc_apply_joins_sync`** as long as **storage** calls are thread-safe or each job pins to one thread with **storage lock** (match **python_ai** `api_*` serialization if dual-host). |

### 10.7 Shutdown and idempotency

- **Shutdown:** Set **quit flag** → broadcast condition → **`pthread_join`** each worker → drain or log dropped queue items.
- **Idempotency:** Use stable **`_id`** in bulk operations so **re-scans** after policy change **overwrite** same ELK document.
- **ELK down:** Log and optionally **re-queue** with cap; do not block Mongo writes.

### 10.8 Tutorial / implementation order

1. Implement **`storage_elk_ingest`** or direct **`/_bulk`** helper (see **6.11b** gap).
2. Add **single-thread** queue + one worker (like **`embed_worker`**).
3. Add **pool** (`N > 1`) + bulk batching.
4. Wire **cold_start_elk_backfill** after registry load.
5. Add Mongo **change stream** on **`shared_collections`** (or file watch).

### 10.9 Changelog pointer

When this ships, add a row in **section 7** with: pool size default, bulk limits, and whether change stream is Mongo-only.

---

## 11. Related sections index

| Topic | Section |
|--------|---------|
| Join / flat algorithms | 2.5, 6.7b, 6.7c |
| Current code reality (stubs, pthreads today) | 6.11b |
| Optional L1/L2/L3 key probe | 9 |
| **ELK pool + stream + cold start** | **10** |
| Tutorial order | 8 |

