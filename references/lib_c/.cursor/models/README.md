# Per-model rules (`.cursor/models/*.rule.md`)

Use these files to capture **behavior, prompt quirks, and capability flags** per Ollama model (or family). They are meant for **humans + Cursor agents** when editing prompts, RAG, or `python_ai` — not loaded by the C library at runtime unless you add a host-side reader later.

**Library-wide default tags** (when user/config does not set a model): **`.cursor/default_models.md`** + **`include/ollama.h`**. Cursor always-on rule: **`.cursor/rules/default-models.mdc`**. This **`models/`** tree is for **per-tag** behavior only; avoid duplicating the global fallback table here.

**AI Agent (LLM routing):** **`ai_agent.md`** (try order, env, troubleshooting). **File/env naming + `rg` tips:** **`.cursor/naming_conventions.md`**.

## File naming → model id

Ollama model names often contain `:` and `/`, which are awkward in paths. Map the **resolved model id** (e.g. from `OLLAMA_MODEL` or `/api/tags`) to a **slug**:

| Raw `model` | Rule file (slug) |
|-------------|------------------|
| `llama3.1:8b-instruct-q8_0` | `llama3.1-8b-instruct-q8_0.rule.md` |
| `qwen2.5:7b` | `qwen2.5-7b.rule.md` |
| `mistral:latest` | `mistral-latest.rule.md` |

**Slug algorithm**

1. Lowercase.
2. Replace each run of `:`, `/`, or whitespace with a single `-`.
3. Collapse repeated `-`, trim `-` from ends.
4. If the result is empty, use **`default.rule.md`**.

Quick check (repo root):

```bash
python3 scripts/model_rule_slug.py "llama3.1:8b-instruct-q8_0"
# → llama3.1-8b-instruct-q8_0
```

## Resolution order (identify which rule applies)

1. **Exact slug** — `{slug}.rule.md`
2. **Family / prefix** — optional: same file name with a shorter prefix you maintain (e.g. `llama3.1.rule.md` for all `llama3.1:*`), documented in that file’s `aliases` frontmatter
3. **Default** — `default.rule.md`

Agents should read **at most one** specialized file plus `default.rule.md` if you want baseline + overlay (see frontmatter `extends`).

## Frontmatter (optional, for “what this model supports”)

YAML between `---` lines at the top. Intended for future tooling (lint, Python host, or a small script that prints caps).

Suggested keys:

| Key | Meaning |
|-----|--------|
| `model_id` | Canonical Ollama name (with `:`) |
| `aliases` | Other names / tags that map to this rule |
| `supports` | Nested booleans/strings, e.g. `structured_json`, `vietnamese_colloquial`, `long_context` |
| `risk` | e.g. `instruction_leak`, `rag_loop` — reminders from prod incidents |
| `prompt` | Short bullets: tone, what to avoid (e.g. don’t fight user kinship terms) |
| `extends` | Optional: always merge `default.rule.md` semantics |

Body markdown is free-form: examples, anti-patterns, links to issues.

## Relation to Cursor `rules/`

Project-wide agent rules live in **`.cursor/rules/*.mdc`**. This folder is **model-specific** reference material; see **`models-folder.mdc`** for the instruction to open the slug-matched file when working on model-dependent behavior.

**AI Agent (LLM routing):** **`ai_agent.md`** (try order, env names, troubleshooting).
