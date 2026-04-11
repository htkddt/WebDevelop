# `c-lib/.cursor` naming conventions

**Why:** Keeps docs and rules **grep-friendly** and consistent. If a new file does not match, prefer renaming early (before many cross-links accumulate).

---

## 1. Markdown in `.cursor/` (repo root = `c-lib/`)

| Pattern | Example | Avoid |
|---------|---------|--------|
| **`snake_case.md`** | `default_models.md`, `model_switch.md`, `model_routing_index.md`, `embed_migration.md` | `ALL_CAPS.md`, `PascalCase.md` |
| **Prefix by topic** (optional) | `ai_agent_fallback.md` (pool policy at **root**), `chat_l1_memory.md` | Vague names like `notes.md` |

**Cloud design docs** that are **per-model-folder** material live under **`.cursor/models/`** with a shared prefix **`cloud_*`** so one glob finds them:

```bash
ls c-lib/.cursor/models/cloud_*.md
# ai_agent_overview.md
# ai_agent_prompts.md
# cloud_router_pre_impl_flow.md
```

---

## 2. `.cursor/models/`

| Pattern | Role |
|---------|------|
| **`<slug>.rule.md`** | Per-model behavior for agents (`model_rule_slug.py` → slug). |
| **`default.rule.md`** | Fallback rule file. |
| **`README.md`** | Folder index + slug algorithm. |
| **`cloud_*.md`** | Hosted-LLM **design** only (not Cursor rule files). |

---

## 3. Cursor rules `.cursor/rules/*.mdc`

| Pattern | Example |
|---------|---------|
| **kebab-case** + **`.mdc`** | `default-models.mdc`, `models-folder.mdc`, `embed-vector-metadata.mdc` |

These are **agent instructions** (often `alwaysApply`). Long reference stays in **`.md`**.

---

## 4. Environment variables (documented / planned)

| Prefix | Meaning |
|--------|---------|
| **`M4_*`** | App: lanes, routing, optional cloud flags (`M4_MODEL_EDUCATION`, `M4_CHAT_BACKEND`, `M4_CLOUD_GROQ_MODEL`, …). |
| **`OLLAMA_*`** | Ollama host/port/model overrides; compile-time defaults in **`include/ollama.h`**. |
| **`<PROVIDER>_API_KEY`** | Vendor secrets (`GROQ_API_KEY`, `GEMINI_API_KEY`, `CEREBRAS_API_KEY`) — never commit. |

---

## 5. C symbols (reference)

| Area | Style |
|------|--------|
| Public API | `model_switch_resolve`, `ollama_query`, … |
| Macros | `OLLAMA_DEFAULT_MODEL`, `MODEL_SWITCH_FLAG_*` |

---

## 6. Grep / ripgrep cheatsheet (from repo root `ai/` or `c-lib/`)

```bash
# Doc map + cloud entry point
rg 'model_routing_index' c-lib/.cursor

# All cloud design files under models/
rg --files -g 'cloud_*.md' c-lib/.cursor/models

# Canonical cloud spec + prompt types
rg 'ai_agent_(overview|prompts)' c-lib/.cursor

# Generic pool policy doc
rg 'ai_agent_fallback' c-lib/.cursor
```

---

## 7. Cross-link style (for authors)

| From | To `models/` sibling | To `.cursor` root |
|------|----------------------|-------------------|
| **`models/cloud_*.md`** | Basename OK: `cloud_router_pre_impl_flow.md` | Full: **`.cursor/model_routing_index.md`** |
| **`.cursor/*.md`**, **rules** | Full: **`.cursor/models/ai_agent_overview.md`** | Basename OK for same dir |

Full paths make **rg 'ai_agent_overview'** hits unambiguous.

---

## 8. Canonical doc map

One file lists reading order for **defaults + cloud (design):** **`.cursor/model_routing_index.md`**.
