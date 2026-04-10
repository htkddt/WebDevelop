# Chat streaming: c-lib vs Flask router (why Ollama can still appear)

This document explains how **`POST /api/chat`** and **`POST /api/chat/stream`** differ, what the **server** and **FE** control, and why streams may show **Ollama** even when cloud keys are set. **No c-lib source changes** are required to read this; behavior described here matches **c-lib docs** (`c-lib/docs/api.md`, `c-lib/.cursor/models/cloud_free_tier_overview.md`).

## 1. Two different completion engines for “chat”

| Path | c-lib entry | Where hosted (Groq / Cerebras / Gemini) runs |
|------|-------------|-----------------------------------------------|
| **`POST /api/chat`** (JSON) | **`api_chat`** | **Inside c-lib** — `cloud_llm.c` (libcurl), order from **`M4_CLOUD_TRY_ORDER`**, keys **`GROQ_API_KEY`**, **`CEREBRAS_API_KEY`**, **`GEMINI_API_KEY`**, backend **`M4_CHAT_BACKEND`**. |
| **`POST /api/chat/stream`** (SSE) | Depends on **`M4_CHAT_STREAM_MODE`** (see §2). | **Not the same path as `api_chat`** for token streaming unless you use the c-lib-only stream mode (§2). |

**Important (c-lib fact):** **`api_chat_stream`** in the library follows the same **RAG / prompt / session** work as **`api_chat`**, but the **token stream** is implemented as **Ollama NDJSON streaming** (`ollama_query_stream`). Hosted free-tier **streaming** for Groq/Cerebras/Gemini is **not** implemented inside **`api_chat_stream`** today; that is called out explicitly in **`cloud_free_tier_overview.md`** (“`api_chat_stream` remains **Ollama-only** unless extended”).

So: **“Full cloud routing in c-lib”** matches **`api_chat`**, not **`api_chat_stream`**, until c-lib gains hosted streaming there.

## 2. Flask `M4_CHAT_STREAM_MODE`

| Mode | What runs | Hosted tokens |
|------|-----------|----------------|
| **`router`** (default if env unset or empty) | **`stream_chat_backends.run_router_stream`**: **`api_chat_prepare_external_llm`** → optional Redis short-circuit → **if c-lib sets an Ollama model pin**, **`api_chat_stream_from_prepared`** (Ollama only) → else **Python** OpenAI-compatible SSE to **Groq/Cerebras** → on failure **`api_chat_stream_from_prepared`** with no pin (Ollama fallback). | **Python** HTTP SSE for Groq/Cerebras only (Gemini tier skipped in stream). |
| **`ollama`** (or **`legacy`**) | **`api_chat_stream`** only. | **None in c-lib stream path** — Ollama stream after c-lib RAG/session logic. |

There is **no FE flag** that selects this mode; only **`M4_CHAT_STREAM_MODE`** in the server environment (and **`python_ai/server/.env`** via **`env_load.load_server_env`**).

## 3. Why the stream still “uses Ollama” (priority is not “low” — it is explicit)

These are **design outcomes**, not hidden FE defaults:

1. **Ollama model pin after prepare**  
   If **`api_chat_prepare_external_llm`** fills a **non-empty** Ollama model name (lane / `OLLAMA_MODEL` / `model_switch` / default Ollama tag — **c-lib resolution**), **`run_router_stream`** calls **`api_chat_stream_from_prepared`** and **never** calls the Python Groq/Cerebras SSE path. That matches **explicit local model** semantics in c-lib docs.

2. **Hosted SSE failure**  
   If Groq/Cerebras returns non-200 or errors, **`_openai_compatible_stream_chat`** fails and the router **falls back** to **`api_chat_stream_from_prepared`** (Ollama). Set **`M4_DEBUG_CHAT_STREAM=1`** for stderr HTTP hints.

3. **Mode `ollama`**  
   Forces **c-lib `api_chat_stream`** only (Ollama stream side).

4. **`.env` not visible to the process**  
   **`load_dotenv(..., override=False)`**: variables **already set in the shell** (even empty) **override** `.env`. Unset conflicting vars and restart the server.

5. **Gemini in `M4_CLOUD_TRY_ORDER`**  
   For **`router`** stream, the **gemini** tier is **skipped** (no Python SSE adapter yet). Order still tries Groq/Cerebras first if keys exist.

## 4. Hardcoding audit (server + FE only)

| Location | Hardcodes “use Ollama for stream”? |
|----------|-------------------------------------|
| **`fe/src/main.js`** | **No.** In-progress label **`CHAT_STREAMING_SOURCE_LABEL`** defaults to **`"stream"`** (cosmetic). **`normalizeSource`** maps server labels (`GROQ`, `OLLAMA`, …). The client does **not** send stream provider. |
| **`server/app.py`** | **No** implicit Ollama default for mode: **`resolved_chat_stream_mode`** → default **`router`**. |
| **`server/stream_chat_backends.py`** | **Branching logic** (pin → Ollama prepared; else Python cloud; else Ollama fallback). Groq/Cerebras URLs/models come from **env**, not literals for keys. |
| **c-lib** | **Not modified here.** Routing for **non-stream** chat is in **`cloud_llm.c`**; **stream** hot path in **`api_chat_stream`** is Ollama NDJSON per current c-lib design. |

## 5. Tests that lock this behavior (app level)

- **`tests/test_chat_stream_config.py`** — default mode **`router`**; **`resolved_chat_stream_mode`**; unknown mode → error event.
- **`tests/test_stream_router_branching.py`** — mocked **`api_chat_prepare_external_llm`**: proves **Ollama pin skips** Python cloud tiers; **no pin + failed tiers** → **`api_chat_stream_from_prepared`** fallback.
- **`tests/test_server.py`** — **`TestM4ServerAPI`**: patches **`stream_chat_backends.dispatch_chat_stream`**, asserts Flask passes **`router`** when env unset and **`ollama`** when set.

**Note:** The stream **worker** resolves **`_invoke_stream_dispatch`** with **`sys.modules[__name__].__dict__["_invoke_stream_dispatch"](...)`** (and that helper reads **`stream_chat_backends.dispatch_chat_stream`** from the backends module’s **`__dict__`**). Plain **`LOAD_GLOBAL`** on nested functions can stay specialized to the first function object in CPython; **`__dict__` lookups** stay patch-friendly for **`unittest.mock`**.

## 6. If you want “same as `api_chat`” but streaming

That requires **hosted streaming inside c-lib** (or a single new c-lib entry that reuses **`cloud_llm.c`** with a streaming API). The Flask **router** is a **Python-side** approximation for Groq/Cerebras only. **Gemini streaming** and **bit-identical** parity with **`api_chat`** are **not** promised by the current server-only router.
