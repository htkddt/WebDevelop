# Stat module

The **stat** module tracks runtime stats for the library: memory, connection status of every mod (Mongo, Redis, ELK), and error/warning counts. It can also write errors and warnings to MongoDB **ai_logs** and optionally to ELK.

**Per-mode rule (see c-lib rule.md):** In **MODE_ONLY_MEMORY**, the library never writes to MongoDB or ELK. Error/warning counts are updated via `stat_inc_error` / `stat_inc_warning` only.

## 1. What is tracked

| Stat | Description | How to set |
|------|-------------|------------|
| **memory_bytes** | Library heap estimate (bytes). 0 = unknown. | `stat_set_memory_bytes()` |
| **mongo_connected** | MongoDB: 0 = disconnected, 1 = connected | `stat_set_mongo_connected()` (e.g. from `storage_mongo_connected(storage)`) |
| **mongoc_linked** | *(on `api_stats_t` from `api_get_stats` only)* 1 = built with `USE_MONGOC=1`; 0 = storage uses stubs, **`mongo_connected` stays 0** even in MONGO modes | Compile-time |
| **redis_connected** | Redis: 0/1 | `stat_set_redis_connected()` |
| **elk_enabled** | ELK configured and should be used: 0/1 | `stat_set_elk_enabled()` |
| **elk_connected** | ELK ingest reachable: 0/1 | `stat_set_elk_connected()` |
| **error_count** | Total errors (monotonic) | `stat_inc_error()` |
| **warning_count** | Total warnings (monotonic) | `stat_inc_warning()` |
| **processed** / **errors** | Engine-style counts | `stat_set_processed()`, `stat_set_errors()` (optional) |

## 2. Usage

```c
#include "stat.h"
#include "storage.h"

stat_ctx_t *stat = stat_create();
storage_ctx_t *storage = ...;

// After storage_connect():
stat_set_mongo_connected(stat, storage_mongo_connected(storage));
stat_set_elk_enabled(stat, (config->es_host && config->es_host[0]) ? 1 : 0);

// On error (increments count and writes to ai_logs + optional ELK):
stat_inc_error(stat);

// Snapshot for display or metrics:
stat_snapshot_t snap;
stat_get_snapshot(stat, &snap);
printf("errors=%lu warnings=%lu mongo=%d elk_enabled=%d\n",
       (unsigned long)snap.error_count, (unsigned long)snap.warning_count,
       snap.mongo_connected, snap.elk_enabled);

stat_destroy(stat);
```

## 3. ai_logs collection (MongoDB)

Error/warning counts are incremented via **`stat_inc_error()`** / **`stat_inc_warning()`**. For writing to MongoDB ai_logs, use **`storage_append_ai_log()`** directly when needed.

- **Database:** `STORAGE_AI_LOGS_DB` (default `bot`)
- **Collection:** `STORAGE_AI_LOGS_COLLECTION` (default `ai_logs`)

Document shape: `{ tenant_id, level ("error"|"warning"), message, ts }`. This relates to error/warning counts and can be used for dashboards or ELK. If ELK is configured (`es_host` set), the same message is also sent via **`storage_elk_ingest()`** (e.g. for analytics).

## 4. Headers and API

See **include/stat.h** for the full API: `stat_create`, `stat_destroy`, `stat_get_snapshot`, `stat_inc_error`, `stat_inc_warning`, `stat_set_*`.
