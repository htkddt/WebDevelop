#ifndef M4_TERMINAL_UI_H
#define M4_TERMINAL_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERMINAL_UI_INPUT_MAX 256

/* Ncurses terminal UI for AI bot. */
int terminal_ui_init(void);
void terminal_ui_shutdown(void);

void terminal_ui_refresh(void);
void terminal_ui_print_status(const char *mode, uint64_t processed, uint64_t errors);
void terminal_ui_print_line(const char *line);
void terminal_ui_overwrite_last_line(const char *line);  /* update last printed line (e.g. thinking . .. ...) */
/* Redraw entire log area from history (lines[0..n-1]); use for last 30 chats. */
void terminal_ui_redraw_log(const char **lines, int n);
void terminal_ui_print_error(const char *msg);

/* Chat input at bottom: feed key, draw line, get submitted line on Enter. */
int terminal_ui_input_put_key(int key);  /* 2=submitted, 1=consumed, 0=not consumed */
void terminal_ui_input_draw(void);
int terminal_ui_input_get_line(char *out, size_t size);  /* returns length, clears buffer */
void terminal_ui_input_clear(void);

int terminal_ui_getch_nonblock(int *key);
bool terminal_ui_running(void);
void terminal_ui_set_running(bool running);

#endif /* M4_TERMINAL_UI_H */
