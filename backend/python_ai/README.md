# python_ai

Python shell for the **M4 engine**: loads **only** ``libm4engine`` from ``python_ai/lib/`` or ``python_ai/libs/`` (ctypes). The shared library is supplied via **git** (releases, submodule, CI) — not by pointing Python at a local ``c-lib`` checkout.

## How to start with python_ai

Use the **c-lib** repo as the reference. In the **c-lib** repository, follow:

1. **Integration tutorial** — **`c-lib/docs/tutorial_app_integration.md`**: build, Mongo, modes, Flask env, C link line, checklist.
2. **c-lib README** — prerequisites, `make lib`, layout.
3. **Consumer guide** — **`c-lib/docs/consumers.md`**: no root build, paths.
4. **Python steps** — Pull **`libm4engine.*`** from your engine distribution (e.g. **ngoky/lib_c** GitHub Releases tarball or submodule) into **`python_ai/lib/`** or **`python_ai/libs/`**, then run CLI/server.

(If c-lib and python_ai live in the same workspace, paths like `c-lib/README.md`, `c-lib/docs/consumers.md`, and `c-lib/docs/getting-started-python.md` refer to that c-lib repo.)

### Quick start (no root build)

Place **`libm4engine.dylib`** / **`.so`** under **`python_ai/lib/`** or **`python_ai/libs/`** (from git release, submodule, or build pipeline — see **ngoky/lib_c**).

```bash
# 1. (Off-repo) obtain libm4engine.* → copy into python_ai/lib/ or libs/

# 2. Optional: Makefile ties pip + engine check (see server/requirements.txt header)
cd python_ai
make help
make install-py-deps    # server/.venv + pip install requirements
make check-engine-lib    # errors if libm4engine missing under lib/ or libs/
make doctor              # install-py-deps + check-engine-lib + print find_lib()

# 3. Run python_ai
python3 validate_lang.py            # optional: validate lang_detect() (same tests as c-lib)
python3 run_ai.py "Your question"   # CLI: one-shot query
python3 run_ai_tui.py               # Terminal UI: chat, status top-right, last 30 messages, input at bottom
```

**HTTP API (port 5000) + Vite UI (port 8000):** see **[../docs/COMBINE_TUTORIAL.md](../docs/COMBINE_TUTORIAL.md)** — from `python_ai/server`: `pip install -r requirements.txt && python3 app.py`; from repo `fe/`: `npm install && npm run dev`.

### Tests

Unit tests for display formatting (epoch ts, You/Bot prefix), SOURCE_LABELS, and c-lib API (when lib is available):

```bash
cd python_ai
python3 -m unittest tests.test_display_and_sources -v
```

Requires **`libm4engine`** under **`python_ai/lib/`** or **`python_ai/libs/`** for API tests; display/source tests run without the lib.

**Ollama** must be running for **TUI / `api_chat` direct** and for **Flask fallback** after free-tier cloud. Default chat tag is **`OLLAMA_DEFAULT_MODEL`** in **`m4_default_models.py`** (must match **`c-lib/include/ollama.h`**). When changing defaults, follow **`c-lib/.cursor/default_models.md`**. Override anytime with **`OLLAMA_MODEL`**.

**Flask `POST /api/chat`:** calls c-lib **`api_chat`** only; routing is in **`c-lib/src/cloud_llm.c`**. Default pool then Ollama; **`M4_CHAT_BACKEND=ollama`** for local-only. See **`c-lib/docs/tutorial_app_integration.md`**, **`server/app.py`**, and **`c-lib/.cursor/models/cloud_free_tier_overview.md`**.

**Flask `POST /api/chat/stream` (SSE):** **Default (omit or empty `M4_CHAT_STREAM_MODE`)** → **`router`**: hosted Groq/Cerebras token stream (needs **`GROQ_API_KEY`** / **`CEREBRAS_API_KEY`**) + Ollama fallback. **`M4_CHAT_STREAM_MODE=ollama`** → **`api_chat_stream`** only (local Ollama). Implementation: **`server/stream_chat_backends.py`**. Event shape: **`server/chat_stream_callbacks.md`**; HTTP spec: **`c-lib/docs/api.md`** § Streaming; Vite + curl: **`docs/COMBINE_TUTORIAL.md`**.

### Where Python loads the library (no env vars, no `../c-lib`)

1. **`python_ai/lib/libm4engine.dylib`** or **`.so`**
2. **`python_ai/libs/`** — same filename

Populate these directories from **git** (release asset, submodule, or your CI), not from a Python path into a C source tree.

## Layout

| File            | Description |
|-----------------|-------------|
| `run_ai.py`     | CLI: loads c-lib, calls `ollama_query`; `python3 run_ai.py "prompt"`. |
| `run_ai_tui.py` | Terminal UI: status top-right, chat history above, input at bottom; uses **full options** from `training`. |
| `validate_lang.py` | Validates `lang_detect()` via c-lib (same cases as c-lib/tests/validate_lang.c). Run before/with TUI testing. |
| `engine_ctypes.py` | ctypes bindings for c-lib public API (api.h): **`api_chat`**, **`api_chat_stream`** + **`STREAM_TOKEN_CB`**, history/stats, **`lang_detect`**. **`ApiOptions`** matches **`api.h`** (incl. **`inject_geo_knowledge`**, **`vector_gen_backend`** / **`vector_ollama_model`**). |
| `server/`       | Flask API (default **5000**, may use **5001** if busy; **`M4ENGINE_SERVER_PORT`** / **`M4ENGINE_SERVER_HOST`**). **`POST /api/chat`**, **`POST /api/chat/stream`** (SSE — see **`server/chat_stream_callbacks.md`**, **`M4_CHAT_STREAM_MODE`**), history, stats. Same default options as **`run_ai_tui.py`** (`build_max_api_options`); **`M4ENGINE_SERVER_MAX=0`** for MEMORY-only. See **`../docs/COMBINE_TUTORIAL.md`**. |
| `training/`     | **Full option** config: all API options via env (`M4ENGINE_MODE`, `M4ENGINE_MONGO_URI`, `M4ENGINE_CONTEXT_BATCH_SIZE`, etc.). See `training/README.md`. |
| `README.md`     | This file — how to start; references c-lib/README.md and c-lib/docs/*.md. |

## Requirements

- Python 3.7+
- **libm4engine.dylib** (macOS) or **libm4engine.so** (Linux) in **`lib/`** or **`libs/`** (from engine repo / release).
- Ollama running for AI responses.
