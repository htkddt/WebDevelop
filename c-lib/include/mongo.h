/*
 * Mongo module — M4 storage.
 * Module rules: .cursor/mongo.md (update this header if rules change API or constants).
 */
#ifndef M4_MONGO_H
#define M4_MONGO_H

#include <stddef.h>
#include <stdint.h>

#define MONGO_DB_NAME        "m4_ai"
#define MONGO_COLLECTION     "records"
#define MONGO_CHAT_DB        "bot"
#define MONGO_CHAT_COLLECTION "records"
#define MONGO_AI_LOGS_DB     "bot"
#define MONGO_AI_LOGS_COLLECTION "ai_logs"
#define MONGO_NAME_MAX       63

typedef struct mongo_ctx mongo_ctx_t;

mongo_ctx_t *mongo_create(const char *uri);
void mongo_destroy(mongo_ctx_t *ctx);

int mongo_initial(mongo_ctx_t *ctx);
void mongo_disconnect(mongo_ctx_t *ctx);
int mongo_connected(mongo_ctx_t *ctx);

int mongo_set_batch(mongo_ctx_t *ctx, const char *tenant_id, const void *records, size_t count);
int mongo_set_chat(mongo_ctx_t *ctx, const char *tenant_id, const char *role,
                   const char *content, const char *timestamp);
int mongo_set_ai_log(mongo_ctx_t *ctx, const char *tenant_id, const char *level, const char *message);
int mongo_set_ai_logs_collection(mongo_ctx_t *ctx, const char *db, const char *coll);

int mongo_search_vector(mongo_ctx_t *ctx, const char *tenant_id, const float *vector,
                        size_t dim, size_t k, void *out_ids, size_t *out_count);
typedef void (*mongo_chat_history_cb)(const char *role, const char *content, void *userdata);
int mongo_search_chat_history(mongo_ctx_t *ctx, const char *tenant_id, int limit,
                              mongo_chat_history_cb callback, void *userdata);

#endif /* M4_MONGO_H */
