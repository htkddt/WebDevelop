# Getting started with python_ai (standalone repos, no root build)

Step-by-step: use the **c-lib** from the **python_ai** repo when both are separate repositories and there is no parent build.

## Prerequisites

- **Python 3** (3.7+)
- **Ollama** installed and running (e.g. `ollama run llama3.2`) for AI replies  
  Per-model prompt notes for contributors live under **c-lib** [`.cursor/models/`](../.cursor/models/README.md) (`{slug}.rule.md`; slug from `python3 scripts/model_rule_slug.py "<model>"`).
- **c-lib** build dependencies (only if you build the library yourself): Clang/GCC, ncurses, libcurl — see [c-lib README](../README.md#prerequisites-cross-platform)

## Step 1: Get and build c-lib

Clone the c-lib repo and build the shared library (no executable):

```bash
git clone <c-lib-repo-url> c-lib
cd c-lib
make validate
make lib
```

You should see **`c-lib/lib/libm4engine.dylib`** (macOS) or **`c-lib/lib/libm4engine.so`** (Linux).

Optional with MongoDB: `make lib USE_MONGOC=1` (see [c-lib README](../README.md)).

## Step 2: Get python_ai

Clone the python_ai repo. Typical layout:

```text
your-workspace/
  c-lib/          # from Step 1 (with c-lib/lib/ containing the built library)
  python_ai/      # this repo
```

```bash
git clone <python_ai-repo-url> python_ai
cd python_ai
```

## Step 3: Point python_ai at the library

**Option A — c-lib next to python_ai (recommended)**

If your layout is:

```text
…/c-lib/lib/libm4engine.dylib
…/python_ai/run_ai.py
```

then **run from the `python_ai` directory**; the script will look for **`../c-lib/lib`** automatically:

```bash
cd python_ai
python3 run_ai.py "Your question"
```

**Option B — Environment variable**

Set the path to the shared library file:

```bash
export M4ENGINE_LIB=/absolute/path/to/c-lib/lib/libm4engine.dylib   # macOS
# or
export M4ENGINE_LIB=/absolute/path/to/c-lib/lib/libm4engine.so      # Linux

python3 run_ai.py "Your question"
```

**Option C — Copy lib into python_ai**

Copy the built library into python_ai:

```bash
mkdir -p python_ai/lib
cp c-lib/lib/libm4engine.dylib python_ai/lib/   # macOS
# or
cp c-lib/lib/libm4engine.so   python_ai/lib/    # Linux

cd python_ai
python3 run_ai.py "Your question"
```

## Step 4: Run

```bash
cd python_ai
python3 run_ai.py "Hello, respond in one short sentence."
# or default prompt:
python3 run_ai.py
```

Ensure **Ollama** is running (e.g. `ollama run llama3.2` in another terminal). If the library is not found, you’ll get a clear error; fix the path using one of the options in Step 3.

## Step 5 (optional): HTTP chat server (`server/app.py`)

Same **M4ENGINE_LIB** / sibling **c-lib** layout as above. From the **python_ai** directory:

```bash
cd python_ai
python3 -m pip install -r server/requirements.txt   # flask, flask-cors
python3 server/app.py
```

The process prints **`http://127.0.0.1:5000`** (or **5001** if 5000 is taken). Endpoints include **`POST /api/chat`**, **`POST /api/chat/stream`** (SSE), **`GET /api/history`**, **`GET /api/stats`**, **`POST /api/geo/import`** (CSV → **`api_geo_atlas_import_row`**). **`GET /`** returns a small JSON map of routes.

- **Model:** set **`OLLAMA_MODEL`** before starting the server so it matches what you pulled in Ollama (overrides c-lib **`OLLAMA_DEFAULT_MODEL`** in **`include/ollama.h`** when the library passes a NULL model).
- **Options:** **`M4ENGINE_SERVER_MAX=0`** uses slimmer **`build_api_options()`** instead of **`build_max_api_options()`**; Mongo/Redis/ELK URIs use **`M4ENGINE_MONGO_URI`**, **`M4ENGINE_REDIS_HOST`**, etc. — see **`python_ai/server/app.py`** (top docstring) and **`python_ai/training/README.md`**.
- **Geo CSV:** default import does **not** call Ollama; add query **`embed=1`** only if you want embeddings.

Mapping table: [api.md § Python HTTP server](api.md#python-http-server-reference-consumer).

## Summary

1. Clone **c-lib** → `make lib` (see [c-lib README](../README.md)).
2. Clone **python_ai** next to (or elsewhere and set **M4ENGINE_LIB**).
3. Point to **c-lib/lib** (sibling, env var, or copy into python_ai).
4. Run **`python3 run_ai.py "…"`** from the python_ai directory, **or** **`python3 server/app.py`** for the HTTP API (Step 5).

For the C consumer (c_ai), see [consumers.md](consumers.md) and the c_ai repo README.
