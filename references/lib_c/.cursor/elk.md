# ELK module — rules

Do not modify without updating **include/elk.h** if API or constants change.  
Rule reference is injected at the top of `include/elk.h`.

---

## 1. Scope

ELK module provides: **initial** (base URL / config, no persistent connection), **set** (ingest document, index doc), **search** (optional). Used for auto-language ingest pipeline and analytics when execution mode is MONGO_REDIS_ELK.

## 2. Ingest pipeline

- Every ingest MUST use pipeline **`auto_lang_processor`** for real-time language detection (rule §5). Index with `?pipeline=auto_lang_processor`.
- **Set (ingest):** POST JSON to `{base_url}/ai_index/_doc?pipeline=auto_lang_processor`. Body e.g. `{"raw_content": raw_text}` or equivalent. Use libcurl; timeout 10s, TCP_NODELAY 1.
- **Fallback:** If ELK is down, default to English (en-US). No Iraq/Thai hallucination (rule §5).

## 3. Connection and lifecycle

- **Stateless HTTP:** No long-lived connection. Base URL from config (host:port, e.g. 127.0.0.1:9200). Use 127.0.0.1 for local (not localhost).
- **Initial:** Store base URL and optional index name; no connect call. Optional health check GET on first use.
- **Destroy:** N/A (no connection to close).

## 4. Index and search (optional)

- Index name: e.g. `ai_index` or from config. Document ID optional; pipeline handles language.
- **Search:** Optional full-text or filtered search API; not required for minimal ingest-only use. When implemented, use same base URL and index.

## 5. Geo learning — **observability** (separate from `ai_index`)

Geo landmarks are **not** ingested through `auto_lang_processor` for the default design. Use a **dedicated audit index** for **OBSERVABLE** telemetry only:

| Aspect | Guidance |
|--------|----------|
| **Purpose** | Dashboards (Kibana): insert rate, `verification_status` mix, per-tenant growth — **not** chat or RAG. |
| **Index** | Suggested: `geo_atlas_audit` (see `.cursor/geo_leanring.md` §8). |
| **Pipeline** | Usually **omit** pipeline (structured JSON). Do not block `api_chat`. |
| **Implementation** | Not in `storage_elk_ingest` stub today; add async HTTP from `geo_learning` worker or a small queue. Same **non-critical** rules: log warning on failure, no retries on hot path. |

Full schema, ECS-style fields, and Kibana ideas: **`.cursor/geo_leanring.md` §8**.

## 6. SharedCollection pipeline logging

- **`M4_ELK_LOG`**: **unset** defaults to flow milestones on stderr (`[ELK flow]`); set **`0`** / `false` / `off` to disable. **`2`** / `verbose` — log each successful `elk_index_json` (heavy on bulk). See `include/m4_elk_log.h`, `storage.c`, `elk_sync_pool.c`.
- **`M4_ELK_DIAG=1`**: one-shot registry/pool/Mongo snapshot at `storage_connect`.

## 7. Stats flags (`elk_enabled` / `elk_connected`)

> Merged from `ELK_KIBANA.md`.

| Field | Meaning |
|-------|---------|
| **`elk_enabled`** | `api_stats_t`: engine config has a non-empty `es_host`. Only says "we intend to talk to ES". |
| **`elk_connected`** | One-shot HTTP GET to `http://{es_host}:{es_port}/` within 2s. 401/403 = reachable. Timeout/refused = not. |

### SharedCollection indexing (what runs today)
- JSON loaded from `api_options_t.shared_collection_json_path` when `es_host` is set.
- Per collection with `elk.allow: true`, index name = `elk.index` or default `idx_{collection}`.
- Optional prefix: `M4_ELK_INDEX_PREFIX` (e.g. `m4_`).
- Cold backfill: `storage_elk_cold_backfill` walks Mongo and enqueues all docs.
- Live: `M4_SHARED_COLLECTION_STREAM=1` (change stream) or `M4_SHARED_COLLECTION_FILE_POLL=1` (mtime).

### ELK index composition

> [DESIGN - Not fully implemented] Merged from `elk_index_data.md`.

Target: public field filtering, `flat_snapshot`, joins, `sc_sanitize_for_elk`. Current code exports BSON-as-relaxed-JSON without public field filtering. Full compose spec is in `shared_collection.md` §6.

## 8. Reference

- Execution modes: `.cursor/rule.md` §2, §5. Module API: `include/elk.h`.
- Geo ELK observability (audit index, async rules): `.cursor/geo_learning.md` §9.
- SharedCollection schema and compose target: `.cursor/shared_collection.md`.
- NL routing (when to use ELK search vs aggregations vs vector RAG): `.cursor/elk_nl_routing.md`.
