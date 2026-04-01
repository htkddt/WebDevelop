/*
 * Geo learning worker: after turn persisted, extract place names (any region) via Ollama,
 * dedupe by vector similarity, insert into geo_atlas. Optional: does not block chat. See .cursor/geo_leanring.md.
 */
#include "geo_learning.h"
#include "geo_authority.h"
#include "storage.h"
#include "embed.h"
#include "ollama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>

#define GEO_QUEUE_CAP      16
#define GEO_INPUT_SIZE     1024
#define GEO_ASSISTANT_SIZE 1024
#define GEO_TENANT_SIZE    64
#define GEO_TS_SIZE        32
#define GEO_EXTRACT_BUF    (8 * 1024)
#define GEO_ENTITY_NAME    256

typedef struct {
    char input[GEO_INPUT_SIZE];
    char assistant[GEO_ASSISTANT_SIZE];
    char tenant_id[GEO_TENANT_SIZE];
    char user_id[GEO_TENANT_SIZE];
    char timestamp[GEO_TS_SIZE];
} geo_turn_item_t;

static storage_ctx_t *s_storage;
static geo_turn_item_t s_queue[GEO_QUEUE_CAP];
static int s_head, s_tail, s_count;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static volatile int s_shutdown;
static pthread_t s_worker;

/** After trim: `[]` only → model said no places; not an error. */
static int geo_extract_output_is_empty_array(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') s++;
    if (s[0] != '[' || s[1] != ']') return 0;
    s += 2;
    while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') s++;
    return *s == '\0';
}

static void strncpy_safe(char *dst, const char *src, size_t dsize) {
    if (!dst || dsize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dsize) n = dsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/** Simple normalization: lowercase, replace spaces with underscore (libpostal stub). */
static void normalize_name(const char *name, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!name) { out[0] = '\0'; return; }
    size_t i = 0;
    for (; name[i] && i < out_size - 1; i++) {
        int c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c == ' ' || c == '\t') c = '_';
        out[i] = (char)c;
    }
    out[i] = '\0';
}

/** If axis is only macro compass (South/North/Central), treat as region (schema §13). */
static void relocate_axis_to_region(char *axis, char *region, size_t rsize) {
    if (!axis || !region || rsize == 0) return;
    if (!axis[0]) return;
    if (strcasecmp(axis, "South") == 0 || strcasecmp(axis, "North") == 0 || strcasecmp(axis, "Central") == 0) {
        if (!region[0]) {
            strncpy(region, axis, rsize - 1);
            region[rsize - 1] = '\0';
        }
        axis[0] = '\0';
    }
}

/** Extract one entity from JSON snippet. */
static int parse_one_entity(const char *json, char *name, size_t nsize,
                           char *district, size_t dsize, char *axis, size_t asize,
                           char *category, size_t csize, char *city, size_t citysize,
                           char *region, size_t rsize, char *landmarks, size_t lmsize,
                           char *country, size_t cnsize, char *merged_into, size_t msize,
                           char *admin_action, size_t aasize, char *admin_detail, size_t adsize) {
    if (!json || !name) return 0;
    name[0] = district[0] = axis[0] = category[0] = '\0';
    if (city && citysize) city[0] = '\0';
    if (region && rsize) region[0] = '\0';
    if (landmarks && lmsize) landmarks[0] = '\0';
    if (country && cnsize) country[0] = '\0';
    if (merged_into && msize) merged_into[0] = '\0';
    if (admin_action && aasize) admin_action[0] = '\0';
    if (admin_detail && adsize) admin_detail[0] = '\0';
    const char *p;
#define FIND_KEY(Var, key, buf, len) do {\
    p = strstr(json, key);\
    if (!p) break;\
    p += sizeof(key) - 1;\
    while (*p == ' ') p++;\
    if (*p != '"') break;\
    p++;\
    const char *end = strchr(p, '"');\
    if (!end) break;\
    size_t L = (size_t)(end - p);\
    if (L >= len) L = len - 1;\
    memcpy(buf, p, L); buf[L] = '\0';\
} while(0)
    FIND_KEY(p, "\"name\":", name, nsize);
    FIND_KEY(p, "\"district\":", district, dsize);
    FIND_KEY(p, "\"axis\":", axis, asize);
    FIND_KEY(p, "\"category\":", category, csize);
    if (city && citysize)
        FIND_KEY(p, "\"city\":", city, citysize);
    if (region && rsize)
        FIND_KEY(p, "\"region\":", region, rsize);
    if (landmarks && lmsize)
        FIND_KEY(p, "\"landmarks\":", landmarks, lmsize);
    if (country && cnsize)
        FIND_KEY(p, "\"country\":", country, cnsize);
    if (merged_into && msize)
        FIND_KEY(p, "\"merged_into\":", merged_into, msize);
    if (admin_action && aasize)
        FIND_KEY(p, "\"admin_action\":", admin_action, aasize);
    if (admin_detail && adsize)
        FIND_KEY(p, "\"admin_detail\":", admin_detail, adsize);
#undef FIND_KEY
    return name[0] != '\0';
}

/**
 * §7 internal check: LLM YES/NO plausibility. GEO_INTEGRITY_VERIFY=0 skips (assume YES).
 * Returns 1 = plausible, 0 = not, -1 = Ollama error (caller treats as not verified).
 */
static int geo_verify_place_plausible(const char *name, const char *district, const char *city) {
    const char *ev = getenv("GEO_INTEGRITY_VERIFY");
    if (ev && ev[0] == '0') return 1;
    if (!name || !name[0]) return 0;
    char prompt[1024];
    int pl = snprintf(prompt, sizeof(prompt),
        "Answer with exactly one word: YES or NO.\n"
        "Is it geographically plausible that the place \"%s\" is located in area \"%s\", city/region \"%s\"?\n"
        "If area or city is unspecified, answer YES only if \"%s\" is a real place name.",
        name,
        district && district[0] ? district : "(unspecified)",
        city && city[0] ? city : "(unspecified)",
        name);
    if (pl <= 0 || pl >= (int)sizeof(prompt)) return -1;
    char resp[2048];
    if (ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, NULL, prompt, resp, sizeof(resp)) != 0)
        return -1;
    /* Find YES before NO */
    for (const char *p = resp; *p; p++) {
        if (strncasecmp(p, "YES", 3) == 0)
            return 1;
    }
    for (const char *p = resp; *p; p++) {
        if (strncasecmp(p, "NO", 2) == 0)
            return 0;
    }
    return 0;
}

/** Process one turn: Ollama extraction -> for each entity embed, find_similar, insert if new. */
static void process_turn(const geo_turn_item_t *item) {
    if (!s_storage) {
        fprintf(stderr, "[GEO_LEARNING] process_turn: s_storage is NULL\n");
        return;
    }
    char prompt[GEO_EXTRACT_BUF];
    int plen = snprintf(prompt, sizeof(prompt),
        "Extract any real-world places mentioned in the conversation: cities, districts, neighborhoods, "
        "landmarks, streets, or regions. "
        "Output strictly a JSON array of objects with keys: "
        "name, district, axis, category, city, region, country, landmarks, merged_into, "
        "admin_action, admin_detail. "
        "Use \"district\" for administrative center / capital when applicable; empty if N/A. "
        "Use \"axis\" for routes/navigation (e.g. QL1) — NOT for macro compass alone; "
        "if you only have South/North as macro area, put that in \"region\" and leave axis empty. "
        "Use \"region\" for areas like Mekong Delta, South, Central Highlands; empty if unknown. "
        "Use \"country\" (e.g. Vietnam); empty defaults to Vietnam in storage. "
        "Use \"landmarks\" as comma-separated POIs if listed; empty if none. "
        "Use \"merged_into\" only if the place was administratively merged into another named entity (official target name); empty otherwise. "
        "Optional \"admin_action\": one of merge, split, expand, upgrade, rename, alias, status (administrative change type). "
        "Optional \"admin_detail\": short UTF-8 note (e.g. target name, old name); avoid double-quotes inside.\n\n"
        "User: %s\n\nAssistant: %s\n\nJSON array:",
        item->input, item->assistant);
    if (plen <= 0 || plen >= (int)sizeof(prompt)) {
        fprintf(stderr, "[GEO_LEARNING] prompt too long (%d)\n", plen);
        return;
    }

    char out[GEO_EXTRACT_BUF];
    if (ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, NULL, prompt, out, sizeof(out)) != 0) {
        fprintf(stderr, "[GEO_LEARNING] ollama_query failed (is Ollama running?)\n");
        return;
    }
    if (geo_extract_output_is_empty_array(out))
        return;

    /* Skip markdown code block if present (e.g. ```json ... ```) */
    char *parse_start = out;
    while (*parse_start == ' ' || *parse_start == '\n' || *parse_start == '\t') parse_start++;
    if (strncmp(parse_start, "```json", 7) == 0) parse_start += 7;
    else if (strncmp(parse_start, "```", 3) == 0) parse_start += 3;
    while (*parse_start == ' ' || *parse_start == '\n') parse_start++;
    if (parse_start != out) {
        size_t rest = strlen(parse_start) + 1;
        memmove(out, parse_start, rest);
    }
    if (geo_extract_output_is_empty_array(out))
        return;

    if (strlen(out) < 10) {
        fprintf(stderr, "[GEO_LEARNING] extract response too short for JSON (< 10 chars): %.200s\n", out);
        return;
    }

    m4_embed_options_t emb_opt;
    m4_embed_options_geo_env(&emb_opt);

    /* Parse array: split by "},{", parse each segment. */
    char name[GEO_ENTITY_NAME], district[GEO_ENTITY_NAME], axis[128], category[128], city[GEO_ENTITY_NAME];
    char region[256], landmarks[512], country[128], merged_into[256];
    char admin_action[64], admin_detail[512];
    float embed_vec[OLLAMA_EMBED_MAX_DIM];
    size_t embed_dim = 0;

    static char copy[GEO_EXTRACT_BUF];
    size_t copylen = strlen(out);
    if (copylen >= sizeof(copy)) copylen = sizeof(copy) - 1;
    memcpy(copy, out, copylen + 1);
    copy[copylen] = '\0';
    char *seg = copy;
    /* Skip leading [ or whitespace */
    while (*seg && (*seg == '[' || *seg == ' ' || *seg == '\n')) seg++;
    int entities_found = 0;
    int entities_inserted = 0;
    const char *tid = (item->tenant_id[0] != '\0') ? item->tenant_id : "default";
    for (;;) {
        char *end = strstr(seg, "},{");
        if (end) *end = '\0';
        if (strstr(seg, "\"name\"") && parse_one_entity(seg, name, sizeof(name), district, sizeof(district),
                axis, sizeof(axis), category, sizeof(category), city, sizeof(city),
                region, sizeof(region), landmarks, sizeof(landmarks),
                country, sizeof(country), merged_into, sizeof(merged_into),
                admin_action, sizeof(admin_action), admin_detail, sizeof(admin_detail))) {
            relocate_axis_to_region(axis, region, sizeof(region));
            entities_found++;
            fprintf(stderr, "[GEO_LEARNING] embedding '%s' (backend=%s)...\n", name,
                    emb_opt.backend == M4_EMBED_BACKEND_CUSTOM ? "custom" : "ollama");
            char name_norm[GEO_ENTITY_NAME];
            normalize_name(name, name_norm, sizeof(name_norm));
            char country_buf[128];
            if (country[0]) {
                strncpy(country_buf, country, sizeof(country_buf) - 1);
                country_buf[sizeof(country_buf) - 1] = '\0';
            } else {
                memcpy(country_buf, "Vietnam", sizeof("Vietnam"));
            }

            if (strcasecmp(country_buf, "Vietnam") != 0) {
                fprintf(stderr, "[GEO_LEARNING] non-Vietnam country '%s' — pending verification\n", country_buf);
            }

            if (district[0] && strcasecmp(name, district) == 0)
                fprintf(stderr, "[GEO_LEARNING] district equals name (redundant): %s\n", name);

            char merged_norm[GEO_ENTITY_NAME];
            merged_norm[0] = '\0';
            if (merged_into[0])
                normalize_name(merged_into, merged_norm, sizeof(merged_norm));

            if (storage_geo_atlas_exists_normalized_country(s_storage, tid, name_norm, country_buf)) {
                fprintf(stderr, "[GEO_LEARNING] duplicate composite key tenant+name_norm+country: %s / %s\n", name, country_buf);
                if (!end) break;
                seg = end + 3;
                continue;
            }

            char embed_model_id[128];
            int embed_ret =
                m4_embed_text(&emb_opt, name, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim, embed_model_id, sizeof(embed_model_id));
            if (embed_ret == 0 && embed_dim > 0) {
                fprintf(stderr, "[GEO_LEARNING] embedding success: dim=%zu model=%s\n", embed_dim, embed_model_id);
                if (storage_redis_connected(s_storage) &&
                    storage_geo_redis_find_similar(s_storage, tid, embed_vec, embed_dim, GEO_ATLAS_SIMILARITY_THRESHOLD)) {
                    fprintf(stderr, "[GEO_LEARNING] duplicate (Redis geo L2): %s\n", name);
                } else if (storage_geo_atlas_find_similar(s_storage, tid, embed_vec, embed_dim, GEO_ATLAS_SIMILARITY_THRESHOLD)) {
                    fprintf(stderr, "[GEO_LEARNING] duplicate (Mongo similarity >= %.2f): %s\n", GEO_ATLAS_SIMILARITY_THRESHOLD, name);
                } else {
                    int seed_conflict = storage_geo_atlas_seed_conflict(s_storage, tid, name_norm, district, city);
                    int verify_r = geo_verify_place_plausible(name, district, city);
                    const char *vstatus = STORAGE_GEO_STATUS_VERIFIED;
                    double trust = 0.88;
                    if (merged_into[0] && merged_norm[0]) {
                        vstatus = STORAGE_GEO_STATUS_MERGED;
                        trust = 1.0;
                        fprintf(stderr, "[GEO_LEARNING] merged_into set → status=%s\n", vstatus);
                    } else if (strcasecmp(country_buf, "Vietnam") != 0) {
                        vstatus = STORAGE_GEO_STATUS_PENDING_VERIFICATION;
                        trust = 0.42;
                    } else if (seed_conflict) {
                        vstatus = STORAGE_GEO_STATUS_PENDING_VERIFICATION;
                        trust = 0.35;
                        fprintf(stderr, "[GEO_LEARNING] §7 seed conflict → %s\n", vstatus);
                    } else if (verify_r < 0) {
                        vstatus = STORAGE_GEO_STATUS_PENDING_VERIFICATION;
                        trust = 0.40;
                        fprintf(stderr, "[GEO_LEARNING] §7 verify error → %s\n", vstatus);
                    } else if (verify_r == 0) {
                        vstatus = STORAGE_GEO_STATUS_PENDING_VERIFICATION;
                        trust = 0.45;
                        fprintf(stderr, "[GEO_LEARNING] §7 verify NO → %s\n", vstatus);
                    }
                    char merged_storage[GEO_ENTITY_NAME];
                    merged_storage[0] = '\0';
                    if (merged_into[0])
                        normalize_name(merged_into, merged_storage, sizeof(merged_storage));

                    storage_geo_atlas_doc_t doc;
                    memset(&doc, 0, sizeof(doc));
                    doc.tenant_id = tid;
                    doc.name = name;
                    doc.name_normalized = name_norm;
                    doc.district = district;
                    doc.axis = axis;
                    doc.category = category;
                    doc.city = city;
                    doc.region = region[0] ? region : NULL;
                    doc.country = country_buf;
                    doc.landmarks = landmarks[0] ? landmarks : NULL;
                    doc.merged_into = merged_storage[0] ? merged_storage : NULL;
                    doc.admin_action = admin_action[0] ? admin_action : NULL;
                    doc.admin_detail = admin_detail[0] ? admin_detail : NULL;
                    doc.vector = embed_vec;
                    doc.vector_dim = embed_dim;
                    doc.embed_model_id = embed_model_id;
                    doc.source = STORAGE_GEO_SOURCE_USER;
                    doc.verification_status = vstatus;
                    doc.trust_score = trust;
                    int insert_ret = storage_geo_atlas_insert_doc(s_storage, &doc);
                    if (insert_ret == 0) {
                        entities_inserted++;
                        fprintf(stderr, "[GEO_LEARNING] insert ok: %s status=%s trust=%.2f\n", name, vstatus, trust);
                        if (strcmp(vstatus, STORAGE_GEO_STATUS_VERIFIED) == 0
                            || strcmp(vstatus, STORAGE_GEO_STATUS_MERGED) == 0)
                            (void)geo_authority_upsert_learned(name, name_norm, -1, trust,
                                                             merged_norm[0] ? merged_norm : NULL);
                        if (storage_redis_connected(s_storage)) {
                            char doc_id[160];
                            (void)snprintf(doc_id, sizeof(doc_id), "geo_%s_%lld", name_norm, (long long)time(NULL));
                            (void)storage_geo_redis_index_landmark(s_storage, tid, doc_id, embed_vec, embed_dim, name);
                        }
                    } else {
                        fprintf(stderr, "[GEO_LEARNING] insert call failed (ret=%d) for: %s\n", insert_ret, name);
                    }
                }
            } else {
                fprintf(stderr, "[GEO_LEARNING] embedding failed for '%s': ret=%d, dim=%zu\n", name, embed_ret, embed_dim);
                fprintf(stderr, "[GEO_LEARNING]   → Check: Ollama running? Embedding model available? (ollama list)\n");
            }
        }
        if (!end) break;
        seg = end + 3;
    }
    if (entities_found == 0) {
        fprintf(stderr,
                "[GEO_LEARNING] no place entities in extract JSON (non-geo chat is normal): %.80s...\n",
                out);
    } else {
        fprintf(stderr, "[GEO_LEARNING] processed: %d entities found, %d inserted\n", entities_found, entities_inserted);
    }
}

static void *worker_fn(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_mutex);
        while (s_count == 0 && !s_shutdown)
            pthread_cond_wait(&s_cond, &s_mutex);
        if (s_shutdown && s_count == 0) {
            pthread_mutex_unlock(&s_mutex);
            break;
        }
        geo_turn_item_t item = s_queue[s_head];
        s_head = (s_head + 1) % GEO_QUEUE_CAP;
        s_count--;
        pthread_mutex_unlock(&s_mutex);
        process_turn(&item);
    }
    return NULL;
}

static int s_worker_started;

int geo_learning_init(struct storage_ctx *st) {
    if (!st) return -1;
    s_storage = st;
    s_head = s_tail = s_count = 0;
    s_shutdown = 0;
    s_worker_started = 0;
    if (pthread_create(&s_worker, NULL, worker_fn, NULL) != 0) {
        fprintf(stderr, "[GEO_LEARNING] pthread_create failed\n");
        s_storage = NULL;
        return -1;
    }
    s_worker_started = 1;
    fprintf(stderr, "[GEO_LEARNING] worker started\n");
    return 0;
}

void geo_learning_enqueue_turn(const char *tenant_id, const char *user_id,
                               const char *input, const char *assistant,
                               const char *timestamp) {
    if (!input && !assistant) return;
    if (!s_storage) {
        fprintf(stderr, "[GEO_LEARNING] enqueue: s_storage is NULL (worker not initialized?)\n");
        return;
    }
    pthread_mutex_lock(&s_mutex);
    if (s_count >= GEO_QUEUE_CAP) {
        fprintf(stderr, "[GEO_LEARNING] queue full, dropping turn\n");
        pthread_mutex_unlock(&s_mutex);
        return;
    }
    geo_turn_item_t *slot = &s_queue[s_tail];
    strncpy_safe(slot->input, input, GEO_INPUT_SIZE);
    strncpy_safe(slot->assistant, assistant ? assistant : "", GEO_ASSISTANT_SIZE);
    strncpy_safe(slot->tenant_id, tenant_id ? tenant_id : "default", GEO_TENANT_SIZE);
    strncpy_safe(slot->user_id, user_id ? user_id : "default", GEO_TENANT_SIZE);
    strncpy_safe(slot->timestamp, timestamp ? timestamp : "", GEO_TS_SIZE);
    s_tail = (s_tail + 1) % GEO_QUEUE_CAP;
    s_count++;
    fprintf(stderr, "[GEO_LEARNING] enqueued turn (queue: %d/%d)\n", s_count, GEO_QUEUE_CAP);
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mutex);
}

void geo_learning_shutdown(void) {
    if (!s_worker_started) return;
    pthread_mutex_lock(&s_mutex);
    s_shutdown = 1;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mutex);
    pthread_join(s_worker, NULL);
    s_worker_started = 0;
    s_storage = NULL;
}
