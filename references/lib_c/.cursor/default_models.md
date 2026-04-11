# Default Ollama models (single checklist)

**Cursor rule:** `.cursor/rules/default-models.mdc` (always on). **Per-model behavior** (prompt quirks): `.cursor/models/*.rule.md` via `.cursor/rules/models-folder.mdc`. **All model + cloud doc order:** `.cursor/model_routing_index.md`. **Names & grep:** `.cursor/naming_conventions.md`.

**Doc convention:** In `.cursor/*.md`, **do not hardcode** a live Ollama tag (e.g. `qwen…`, `llama…`) unless the file is **model-specific** (`.cursor/models/{slug}.rule.md` frontmatter) or a **generic example** clearly labeled. Prefer **`OLLAMA_DEFAULT_MODEL`**, **`include/ollama.h`**, or “the compile-time default macro”.

---

## Config not set → fallback (all flows)

| Layer | If unset / empty | Falls back to |
|--------|------------------|----------------|
| **Lane row `model`** (`model_switch`) | `""` or missing | `M4_MODEL_<KEY>` → `fallback_model` → `OLLAMA_MODEL` → **`OLLAMA_DEFAULT_MODEL`** |
| **Chat `ollama_query*(…, NULL, …)`** | No explicit model | `GET /api/tags` first name → `OLLAMA_MODEL` → **`OLLAMA_DEFAULT_MODEL`** |
| **Embed `ollama_embeddings` / resolve** | No `OLLAMA_EMBED_MODEL` and no preferred | Same chain as chat → **`OLLAMA_DEFAULT_MODEL`**; storage fallback → **`OLLAMA_DEFAULT_EMBED_MODEL`** (`api_resolve_stored_embed_model_id`) |
| **Python `embed_model_name()`** | No env, tags empty | **`m4_default_models.OLLAMA_DEFAULT_MODEL`** (must match `ollama.h`) |
| **smart_topic mini model** | `model_tiny` / `model_b2` NULL | Ollama discovery / env (see `smart_topic.c`, `.cursor/smart_topic_ai_switch.md`) |

---

**Authoritative (C, compile-time):** `include/ollama.h`

| Macro | Role |
|--------|------|
| **`OLLAMA_DEFAULT_MODEL`** | Chat / `/api/generate` when the caller passes a NULL model and **`OLLAMA_MODEL`** is unset, after **`GET /api/tags`** fails or is unused in that path. Also the last step in **`ollama_resolve_embed_model`** when **`OLLAMA_EMBED_MODEL`** is unset (unified with chat) — if the chat default is **not** an embed-capable model, set **`OLLAMA_EMBED_MODEL`** explicitly (e.g. **`nomic-embed-text`**). |
| **`OLLAMA_DEFAULT_EMBED_MODEL`** | **Fallback only** when embed resolution fails (e.g. `api_resolve_stored_embed_model_id`); not the primary default when the unified chain works. |

**Do not** hardcode the same tag string elsewhere in C; always use these macros.

---

## When you change `OLLAMA_DEFAULT_MODEL` or `OLLAMA_DEFAULT_EMBED_MODEL`

**Stored vectors:** Existing Mongo **`vector`** + **`metadata.model_id`** / **`embed_family`** / **`vector_dim`** do not auto-update. Plan async re-embed or Redis index rebuild per **`.cursor/embed_migration.md`**.

1. **Edit** `c-lib/include/ollama.h` (only place for C defaults).
2. **Python (sibling repo):** `python_ai/m4_default_models.py` — keep **`OLLAMA_DEFAULT_MODEL`** string **identical** to the macro (used when env + `/api/tags` do not supply a name).
3. **Examples / docs (search & update):**
   - `c-lib/.cursor/model_switch.md` (lane table examples)
   - `c-lib/include/model_switch.h` (comment example)
   - `python_ai/README.md` (pull / mention line)
   - `c-lib/.cursor/engine.md` (ops examples, if they name a tag)
   - `c-lib/tests/data.json` or fixtures if they embed a model id
4. **Rebuild** c-lib (`make lib` …) and ensure consumers load the new `.dylib`/`.so`.
5. **Per-model Cursor rules:** If behavior differs by family, add/update `.cursor/models/*.rule.md` (see `.cursor/rules/models-folder.mdc`).

**Runtime overrides (no code edit):** `OLLAMA_MODEL`, `OLLAMA_EMBED_MODEL`, `model_switch_opts` / `M4_MODEL_*`, smart_topic tiny env vars.

---

## Resolution order (do not duplicate logic)

- **Chat NULL model:** `ollama.c` — `/api/tags` first name → `OLLAMA_MODEL` → `OLLAMA_DEFAULT_MODEL`.
- **Embed NULL model:** `ollama_resolve_embed_model` — optional `OLLAMA_EMBED_MODEL` → same chain as chat → `OLLAMA_DEFAULT_MODEL`.

Full narrative: `.cursor/geo_leanring.md` (banner), `.cursor/model_switch.md`.

---

## Future: cloud-primary routing (design only)

If you add **hosted free-tier** chat APIs ahead of Ollama, keep **`OLLAMA_DEFAULT_MODEL`** as the **compile-time last resort** for local inference (lightweight tag in **`ollama.h`**, e.g. after **`ollama pull`** for that tag). Optional env **`M4_OLLAMA_FALLBACK_MODEL`** can override only the router’s last step without changing the macro — see **`models/ai_agent.md`** §9.

**Where to read (in order — avoids duplicate/conflicting “sources of truth”):**

1. **`.cursor/model_routing_index.md`** — map of all model/cloud docs and Cursor rules.  
2. **`.cursor/models/ai_agent.md`** — **canonical** spec: try order, **`model_switch` vs router**, **no model in config** → pool map, user override, env names.  
3. **`.cursor/models/cloud_router_pre_impl_flow.md`** — pre-implementation flow and touchpoints.  
4. **`.cursor/models/models/ai_agent.md`** — backend **types** (Groq/Cerebras/Gemini/Ollama), prompt mapping, **`ptomp.md`** vs per-model rules.  
5. **`.cursor/models/ai_agent.md`** — pool **policies** (rotation, buckets, 429, checklist).

Not wired in code until you complete that work; until then behavior remains **Ollama-only** per the matrix above.
