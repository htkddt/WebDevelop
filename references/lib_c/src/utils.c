#include "utils.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct m4_ht_entry {
    char *key;
    void *value;
    struct m4_ht_entry *next;
} m4_ht_entry_t;

struct m4_ht {
    m4_ht_entry_t **buckets;
    size_t nbuckets;
    size_t count;
};

static uint32_t hash_djb2(const char *s) {
    uint32_t h = 5381u;
    while (*s) h = ((h << 5) + h) + (uint32_t)(unsigned char)*s++;
    return h;
}

m4_ht_t *m4_ht_create(size_t nbuckets) {
    if (nbuckets < 16) nbuckets = 16;
    m4_ht_t *ht = (m4_ht_t *)calloc(1, sizeof(m4_ht_t));
    if (!ht) return NULL;
    ht->buckets = (m4_ht_entry_t **)calloc(nbuckets, sizeof(m4_ht_entry_t *));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    ht->nbuckets = nbuckets;
    return ht;
}

static void free_chain(m4_ht_entry_t *e, void (*free_value)(void *)) {
    while (e) {
        m4_ht_entry_t *n = e->next;
        free(e->key);
        if (free_value) free_value(e->value);
        else free(e->value);
        free(e);
        e = n;
    }
}

void m4_ht_destroy(m4_ht_t *ht, void (*free_value)(void *value)) {
    if (!ht) return;
    for (size_t i = 0; i < ht->nbuckets; i++)
        free_chain(ht->buckets[i], free_value);
    free(ht->buckets);
    free(ht);
}

void *m4_ht_get(const m4_ht_t *ht, const char *key) {
    if (!ht || !key) return NULL;
    size_t i = (size_t)hash_djb2(key) % ht->nbuckets;
    for (m4_ht_entry_t *e = ht->buckets[i]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e->value;
    }
    return NULL;
}

int m4_ht_set(m4_ht_t *ht, const char *key, void *value) {
    if (!ht || !key) return -1;
    size_t i = (size_t)hash_djb2(key) % ht->nbuckets;
    for (m4_ht_entry_t *e = ht->buckets[i]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            void *old = e->value;
            e->value = value;
            free(old);
            return 0;
        }
    }
    m4_ht_entry_t *ne = (m4_ht_entry_t *)malloc(sizeof(m4_ht_entry_t));
    if (!ne) return -1;
    ne->key = strdup(key);
    if (!ne->key) {
        free(ne);
        return -1;
    }
    ne->value = value;
    ne->next = ht->buckets[i];
    ht->buckets[i] = ne;
    ht->count++;
    return 0;
}

size_t m4_ht_count(const m4_ht_t *ht) {
    return ht ? ht->count : 0;
}

void m4_ht_foreach(m4_ht_t *ht, m4_ht_iter_fn fn, void *userdata) {
    if (!ht || !fn) return;
    for (size_t i = 0; i < ht->nbuckets; i++) {
        for (m4_ht_entry_t *e = ht->buckets[i]; e; e = e->next)
            fn(e->key, e->value, userdata);
    }
}

void *m4_ht_take(m4_ht_t *ht, const char *key) {
    if (!ht || !key) return NULL;
    size_t i = (size_t)hash_djb2(key) % ht->nbuckets;
    m4_ht_entry_t **pp = &ht->buckets[i];
    while (*pp) {
        m4_ht_entry_t *e = *pp;
        if (strcmp(e->key, key) == 0) {
            *pp = e->next;
            void *v = e->value;
            free(e->key);
            free(e);
            if (ht->count > 0) ht->count--;
            return v;
        }
        pp = &e->next;
    }
    return NULL;
}
