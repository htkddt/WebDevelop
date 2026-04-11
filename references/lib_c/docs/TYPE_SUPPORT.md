# Type support vs rule.md

Checklist mapping `.cursor/rule.md` to implementation.

## §7 – Execution flow (4 types)

| Rule type | Enum in code | Status |
|-----------|--------------|--------|
| A. MODE_ONLY_MEMORY | `MODE_ONLY_MEMORY` | ✅ In `engine.h` (`execution_mode_t`). Behavior: storage can skip Mongo/Redis when this mode is set (to be wired). |
| B. MODE_ONLY_MONGO | `MODE_ONLY_MONGO` | ✅ In `engine.h`. Used as default in `main.c` when Mongo is linked. |
| C. MODE_MONGO_REDIS | `MODE_MONGO_REDIS` | ✅ In `engine.h`. Redis fast-path + Change Streams not yet implemented. |
| D. MODE_MONGO_REDIS_ELK | `MODE_MONGO_REDIS_ELK` | ✅ In `engine.h`. ELK pipeline `auto_lang_processor` stubbed in storage. |

**Config:** `engine_config_t.execution_mode` set in `main.c` (default `MODE_ONLY_MONGO`).

---

## §7 – Inference stage

| Rule | Location | Status |
|------|----------|--------|
| libcurl to `127.0.0.1:11434` (Ollama) | `c-lib/src/ollama.c` | ✅ |
| `CURLOPT_TIMEOUT` 10s | `c-lib/src/ollama.c` | ✅ |
| `CURLOPT_TCP_NODELAY` 1 | `c-lib/src/ollama.c` | ✅ |

---

## §7 – Storage stage

| Rule | Status |
|------|--------|
| Chat in local queue (memory buffer) | ✅ Circular buffer in `main.c`. |
| Every 5–10 records → `mongoc_bulk_operation_t` | ⏳ Not yet: currently one insert per message in `storage_append_chat`. |

---

## §8 – Memory & buffer

| Rule | Location | Status |
|------|----------|--------|
| `OL_BUF_SIZE` 32768 (32KB) | `c-lib/include/ollama.h` | ✅ |
| `setlocale(LC_ALL, "")` in main | `c-lib/src/main.c` | ✅ |
| Cursor safety / destroy `mongoc_cursor_t` | `c-lib/src/storage.c` | ✅ (cursors destroyed). |
| `mongoc_client_pool_t` (no new client per request) | `c-lib/src/storage.c` | ⏳ Single client used; pool to be added. |

---

## §7 – Fast-path (Redis) and ELK

| Rule | Status |
|------|--------|
| Count/Status → Redis Hash first, skip LLM | ⏳ Not implemented. |
| Mongo Change Streams → sync Redis | ⏳ Not implemented. |
| ELK `_doc?pipeline=auto_lang_processor` | ⏳ Stub only in `storage_elk_ingest`. |

---

## Compute vs execution modes

- **`run_mode_t`** (M4 NPU / CUDA remote / Hybrid): compute backend; used by dispatcher.
- **`execution_mode_t`** (A/B/C/D): storage/data path; defined in rule §7, implemented in `c-lib/include/engine.h` and defaulted in `main.c`.
