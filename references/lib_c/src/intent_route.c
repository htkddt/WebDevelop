/*
 * Intent routing — Phase 1 classify using NL learning scores + SharedCollection vocab.
 * Design: .cursor/intent_routing.md
 */
#include "intent_route.h"
#include "intent_learn.h"
#include "debug_log.h"
#include "vector_generate.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ir_lower(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r' || c == '\t') continue;
        if (c == '\n') { dst[j++] = ' '; continue; }
        dst[j++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    dst[j] = '\0';
    /* collapse spaces */
    size_t w = 0;
    int sp = 1;
    for (size_t i = 0; dst[i]; i++) {
        if (dst[i] == ' ') { if (!sp) { dst[w++] = ' '; sp = 1; } }
        else { dst[w++] = dst[i]; sp = 0; }
    }
    if (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
}

/** Score intent by scanning ALL learned terms against the normalized text.
 *  No hardcoded phrases — uses whatever nl_learn_cues has recorded. */
static int64_t ir_score_intent(nl_learn_terms_t *lt, const char *norm, const char *intent) {
    if (!lt || !norm || !norm[0]) return 0;
    return nl_learn_terms_score_text(lt, norm, intent);
}

/* Callback for sc_registry_foreach_elk — check SC:{col} score for each collection. */
typedef struct {
    nl_learn_terms_t *lt;
    const char *norm;
    char best_col[160];
    int64_t best_score;
} ir_sc_foreach_t;

static void ir_sc_foreach_cb(const char *collection, const char *elk_index, void *user) {
    (void)elk_index;
    ir_sc_foreach_t *f = (ir_sc_foreach_t *)user;
    char sc_intent[192];
    snprintf(sc_intent, sizeof(sc_intent), "SC:%s", collection);
    int64_t sc = nl_learn_terms_score_text(f->lt, f->norm, sc_intent);
    if (sc > f->best_score) {
        f->best_score = sc;
        snprintf(f->best_col, sizeof(f->best_col), "%s", collection);
    }
}

/** Find best collection match using: (1) vocab, (2) synonym, (3) learned SC:* scores.
 *  Learned scores are the primary signal — they capture what the LLM background worker discovered. */
static void ir_resolve_collection(nl_learn_terms_t *lt, const sc_term_vocab_t *vocab,
                                  const sc_registry_t *registry,
                                  const char *norm, intent_route_result_t *out) {
    out->collection[0] = '\0';
    out->field[0] = '\0';
    out->collection_score = 0;

    char seen_cols[8][160];
    int64_t seen_scores[8];
    char seen_fields[8][64];
    int nseen = 0;

    /* Step 1: Vocab + synonym lookup (immediate, from config). */
    if (vocab) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", norm);
        char *save = NULL;
        char *tok = strtok_r(buf, " ,?!.;:\"'()/", &save);
        while (tok) {
            if (strlen(tok) >= 2) {
                const char *col = NULL, *field = NULL;
                if (sc_term_vocab_lookup(vocab, tok, &col, &field) == 0 && col) {
                    int found = -1;
                    for (int i = 0; i < nseen; i++)
                        if (strcmp(seen_cols[i], col) == 0) { found = i; break; }
                    if (found < 0 && nseen < 8) {
                        found = nseen;
                        snprintf(seen_cols[nseen], sizeof(seen_cols[0]), "%s", col);
                        seen_fields[nseen][0] = '\0';
                        seen_scores[nseen] = 1; /* base score from vocab hit */
                        nseen++;
                    } else if (found >= 0) {
                        seen_scores[found]++;
                    }
                    if (found >= 0 && field && field[0] && !seen_fields[found][0])
                        snprintf(seen_fields[found], sizeof(seen_fields[0]), "%s", field);
                } else {
                    m4_synonym_table_t *syn = m4_synonym_get_global();
                    if (syn) {
                        const char *canonical = m4_synonym_lookup(syn, tok);
                        if (canonical && sc_term_vocab_lookup(vocab, canonical, &col, &field) == 0 && col) {
                            int found = -1;
                            for (int i = 0; i < nseen; i++)
                                if (strcmp(seen_cols[i], col) == 0) { found = i; break; }
                            if (found < 0 && nseen < 8) {
                                found = nseen;
                                snprintf(seen_cols[nseen], sizeof(seen_cols[0]), "%s", col);
                                seen_fields[nseen][0] = '\0';
                                seen_scores[nseen] = 1;
                                nseen++;
                            } else if (found >= 0)
                                seen_scores[found]++;
                        }
                    }
                }
            }
            tok = strtok_r(NULL, " ,?!.;:\"'()/", &save);
        }
    }

    /* Step 2: Scan learned SC:* scores for EVERY elk.allow collection.
     * score_text(norm, "SC:carts") checks if any stored term (e.g. "orders", "sold")
     * appears in norm AND has intent "SC:carts". This catches what vocab missed. */
    if (lt && registry) {
        ir_sc_foreach_t best_learned = { .lt = lt, .norm = norm, .best_col = {0}, .best_score = 0 };
        sc_registry_foreach_elk(registry, ir_sc_foreach_cb, &best_learned);
        if (best_learned.best_score > 0) {
            /* Add or boost this collection in seen_cols. */
            int found = -1;
            for (int i = 0; i < nseen; i++)
                if (strcmp(seen_cols[i], best_learned.best_col) == 0) { found = i; break; }
            if (found < 0 && nseen < 8) {
                found = nseen;
                snprintf(seen_cols[nseen], sizeof(seen_cols[0]), "%s", best_learned.best_col);
                seen_fields[nseen][0] = '\0';
                seen_scores[nseen] = best_learned.best_score;
                nseen++;
            } else if (found >= 0) {
                seen_scores[found] += best_learned.best_score;
            }
        }
    }

    /* Pick best. */
    int best = -1;
    int64_t best_score = 0;
    for (int i = 0; i < nseen; i++) {
        if (seen_scores[i] > best_score) {
            best_score = seen_scores[i];
            best = i;
        }
    }
    if (best >= 0) {
        snprintf(out->collection, sizeof(out->collection), "%s", seen_cols[best]);
        snprintf(out->field, sizeof(out->field), "%s", seen_fields[best]);
        out->collection_score = best_score;
    }
}

int intent_route_classify(const char *user_msg,
                          nl_learn_terms_t *lt,
                          const sc_term_vocab_t *vocab,
                          const sc_registry_t *registry,
                          int64_t min_score_threshold,
                          intent_route_result_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->intent = INTENT_ROUTE_CHAT;

    if (!user_msg || !user_msg[0]) return 0;

    char norm[4096];
    ir_lower(norm, sizeof(norm), user_msg);
    if (!norm[0]) return 0;

    /* Score each intent using phrase matching + word-level score_sum. */
    int64_t s_analytics = ir_score_intent(lt, norm, "ELK_ANALYTICS");
    int64_t s_search    = ir_score_intent(lt, norm, "ELK_SEARCH");
    int64_t s_rag       = ir_score_intent(lt, norm, "RAG_VECTOR");
    int64_t s_chat      = ir_score_intent(lt, norm, "CHAT");

    /* Pick highest non-CHAT intent. */
    intent_route_t best_intent = INTENT_ROUTE_CHAT;
    int64_t best_score = 0;

    if (s_analytics > best_score) { best_score = s_analytics; best_intent = INTENT_ROUTE_ELK_ANALYTICS; }
    if (s_search > best_score)    { best_score = s_search;    best_intent = INTENT_ROUTE_ELK_SEARCH; }
    if (s_rag > best_score)       { best_score = s_rag;       best_intent = INTENT_ROUTE_RAG_VECTOR; }

    /* Only route if score exceeds threshold. */
    if (best_score >= min_score_threshold) {
        out->intent = best_intent;
        out->intent_score = best_score;
    } else {
        out->intent = INTENT_ROUTE_CHAT;
        out->intent_score = s_chat;
    }

    /* Resolve collection from vocab + learned SC: scores. */
    ir_resolve_collection(lt, vocab, registry, norm, out);

    m4_log("INTENT_ROUTE", M4_LOG_DEBUG,
           "classify: intent=%s score=%lld collection=%s field=%s "
           "(analytics=%lld search=%lld rag=%lld chat=%lld threshold=%lld)",
           intent_route_label(out->intent), (long long)out->intent_score,
           out->collection[0] ? out->collection : "(none)",
           out->field[0] ? out->field : "(none)",
           (long long)s_analytics, (long long)s_search, (long long)s_rag, (long long)s_chat,
           (long long)min_score_threshold);

    return 0;
}

const char *intent_route_label(intent_route_t intent) {
    switch (intent) {
        case INTENT_ROUTE_ELK_ANALYTICS: return "ELK_ANALYTICS";
        case INTENT_ROUTE_ELK_SEARCH:    return "ELK_SEARCH";
        case INTENT_ROUTE_RAG_VECTOR:    return "RAG_VECTOR";
        case INTENT_ROUTE_CHAT:          return "CHAT";
        default:                         return "CHAT";
    }
}

/* ========== Phase 3: EXECUTE — build ELK query + run ========== */

#include "storage.h"

/** Escape a string for JSON value (minimal: quotes and backslashes). */
static size_t ir_json_escape(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < cap; i++) {
        if (src[i] == '"' || src[i] == '\\') dst[j++] = '\\';
        dst[j++] = src[i];
    }
    dst[j] = '\0';
    return j;
}

/* ---------- Filter validation ---------- */

int intent_route_validate_field(const char *collection, const char *field,
                                const sc_registry_t *registry,
                                const sc_term_vocab_t *vocab) {
    if (!field || !field[0]) return 0;

    /* Check 1: SharedCollection field_hints. */
    if (registry && collection && sc_registry_has_field(registry, collection, field))
        return 1;

    /* Check 2: vocab table — if the field name was parsed as a vocab entry. */
    if (vocab) {
        const char *vcol = NULL, *vfield = NULL;
        if (sc_term_vocab_lookup(vocab, field, &vcol, &vfield) == 0 && vfield)
            return 1;
    }

    /* Check 3: common ELK fields always valid. */
    if (strcmp(field, "@timestamp") == 0 || strcmp(field, "_id") == 0 ||
        strcmp(field, "created_at") == 0 || strcmp(field, "updated_at") == 0 ||
        strcmp(field, "createdAt") == 0 || strcmp(field, "updatedAt") == 0)
        return 1;

    return 0;
}

/** Validate and filter a cached filter array. Removes filters with unknown fields.
 *  Writes validated filters back to out. Returns number of valid filters. */
static int ir_validate_filters(const char *filters_json, const char *collection,
                                const sc_registry_t *registry, const sc_term_vocab_t *vocab,
                                char *out, size_t out_cap) {
    if (!filters_json || filters_json[0] != '[' || strcmp(filters_json, "[]") == 0) return 0;

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_cap - pos, "[");
    int valid_count = 0;

    const char *p = filters_json + 1;
    while (*p) {
        while (*p && *p != '{') { if (*p == ']') goto done; p++; }
        if (*p != '{') break;

        /* Save start of this filter object. */
        const char *obj_start = p;
        int depth = 1; p++;
        while (*p && depth > 0) { if (*p == '{') depth++; else if (*p == '}') depth--; p++; }
        size_t obj_len = (size_t)(p - obj_start);

        /* Extract field name from this object. */
        char field[128] = {0};
        char *ff = strstr((char*)obj_start, "\"field\"");
        if (ff) {
            ff = strchr(ff + 7, '"');
            if (ff) {
                ff++;
                size_t i = 0;
                while (*ff && *ff != '"' && i + 1 < sizeof(field)) field[i++] = *ff++;
                field[i] = '\0';
            }
        }

        /* Validate field. */
        if (field[0] && intent_route_validate_field(collection, field, registry, vocab)) {
            if (valid_count > 0 && pos + 1 < out_cap) out[pos++] = ',';
            if (pos + obj_len < out_cap) {
                memcpy(out + pos, obj_start, obj_len);
                pos += obj_len;
            }
            valid_count++;
            m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "validate: field '%s' OK for %s", field, collection);
        } else if (field[0]) {
            m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "validate: field '%s' UNKNOWN for %s — dropped", field, collection);
        }
    }

done:
    pos += (size_t)snprintf(out + pos, out_cap - pos, "]");
    return valid_count;
}

/** Build ELK bool query from cached filters JSON: [{"field":"x","op":"eq","value":"y"}, ...] */
static int ir_build_query_from_filters(const char *filters_json, const char *operation,
                                        char *query, size_t qcap) {
    if (!filters_json || filters_json[0] != '[' || strcmp(filters_json, "[]") == 0) return -1;

    /* Parse filter array and build bool.must clauses. */
    size_t pos = 0;
    pos += (size_t)snprintf(query + pos, qcap - pos,
                            "{\"size\":%s,\"query\":{\"bool\":{\"must\":[",
                            (operation && strcmp(operation, "list") == 0) ? "5" : "0");

    const char *p = filters_json + 1; /* skip [ */
    int first = 1;
    while (*p) {
        /* Find next { */
        while (*p && *p != '{') { if (*p == ']') goto done; p++; }
        if (*p != '{') break;

        char field[128] = {0}, op[16] = {0}, value[256] = {0};
        /* Minimal parse of {"field":"...","op":"...","value":"..."} */
        const char *obj = p;
        int depth = 1; p++;
        while (*p && depth > 0) { if (*p == '{') depth++; else if (*p == '}') depth--; p++; }

        /* Extract fields from this object. */
        char *ff = strstr((char*)obj, "\"field\"");
        char *fo = strstr((char*)obj, "\"op\"");
        char *fv = strstr((char*)obj, "\"value\"");
        if (!ff || !fo || !fv) continue;

        /* field */
        ff = strchr(ff + 7, '"'); if (!ff) continue; ff++;
        { size_t i = 0; while (*ff && *ff != '"' && i + 1 < sizeof(field)) field[i++] = *ff++; field[i] = '\0'; }
        /* op */
        fo = strchr(fo + 4, '"'); if (!fo) continue; fo++;
        { size_t i = 0; while (*fo && *fo != '"' && i + 1 < sizeof(op)) op[i++] = *fo++; op[i] = '\0'; }
        /* value */
        fv = strchr(fv + 7, '"'); if (!fv) continue; fv++;
        { size_t i = 0; while (*fv && *fv != '"' && i + 1 < sizeof(value)) value[i++] = *fv++; value[i] = '\0'; }

        if (!field[0] || !value[0]) continue;
        if (!first && pos + 1 < qcap) query[pos++] = ',';
        first = 0;

        char ef[128], ev[256];
        ir_json_escape(ef, sizeof(ef), field);
        ir_json_escape(ev, sizeof(ev), value);

        if (strcmp(op, "eq") == 0)
            pos += (size_t)snprintf(query + pos, qcap - pos, "{\"term\":{\"%s\":\"%s\"}}", ef, ev);
        else if (strcmp(op, "gte") == 0 || strcmp(op, "gt") == 0 ||
                 strcmp(op, "lte") == 0 || strcmp(op, "lt") == 0)
            pos += (size_t)snprintf(query + pos, qcap - pos,
                                    "{\"range\":{\"%s\":{\"%s\":\"%s\"}}}", ef, op, ev);
        else if (strcmp(op, "contains") == 0)
            pos += (size_t)snprintf(query + pos, qcap - pos, "{\"match\":{\"%s\":\"%s\"}}", ef, ev);
        else
            pos += (size_t)snprintf(query + pos, qcap - pos, "{\"term\":{\"%s\":\"%s\"}}", ef, ev);
    }
done:
    pos += (size_t)snprintf(query + pos, qcap - pos, "]}}}");
    if (operation && strcmp(operation, "list") == 0)
        pos += (size_t)snprintf(query + pos - 1, qcap - pos + 1, ",\"_source\":true}") - 1;
    return first ? -1 : 0; /* -1 if no filters were added */
}

/** Build ELK_ANALYTICS query. Tries cached query plan first, falls back to match_all. */
static int ir_build_analytics_query(const intent_route_result_t *classify,
                                    const char *user_msg,
                                    const sc_registry_t *registry,
                                    const sc_term_vocab_t *vocab,
                                    char *query, size_t qcap) {
    /* Try cached query plan from background Phase 2 learning. */
    char cached_col[160], cached_op[32], cached_filters[2048];
    if (intent_learn_cache_lookup(user_msg, cached_col, sizeof(cached_col),
                                   cached_op, sizeof(cached_op),
                                   cached_filters, sizeof(cached_filters)) == 0) {
        /* Validate filters against known fields before using. */
        const char *col = classify->collection[0] ? classify->collection : cached_col;
        char valid_filters[2048];
        int nvalid = ir_validate_filters(cached_filters, col, registry, vocab,
                                          valid_filters, sizeof(valid_filters));
        if (nvalid > 0 && ir_build_query_from_filters(valid_filters, cached_op, query, qcap) == 0) {
            m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "Phase 3: using %d validated filters for analytics", nvalid);
            return 0;
        }
    }
    /* Fallback: count all in the resolved collection. */
    snprintf(query, qcap, "{\"size\":0,\"query\":{\"match_all\":{}}}");
    return 0;
}

/** Build ELK_SEARCH query. Validates cached filters, falls back to match_all. */
static int ir_build_search_query(const char *user_msg, const char *collection,
                                  const sc_registry_t *registry,
                                  const sc_term_vocab_t *vocab,
                                  char *query, size_t qcap) {
    char cached_col[160], cached_op[32], cached_filters[2048];
    if (intent_learn_cache_lookup(user_msg, cached_col, sizeof(cached_col),
                                   cached_op, sizeof(cached_op),
                                   cached_filters, sizeof(cached_filters)) == 0) {
        const char *col = (collection && collection[0]) ? collection : cached_col;
        char valid_filters[2048];
        int nvalid = ir_validate_filters(cached_filters, col, registry, vocab,
                                          valid_filters, sizeof(valid_filters));
        if (nvalid > 0 && ir_build_query_from_filters(valid_filters, "list", query, qcap) == 0) {
            m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "Phase 3: using %d validated filters for search", nvalid);
            return 0;
        }
    }
    snprintf(query, qcap, "{\"size\":5,\"query\":{\"match_all\":{}},\"_source\":true}");
    return 0;
}

/** Parse total hits from ELK response JSON. Handles both {"hits":{"total":{"value":N}}} and {"count":N}. */
static long long ir_parse_hit_count(const char *json) {
    /* Try "count": N (from _count endpoint) */
    const char *cp = strstr(json, "\"count\"");
    if (cp) {
        cp += 7;
        while (*cp == ' ' || *cp == ':') cp++;
        return strtoll(cp, NULL, 10);
    }
    /* Try "total":{"value": N} */
    const char *tp = strstr(json, "\"total\"");
    if (tp) {
        const char *vp = strstr(tp, "\"value\"");
        if (vp) {
            vp += 7;
            while (*vp == ' ' || *vp == ':') vp++;
            return strtoll(vp, NULL, 10);
        }
        /* total might be a plain number (ES 6.x) */
        tp += 7;
        while (*tp == ' ' || *tp == ':') tp++;
        if (*tp >= '0' && *tp <= '9')
            return strtoll(tp, NULL, 10);
    }
    return -1;
}

/** Extract first N source snippets from _search response for display. */
static size_t ir_parse_hit_snippets(const char *json, char *out, size_t out_cap) {
    out[0] = '\0';
    size_t written = 0;
    /* Find "hits":{"total":...,"hits":[ and extract _source objects */
    const char *hits = strstr(json, "\"hits\":[");
    if (!hits) return 0;
    hits += 8;

    int doc_count = 0;
    const char *p = hits;
    while (*p && doc_count < 3) {
        const char *src = strstr(p, "\"_source\":");
        if (!src) break;
        src += 10;
        /* Find the end of the _source object */
        if (*src == '{') {
            int depth = 1;
            const char *start = src;
            src++;
            while (*src && depth > 0) {
                if (*src == '{') depth++;
                else if (*src == '}') depth--;
                src++;
            }
            size_t obj_len = (size_t)(src - start);
            if (obj_len > 500) obj_len = 500; /* cap per doc */
            if (written + obj_len + 2 < out_cap) {
                if (written > 0) out[written++] = '\n';
                memcpy(out + written, start, obj_len);
                written += obj_len;
                out[written] = '\0';
            }
            doc_count++;
        }
        p = src;
    }
    return written;
}

int intent_route_execute(const intent_route_result_t *classify,
                         const char *user_msg,
                         const sc_registry_t *registry,
                         struct storage_ctx *storage,
                         intent_route_elk_result_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!classify || !user_msg || !registry || !storage) return -1;
    if (classify->intent == INTENT_ROUTE_CHAT || classify->intent == INTENT_ROUTE_RAG_VECTOR)
        return -1;
    /* Determine collection: prefer cache lookup if it found a valid collection. */
    char use_collection[160];
    snprintf(use_collection, sizeof(use_collection), "%s", classify->collection);

    /* Check if the query plan cache has a better collection (learned from LLM). */
    {
        char cached_col[160] = {0}, cached_op[32] = {0}, cached_filters[2048] = {0};
        if (intent_learn_cache_lookup(user_msg, cached_col, sizeof(cached_col),
                                       cached_op, sizeof(cached_op),
                                       cached_filters, sizeof(cached_filters)) == 0
            && cached_col[0]
            && sc_registry_elk_allowed(registry, cached_col)) {
            /* Cache found a valid collection — prefer it over vocab. */
            if (strcmp(cached_col, use_collection) != 0) {
                m4_log("INTENT_ROUTE", M4_LOG_DEBUG,
                       "execute: cache overrides collection %s → %s",
                       use_collection[0] ? use_collection : "(none)", cached_col);
                snprintf(use_collection, sizeof(use_collection), "%s", cached_col);
            }
        }
    }

    if (!use_collection[0]) {
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "execute: no collection resolved, skipping ELK");
        return -1;
    }

    /* Resolve ELK index from collection name. */
    if (sc_registry_elk_index(registry, use_collection, out->elk_index, sizeof(out->elk_index)) != 0) {
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "execute: collection '%s' not in ELK registry", use_collection);
        return -1;
    }
    if (!sc_registry_elk_allowed(registry, use_collection)) {
        m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "execute: collection '%s' elk.allow=false", use_collection);
        return -1;
    }

    /* Build query based on intent. Validate cached filters against known fields. */
    /* Create a modified classify with the resolved collection for the query builders. */
    intent_route_result_t resolved = *classify;
    snprintf(resolved.collection, sizeof(resolved.collection), "%s", use_collection);

    if (classify->intent == INTENT_ROUTE_ELK_ANALYTICS)
        ir_build_analytics_query(&resolved, user_msg, registry, NULL,
                                  out->elk_query, sizeof(out->elk_query));
    else
        ir_build_search_query(user_msg, use_collection, registry, NULL,
                               out->elk_query, sizeof(out->elk_query));

    m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "execute: index=%s query=%s", out->elk_index, out->elk_query);

    /* Execute. */
    if (storage_elk_search(storage, out->elk_index, out->elk_query,
                           out->elk_response, sizeof(out->elk_response)) != 0) {
        m4_log("INTENT_ROUTE", M4_LOG_WARN, "execute: elk_search failed index=%s", out->elk_index);
        return -1;
    }
    out->executed = 1;

    /* Parse results. */
    out->result_count = ir_parse_hit_count(out->elk_response);

    if (classify->intent == INTENT_ROUTE_ELK_SEARCH)
        ir_parse_hit_snippets(out->elk_response, out->result_snippet, sizeof(out->result_snippet));

    m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "execute: ok count=%lld snippet_len=%zu",
           out->result_count, strlen(out->result_snippet));

    return 0;
}

/* ========== Phase 4: FORMAT — inject [DATA_RESULT] into prompt ========== */

size_t intent_route_format_data_result(const intent_route_result_t *classify,
                                       const intent_route_elk_result_t *elk,
                                       const char *user_msg,
                                       char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!classify || !elk || !elk->executed) return 0;

    char escaped_q[512];
    ir_json_escape(escaped_q, sizeof(escaped_q), user_msg ? user_msg : "");

    size_t n = 0;
    if (classify->intent == INTENT_ROUTE_ELK_ANALYTICS) {
        n = (size_t)snprintf(out, out_size,
            "[DATA_RESULT] {\"question\":\"%s\",\"intent\":\"analytics\","
            "\"collection\":\"%s\",\"index\":\"%s\",\"result_count\":%lld}\n"
            "Answer the user's question using the data result above. "
            "Be concise and use the exact number from the result.\n\n",
            escaped_q, classify->collection, elk->elk_index, elk->result_count);
    } else if (classify->intent == INTENT_ROUTE_ELK_SEARCH) {
        if (elk->result_snippet[0]) {
            n = (size_t)snprintf(out, out_size,
                "[DATA_RESULT] {\"question\":\"%s\",\"intent\":\"search\","
                "\"collection\":\"%s\",\"index\":\"%s\",\"total\":%lld,"
                "\"top_hits\":%s}\n"
                "Answer the user's question using the search results above. "
                "Summarize the relevant items found.\n\n",
                escaped_q, classify->collection, elk->elk_index,
                elk->result_count,
                elk->result_snippet[0] == '{' ? elk->result_snippet : "[]");
        } else {
            n = (size_t)snprintf(out, out_size,
                "[DATA_RESULT] {\"question\":\"%s\",\"intent\":\"search\","
                "\"collection\":\"%s\",\"index\":\"%s\",\"total\":%lld}\n"
                "Answer the user's question. %lld results were found.\n\n",
                escaped_q, classify->collection, elk->elk_index,
                elk->result_count, elk->result_count);
        }
    }
    if (n >= out_size) n = out_size - 1;

    m4_log("INTENT_ROUTE", M4_LOG_DEBUG, "format: injected %zu bytes for %s",
           n, intent_route_label(classify->intent));

    return n;
}
