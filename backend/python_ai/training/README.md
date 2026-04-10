# Training / full options

All API options are configurable via environment variables. Used by `run_ai_tui.py` and any script that needs full control.

## Max option (MongoDB + Redis + ELK)

In `run_ai_tui.py`, set **`USE_MAX_OPTIONS = True`** to use the full stack: **mode=MONGO_REDIS_ELK** with MongoDB, Redis, and ELK enabled. Default connection strings (env overrides):

| Variable | Max default |
|----------|-------------|
| `M4ENGINE_MONGO_URI` | `mongodb://127.0.0.1:27017` |
| `M4ENGINE_REDIS_HOST` | `127.0.0.1` |
| `M4ENGINE_REDIS_PORT` | `6379` |
| `M4ENGINE_ES_HOST` | `127.0.0.1` |
| `M4ENGINE_ES_PORT` | `9200` |

Set **`USE_MAX_OPTIONS = False`** to use env-only options (e.g. MEMORY mode if nothing set).

**Why does Bot always show [OLLAMA] even with Redis host set?** The c-lib Redis module is a **stub**: it reports "connected" but does not store or search. So the reply-cache path never finds hits and every reply comes from Ollama. See c-lib `docs/WHY_ONLY_OLLAMA_NOT_REDIS.md` for the full trace (TUI → api_chat → storage_rag_search → redis_search_semantic stub). To get [REDIS] replies you must implement Redis L2 in c-lib (Hiredis + RediSearch).

## Environment variables (full option set)

| Variable | Meaning | Example |
|----------|---------|---------|
| `M4ENGINE_MODE` | 0=MEMORY, 1=MONGO, 2=MONGO_REDIS, 3=MONGO_REDIS_ELK | `0` |
| `M4ENGINE_MONGO_URI` | MongoDB connection string | `mongodb://127.0.0.1:27017` |
| `M4ENGINE_REDIS_HOST` | Redis host | `127.0.0.1` |
| `M4ENGINE_REDIS_PORT` | Redis port (0 = default 6379) | `6379` |
| `M4ENGINE_ES_HOST` | Elasticsearch host (empty = disabled) | `` |
| `M4ENGINE_ES_PORT` | Elasticsearch port (0 = 9200) | `9200` |
| `M4ENGINE_LOG_DB` | ai_logs DB override (validated) | `mydb` |
| `M4ENGINE_LOG_COLL` | ai_logs collection override (validated) | `my_logs` |
| `M4ENGINE_CONTEXT_BATCH_SIZE` | History size / context batch (0 = 30) | `20` |
| `M4ENGINE_SMART_TOPIC` | 1 = enable smart topic (intent-based temperature: TECH 0.0, CHAT 0.8, DEFAULT 0.5) | `0` |
| `M4ENGINE_SMART_TOPIC_COLLECTION` | Mongo collection for smart topic (default `smart_topic`) | `smart_topic` |
| `M4ENGINE_SMART_TOPIC_MODEL_TINY` | Model name for tiny tier (e.g. `tinyllama`) | (optional) |
| `M4ENGINE_SMART_TOPIC_MODEL_B2` | Model name for B2 tier | (optional) |

Unset values use C library defaults (e.g. MEMORY mode, no Mongo/Redis if not set).

## Usage

```bash
cd python_ai
# Default: MEMORY mode, context_batch_size=0 (30)
python3 run_ai_tui.py

# Full option: MONGO + custom history size
export M4ENGINE_MODE=1
export M4ENGINE_MONGO_URI=mongodb://localhost:27017
export M4ENGINE_CONTEXT_BATCH_SIZE=20
python3 run_ai_tui.py
```

From Python:

```python
from training.full_options import build_api_options, get_full_options

opts, history_size, smart_topic_keepalive = build_api_options()
# opts is ApiOptions for api_create; history_size for deque(maxlen=...)
# Keep smart_topic_keepalive in scope when calling api_create (so smart_topic_opts pointer stays valid)
```
