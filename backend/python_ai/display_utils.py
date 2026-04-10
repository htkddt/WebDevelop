"""
Display helpers for chat TUI: timestamp formatting and message prefix.
Testable without curses; used by run_ai_tui.py.
"""
import time
from typing import Optional


def format_ts(ts: Optional[str]) -> str:
    """Format timestamp for display. Accepts epoch ms string (digits) or legacy HH:MM:SS."""
    if not ts:
        return ""
    s = ts.strip()
    if not s:
        return ""
    if s.isdigit() and len(s) >= 10:
        ms = int(s)
        if ms < 1e12:
            ms *= 1000
        t = time.localtime(ms / 1000.0)
        return time.strftime("%H:%M:%S", t)
    return s


def build_message_prefix(role: str, ts_raw: Optional[str], source_label: str) -> str:
    """Build 'You [ts]:' or 'Bot [ts]-[label]:' for display. ts_raw can be epoch ms string or legacy."""
    ts = format_ts(ts_raw) if ts_raw else ""
    if role == "user":
        return f"You [{ts}]: " if ts else "You: "
    return f"Bot [{ts}]-[{source_label}]: " if ts else f"Bot-[{source_label}]: "
