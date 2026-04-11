/*
 * Geo authority L1 cache — in-memory validation (.cursor/auth_geo.md).
 * Thread-safe lookups; optional CSV/JSON buffer load; hooks for geo_learning + stream audit.
 */
#ifndef M4_GEO_AUTHORITY_H
#define M4_GEO_AUTHORITY_H

#include <stddef.h>
#include <stdint.h>

/** Max entries (spec: ~15k provinces/districts/landmarks). */
#define GEO_AUTHORITY_MAX_ENTRIES 15000

typedef struct geo_authority_entry {
    char *display_name;
    int32_t parent_id; /* -1 = top-level (e.g. province); else opaque index */
    double trust_score; /* 1.0 = system seed; lower = user-learned */
    /** If set: administrative merge — listing this name alongside `merged_into_key` as separate items is a logic conflict (.cursor/conflict_detector.md). */
    char *merged_into_key;
} geo_authority_entry_t;

/** Init/shutdown the global cache (idempotent init). */
int geo_authority_init(void);
void geo_authority_shutdown(void);

/** Normalize key: lowercase Latin A–Z, spaces → '_'. UTF-8 bytes preserved (same idea as geo_learning). */
void geo_authority_normalize_key(const char *name, char *out, size_t out_size);

/**
 * Load CSV or JSON-ish buffer into the cache (wrlock). CSV: header row with name/location_name column.
 * Returns number of rows inserted, or -1 on fatal parse error.
 */
int geo_authority_load_buffer(const char *data);

/** Lookup by normalized key (rdlock). Returns NULL if missing. */
const geo_authority_entry_t *geo_authority_lookup(const char *name_normalized);

/**
 * Upsert after geo_learning insert (wrlock).
 * merged_into_normalized: optional authority merge target key (same normalization as name_normalized); NULL if none.
 */
int geo_authority_upsert_learned(const char *display_name, const char *name_normalized,
                                 int32_t parent_id, double trust_score,
                                 const char *merged_into_normalized_or_null);

/** Short line of seeded place names for prompt injection (no I/O). */
void geo_authority_prompt_hint(char *out, size_t out_size);

/**
 * Scan assistant text: log unknown place-like tokens to stderr (heuristic; rdlock).
 * Best-effort guard — does not rewrite text (full "Trảm" intercept is app-level).
 */
void geo_authority_audit_response_text(const char *utf8);

#endif /* M4_GEO_AUTHORITY_H */
