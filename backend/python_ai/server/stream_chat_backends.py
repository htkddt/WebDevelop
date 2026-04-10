# -*- coding: utf-8 -*-
"""
Chat stream driver for ``/api/chat/stream``.

Uses the unified ``api_chat`` with ``stream_cb != NULL``. All routing (cloud pool + local Ollama
fallback) is handled inside c-lib ``ai_agent.c``. Python only forwards SSE events.
"""
from __future__ import annotations

import ctypes
import os
import sys
from typing import Any, Callable, Dict, List

_PY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PY_ROOT not in sys.path:
    sys.path.insert(0, _PY_ROOT)

from engine_ctypes import (
    CHAT_WIRE_LABELS,
    OL_BUF_SIZE,
    STREAM_TOKEN_CB,
    completion_source_label,
)

StreamSink = Callable[[Dict[str, Any]], None]


def resolved_chat_stream_mode(raw: str | None) -> str:
    """Normalize ``M4_CHAT_STREAM_MODE``. Kept for compat — all modes use unified api_chat now."""
    if not raw or not raw.strip():
        return "default"
    return raw.strip().lower()


def _run_stream(
    lib: Any,
    ctx: Any,
    tid: bytes,
    user_s: str,
    message: str,
    context_json: Any,
    temp_message_id: str,
    sink: StreamSink,
    gil_held_for_c_callback: Any,
    refs_holder: List[Any],
) -> None:
    """Unified stream via api_chat with stream_cb. c-lib handles all routing (cloud + local)."""
    tid_c = ctypes.c_char_p(tid)
    uid_c = ctypes.c_char_p(user_s.encode("utf-8"))
    msg_c = ctypes.c_char_p(message.encode("utf-8"))
    ctx_json_c = ctypes.c_char_p(context_json) if context_json else None
    refs_holder.extend([tid_c, uid_c, msg_c, ctx_json_c])

    def py_on_token(token, msg_id, done_flag, userdata):
        with gil_held_for_c_callback():
            tok = (token or b"").decode("utf-8", errors="replace")
            mid = (msg_id or b"").decode("utf-8", errors="replace")
            row: Dict[str, Any] = {"token": tok, "temp_message_id": mid, "done": bool(done_flag)}
            if done_flag:
                # Read real source from c-lib stats — not hardcoded
                from engine_ctypes import ApiStats
                stats = ApiStats()
                lib.api_get_stats(ctx, ctypes.byref(stats))
                lm = bytes(stats.last_llm_model).split(b"\x00", 1)[0].decode("utf-8", errors="replace").strip()
                row["source"] = completion_source_label(stats.last_reply_source, lm or None)
                if lm:
                    row["llm_model"] = lm
                wire_u = int(stats.last_chat_wire)
                if wire_u:
                    row["chat_wire"] = CHAT_WIRE_LABELS.get(wire_u, str(wire_u))
                row["assistant_meta"] = True
            sink(row)

    cb = STREAM_TOKEN_CB(py_on_token)
    refs_holder.append(cb)

    try:
        out_buf = ctypes.create_string_buffer(OL_BUF_SIZE)
        rc = lib.api_chat(ctx, tid_c, uid_c, msg_c, ctx_json_c, out_buf, ctypes.c_size_t(OL_BUF_SIZE), cb, None)
        if rc != 0:
            sink(
                {
                    "token": "",
                    "temp_message_id": temp_message_id or "",
                    "done": True,
                    "error": "api_chat (stream) failed — check cloud API keys or Ollama.",
                }
            )
    except Exception as ex:
        sink(
            {
                "token": "",
                "temp_message_id": temp_message_id or "",
                "done": True,
                "error": str(ex),
            }
        )


def dispatch_chat_stream(
    *,
    mode: str,
    lib: Any,
    ctx: Any,
    tid: bytes,
    user_s: str,
    message: str,
    context_json: Any = None,
    temp_message_id: str,
    sink: StreamSink,
    gil_held_for_c_callback: Any,
) -> None:
    """Dispatch to unified api_chat stream. c-lib handles all routing internally."""
    refs: List[Any] = []
    _run_stream(
        lib, ctx, tid, user_s, message, context_json,
        temp_message_id, sink, gil_held_for_c_callback, refs,
    )
