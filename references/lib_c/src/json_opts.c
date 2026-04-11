/*
 * Minimal JSON parser for api_create options.
 * Handles: strings, ints, doubles, bools, arrays of strings, arrays of objects.
 * Not a general-purpose parser — just enough for api_options_t fields.
 */
#include "json_opts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *skip_ws(const char *p) {
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Parse a JSON string (after opening "). Returns heap-allocated string or NULL. Advances *pp past closing ". */
static char *parse_string(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 256, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    while (*p && *p != '"') {
        if (len + 4 >= cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) return NULL; }
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case '"': out[len++] = '"'; break;
                case '\\': out[len++] = '\\'; break;
                case 'n': out[len++] = '\n'; break;
                case 'r': out[len++] = '\r'; break;
                case 't': out[len++] = '\t'; break;
                default: out[len++] = *p; break;
            }
            p++;
        } else {
            out[len++] = *p++;
        }
    }
    out[len] = '\0';
    if (*p == '"') p++;
    *pp = p;
    return out;
}

static int parse_int(const char **pp) {
    const char *p = *pp;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return neg ? -v : v;
}

static double parse_double(const char **pp) {
    char *end = NULL;
    double v = strtod(*pp, &end);
    if (end) *pp = end;
    return v;
}

/* Skip one JSON value (string, number, object, array, true, false, null). */
static void skip_value(const char **pp) {
    const char *p = skip_ws(*pp);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p == '"') p++;
    } else if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } }
            p++;
        }
    } else if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } }
            p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    }
    *pp = p;
}

/* Parse ["str1","str2",...] into heap-allocated array. */
static char **parse_string_array(const char **pp, int *count_out) {
    const char *p = skip_ws(*pp);
    *count_out = 0;
    if (*p != '[') return NULL;
    p++;
    int cap = 16, n = 0;
    char **arr = (char **)calloc((size_t)cap, sizeof(char *));
    if (!arr) return NULL;
    while (*p) {
        p = skip_ws(p);
        if (*p == ']') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (*p == '"') {
            char *s = parse_string(&p);
            if (s) {
                if (n >= cap) { cap *= 2; arr = (char **)realloc(arr, (size_t)cap * sizeof(char *)); }
                arr[n++] = s;
            }
        } else {
            skip_value(&p);
        }
    }
    *pp = p;
    *count_out = n;
    return arr;
}

int json_opts_parse(const char *json, json_opts_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    if (!json || !json[0]) return 0; /* empty = all defaults */
    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') { p++; continue; }

        char *key = parse_string(&p);
        if (!key) break;
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);

        /* Match known keys */
        if (strcmp(key, "mode") == 0) { out->mode = parse_int(&p); out->has_mode = 1; }
        else if (strcmp(key, "mongo_uri") == 0) { out->mongo_uri = parse_string(&p); }
        else if (strcmp(key, "redis_host") == 0) { out->redis_host = parse_string(&p); }
        else if (strcmp(key, "redis_port") == 0) { out->redis_port = parse_int(&p); }
        else if (strcmp(key, "es_host") == 0) { out->es_host = parse_string(&p); }
        else if (strcmp(key, "es_port") == 0) { out->es_port = parse_int(&p); }
        else if (strcmp(key, "log_db") == 0) { out->log_db = parse_string(&p); }
        else if (strcmp(key, "log_coll") == 0) { out->log_coll = parse_string(&p); }
        else if (strcmp(key, "context_batch_size") == 0) { out->context_batch_size = parse_int(&p); }
        else if (strcmp(key, "inject_geo_knowledge") == 0) { out->inject_geo_knowledge = parse_int(&p); }
        else if (strcmp(key, "disable_auto_system_time") == 0) { out->disable_auto_system_time = parse_int(&p); }
        else if (strcmp(key, "geo_authority") == 0) { out->geo_authority = parse_int(&p); }
        else if (strcmp(key, "vector_gen_backend") == 0) { out->vector_gen_backend = parse_int(&p); }
        else if (strcmp(key, "vector_ollama_model") == 0) { out->vector_ollama_model = parse_string(&p); }
        else if (strcmp(key, "embed_migration_autostart") == 0) { out->embed_migration_autostart = parse_int(&p); }
        else if (strcmp(key, "session_idle_seconds") == 0) { out->session_idle_seconds = parse_int(&p); }
        else if (strcmp(key, "shared_collection_mongo_uri") == 0) { out->shared_collection_mongo_uri = parse_string(&p); }
        else if (strcmp(key, "shared_collection_json_path") == 0) { out->shared_collection_json_path = parse_string(&p); }
        else if (strcmp(key, "shared_collection_backfill_db") == 0) { out->shared_collection_backfill_db = parse_string(&p); }
        else if (strcmp(key, "learning_terms_path") == 0) { out->learning_terms_path = parse_string(&p); }
        else if (strcmp(key, "enable_learning_terms") == 0) { out->enable_learning_terms = parse_int(&p); }
        else if (strcmp(key, "defer_learning_terms_load") == 0) { out->defer_learning_terms_load = parse_int(&p); }
        else if (strcmp(key, "query_cache_path") == 0) { out->query_cache_path = parse_string(&p); }
        else if (strcmp(key, "default_persona") == 0) { out->default_persona = parse_string(&p); }
        else if (strcmp(key, "default_instructions") == 0) { out->default_instructions = parse_string(&p); }
        else if (strcmp(key, "default_model_lane") == 0) { out->default_model_lane = parse_int(&p); }
        else if (strcmp(key, "geo_authority_csv_path") == 0) { out->geo_authority_csv_path = parse_string(&p); }
        else if (strcmp(key, "geo_migrate_legacy") == 0) { out->geo_migrate_legacy = parse_int(&p); }
        else if (strcmp(key, "enable_stats") == 0) { out->enable_stats = parse_int(&p); }
        else if (strcmp(key, "schedule_refresh") == 0) { out->schedule_refresh = parse_int(&p); }
        else if (strcmp(key, "debug_modules") == 0) {
            out->debug_modules = parse_string_array(&p, &out->debug_module_count);
        }
        else if (strcmp(key, "lanes") == 0) {
            /* Parse array of lane objects: [{"key":"...","model":"...","inject":"...","api_url":"...","api_key":"..."}] */
            p = skip_ws(p);
            if (*p == '[') {
                p++;
                int cap = 16;
                out->lanes = (json_lane_t *)calloc((size_t)cap, sizeof(json_lane_t));
                out->lane_count = 0;
                while (*p) {
                    p = skip_ws(p);
                    if (*p == ']') { p++; break; }
                    if (*p == ',') { p++; continue; }
                    if (*p == '{') {
                        p++;
                        if (out->lane_count >= cap) {
                            cap *= 2;
                            out->lanes = (json_lane_t *)realloc(out->lanes, (size_t)cap * sizeof(json_lane_t));
                        }
                        json_lane_t *lane = &out->lanes[out->lane_count];
                        memset(lane, 0, sizeof(*lane));
                        while (*p) {
                            p = skip_ws(p);
                            if (*p == '}') { p++; break; }
                            if (*p == ',') { p++; continue; }
                            char *lk = parse_string(&p);
                            if (!lk) break;
                            p = skip_ws(p);
                            if (*p == ':') p++;
                            p = skip_ws(p);
                            if (strcmp(lk, "key") == 0) lane->key = parse_string(&p);
                            else if (strcmp(lk, "model") == 0) lane->model = parse_string(&p);
                            else if (strcmp(lk, "inject") == 0) lane->inject = parse_string(&p);
                            else if (strcmp(lk, "api_url") == 0) lane->api_url = parse_string(&p);
                            else if (strcmp(lk, "api_key") == 0) lane->api_key = parse_string(&p);
                            else skip_value(&p);
                            free(lk);
                        }
                        out->lane_count++;
                    } else {
                        skip_value(&p);
                    }
                }
            } else {
                skip_value(&p);
            }
        }
        else {
            /* Unknown key — skip value */
            skip_value(&p);
        }
        free(key);
    }
    return 0;
}

void json_opts_free(json_opts_t *o) {
    if (!o) return;
    free(o->mongo_uri);
    free(o->redis_host);
    free(o->es_host);
    free(o->log_db);
    free(o->log_coll);
    free(o->vector_ollama_model);
    free(o->shared_collection_mongo_uri);
    free(o->shared_collection_json_path);
    free(o->shared_collection_backfill_db);
    free(o->learning_terms_path);
    free(o->default_persona);
    free(o->default_instructions);
    free(o->geo_authority_csv_path);
    if (o->debug_modules) {
        for (int i = 0; i < o->debug_module_count; i++) free(o->debug_modules[i]);
        free(o->debug_modules);
    }
    if (o->lanes) {
        for (int i = 0; i < o->lane_count; i++) {
            free(o->lanes[i].key);
            free(o->lanes[i].model);
            free(o->lanes[i].inject);
            free(o->lanes[i].api_url);
            free(o->lanes[i].api_key);
        }
        free(o->lanes);
    }
    memset(o, 0, sizeof(*o));
}
