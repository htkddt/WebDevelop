#ifndef M4_UTILS_H
#define M4_UTILS_H

#include <stddef.h>

/** Open-addressing-free string hash map (separate chaining). Not thread-safe; wrap with your lock. */
typedef struct m4_ht m4_ht_t;

m4_ht_t *m4_ht_create(size_t nbuckets);
void m4_ht_destroy(m4_ht_t *ht, void (*free_value)(void *value));

/** Insert or replace. Copies key with strdup; value must be heap pointer (owned by table after success). Returns -1 on OOM. */
int m4_ht_set(m4_ht_t *ht, const char *key, void *value);

void *m4_ht_get(const m4_ht_t *ht, const char *key);
size_t m4_ht_count(const m4_ht_t *ht);

/** Walk all entries (for eviction sweeps). Not thread-safe. */
typedef void (*m4_ht_iter_fn)(const char *key, void *value, void *userdata);
void m4_ht_foreach(m4_ht_t *ht, m4_ht_iter_fn fn, void *userdata);

/** Remove key; returns value pointer for caller to free, or NULL if missing. */
void *m4_ht_take(m4_ht_t *ht, const char *key);

#endif /* M4_UTILS_H */
