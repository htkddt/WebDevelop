# Consumer repos: how to use c-lib (no root build)

This guide is for **consumer repositories** (e.g. **c_ai**, **python_ai**) that use the M4-Hardcore AI Engine C library. It assumes there is **no parent “root” build** — c-lib, c_ai, and python_ai are separate repos. You get the library from c-lib and inject or link it into your app.

## 1. Get the library

Choose one:

### A. Build from c-lib (clone the c-lib repo)

```bash
git clone <c-lib-repo-url> c-lib
cd c-lib
make validate
make lib
```

Output: **`c-lib/lib/libm4engine.dylib`** (macOS) or **`c-lib/lib/libm4engine.so`** (Linux), and optionally **`c-lib/lib/libm4engine.a`** (static).

### B. Download a prebuilt package

If c-lib publishes releases (e.g. GitHub Releases), download the tarball for your OS/arch, e.g.:

- `m4engine-1.0.0-darwin-arm64.tar.gz`
- `m4engine-1.0.0-linux-amd64.tar.gz`

Then unpack and use the **`lib/`** and **`include/`** from the tarball. See [README.md](../README.md) and [PRIVATE_REGISTRY.md](PRIVATE_REGISTRY.md) (if present in c-lib) for release URLs.

## 2. Where to put the library for your app

- **Option 1 (recommended):** Clone c-lib next to your consumer repo and point your app at **`../c-lib/lib`** (or set an env var, see below).
- **Option 2:** Copy **`lib/libm4engine.dylib`** (or `.so`) and optionally **`include/*.h`** into your consumer repo (e.g. `python_ai/lib/`, `c_ai/lib/`).
- **Option 3:** Set **`M4ENGINE_LIB`** (or **`M4ENGINE_LIB_PATH`**) to the full path of the shared library file (e.g. `/path/to/c-lib/lib/libm4engine.dylib`). Consumer apps that support this will load from there.

## 3. Consumer-specific guides

| Consumer   | Guide |
|-----------|--------|
| **Python** | [getting-started-python.md](getting-started-python.md) — clone c-lib, build lib, clone python_ai, set path, run **`run_ai.py`** (CLI) or **`server/app.py`** (HTTP: `/api/chat`, `/api/chat/stream`, `/api/history`, `/api/stats`, `/api/geo/import`). API ↔ C mapping: [api.md § Python HTTP server](api.md#python-http-server-reference-consumer). |
| **C (c_ai)** | Build c-lib first; in c_ai use `-I../c-lib/include -L../c-lib/lib -lm4engine` (or equivalent). See c_ai README. |

## 4. API reference

Public C API is in **c-lib/include/**:

- **api.h** — Recommended: `api_create`, `api_destroy`, `api_chat`, `api_query`, `api_get_stats`, `api_set_log_collection`. See [api.md](api.md).
- **engine.h** — `engine_create`, `engine_destroy`, `engine_append_chat`, `engine_get_stats`, `engine_init` (calls **initial_smart_topic** when `smart_topic_opts` set).
- **smart_topic.h** — `initial_smart_topic`, `get_smart_topic`, `smart_topic_temperature_for_query`; intent-based temperature (TECH 0.0 / CHAT 0.8 / DEFAULT 0.5). See [smart_topic.md](smart_topic.md).
- **ollama.h** — `ollama_query(host, port, model, prompt, out, out_size)`; `ollama_query_with_options(..., temperature, out, out_size)` when using smart_topic.
- **storage.h**, **tenant.h**, **validate.h** — see headers.

See **[README.md](../README.md)** in c-lib for build options (e.g. `USE_MONGOC=1`) and prerequisites. For the TINY model (e.g. TinyLlama) used by smart_topic: `make ollama-tiny` or `ollama pull tinyllama`.
