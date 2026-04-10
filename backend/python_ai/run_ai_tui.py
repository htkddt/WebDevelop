#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Terminal UI for python_ai: status top-right, chat history above, input at bottom.
History size is controlled by context_batch_size (batchContextSize); 0 = default (30).
Uses c-lib via engine_ctypes (api_create, api_chat, api_destroy).
Run: python3 run_ai_tui.py
Unicode input (e.g. Vietnamese): getch() bytes are decoded as UTF-8; locale set for UTF-8.
"""
import ctypes
import curses
import locale
import os
import sys
import time
from collections import deque
from typing import Optional

from engine_ctypes import (
    API_DEFAULT_TENANT_ID,
    ApiStats,
    OL_BUF_SIZE,
    completion_source_label,
    load_lib,
)
from display_utils import format_ts
from training.full_options import (
    build_api_options,
    build_max_api_options,
    get_full_options,
    get_max_options,
    log_resolved_engine_options_at_startup,
)

# Use same tenant key for load and chat so memory (Mongo history) and current session match.
TENANT_ID = API_DEFAULT_TENANT_ID

# Max option: MongoDB + Redis + ELK full stack (mode=MONGO_REDIS_ELK). Set True to use.
# Note: Bot replies show [OLLAMA] every time because c-lib Redis is a stub (no store/search).
# See c-lib docs/WHY_ONLY_OLLAMA_NOT_REDIS.md and .cursor/REDIS_KEYS.md for the trace and how to implement Redis L2.
USE_MAX_OPTIONS = True

TITLE = "python_ai"
INPUT_PROMPT = "> "

# Color pair indices for chat history (You vs Bot)
COLOR_PAIR_YOU = 1
COLOR_PAIR_BOT = 2


def chat_timestamp() -> str:
    """Current time as epoch ms string (for storage/ordering); display uses format_ts() from display_utils."""
    return str(int(time.time() * 1000))


# Curses special key codes are typically > 255; raw bytes are 0-255
KEY_ENTER_VALUES = (curses.KEY_ENTER, ord("\n"), ord("\r"))
KEY_BACKSPACE_VALUES = (curses.KEY_BACKSPACE, 127)


def utf8_lead_length(byte: int) -> int:
    """Return expected UTF-8 sequence length (1-4) for lead byte, or 0 if continuation/invalid."""
    if byte <= 0x7F:
        return 1
    if 0xC2 <= byte <= 0xDF:
        return 2
    if 0xE0 <= byte <= 0xEF:
        return 3
    if 0xF0 <= byte <= 0xF4:
        return 4
    return 0


def wrap_lines(text: str, width: int) -> list[str]:
    if width <= 0:
        return [text] if text else []
    lines = []
    for paragraph in text.split("\n"):
        while paragraph:
            if len(paragraph) <= width:
                lines.append(paragraph)
                break
            lines.append(paragraph[:width])
            paragraph = paragraph[width:]
    return lines if lines else [""]


def run_tui(stdscr):
    set_utf8_locale()
    curses.curs_set(1)
    stdscr.keypad(True)
    stdscr.timeout(50)
    # Colors for You [ts] vs Bot [ts]
    try:
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(COLOR_PAIR_YOU, curses.COLOR_CYAN, -1)
        curses.init_pair(COLOR_PAIR_BOT, curses.COLOR_GREEN, -1)
    except curses.error:
        pass

    # Load lib and create context (MEMORY mode, no Mongo)
    try:
        lib = load_lib()
    except FileNotFoundError as e:
        stdscr.addstr(0, 0, str(e)[:80])
        stdscr.refresh()
        stdscr.getch()
        return

    # Full options from training; max option = Mongo + Redis + ELK (env overrides defaults).
    # Keep (smart_topic_opts, option_string_blobs) alive so c_char_p fields stay valid after api_create.
    _server_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "server")
    if _server_dir not in sys.path:
        sys.path.insert(0, _server_dir)
    try:
        from env_init_log import log_dotenv_file_at_engine_init  # noqa: PLC0415

        log_dotenv_file_at_engine_init(_server_dir)
    except ImportError:
        pass

    if USE_MAX_OPTIONS:
        resolved_opts = get_max_options()
        opts, history_size, _smart_topic_keepalive = build_max_api_options()
        log_resolved_engine_options_at_startup(
            resolved_opts, history_size=history_size, source="tui_max_defaults_env"
        )
    else:
        resolved_opts = get_full_options()
        opts, history_size, _smart_topic_keepalive = build_api_options()
        log_resolved_engine_options_at_startup(resolved_opts, history_size=history_size, source="tui_env_only")
    ctx = lib.api_create(ctypes.byref(opts))
    if not ctx:
        stdscr.addstr(0, 0, "api_create failed")
        stdscr.refresh()
        stdscr.getch()
        return

    # When MongoDB is in options, load chat history for tenant (same key as api_chat)
    lib.api_load_chat_history(ctx, TENANT_ID, None)
    n = lib.api_get_history_count(ctx)
    role_buf = ctypes.create_string_buffer(32)
    content_buf = ctypes.create_string_buffer(2048)
    ts_buf = ctypes.create_string_buffer(64)
    llm_buf = ctypes.create_string_buffer(256)
    source_char = ctypes.c_char()
    history = deque(maxlen=history_size)  # (role, content, ts_or_none, source_label)
    for i in range(n):
        if (
            lib.api_get_history_message(
                ctx,
                i,
                role_buf,
                32,
                content_buf,
                2048,
                ctypes.byref(source_char),
                ts_buf,
                64,
                llm_buf,
                256,
            )
            == 0
        ):
            role = role_buf.value.decode("utf-8", errors="replace") or "user"
            content = content_buf.value.decode("utf-8", errors="replace") or ""
            ts_val = ts_buf.value.decode("utf-8", errors="replace").strip() or None
            llm_h = llm_buf.value.decode("utf-8", errors="replace").strip() or None
            source_label = completion_source_label(source_char.value, llm_h)
            role = "assistant" if role == "bot" else (role if role in ("user", "assistant") else "user")
            history.append((role, content, ts_val, source_label))
    input_buf = []  # list of str (Unicode chars)
    utf8_byte_buf = []  # incomplete UTF-8 bytes from getch()
    skip_next_newline = False  # avoid double send when terminal sends \r\n for Enter
    thinking = False
    scroll_offset = 0  # 0 = show bottom (newest); increase = scroll up to see older
    max_scroll = [0]   # updated in redraw(); max valid scroll_offset

    def format_status_from_stats(stats: ApiStats) -> str:
        """Build status string from api_get_stats (no hardcoded Ready)."""
        parts = []
        parts.append("Ollama:" + ("✓" if stats.ollama_connected else "-"))
        if stats.mongoc_linked:
            parts.append("Mongo:✓" if stats.mongo_connected else "Mongo:-")
        else:
            parts.append("Mongo:!mongoc")
        if stats.redis_connected:
            parts.append("Redis:✓")
        else:
            parts.append("Redis:-")
        if stats.elk_enabled:
            parts.append("ELK:" + ("✓" if stats.elk_connected else "-"))
        parts.append(f"err:{stats.error_count}")
        parts.append(f"proc:{stats.processed}")
        if stats.errors:
            parts.append(f"e:{stats.errors}")
        return " ".join(parts)

    def redraw():
        stdscr.erase()
        h, w = stdscr.getmaxyx()
        if h < 2 or w < 10:
            return

        # Top line: title left, status right from api_get_stats (or "Thinking...")
        title = TITLE[: w - 2]
        stdscr.addstr(0, 0, title, curses.A_BOLD)
        if thinking:
            status_str = "Thinking..."
        else:
            stats = ApiStats()
            lib.api_get_stats(ctx, ctypes.byref(stats))
            status_str = format_status_from_stats(stats)
        status_str = status_str[: w - 2]
        if status_str:
            status_x = max(len(title) + 1, w - len(status_str))
            try:
                stdscr.addstr(0, status_x, status_str[: w - status_x], curses.A_DIM)
            except curses.error:
                pass

        # Chat area: from row 1 to row h-2 (inclusive), width w
        chat_height = h - 2
        chat_width = w
        chat_y = 1

        # Build display lines: "You [ts]: ..." / "Bot [ts]-[type]: ..." with color per role (ts from epoch)
        display_lines = []  # list of (line_text, is_user)
        for item in history:
            role = item[0] if len(item) >= 1 else "user"
            text = item[1] if len(item) >= 2 else ""
            ts_raw = item[2] if len(item) >= 3 else None
            ts = format_ts(ts_raw) if ts_raw else ""
            source_label = item[3] if len(item) >= 4 else ("MEMORY" if role == "user" else "OLLAMA")
            is_user = role == "user"
            if is_user:
                prefix = f"You [{ts}]: " if ts else "You: "
            else:
                prefix = f"Bot [{ts}]-[{source_label}]: " if ts else f"Bot-[{source_label}]: "
            combined = prefix + (text or "")
            for line in wrap_lines(combined, chat_width):
                display_lines.append((line, is_user))

        # Newest at top: reverse so first row = latest; scroll_offset 0 = top (newest), scroll = see older
        display_lines = list(reversed(display_lines))
        total = len(display_lines)
        max_scroll[0] = max(0, total - chat_height)
        start = min(scroll_offset, max_scroll[0])
        for i, (line, is_user) in enumerate(display_lines[start : start + chat_height]):
            try:
                attr = curses.color_pair(COLOR_PAIR_YOU) if is_user else curses.color_pair(COLOR_PAIR_BOT)
                stdscr.addstr(chat_y + i, 0, line[:chat_width], attr)
            except curses.error:
                pass

        # Scroll hint when not at top (scroll_offset > 0 = scrolled to older)
        if scroll_offset > 0 and total > chat_height:
            hint = f" [↑ newer ↓ older PgUp/PgDn, offset={scroll_offset}]"
            try:
                stdscr.addstr(chat_y + chat_height - 1, max(0, chat_width - len(hint)), hint[:chat_width], curses.A_DIM)
            except curses.error:
                pass

        # Input line at bottom
        input_row = h - 1
        line = INPUT_PROMPT + "".join(input_buf)
        try:
            disp = line[:chat_width]
            stdscr.addstr(input_row, 0, disp, curses.A_REVERSE)
            stdscr.move(input_row, min(len(disp), w - 1))
        except curses.error:
            pass
        stdscr.refresh()

    def send_message():
        nonlocal thinking, scroll_offset
        if not input_buf:
            return
        text = "".join(input_buf).strip()
        if not text:
            input_buf.clear()
            return

        user_ts = chat_timestamp()
        history.append(("user", text, user_ts, "MEMORY"))
        input_buf.clear()
        # Keep current scroll (do not reset); latest is at top so new message visible when at offset 0
        thinking = True
        redraw()

        out = ctypes.create_string_buffer(OL_BUF_SIZE)
        rc = lib.api_chat(
            ctx,
            TENANT_ID,
            API_DEFAULT_TENANT_ID,
            text.encode("utf-8"),
            out,
            OL_BUF_SIZE,
        )
        bot_ts = chat_timestamp()
        thinking = False
        llm_tail = ctypes.create_string_buffer(256)
        lib.api_get_last_llm_model(ctx, llm_tail, 256)
        lm = llm_tail.value.decode("utf-8", errors="replace").strip() or None
        src = lib.api_get_last_reply_source(ctx)
        source_label = completion_source_label(src.value if src else None, lm)
        if rc == 0:
            reply = out.value.decode("utf-8", errors="replace").strip() or "(no reply)"
            history.append(("assistant", reply, bot_ts, source_label))
        else:
            history.append(("assistant", "[Error: api_chat failed. Is Ollama running?]", bot_ts, "OLLAMA"))
        redraw()

    def flush_utf8_to_input():
        """Decode utf8_byte_buf to a character and append to input_buf; clear buf on success or invalid."""
        nonlocal utf8_byte_buf
        if not utf8_byte_buf:
            return
        try:
            b = bytes(utf8_byte_buf)
            s = b.decode("utf-8")
            if len(s) == 1 and ord(s) >= 32 and ord(s) != 127:
                input_buf.append(s)
        except (UnicodeDecodeError, ValueError):
            pass
        utf8_byte_buf.clear()

    while True:
        redraw()
        try:
            key = stdscr.getch()
        except curses.error:
            continue

        if key == curses.KEY_RESIZE:
            continue
        if key == -1:
            continue  # timeout

        # Avoid double send when terminal sends \r then \n (or \n then \r) for one physical Enter
        if skip_next_newline and key in (ord("\n"), ord("\r")):
            skip_next_newline = False
            continue

        # Special keys (curses codes are often > 255; or ASCII control)
        if key in KEY_ENTER_VALUES:
            flush_utf8_to_input()
            send_message()
            skip_next_newline = True
            continue
        if key in KEY_BACKSPACE_VALUES:
            if utf8_byte_buf:
                utf8_byte_buf.clear()
            elif input_buf:
                input_buf.pop()
            continue
        if key in (ord("q"), ord("Q")) and not input_buf and not utf8_byte_buf:
            break

        # Scroll chat: only when not typing (input_buf empty)
        if not input_buf and not utf8_byte_buf:
            if key == curses.KEY_UP:
                scroll_offset = min(scroll_offset + 1, max_scroll[0])
                redraw()
                continue
            if key == curses.KEY_DOWN:
                scroll_offset = max(0, scroll_offset - 1)
                redraw()
                continue
            if key == curses.KEY_PPAGE:  # Page Up
                scroll_offset = min(scroll_offset + max(1, max_scroll[0] // 3), max_scroll[0])
                redraw()
                continue
            if key == curses.KEY_NPAGE:  # Page Down
                scroll_offset = max(0, scroll_offset - max(1, max_scroll[0] // 3))
                redraw()
                continue

        # Byte 0-255: part of input (ASCII or UTF-8)
        if 0 <= key <= 255:
            utf8_byte_buf.append(key)
            need = utf8_lead_length(utf8_byte_buf[0]) if utf8_byte_buf else 0
            if need == 0:
                utf8_byte_buf.clear()
            elif len(utf8_byte_buf) >= need:
                flush_utf8_to_input()
            continue

        # Curses special key (e.g. KEY_ENTER as 343) not handled above
        if key in (ord("q"), ord("Q")) and not input_buf:
            break

    lib.api_destroy(ctx)


def set_utf8_locale():
    """Prefer a UTF-8 locale so terminal sends Vietnamese/Unicode as UTF-8."""
    for loc in ("", "C.UTF-8", "en_US.UTF-8", "vi_VN.UTF-8", "en_GB.UTF-8"):
        try:
            locale.setlocale(locale.LC_ALL, loc)
            return
        except locale.Error:
            continue


def main():
    set_utf8_locale()
    try:
        curses.wrapper(run_tui)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
