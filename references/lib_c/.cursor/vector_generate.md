# Vector Generate — built-in embedding engine

Implementation: `include/vector_generate.h`, `src/vector_generate.c`.
Orchestration: `include/embed.h`, `src/embed.c` (router).

## Design principle

**`vector_generate` is the library's built-in default.** No network, no external dependency. It always works.

**Embed model is an optional user override.** When the user configures an embed backend (today: Ollama `/api/embed`; future: any provider), it replaces the default. If the override fails, the library can fall back to `vector_generate`.

```
api_options_t.vector_gen_backend
  │
  ├── API_VECTOR_GEN_CUSTOM (0, default)
  │   └── vector_generate_custom()
  │       Built-in hash: bag-of-words + bigrams → 384-D sparse float vector
  │       No network. No config. Always available.
  │
  └── API_VECTOR_GEN_OLLAMA (1, user opt-in)
      └── ollama_embeddings() via ollama.c
          User chose to use an external embed model.
          Optional: M4_EMBED_FALLBACK_CUSTOM=1 → fall back to built-in on failure.
```

## How it's used

| Caller | What it does | Embed source |
|--------|-------------|--------------|
| `api_chat` (turn persist) | Embed user message for Redis RAG + Mongo storage | `m4_embed_for_engine` → config-based |
| `api_chat` (RAG search) | Embed user message → search Redis L2 for cached reply | Same as above |
| `geo_learning` worker | Embed landmark names for dedup/similarity | `m4_embed_text` + `m4_embed_options_geo_env` |
| `api_geo_atlas_import_row` | Optional: caller provides pre-computed vector | N/A (vector passed in) |

**All callers go through `embed.c`** (`m4_embed_text` / `m4_embed_for_engine`). Never call `vector_generate_custom` or `ollama_embeddings` directly from feature modules.

## Built-in engine (`vector_generate_custom`)

- **Algorithm:** Bag-of-words + bigrams → FNV-1a hash → two indices per token in 384-D → L2 normalize
- **Dimension:** 384 (fixed, `VECTOR_GEN_DIM`)
- **Model ID:** `m4-vector-hash-v1-384` (`VECTOR_GEN_MODEL_ID`)
- **Sparse:** Most coordinates are 0; non-zero buckets ~0.1–0.28 after normalization. Cosine similarity still works correctly on sparse vectors.
- **Deterministic:** Same input always produces same vector (no randomness, no model weights)

## User-configured embed (optional override)

When the user sets `api_options_t.vector_gen_backend = API_VECTOR_GEN_OLLAMA`:

- Calls Ollama `POST /api/embed` with the configured model
- Dimension depends on model (768 for nomic-embed-text, 2048 for others)
- **Future:** this override slot could support any embed provider (OpenAI, Cohere, custom endpoint) — not locked to Ollama

**Important:** Stored vectors and query vectors must share the same embed family and dimension. Do not mix built-in 384-D vectors with Ollama 768-D vectors in the same Redis/Mongo index without migration.

## Configuration

| Option | Default | Effect |
|--------|---------|--------|
| `api_options_t.vector_gen_backend` | `0` (CUSTOM) | `0` = built-in hash. `1` = user's embed model (Ollama today). |
| `api_options_t.vector_ollama_model` | NULL | Override embed model tag when backend=1. NULL = Ollama's resolve chain. |
| `M4_EMBED_FALLBACK_CUSTOM=1` | off | If user's embed model fails, fall back to built-in hash (watch dimension mismatch). |
| `M4_EMBED_BACKEND` (geo only) | `ollama` | Geo worker embed: `custom`/`0` → hash. Unset/`ollama`/`1` → Ollama. |

## Metadata pairing

Every stored vector must be paired with metadata (`.cursor/rules/embed-vector-metadata.mdc`):
- `embed_schema` — version
- `vector_dim` — actual dimension
- `embed_family` — `custom` or `ollama`
- `model_id` — e.g. `m4-vector-hash-v1-384` or `ollama:nomic-embed-text`

Never treat a float array as self-describing.

## Files

| File | Role |
|------|------|
| `include/vector_generate.h` | Built-in hash vector API |
| `src/vector_generate.c` | Hash implementation |
| `include/embed.h` | Router: picks built-in or user's embed model |
| `src/embed.c` | Router implementation (`m4_embed_text`, `m4_embed_for_engine`) |
| `include/ollama.h` | `ollama_embeddings` (used by embed.c when backend=OLLAMA) |

## Tests

- `make test-vector-generate` — tests built-in hash primitive
- Integration: `m4_embed_*` calls in `api.c` / `geo_learning.c`

## LangDetector + Pre-query RAG flow

> Merged from `lang_vector_phase1.md` and `PRE_QUERY_RAG_FLOW.md`.

### LangDetector (CLD3 wrapper)
- **Backend:** CLD3 (Compact Language Detector 3). Build with `USE_CLD3=1`.
- **Output:** `{ lang, score }` — ISO 639-1 code. If `score < 0.5`, set `lang` to `"mixed"`.
- **Threading:** Can run in pthread parallel with VectorGen.

### Pre-query RAG flow
| Step | Action |
|------|--------|
| 1 | User input (raw message) |
| 2 | Generate vector (built-in hash or user's embed model) |
| 3 | Auto-detect language (LangDetector — parallel with step 2) |
| 4 | Search storage by `{ tenant, user, vector }` — Redis L2 / Mongo |
| 5 | If Redis high-score hit → return as chat, skip LLM |
| 5b | If no cache hit → build prompt context from hits |
| 6 | Send to AI model (prompt context + user message) |

Both vector and lang must be populated **before** any Redis/ELK/Mongo write. Fallback: LangDetector fails → `"mixed"`. VectorGen fails → skip vector write.
