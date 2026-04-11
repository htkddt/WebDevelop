/*
 * ELK module — M4 storage.
 * Module rules: .cursor/elk.md (update this header if rules change API or constants).
 */
#ifndef M4_ELK_H
#define M4_ELK_H

#include <stddef.h>

#define ELK_PIPELINE_AUTO_LANG "auto_lang_processor"
#define ELK_DEFAULT_PORT      9200

typedef struct elk_ctx elk_ctx_t;

elk_ctx_t *elk_create(const char *host, int port);
void elk_destroy(elk_ctx_t *ctx);

int elk_initial(elk_ctx_t *ctx);

int elk_set_ingest(elk_ctx_t *ctx, const char *base_url, const char *raw_text);
int elk_set_doc(elk_ctx_t *ctx, const char *tenant_id, const char *doc_id,
               const char *json_body, size_t body_len);

/** Index arbitrary JSON at `index` (no ingest pipeline). doc_id NULL → ES assigns id. */
int elk_index_json(elk_ctx_t *ctx, const char *index, const char *doc_id,
                   const char *json_body, size_t body_len);

/** POST NDJSON body to `/_bulk` (two lines per doc: action + source). */
int elk_bulk_ndjson(elk_ctx_t *ctx, const char *ndjson_body, size_t body_len);

int elk_search(elk_ctx_t *ctx, const char *index, const char *query_json,
               char *out, size_t out_size);

#endif /* M4_ELK_H */
