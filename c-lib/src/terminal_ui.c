#include "terminal_ui.h"
#include <ncurses.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static bool s_running = true;
static int s_status_row = 0;
static int s_log_start_row = 2;
static int s_log_row = 2;
static int s_last_line_row = -1;  /* row of last print_line, for overwrite_last_line */
static char s_input_buf[TERMINAL_UI_INPUT_MAX];
static size_t s_input_len = 0;
/* UTF-8 multi-byte input: collect bytes until sequence complete */
static unsigned char s_utf8_pending[4];
static int s_utf8_len = 0;
static int s_utf8_need = 0;

static int utf8_sequence_len(unsigned char b) {
    if (b < 0x80u) return 1;
    if (b < 0xC2u) return 0;
    if (b < 0xE0u) return 2;
    if (b < 0xF0u) return 3;
    if (b < 0xF8u) return 4;
    return 0;
}

static size_t utf8_last_char_len(const char *buf, size_t len) {
    if (len == 0) return 0;
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0u) == 0x80u)
        i--;
    return len - i;
}

/* Count display columns: 1 per UTF-8 code point (correct for Latin/Vietnamese). */
static int utf8_display_cols(const char *buf, size_t byte_len) {
    int cols = 0;
    size_t i = 0;
    while (i < byte_len) {
        unsigned char b = (unsigned char)buf[i];
        if (b < 0x80u) {
            cols++;
            i++;
        } else {
            int seq = utf8_sequence_len(b);
            if (seq <= 0) { i++; continue; }
            if (i + (size_t)seq > byte_len) break;  /* incomplete sequence */
            cols++;
            i += (size_t)seq;
        }
    }
    return cols;
}

static int input_row(void) {
    int maxy = getmaxy(stdscr);
    return maxy > 0 ? maxy - 1 : 0;
}

int terminal_ui_init(void) {
    if (initscr() == NULL) return -1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);  /* cursor shown only in input_draw when needed */
    s_status_row = 0;
    s_log_row = s_log_start_row;
    s_running = true;
    return 0;
}

void terminal_ui_shutdown(void) {
    endwin();
}

void terminal_ui_refresh(void) {
    refresh();
}

void terminal_ui_print_status(const char *mode, uint64_t processed, uint64_t errors) {
    move(s_status_row, 0);
    clrtoeol();
    printw(" [M4 AI Engine] mode=%s | processed=%lu | errors=%lu ", mode, (unsigned long)processed, (unsigned long)errors);
}

void terminal_ui_print_line(const char *line) {
    int maxy = getmaxy(stdscr);
    int last_log_row = maxy - 2;  /* reserve last line for input */
    if (last_log_row < s_log_start_row) last_log_row = s_log_start_row;
    if (s_log_row >= last_log_row) {
        s_log_row = s_log_start_row;
        for (int r = s_log_start_row; r < last_log_row; r++) {
            move(r, 0);
            clrtoeol();
        }
    }
    move(s_log_row, 0);
    clrtoeol();
    printw("%s", line);
    s_last_line_row = s_log_row;
    s_log_row++;
}

void terminal_ui_overwrite_last_line(const char *line) {
    if (s_last_line_row < 0) return;
    move(s_last_line_row, 0);
    clrtoeol();
    printw("%s", line);
}

void terminal_ui_redraw_log(const char **lines, int n) {
    int maxy = getmaxy(stdscr);
    int last_log_row = maxy - 2;
    if (last_log_row < s_log_start_row) last_log_row = s_log_start_row;
    int max_visible = last_log_row - s_log_start_row;
    int start = n > max_visible ? n - max_visible : 0;
    int i;
    for (i = 0; i < max_visible; i++) {
        move(s_log_start_row + i, 0);
        clrtoeol();
        if (start + i < n && lines[start + i])
            printw("%s", lines[start + i]);
    }
    s_log_row = s_log_start_row + (n < max_visible ? n : max_visible);
    s_last_line_row = (s_log_row > s_log_start_row) ? s_log_row - 1 : -1;
}

void terminal_ui_print_error(const char *msg) {
    attron(A_REVERSE | A_BOLD);
    terminal_ui_print_line(msg);
    attroff(A_REVERSE | A_BOLD);
}

int terminal_ui_getch_nonblock(int *key) {
    int c = getch();
    if (c == ERR) return 0;
    *key = c;
    return 1;
}

bool terminal_ui_running(void) {
    return s_running;
}

void terminal_ui_set_running(bool running) {
    s_running = running;
}

int terminal_ui_input_put_key(int key) {
    if (key == '\n' || key == '\r' || key == KEY_ENTER) {
        s_utf8_len = 0;
        s_utf8_need = 0;
        return 2;  /* submitted */
    }
    if (key == KEY_BACKSPACE || key == 127) {
        s_utf8_len = 0;
        s_utf8_need = 0;
        if (s_input_len > 0) {
            size_t n = utf8_last_char_len(s_input_buf, s_input_len);
            s_input_len -= n;
            s_input_buf[s_input_len] = '\0';
        }
        return 1;
    }
    /* UTF-8: completing a multi-byte sequence */
    if (s_utf8_need > 0) {
        if (key >= 0x80 && key <= 0xBF && s_utf8_len < 4) {
            s_utf8_pending[s_utf8_len++] = (unsigned char)key;
            s_utf8_need--;
            if (s_utf8_need == 0) {
                if (s_input_len + s_utf8_len < TERMINAL_UI_INPUT_MAX) {
                    for (int i = 0; i < s_utf8_len; i++)
                        s_input_buf[s_input_len++] = (char)s_utf8_pending[i];
                    s_input_buf[s_input_len] = '\0';
                }
                s_utf8_len = 0;
            }
            return 1;
        }
        s_utf8_len = 0;
        s_utf8_need = 0;
    }
    /* UTF-8: start of multi-byte sequence */
    if (key >= 0x80 && key <= 0xFF) {
        int total = utf8_sequence_len((unsigned char)key);
        if (total > 1) {
            s_utf8_pending[0] = (unsigned char)key;
            s_utf8_len = 1;
            s_utf8_need = total - 1;
            return 1;
        }
        if (total == 1 && s_input_len < TERMINAL_UI_INPUT_MAX - 1) {
            s_input_buf[s_input_len++] = (char)key;
            s_input_buf[s_input_len] = '\0';
            return 1;
        }
        return 0;
    }
    /* ASCII printable */
    if (key >= 32 && key <= 126 && s_input_len < TERMINAL_UI_INPUT_MAX - 1) {
        s_input_buf[s_input_len++] = (char)key;
        s_input_buf[s_input_len] = '\0';
        return 1;
    }
    return 0;
}

void terminal_ui_input_draw(void) {
    int row = input_row();
    move(row, 0);
    clrtoeol();
    attron(A_BOLD);
    printw("> ");
    attroff(A_BOLD);
    printw("%s", s_input_buf);
    move(row, 2 + utf8_display_cols(s_input_buf, s_input_len));
    curs_set(1);  /* show cursor at end of input */
}

int terminal_ui_input_get_line(char *out, size_t size) {
    if (!out || size == 0) return -1;
    size_t n = s_input_len < size - 1 ? s_input_len : size - 1;
    memcpy(out, s_input_buf, n);
    out[n] = '\0';
    s_input_len = 0;
    s_input_buf[0] = '\0';
    return (int)n;
}

void terminal_ui_input_clear(void) {
    s_input_len = 0;
    s_input_buf[0] = '\0';
}
