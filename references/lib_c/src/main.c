/*
 * M4-Hardcore AI Engine — AI Terminal (ai_bot)
 * Run: ./bin/ai_bot --mode hybrid
 * Boot flow from temp.c: validate_environment -> engine_init -> terminal loop.
 */

#include "engine.h"
#include "embed.h"
#include "smart_topic.h"
#include "validate.h"
#include "terminal_ui.h"
#include "debug_monitor.h"
#include "ollama.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <locale.h>
#include <sys/time.h>

#define DEFAULT_MONGO_URI "mongodb://127.0.0.1:27017/bot?directConnection=true"
#define DEFAULT_REDIS_HOST "127.0.0.1"
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_ES_HOST "127.0.0.1"
#define DEFAULT_ES_PORT 9200

static run_mode_t parse_mode(const char *arg) {
    if (!arg) return MODE_HYBRID;
    if (strcmp(arg, "m4") == 0 || strcmp(arg, "npu") == 0) return MODE_M4_NPU;
    if (strcmp(arg, "cuda") == 0 || strcmp(arg, "remote") == 0) return MODE_CUDA_REMOTE;
    if (strcmp(arg, "hybrid") == 0) return MODE_HYBRID;
    return MODE_HYBRID;
}

static const char *mode_str(run_mode_t m) {
    switch (m) {
        case MODE_M4_NPU:      return "m4_npu";
        case MODE_CUDA_REMOTE: return "cuda_remote";
        case MODE_HYBRID:      return "hybrid";
        default:               return "unknown";
    }
}

/* Fill buf with current time HH:MM:SS.mmm for chat performance. */
static void chat_timestamp(char *buf, size_t size) {
    struct timeval tv;
    struct tm *tm;
    time_t sec;
    gettimeofday(&tv, NULL);
    sec = (time_t)tv.tv_sec;
    tm = localtime(&sec);
    if (tm)
        snprintf(buf, size, "%02d:%02d:%02d.%03ld",
                 tm->tm_hour, tm->tm_min, tm->tm_sec, (long)(tv.tv_usec / 1000));
    else
        snprintf(buf, size, "?");
}

/* Bot response via Ollama; when smart_topic enabled, micro-query for intent and set temperature (TECH 0.0, CHAT 0.8, DEFAULT 0.5). */
static void get_bot_response(const char *user_input, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!user_input) user_input = "";
    out[0] = '\0';
    /* Auto language: ask model to reply in the same language as the user (Vietnamese, English, etc.). */
    char prompt_buf[512];
    snprintf(prompt_buf, sizeof(prompt_buf),
             "Respond in the same language as the user. If the user writes in Vietnamese, reply in Vietnamese; in English, reply in English. Do not switch to another language.\n\nUser: %s",
             user_input);

    double temperature = -1.0;  /* default: no options */
    char st_buf[64];
    if (get_smart_topic(st_buf, sizeof(st_buf)) == 0 && strstr(st_buf, "disabled") == NULL) {
        smart_topic_temperature_for_query(user_input, &temperature);
    }
    /* NULL model: use OLLAMA_MODEL env else OLLAMA_DEFAULT_MODEL */
    int ok = (temperature >= 0.0)
        ? ollama_query_with_options(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, NULL,
                                    prompt_buf, temperature, out, out_size)
        : ollama_query(OLLAMA_DEFAULT_HOST, OLLAMA_DEFAULT_PORT, NULL,
                       prompt_buf, out, out_size);
    if (ok == 0 && out[0] != '\0') {
        size_t len = strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
        return;
    }
    snprintf(out, out_size, "%s is being processed. (Start Ollama for AI replies.)", user_input[0] ? user_input : "(empty)");
}

/* Thread: run get_bot_response and set result + done. */
typedef struct {
    char *user_input;
    char *result;
    size_t result_size;
    volatile int done;
    pthread_mutex_t mtx;
} ollama_worker_t;

static void *ollama_worker_thread(void *arg) {
    ollama_worker_t *w = (ollama_worker_t *)arg;
    get_bot_response(w->user_input, w->result, w->result_size);
    pthread_mutex_lock(&w->mtx);
    w->done = 1;
    pthread_mutex_unlock(&w->mtx);
    return NULL;
}

#define CHAT_HISTORY_MAX 60  /* last 30 chats (user + bot) = 60 lines */
static char s_chat_history[CHAT_HISTORY_MAX][512];
static int s_chat_history_count = 0;

static void chat_history_add(const char *line) {
    if (!line) return;
    size_t len = strlen(line);
    if (len >= sizeof(s_chat_history[0])) len = sizeof(s_chat_history[0]) - 1;
    if (s_chat_history_count < CHAT_HISTORY_MAX) {
        memcpy(s_chat_history[s_chat_history_count], line, len + 1);
        s_chat_history[s_chat_history_count][len] = '\0';
        s_chat_history_count++;
    } else {
        memmove(s_chat_history[0], s_chat_history[1], (CHAT_HISTORY_MAX - 1) * sizeof(s_chat_history[0]));
        memcpy(s_chat_history[CHAT_HISTORY_MAX - 1], line, len + 1);
        s_chat_history[CHAT_HISTORY_MAX - 1][len] = '\0';
    }
}

static void chat_history_redraw(void) {
    const char *ptrs[CHAT_HISTORY_MAX];
    for (int i = 0; i < s_chat_history_count; i++) ptrs[i] = s_chat_history[i];
    terminal_ui_redraw_log(ptrs, s_chat_history_count);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    run_mode_t mode = MODE_HYBRID;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = parse_mode(argv[i + 1]);
            i++;
        }
    }

    smart_topic_options_t smart_topic_opts = {
        .enable = true,
        .library_type = MINI_AI_TYPE_TINY,
        .execution_mode = MODE_MONGO_REDIS_ELK,
        .mini_ai_collection = NULL,
        .model_tiny = NULL,
        .model_b2 = NULL
    };
    engine_config_t config = {
        .mode = mode,
        .execution_mode = MODE_MONGO_REDIS_ELK,  /* A/B/C/D: use MONGO when linked; else ONLY_MEMORY */
        .mongo_uri = DEFAULT_MONGO_URI,
        .redis_host = DEFAULT_REDIS_HOST,
        .redis_port = DEFAULT_REDIS_PORT,
        .es_host = DEFAULT_ES_HOST,
        .es_port = DEFAULT_ES_PORT,
        .batch_size = 1000,
        .vector_search_enabled = 1,
        .debug_mode = true,
        .smart_topic_opts = &smart_topic_opts
    };

    printf("\033[1;34m[SYSTEM]\033[0m Initializing Hardcore AI Engine on M4...\n");

    /* Step 1: validate environment (from temp.c) */
    if (validate_environment(&config) != 0) {
        fprintf(stderr, "Validation failed.\n");
        return 1;
    }

    engine_t *engine = engine_create(&config);
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return 1;
    }
    /* Step 2: init drivers (mongoc_init / redisConnect happen inside storage when wired) */
    if (engine_init(engine) != 0) {
        fprintf(stderr, "Engine init failed (MongoDB/Redis/ES may be unavailable)\n");
        engine_destroy(engine);
        return 1;
    }

    printf("\033[1;32m[READY]\033[0m M4 Engine is listening. Bring on the PDF data!\n");

    debug_monitor_t *debug = debug_monitor_create(DEBUG_PIPE_PATH);
    debug_monitor_start(debug);

    if (terminal_ui_init() != 0) {
        fprintf(stderr, "Ncurses init failed\n");
        debug_monitor_destroy(debug);
        engine_destroy(engine);
        return 1;
    }

    chat_history_add("M4-Hardcore AI Engine - Terminal");
    chat_history_add("Mode: hybrid (M4 NPU + Remote CUDA)");
    chat_history_add("Type a message and Enter to chat; 'q' quit, 's' simulate batch");
    chat_history_redraw();
    terminal_ui_refresh();

    while (terminal_ui_running()) {
        uint64_t processed = 0, errors = 0;
        engine_get_stats(engine, &processed, &errors);
        chat_history_redraw();
        terminal_ui_print_status(mode_str(mode), processed, errors);
        terminal_ui_input_draw();

        int key;
        if (terminal_ui_getch_nonblock(&key)) {
            int consumed = terminal_ui_input_put_key(key);
            if (consumed == 1) {
                terminal_ui_input_draw();
                terminal_ui_refresh();
            } else if (consumed == 2) {
                char line[TERMINAL_UI_INPUT_MAX];
                int len = terminal_ui_input_get_line(line, sizeof(line));
                if (len > 0) {
                    char buf[4096];
                    char tmbuf[32];
                    char user_ts_buf[32];
                    struct timeval t_start, t_end;
                    double elapsed_sec;
                    pthread_t th;
                    ollama_worker_t worker = {
                        .user_input = line,
                        .result = buf,
                        .result_size = sizeof(buf),
                        .done = 0
                    };
                    pthread_mutex_init(&worker.mtx, NULL);

                    chat_timestamp(user_ts_buf, sizeof(user_ts_buf));
                    snprintf(buf, sizeof(buf), "You [%s]: %s", user_ts_buf, line);
                    chat_history_add(buf);
                    terminal_ui_input_clear();
                    chat_history_redraw();
                    terminal_ui_print_status(mode_str(mode), processed, errors);
                    terminal_ui_input_draw();
                    terminal_ui_refresh();

                    gettimeofday(&t_start, NULL);
                    terminal_ui_print_line("Bot: (thinking.)");
                    terminal_ui_refresh();

                    pthread_create(&th, NULL, ollama_worker_thread, &worker);
                    for (int dot = 0; ; dot = (dot + 1) % 3) {
                        pthread_mutex_lock(&worker.mtx);
                        int done = worker.done;
                        pthread_mutex_unlock(&worker.mtx);
                        if (done) break;
                        const char *dots[] = { "Bot: (thinking.)", "Bot: (thinking..)", "Bot: (thinking...)" };
                        terminal_ui_overwrite_last_line(dots[dot]);
                        terminal_ui_refresh();
                        usleep(250000);
                    }
                    pthread_join(th, NULL);
                    pthread_mutex_destroy(&worker.mtx);

                    gettimeofday(&t_end, NULL);
                    elapsed_sec = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_usec - t_start.tv_usec) / 1000000.0;
                    chat_timestamp(tmbuf, sizeof(tmbuf));

                    /* Phase 1: vector + lang before storage — .cursor/lang_vector_phase1.md */
                    float embed_vec[OLLAMA_EMBED_MAX_DIM];
                    size_t embed_dim = 0;
                    char lang_buf[16];
                    double lang_score = 0.0;
                    char embed_model_buf[128];
                    const char *embed_model_arg = NULL;
                    if (m4_embed_for_engine(engine, line, embed_vec, OLLAMA_EMBED_MAX_DIM, &embed_dim, embed_model_buf,
                                            sizeof(embed_model_buf)) == 0
                        && embed_dim > 0)
                        embed_model_arg = embed_model_buf;
                    else
                        embed_dim = 0;
                    (void)lang_detect(line, lang_buf, sizeof(lang_buf), &lang_score);

                    engine_append_turn(engine, "default", "default", line, buf, user_ts_buf,
                                     (embed_dim > 0) ? embed_vec : NULL, embed_dim,
                                     lang_buf[0] ? lang_buf : NULL, lang_score, embed_model_arg, NULL, NULL,
                                     0);
                    fprintf(stderr, "[CHAT] request_ts=%s response_ts=%s duration_sec=%.2f user_len=%zu bot_len=%zu\n",
                            user_ts_buf, tmbuf, elapsed_sec, strlen(line), strlen(buf));

                    /* Add bot response lines to history and redraw */
                    char first_line[600];
                    char *rest = buf;
                    char *nl = strchr(rest, '\n');
                    if (nl) {
                        *nl = '\0';
                        snprintf(first_line, sizeof(first_line), "Bot [%s]: %s  (%.2fs)", tmbuf, rest, elapsed_sec);
                        chat_history_add(first_line);
                        rest = nl + 1;
                        while (*rest && (nl = strchr(rest, '\n')) != NULL) {
                            *nl = '\0';
                            chat_history_add(rest);
                            rest = nl + 1;
                        }
                        if (*rest) chat_history_add(rest);
                    } else {
                        snprintf(first_line, sizeof(first_line), "Bot [%s]: %s  (%.2fs)", tmbuf, buf, elapsed_sec);
                        chat_history_add(first_line);
                    }
                    chat_history_redraw();
                    terminal_ui_refresh();
                }
            } else if (consumed == 0) {
                if (key == 'q' || key == 'Q') {
                    terminal_ui_set_running(false);
                    break;
                }
                if (key == 's' || key == 'S') {
                    char dummy[8] = { 't', 'e', 's', 't' };
                    engine_process_batch(engine, "tenant_1", dummy, 1);
                    chat_history_add("Simulated 1 record (tenant_1)");
                    debug_monitor_log_stats(debug, processed + 1, errors);
                }
            }
        }

        terminal_ui_refresh();
        usleep(100000); /* 100ms */
    }

    terminal_ui_shutdown();
    debug_monitor_destroy(debug);

    uint64_t fin_processed = 0, fin_errors = 0;
    engine_get_stats(engine, &fin_processed, &fin_errors);
    printf("Done. Processed=%lu errors=%lu\n", (unsigned long)fin_processed, (unsigned long)fin_errors);

    engine_destroy(engine);
    return 0;
}
