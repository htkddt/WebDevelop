#ifndef M4_JSON_OPTS_H
#define M4_JSON_OPTS_H

/** Parsed lane entry from JSON. */
typedef struct {
    char *key;
    char *model;
    char *inject;
    char *api_url;
    char *api_key;
} json_lane_t;

/** Parsed JSON options — all heap-allocated, freed by json_opts_free. */
typedef struct {
    int mode;
    int has_mode; /* 1 if mode was present in JSON */
    char *mongo_uri;
    char *redis_host;
    int redis_port;
    char *es_host;
    int es_port;
    char *log_db;
    char *log_coll;
    int context_batch_size;
    int inject_geo_knowledge;
    int disable_auto_system_time;
    int geo_authority;
    int vector_gen_backend;
    char *vector_ollama_model;
    int embed_migration_autostart;
    int session_idle_seconds;
    char *shared_collection_mongo_uri;
    char *shared_collection_json_path;
    char *shared_collection_backfill_db;
    char *learning_terms_path;
    int enable_learning_terms;
    int defer_learning_terms_load;
    char *query_cache_path;
    char *default_persona;
    char *default_instructions;
    int default_model_lane;
    char *geo_authority_csv_path;
    int geo_migrate_legacy;
    int enable_stats;
    int schedule_refresh;
    char **debug_modules;
    int debug_module_count;
    json_lane_t *lanes;
    int lane_count;
} json_opts_t;

/** Parse JSON string into json_opts_t. Returns 0 on success, -1 on error. */
int json_opts_parse(const char *json, json_opts_t *out);

/** Free all heap-allocated fields in json_opts_t. */
void json_opts_free(json_opts_t *o);

#endif /* M4_JSON_OPTS_H */
