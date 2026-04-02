# Unused code / minimal public API

We keep a **minimal public API** (api.h for consumers). The following symbols were **removed** as not part of that surface and with no internal callers:

| Removed | Was in |
|--------|--------|
| `engine_set_mode` | engine.h, engine.c |
| `tenant_index_key` | tenant.h, tenant.c |
| `stat_log_error`, `stat_log_warning` | stat.h, stat.c |
| `storage_vector_search` | storage.h, storage.c |
| `storage_inc_counter`, `storage_get_counter` | storage.h, storage.c |
| `storage_index_doc` | storage.h, storage.c |

Internal/advanced headers (engine.h, tenant.h, stat.h, storage.h) are still used by the library implementation but expose fewer functions. Consumers should use **api.h** only.

## Duplicate helpers

- `chat_timestamp()` exists as static in both api.c and main.c (intentional per-TU copy).
