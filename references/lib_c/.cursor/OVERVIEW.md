# .cursor Documentation Overview & Audit

**Date:** 2026-04-04  
**Total files:** 36 `.md` + 7 `.mdc` rules = 43 files (~5,000+ lines)

---

## Current State: Problems Found

### 1. Outdated API References
The public API was refactored from 33 → 7 functions. These docs still reference removed APIs:

| Removed API | Referenced In | Action |
|-------------|--------------|--------|
| `api_set_prompt_tag` / `api_clear_prompt_tags` | ptomp.md | Update: now `api_options_t.default_persona` / `default_instructions` |
| `api_get_last_chat_wire` | ptomp.md | Update: now `api_stats_t.last_chat_wire` field |
| `api_get_last_reply_source` | auth_geo.md, FLOW_DATA_UPDATE_CHECK.md | Update: now `api_stats_t.last_reply_source` field |
| `api_chat_stream` (old signature) | streaming.md | Update: now `api_chat(..., stream_cb, userdata)` unified |
| `api_geo_authority_load_csv` | auth_geo.md | Update: now `api_options_t.geo_authority_csv_path` |
| `api_geo_atlas_migrate_legacy` | geo_leanring.md | Update: now `api_options_t.geo_migrate_legacy` |
| `api_query` | engine.md | Update: internal only, not public |
| `api_set_log_collection` | rule.md | Update: use `api_options_t.log_db` / `log_coll` at create time |

### 2. Massive Overlap (5 topic clusters)

| Topic | Files (redundant) | Keep | Merge/Remove |
|-------|-------------------|------|-------------|
| **ELK** | ELK_KIBANA.md, elk.md, elk_index_data.md, elk_nl_routing.md | elk.md (module rules) | Merge ELK_KIBANA → elk.md, merge elk_index_data → elk.md |
| **Vectors** | vector_generate.md, lang_vector_phase1.md, PRE_QUERY_RAG_FLOW.md | vector_generate.md | Merge lang_vector_phase1 + PRE_QUERY_RAG_FLOW → vector_generate.md |
| **Models** | default_models.md, model_switch.md, model_routing_index.md, ai_agent_fallback.md | default_models.md + model_switch.md | Remove model_routing_index.md (meta-index), merge ai_agent_fallback → model_switch.md |
| **Geo** | geo_leanring.md, auth_geo.md, conflict_detector.md | geo_leanring.md (rename to geo_learning.md) | Merge auth_geo + conflict_detector into geo_learning.md |
| **Storage** | storage.md, STORAGE_MODULES_DISCUSSION.md, mongo.md, redis.md, REDIS_KEYS.md | mongo.md + redis.md | Remove storage.md (stub), merge STORAGE_MODULES_DISCUSSION → rule.md, merge REDIS_KEYS → redis.md |

### 3. Design-Only Files (not implemented in C)

| File | Lines | Status |
|------|-------|--------|
| shared_collection.md | 747 | Design target — partial impl |
| elk_nl_routing.md | 546 | Design — NL routing not wired |
| models/ai_agent.md | 441 | Design — cloud tier config |
| elk_index_data.md | 368 | Design — ELK compose not in code |
| models/cloud_router_pre_impl_flow.md | 172 | Design — pre-impl planning |
| ai_agent_fallback.md | 92 | Design — marked "not enabled" |
| models/cloud_free_tier_prompts.md | 87 | Design — wire format spec |
| **Total** | **~2,453** | **~50% of all docs** |

### 4. Stub/Incomplete Files

| File | Lines | Issue |
|------|-------|-------|
| storage.md | 21 | Just a struct definition, missing everything |
| streaming.md | 16 | Spec only, no implementation detail |
| REDIS_KEYS.md | 36 | States "implementation is a stub" |

### 5. Filename Typo

`geo_leanring.md` → should be `geo_learning.md`

---

## C Module → Documentation Mapping

### Documented Modules (1:1 match)

| C Module | Header | `.cursor/*.md` |
|----------|--------|-----------------|
| Engine | engine.h | engine.md |
| Storage | storage.h | storage.md (stub) |
| MongoDB | mongo.h | mongo.md |
| Redis | redis.h | redis.md |
| ELK | elk.h | elk.md |
| Ollama | ollama.h | default_models.md |
| Cloud LLM | ai_agent.h | ai_agent_fallback.md (design) |
| Smart Topic | smart_topic.h | smart_topic_ai_switch.md |
| Model Switch | model_switch.h | model_switch.md |
| Geo Learning | geo_learning.h | geo_leanring.md |
| Geo Authority | geo_authority.h | auth_geo.md |
| Conflict Detector | conflict_detector.h | conflict_detector.md |
| Language | lang.h | language.md |
| Embed | embed.h | embed_migration.md |
| Vector Generate | vector_generate.h | vector_generate.md |
| NL Learn Terms | nl_learn_terms.h | elk_nl_routing.md |
| NL Learn Cues | nl_learn_cues.h | elk_nl_routing.md |
| Shared Collection | shared_collection.h | shared_collection.md |

### Undocumented Modules (no .cursor/*.md)

| C Module | Header | Notes |
|----------|--------|-------|
| API | api.h | Documented in `docs/api.md` instead |
| Statistics | stat.h | No design doc |
| Tenant | tenant.h | No design doc |
| Dispatcher | dispatcher.h | No design doc |
| Validate | validate.h | No design doc |
| Terminal UI | terminal_ui.h | No design doc |
| Utilities | utils.h | No design doc |
| Debug Monitor | debug_monitor.h | No design doc |
| ELK Sync Pool | elk_sync_pool.h | Referenced in elk.md |
| Embed Worker | embed_worker.h | Referenced in embed_migration.md |

### Orphaned Docs (no single C module)

| File | Type | Action |
|------|------|--------|
| rule.md | Project-wide rules | Keep — foundational |
| naming_conventions.md | Code style | Keep |
| ptomp.md | Prompt strategy | Keep — maps to `ctx_build_prompt` in api.c |
| chat_l1_memory.md | Session design | Keep — maps to session ring in api.c |
| streaming.md | Stream design | Merge into ptomp.md or remove (now unified in api_chat) |
| FLOW_DATA_UPDATE_CHECK.md | Validation flow | Merge relevant parts → engine.md, remove rest |
| PRE_QUERY_RAG_FLOW.md | RAG pipeline | Merge → vector_generate.md |
| STORAGE_MODULES_DISCUSSION.md | Architecture discussion | Merge → rule.md |
| ELK_KIBANA.md | DevOps/infra | Merge → elk.md |
| REDIS_KEYS.md | Key patterns | Merge → redis.md |
| elk_index_data.md | ELK compose design | Merge → elk.md or shared_collection.md |
| model_routing_index.md | Meta-index | Remove — replaced by this overview |
| lang_vector_phase1.md | Cross-module flow | Merge → vector_generate.md |
| ai_agent_fallback.md | Cloud fallback design | Merge → model_switch.md |

---

## Proposed Refactor: 43 files → 18 files

### Keep As-Is (12 files)

| File | Module | Why keep |
|------|--------|---------|
| rule.md | Project rules | Foundational, no overlap |
| naming_conventions.md | Code style | Unique content |
| engine.md | Engine | 1:1 module match |
| mongo.md | MongoDB | 1:1 module match |
| redis.md | Redis | 1:1, absorb REDIS_KEYS.md |
| elk.md | ELK | 1:1, absorb ELK_KIBANA + elk_index_data |
| ptomp.md | Prompt strategy | Unique content, absorb streaming.md |
| default_models.md | Ollama defaults | Canonical, referenced by rules |
| model_switch.md | Model routing | 1:1, absorb ai_agent_fallback |
| embed_migration.md | Embed lifecycle | 1:1 match |
| chat_l1_memory.md | Session memory | Unique design content |
| shared_collection.md | SharedCollection | Large design spec, keep separate |

### Rename + Merge (3 files)

| New Name | From | Absorb |
|----------|------|--------|
| geo_learning.md | geo_leanring.md (rename) | + auth_geo.md + conflict_detector.md |
| vector_generate.md | vector_generate.md | + lang_vector_phase1.md + PRE_QUERY_RAG_FLOW.md |
| elk_nl_routing.md | elk_nl_routing.md | (keep, already covers nl_learn_terms + nl_learn_cues) |

### Keep in `models/` subfolder (3 files)

| File | Why |
|------|-----|
| models/README.md | Index for per-model rules |
| models/ai_agent.md | Large design spec |
| models/default.rule.md | Template |

### Remove (15 files)

| File | Reason |
|------|--------|
| storage.md | Stub (21 lines), content covered by mongo.md + redis.md + rule.md |
| streaming.md | 16 lines, now covered by unified api_chat + ptomp.md |
| FLOW_DATA_UPDATE_CHECK.md | Validation content → merge useful parts to engine.md |
| STORAGE_MODULES_DISCUSSION.md | Architecture discussion → merge to rule.md |
| ELK_KIBANA.md | DevOps content → merge to elk.md |
| REDIS_KEYS.md | Key patterns → merge to redis.md |
| elk_index_data.md | ELK compose → merge to elk.md |
| PRE_QUERY_RAG_FLOW.md | RAG flow → merge to vector_generate.md |
| lang_vector_phase1.md | Phase 1 → merge to vector_generate.md |
| model_routing_index.md | Meta-index → replaced by this OVERVIEW.md |
| ai_agent_fallback.md | Fallback strategy → merge to model_switch.md |
| auth_geo.md | Authority cache → merge to geo_learning.md |
| conflict_detector.md | Conflict rules → merge to geo_learning.md |
| models/cloud_free_tier_prompts.md | Wire format → merge to ai_agent.md |
| models/cloud_router_pre_impl_flow.md | Pre-impl flow → merge to ai_agent.md |
| models/llama3.1-8b-instruct-q8_0.rule.md | Single example rule → keep only if model is used |

### Rules (keep all 7 .mdc files)

All cursor rules are active and non-redundant. No changes needed.

---

## Refactor Steps

| Step | Action | Files touched |
|------|--------|---------------|
| 1 | Fix API references in all .md files (removed APIs → new opts/stats fields) | ~8 files |
| 2 | Rename `geo_leanring.md` → `geo_learning.md` | 1 file + cross-refs |
| 3 | Merge auth_geo.md + conflict_detector.md → geo_learning.md | 3 files |
| 4 | Merge REDIS_KEYS.md → redis.md | 2 files |
| 5 | Merge ELK_KIBANA.md + elk_index_data.md → elk.md | 3 files |
| 6 | Merge lang_vector_phase1.md + PRE_QUERY_RAG_FLOW.md → vector_generate.md | 3 files |
| 7 | Merge ai_agent_fallback.md → model_switch.md | 2 files |
| 8 | Merge streaming.md → ptomp.md | 2 files |
| 9 | Merge STORAGE_MODULES_DISCUSSION.md relevant parts → rule.md | 2 files |
| 10 | Merge FLOW_DATA_UPDATE_CHECK.md relevant parts → engine.md | 2 files |
| 11 | Merge cloud_free_tier_prompts + cloud_router_pre_impl_flow → ai_agent.md | 3 files |
| 12 | Remove model_routing_index.md (replaced by OVERVIEW.md) | 1 file |
| 13 | Remove storage.md stub | 1 file |
| 14 | Update cross-references in remaining files | all |
| 15 | Add `[DESIGN - Not Implemented]` banner to design-only files | ~5 files |
