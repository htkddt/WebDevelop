#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M4 chat HTTP server (default port 5000, auto-falls back to 5001 if 5000 is busy):
  /api/chat, /api/chat/stream (SSE), /api/history, /api/stats, /api/geo/import. Root GET / lists these.

Uses c-lib via engine_ctypes (7 public functions from api.h):
  api_create(json) / api_destroy / api_chat (unified sync+stream) / api_load_chat_history /
  api_get_history_message / api_get_stats / api_geo_atlas_import_row.

Chat identity: JWT auth (default). User profile passed via ``context_json`` parameter to ``api_chat``
  (injected as [CONTEXT] in the LLM prompt — not stored in user message). Set ``M4_CHAT_REQUIRE_AUTH=0``
  for anonymous mode.

Cloud routing: handled entirely in c-lib ``ai_agent.c`` (Groq/Cerebras/Gemini → Ollama fallback).
  Python does NOT make cloud HTTP calls. Env: ``M4_CHAT_BACKEND``, ``GROQ_API_KEY``, etc.

Debug: all 12 c-lib debug modules enabled by default via ``api_create`` JSON config.

Threading: c-lib uses pthreads internally. Python serializes ``api_*`` calls with ``_ctx_lock``;
  stream callbacks use ``c_pthread_bridge.gil_held_for_c_callback`` (GIL).
"""
from __future__ import annotations

import atexit
import ctypes
import json
import os
import queue
import socket
import sys
import threading
from datetime import datetime, timezone
from typing import Any, Dict, Generator, List, Optional, Tuple

# python_ai root (parent of server/) + this directory (for geo_csv_import, c_pthread_bridge)
_SERVER_DIR = os.path.dirname(os.path.abspath(__file__))
_PYTHON_AI_ROOT = os.path.dirname(_SERVER_DIR)
if _PYTHON_AI_ROOT not in sys.path:
    sys.path.insert(0, _PYTHON_AI_ROOT)
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from env_load import load_server_env  # noqa: E402

load_server_env(_SERVER_DIR)

from engine_ctypes import (  # noqa: E402
    API_DEFAULT_TENANT_ID,
    CHAT_WIRE_LABELS,
    OL_BUF_SIZE,
    STREAM_TOKEN_CB,
    ApiStats,
    c_size_t,
    completion_source_label,
    load_lib,
)
from training.full_options import (  # noqa: E402
    build_api_options,
    build_api_options_from_resolved_dict,
    build_max_api_options,
    get_full_options,
    get_max_options,
    log_resolved_engine_options_at_startup,
    resolve_engine_options_dict,
)

import geo_csv_import as geo_csv  # noqa: E402

from flask import Flask, Response, jsonify, redirect, request, stream_with_context
from flask_cors import CORS
from werkzeug.exceptions import Forbidden, Unauthorized

from admin_routes import bp as _admin_bp  # noqa: E402
from auth_jwt import require_jwt  # noqa: E402
from auth_routes import bp as _auth_bp  # noqa: E402
from org_store import init_seed_org_catalog  # noqa: E402
from policy_store import ensure_cross_hr_finance_demo_seed, init_seed_policies  # noqa: E402
from store_routes import bp as _store_bp  # noqa: E402
from user_store import ensure_cross_hr1_demo_user, init_seed_users  # noqa: E402

from access_model_store import access_model_http_response, ensure_access_pages_seeded  # noqa: E402
from app_mongo import app_mongo_health  # noqa: E402

# Aliases "", "tenant", "user", "default" → API_DEFAULT_TENANT_ID (b"default") for c-lib.
# M4ENGINE_TENANT_STRICT=1: pass raw string as tenant bytes (multi-tenant).
_TENANT_STRICT = os.environ.get("M4ENGINE_TENANT_STRICT", "").strip() in ("1", "true", "yes")

from c_pthread_bridge import gil_held_for_c_callback  # noqa: E402
from stream_chat_backends import dispatch_chat_stream, resolved_chat_stream_mode  # noqa: E402


def _chat_require_jwt() -> bool:
    """
    Phase 1: chat + history require a valid JWT; tenant/user for c-lib come from ``user_id`` in claims (app user id), not client body.
    Set ``M4_CHAT_REQUIRE_AUTH=0|false|no|off`` for anonymous / legacy clients (Phase 2 will add temp UUID + localStorage).
    """
    return os.environ.get("M4_CHAT_REQUIRE_AUTH", "1").strip().lower() not in (
        "0",
        "false",
        "no",
        "off",
    )


def _jwt_profile_instructions_text(claims: Dict[str, Any]) -> str:
    """User profile text for context_json. Used by _build_context_json for the [CONTEXT] prompt block."""
    sub = (claims.get("sub") or "").strip()
    uid = str(claims.get("user_id") or "").strip()
    role = str(claims.get("role") or "").strip()
    name = str(claims.get("name") or "").strip()
    dept = str(claims.get("department") or "").strip()
    lines = [
        "[User profile for this session — personalize replies; do not recite as a list unless asked.]",
        f"user_id: {uid}",
        f"email: {sub}",
    ]
    if name:
        lines.append(f"display_name: {name}")
    if role:
        lines.append(f"role: {role}")
    if dept:
        lines.append(f"department: {dept}")
    lines.append("---")
    return "\n".join(lines)


def _build_context_json(claims: Optional[Dict[str, Any]]) -> Optional[bytes]:
    """Build per-request context JSON from JWT claims for api_chat context_json parameter."""
    if not claims:
        return None
    ctx = {}
    sub = (claims.get("sub") or "").strip()
    uid = str(claims.get("user_id") or "").strip()
    role = str(claims.get("role") or "").strip()
    name = str(claims.get("name") or "").strip()
    dept = str(claims.get("department") or "").strip()
    if uid:
        ctx["user_id"] = uid
    if sub:
        ctx["email"] = sub
    if name:
        ctx["display_name"] = name
    if role:
        ctx["role"] = role
    if dept:
        ctx["department"] = dept
    if not ctx:
        return None
    return json.dumps(ctx, ensure_ascii=False).encode("utf-8")


def _tenant_id_bytes(raw: Optional[str]) -> bytes:
    """Bytes passed to api_chat / api_load_chat_history (c-lib tenant_id)."""
    if _TENANT_STRICT:
        t = (raw or "default").strip() or "default"
        return t.encode("utf-8")
    s = (raw or "").strip().lower()
    if s in ("", "tenant", "user", "default"):
        return API_DEFAULT_TENANT_ID
    return (raw or "default").strip().encode("utf-8")


def _tenant_json(tid: bytes, app_user_id: Optional[str] = None) -> Dict[str, str]:
    """Echo tenant for JSON. With JWT pass app_user_id so user != tenant_id (tenant stays default)."""
    s = tid.decode("utf-8", errors="replace")
    if app_user_id and str(app_user_id).strip():
        return {"tenant_id": s, "user": str(app_user_id).strip()}
    return {"tenant_id": s, "user": s}


# Same db/collection as c-lib storage (include/storage.h STORAGE_CHAT_*).
_BOT_CHAT_DB = "bot"
_BOT_CHAT_COLL = "records"
# Align with c-lib API_CONTEXT_BATCH_SIZE_DEFAULT (messages kept in history window).
_CHAT_HISTORY_MESSAGE_CAP = 30


def _engine_mongo_uri() -> str:
    return (os.environ.get("M4ENGINE_MONGO_URI") or "mongodb://127.0.0.1:27017").strip()


def _chat_history_mongo_filter(tenant_id_str: str, app_user_id: str) -> Dict[str, Any]:
    """
    Restrict bot.records to this login.
    - New rows: tenant_id ``default``, user = app user id.
    - Legacy: tenant_id was previously the JWT user id (user = id or ``default``).
    """
    return {
        "$or": [
            {"tenant_id": tenant_id_str, "user": app_user_id},
            {"tenant_id": app_user_id, "user": app_user_id},
            {"tenant_id": app_user_id, "user": "default"},
        ]
    }


def _doc_timestamp_for_history(doc: Dict[str, Any]) -> Optional[str]:
    ts = doc.get("timestamp")
    if isinstance(ts, str) and ts.strip():
        return ts.strip()
    ca = doc.get("createdAt")
    if isinstance(ca, datetime):
        if ca.tzinfo is None:
            ca = ca.replace(tzinfo=timezone.utc)
        return str(int(ca.timestamp() * 1000))
    return None


def _history_messages_from_bot_mongo(
    tenant_id: bytes, app_user_id: str, doc_limit: int = 30
) -> Optional[List[Dict[str, Any]]]:
    """
    Read chat turns from Mongo for the authenticated user (c-lib unchanged).
    Returns None if pymongo is missing or Mongo is unreachable.
    """
    try:
        from pymongo import MongoClient
    except ImportError:
        return None
    tid_str = tenant_id.decode("utf-8", errors="replace")
    flt = _chat_history_mongo_filter(tid_str, app_user_id)
    try:
        with MongoClient(_engine_mongo_uri(), serverSelectionTimeoutMS=5000) as client:
            coll = client[_BOT_CHAT_DB][_BOT_CHAT_COLL]
            docs = list(coll.find(flt).sort("createdAt", -1).limit(max(1, doc_limit)))
    except Exception:
        return None
    docs.reverse()
    out: List[Dict[str, Any]] = []
    for doc in docs:
        ts_val = _doc_timestamp_for_history(doc)
        meta = doc.get("metadata") if isinstance(doc.get("metadata"), dict) else {}
        llm_mid = ""
        embed_mid = ""
        if isinstance(meta.get("llm_model_id"), str):
            llm_mid = meta.get("llm_model_id", "").strip()
        if isinstance(meta.get("model_id"), str):
            embed_mid = meta.get("model_id", "").strip()
        turn = doc.get("turn")
        if isinstance(turn, dict):
            inp = (turn.get("input") or "") if isinstance(turn.get("input"), str) else ""
            ast = (turn.get("assistant") or "") if isinstance(turn.get("assistant"), str) else ""
            out.append(
                {
                    "role": "user",
                    "content": inp,
                    "timestamp": ts_val,
                    "source": "MONGODB",
                    "history_origin": "database",
                }
            )
            ast_src = completion_source_label(None, llm_mid) if llm_mid else "MONGODB"
            ast_row: Dict[str, Any] = {
                "role": "assistant",
                "content": ast,
                "timestamp": ts_val,
                "source": ast_src,
                "history_origin": "database",
            }
            if llm_mid:
                ast_row["llm_model"] = llm_mid
            if embed_mid:
                ast_row["embed_model_id"] = embed_mid
            out.append(ast_row)
            continue
        role_raw = doc.get("role")
        content_raw = doc.get("content")
        role = (role_raw if isinstance(role_raw, str) else "user") or "user"
        if role == "bot":
            role = "assistant"
        if role not in ("user", "assistant"):
            role = "user"
        content = (content_raw if isinstance(content_raw, str) else "") or ""
        ast_src_leg = completion_source_label(None, llm_mid) if (role == "assistant" and llm_mid) else "MONGODB"
        leg: Dict[str, Any] = {
            "role": role,
            "content": content,
            "timestamp": ts_val,
            "source": ast_src_leg if role == "assistant" else "MONGODB",
            "history_origin": "database",
        }
        if role == "assistant":
            if llm_mid:
                leg["llm_model"] = llm_mid
            if embed_mid:
                leg["embed_model_id"] = embed_mid
        out.append(leg)
    if len(out) > _CHAT_HISTORY_MESSAGE_CAP:
        out = out[len(out) - _CHAT_HISTORY_MESSAGE_CAP :]
    return out


# Same as run_ai_tui.py USE_MAX_OPTIONS = True → build_max_api_options().
# Opt out: M4ENGINE_SERVER_MAX=0|false|no → build_api_options() only.
_USE_MAX_DEFAULT = "1"
USE_MAX_OPTIONS = os.environ.get("M4ENGINE_SERVER_MAX", _USE_MAX_DEFAULT).strip().lower() not in (
    "0",
    "false",
    "no",
)

_app = Flask(__name__)
init_seed_users()
ensure_cross_hr1_demo_user()
init_seed_policies()
ensure_cross_hr_finance_demo_seed()
init_seed_org_catalog()
ensure_access_pages_seeded()
_app.register_blueprint(_auth_bp)
_app.register_blueprint(_admin_bp)
_app.register_blueprint(_store_bp)
CORS(
    _app,
    resources={
        r"/api/*": {
            "origins": [
                "http://127.0.0.1:8000",
                "http://localhost:8000",
                "http://127.0.0.1:5173",
                "http://localhost:5173",
            ],
            "allow_headers": ["Content-Type", "Authorization", "X-Guest-Cart-Id"],
        }
    },
)


@_app.errorhandler(Unauthorized)
def _api_unauthorized(e: Unauthorized):
    if request.path.startswith("/api/"):
        return jsonify({"error": "unauthorized", "message": e.description}), 401
    return e.get_response()


@_app.errorhandler(Forbidden)
def _api_forbidden(e: Forbidden):
    if request.path.startswith("/api/"):
        return jsonify({"error": "forbidden", "message": e.description}), 403
    return e.get_response()


def _try_init_product_catalog() -> None:
    try:
        from product_cart_store import ensure_catalog_indexes, init_seed_products_if_empty  # noqa: PLC0415

        ensure_catalog_indexes()
        init_seed_products_if_empty()
    except Exception:
        pass


_try_init_product_catalog()

_lib = None
_ctx = None
# Serialize Python-side api_* calls on _ctx (c-lib also uses internal pthreads; see c_pthread_bridge).
_ctx_lock = threading.Lock()
_role_buf = None
_content_buf = None
_ts_buf = None
_llm_buf = None
_source_char = None
# Keep SmartTopicOptions struct alive for c-lib pointer lifetime (see training.full_options).
_M4_SMART_TOPIC_KEEPALIVE: Any = None


def _init_engine() -> None:
    global _lib, _ctx, _role_buf, _content_buf, _ts_buf, _llm_buf, _source_char, _M4_SMART_TOPIC_KEEPALIVE
    _lib = load_lib()
    from env_init_log import log_dotenv_file_at_engine_init  # noqa: PLC0415

    log_dotenv_file_at_engine_init(_SERVER_DIR)
    # Merge persisted bot_c_lib overrides + optional stack fallback (HIGH→LOW) when enabled.
    merge_db = os.environ.get("M4ENGINE_MERGE_BOT_C_LIB_AT_STARTUP", "1").strip().lower() not in (
        "0",
        "false",
        "no",
    )
    resolved_for_log: Optional[Dict[str, Any]] = None
    resolved_source = ""
    try:
        if merge_db:
            from bot_c_lib_settings_store import get_bot_c_lib_overrides  # noqa: PLC0415

            eff, fb_reasons = resolve_engine_options_dict(USE_MAX_OPTIONS, get_bot_c_lib_overrides())
            resolved_for_log = eff
            resolved_source = "merged_env_db_stack_fallback"
            for r in fb_reasons:
                print(f"[M4] stack fallback: {r}", file=sys.stderr)
            opts, _history_size, keepalive = build_api_options_from_resolved_dict(eff)
        elif USE_MAX_OPTIONS:
            resolved_for_log = get_max_options()
            resolved_source = "max_defaults_env"
            opts, _history_size, keepalive = build_max_api_options()
        else:
            resolved_for_log = get_full_options()
            resolved_source = "env_only"
            opts, _history_size, keepalive = build_api_options()
    except ValueError as e:
        print(
            "[M4][python→c-lib] OPTIONS VALIDATION FAILED — fix env / admin bot-c-lib settings "
            "(see training/full_options.py, include/api.h api_options_t). "
            f"Detail: {e}",
            file=sys.stderr,
        )
        raise
    if resolved_for_log is not None:
        log_resolved_engine_options_at_startup(
            resolved_for_log, history_size=_history_size, source=resolved_source
        )
    # opts is a JSON string; api_create(const char *json_opts) parses it in c-lib.
    _ctx = _lib.api_create(opts.encode("utf-8") if isinstance(opts, str) else opts)
    if not _ctx:
        raise RuntimeError(
            "api_create(json) failed — check libm4engine in python_ai/lib or libs (git-supplied binary) and options JSON."
        )
    tid = API_DEFAULT_TENANT_ID
    _lib.api_load_chat_history(_ctx, tid, None)
    _role_buf = ctypes.create_string_buffer(32)
    _content_buf = ctypes.create_string_buffer(2048)
    _ts_buf = ctypes.create_string_buffer(64)
    _llm_buf = ctypes.create_string_buffer(256)
    _source_char = ctypes.c_char()


def _try_bot_c_lib_indexes() -> None:
    try:
        from bot_c_lib_settings_store import ensure_bot_c_lib_settings_indexes  # noqa: PLC0415

        ensure_bot_c_lib_settings_indexes()
    except Exception:
        pass


_try_bot_c_lib_indexes()


def _destroy_engine() -> None:
    global _ctx, _lib
    with _ctx_lock:
        if _lib and _ctx:
            _lib.api_destroy(_ctx)
            _ctx = None


atexit.register(_destroy_engine)


def _history_messages(
    tenant_id: bytes, reload_mongo: bool, c_lib_user_id: Optional[bytes] = None
) -> List[Dict[str, Any]]:
    global _lib, _ctx
    with _ctx_lock:
        if reload_mongo:
            _lib.api_load_chat_history(_ctx, tenant_id, c_lib_user_id)
        out: List[Dict[str, Any]] = []
        for i in range(10000):  # iterate until api_get_history_message returns -1
            if (
                _lib.api_get_history_message(
                    _ctx,
                    i,
                    _role_buf,
                    32,
                    _content_buf,
                    2048,
                    ctypes.byref(_source_char),
                    _ts_buf,
                    64,
                    _llm_buf,
                    256,
                )
                != 0
            ):
                break
            role = _role_buf.value.decode("utf-8", errors="replace") or "user"
            if role == "bot":
                role = "assistant"
            content = _content_buf.value.decode("utf-8", errors="replace") or ""
            ts_val = _ts_buf.value.decode("utf-8", errors="replace").strip() or None
            llm_s = _llm_buf.value.decode("utf-8", errors="replace").strip() or None
            row: Dict[str, Any] = {
                "role": role if role in ("user", "assistant") else "user",
                "content": content,
                "timestamp": ts_val,
                "source": completion_source_label(_source_char.value, llm_s),
            }
            if llm_s:
                row["llm_model"] = llm_s
            out.append(row)
        return out


def _stats_dict() -> Dict[str, Any]:
    s = ApiStats()
    with _ctx_lock:
        _lib.api_get_stats(_ctx, ctypes.byref(s))
    return {
        "memory_bytes": int(s.memory_bytes),
        "mongo_connected": bool(s.mongo_connected),
        "mongoc_linked": bool(s.mongoc_linked),
        "redis_connected": bool(s.redis_connected),
        "elk_enabled": bool(s.elk_enabled),
        "elk_connected": bool(s.elk_connected),
        "ollama_connected": bool(s.ollama_connected),
        "error_count": int(s.error_count),
        "warning_count": int(s.warning_count),
        "processed": int(s.processed),
        "errors": int(s.errors),
    }


def _prepare_geo_row_vectors(
    row: Dict[str, Any],
    embed_model: str,
    *,
    no_vector: bool = False,
) -> Tuple[Optional[Any], Any, c_size_t]:
    """
    Ollama + float buffer prep — **no** ``_ctx`` / ctypes api calls (safe to run without ``_ctx_lock``).

    Returns ``(keeper, vptr, vdim)`` where *keeper* is the ctypes array (or None); keep *keeper*
    alive until ``api_geo_atlas_import_row`` returns.
    """
    if no_vector:
        return None, None, c_size_t(0)
    host, port = geo_csv.ollama_host_port()
    emb: Optional[List[float]] = row.get("embedding")
    if emb is None:
        emb = geo_csv.ollama_embed(row["embed_text"], embed_model, host, port)
    n = len(emb)
    keeper = (ctypes.c_float * n)(*emb)
    vptr = ctypes.cast(keeper, ctypes.POINTER(ctypes.c_float))
    return keeper, vptr, c_size_t(n)


def _geo_import_row_ctypes_only(
    row: Dict[str, Any],
    tenant_id: str,
    embed_model: str,
    vptr: Any,
    vdim: c_size_t,
) -> int:
    """Call ``api_geo_atlas_import_row`` — caller must hold ``_ctx_lock``."""
    rc = _lib.api_geo_atlas_import_row(
        _ctx,
        ctypes.c_char_p(tenant_id.encode("utf-8")),
        ctypes.c_char_p(row["name"].encode("utf-8")),
        ctypes.c_char_p(row["name_normalized"].encode("utf-8")),
        ctypes.c_char_p((row.get("district") or "").encode("utf-8")),
        ctypes.c_char_p((row.get("axis") or "").encode("utf-8")),
        ctypes.c_char_p((row.get("category") or "").encode("utf-8")),
        ctypes.c_char_p((row.get("city") or "").encode("utf-8")),
        vptr,
        vdim,
        ctypes.c_char_p(embed_model.encode("utf-8")),
        ctypes.c_char_p((row.get("source") or "seed").encode("utf-8")),
        ctypes.c_char_p((row.get("verification_status") or "verified").encode("utf-8")),
        ctypes.c_double(float(row.get("trust_score", 1.0))),
    )
    return int(rc)


def _pick_listen_port(host: str) -> tuple[int, Optional[int]]:
    """
    If M4ENGINE_SERVER_PORT is set, use it exactly.
    Otherwise try 5000 then 5001 (macOS often has 5000 taken by AirPlay / other services).
    Returns (port_to_use, skipped_port_if_fallback) where skipped is None if first choice worked.
    """
    raw = os.environ.get("M4ENGINE_SERVER_PORT", "").strip()
    if raw:
        return (int(raw), None)
    for p in (5000, 5001):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind((host, p))
            except OSError:
                continue
            return (p, None if p == 5000 else 5000)
    raise RuntimeError(
        f"Could not bind {host} on port 5000 or 5001 — set M4ENGINE_SERVER_PORT to a free port."
    )


@_app.route("/", methods=["GET"])
def index():
    """Avoid bare GET / → 404; documents real API paths under /api/."""
    return jsonify(
        {
            "service": "m4-chat",
            "endpoints": {
                "health": "/api/health",
                "auth_register": "POST /api/auth/register — JSON email, password (customer USER)",
                "auth_login": "POST /api/auth/login — JSON email, password → access_token",
                "auth_me": "GET /api/auth/me — Authorization: Bearer <JWT>",
                "auth_admin_ping": "GET /api/auth/admin/ping — staff JWT only (not USER)",
                "admin_policies": "GET /api/admin/policies — list ABAC policies (ADMIN, HR)",
                "admin_users": "GET /api/admin/users — list users (ADMIN, HR)",
                "admin_user_policies": "GET|PUT /api/admin/users/<id>/policies — view or set policy_ids (ADMIN, HR)",
                "admin_policies_builder": "GET /api/admin/policies/builder-options — platform/module/feature dropdown data for policy UI",
                "meta_access_model": "GET /api/meta/access-model?platform=web|admin|all|<id> — WEB/ADMIN slices, full catalog, or any platform id from Mongo access_platforms (404 if unknown)",
                "store_products": "GET /api/store/products?category= — published catalog",
                "store_cart": "GET|PUT /api/store/cart — USER or X-Guest-Cart-Id; staff forbidden",
                "admin_products": "GET|POST /api/admin/products, PATCH /api/admin/products/<id>, POST /api/admin/products/bulk — STORAGE+ADMIN (read: SALE/FINANCE too)",
                "stats": "/api/stats",
                "history": "GET /api/history?reload=1 — JWT required when M4_CHAT_REQUIRE_AUTH=1 (default); tenant is server user_id",
                "chat": "POST /api/chat — JWT + user profile prefix when M4_CHAT_REQUIRE_AUTH=1",
                "chat_stream": "POST /api/chat/stream (SSE) — M4_CHAT_STREAM_MODE=ollama|router; see chat_stream_callbacks.md",
                "geo_import": "POST /api/geo/import — default: no Ollama; use ?embed=1 for embeddings; optional X-Geo-Import-Key",
            },
            "note": "Use /api/history, not /history. GET /api/health includes app_db (M4_APP_MONGO_URI / M4_APP_MONGO_DB — app layer Mongo, separate from M4ENGINE_MONGO_URI bot DB).",
        }
    )


@_app.route("/history", methods=["GET"])
def history_legacy_redirect():
    """Browser / old clients hitting /history → same as /api/history."""
    qs = request.query_string.decode("utf-8", errors="replace")
    dest = "/api/history" + ("?" + qs if qs else "")
    return redirect(dest, code=307)


@_app.route("/api/health", methods=["GET"])
def health():
    return jsonify({"ok": True, "app_db": app_mongo_health()})


@_app.route("/api/meta/access-model", methods=["GET"])
def access_model_route():
    """Platform → module → page → uiMethods. ``platform=web|admin|all`` or any id from the catalog (e.g. MOBILE)."""
    data = access_model_http_response(request.args.get("platform"))
    if data.get("error"):
        return jsonify(data), 404
    return jsonify(data)


@_app.route("/api/stats", methods=["GET"])
def stats():
    return jsonify(_stats_dict())


@_app.route("/api/geo/import", methods=["POST"], strict_slashes=False)
def geo_import():
    """
    Bulk import CSV rows into Mongo `geo_atlas` via c-lib `api_geo_atlas_import_row`.

    **multipart/form-data:** `file` = CSV file; optional `mapping` = JSON string
    ``{\"name\":\"YourNameColumn\",...}`` (logical field → CSV header).

    **application/json:** `csv_text` or `csv` = raw CSV string; optional `mapping` object.

    Query: `tenant_id`, `embed_model` (only if embedding), default **no Ollama** — rows are stored
    with zero-length vectors unless you opt in: **`embed=1`** (or **`no_embed=0`**) to call Ollama
    for embeddings. Matches c-lib: Ollama is optional for this path.

    Optional env **M4ENGINE_GEO_IMPORT_KEY**: if set, require header **X-Geo-Import-Key**.
    """
    exp = os.environ.get("M4ENGINE_GEO_IMPORT_KEY", "").strip()
    if exp and request.headers.get("X-Geo-Import-Key") != exp:
        return jsonify({"error": "unauthorized", "hint": "Set header X-Geo-Import-Key"}), 401

    mapping: Optional[Dict[str, str]] = None
    csv_text = ""
    tenant_id = "default"

    ct = (request.content_type or "").lower()
    if "multipart/form-data" in ct:
        uf = request.files.get("file")
        if not uf:
            return jsonify({"error": "missing file field (multipart)"}), 400
        csv_text = uf.read().decode("utf-8", errors="replace")
        mj = request.form.get("mapping")
        if mj:
            try:
                mapping = json.loads(mj)
            except json.JSONDecodeError as e:
                return jsonify({"error": f"mapping JSON: {e}"}), 400
        tenant_id = (request.form.get("tenant_id") or request.form.get("user") or "default").strip()
    else:
        data = request.get_json(silent=True) or {}
        csv_text = (data.get("csv_text") or data.get("csv") or "").strip()
        if not csv_text:
            return jsonify({"error": "json body needs csv_text or csv"}), 400
        mapping = data.get("mapping")
        if mapping is not None and not isinstance(mapping, dict):
            return jsonify({"error": "mapping must be a JSON object"}), 400
        tenant_id = str(data.get("tenant_id") or data.get("user") or "default").strip() or "default"

    # Default: do not call Ollama (c-lib insert without vectors). Opt-in: embed=1 or no_embed=0.
    if request.args.get("embed", "").lower() in ("1", "true", "yes"):
        no_embed = False
    elif request.args.get("no_embed", "").lower() in ("0", "false", "no"):
        no_embed = False
    elif request.args.get("no_embed", "").lower() in ("1", "true", "yes"):
        no_embed = True
    else:
        no_embed = True

    embed_model = (request.args.get("embed_model") or geo_csv.embed_model_name()).strip()

    rows, parse_errors = geo_csv.parse_geo_csv(csv_text, mapping=mapping)
    imported = 0
    failed: List[Dict[str, Any]] = []

    ollama_info: Dict[str, Any] = (
        {"skipped": True, "reason": "default: no Ollama (pass embed=1 to embed)"}
        if no_embed
        else geo_csv.ollama_import_embed_endpoint()
    )

    for row in rows:
        try:
            # Ollama HTTP is slow; run it outside _ctx_lock so other endpoints can use ctx (c-lib pthread workers + Python threads).
            keeper, vptr, vdim = _prepare_geo_row_vectors(
                row, embed_model, no_vector=no_embed
            )
            with _ctx_lock:
                rc = _geo_import_row_ctypes_only(row, tenant_id, embed_model, vptr, vdim)
            del keeper  # release float buffer after ctypes call
            if rc != 0:
                failed.append({"name": row.get("name"), "error": "api_geo_atlas_import_row returned -1"})
            else:
                imported += 1
        except Exception as ex:
            failed.append({"name": row.get("name"), "error": str(ex)})

    return jsonify(
        {
            "imported": imported,
            "parse_errors": parse_errors,
            "failed": failed,
            "rows_ok": len(rows),
            "tenant_id": tenant_id,
            "ollama_embed_endpoint": ollama_info,
        }
    )


@_app.route("/api/history", methods=["GET"])
def history():
    reload_mongo = request.args.get("reload", "").lower() in ("1", "true", "yes")
    jwt_uid: Optional[str] = None
    if _chat_require_jwt():
        claims = require_jwt()
        jwt_uid = str(claims.get("user_id") or "").strip()
        if not jwt_uid:
            raise Unauthorized(description="token has no user_id — sign in again")
        tid = API_DEFAULT_TENANT_ID
        try:
            mongo_msgs = _history_messages_from_bot_mongo(tid, jwt_uid)
            if mongo_msgs is not None:
                messages = mongo_msgs
            else:
                messages = _history_messages(tid, reload_mongo=reload_mongo, c_lib_user_id=jwt_uid.encode("utf-8"))
        except Exception as e:
            return jsonify({"error": str(e)}), 500
    else:
        tenant = request.args.get("tenant_id") or request.args.get("user")
        tid = _tenant_id_bytes(tenant)
        try:
            messages = _history_messages(tid, reload_mongo=reload_mongo, c_lib_user_id=None)
        except Exception as e:
            return jsonify({"error": str(e)}), 500
    # Greeting: api_greet checks if user needs a welcome message (condition: TODAY by default).
    # Returns 0 = greeting generated, 1 = no greeting needed (already chatted), -1 = error.
    # Always call api_greet — c-lib checks the condition internally (e.g. last activity > 24h).
    if jwt_uid:
        ctx_json = _build_context_json(claims)
        if ctx_json:
            greet_opts = json.dumps({"condition": "TODAY", "response_type": "CHAT"}).encode("utf-8")
            print(
                f"[M4][python] api_greet: tenant={tid!r} user={jwt_uid!r} "
                f"context_json={ctx_json!r} opts={greet_opts!r}",
                file=sys.stderr,
            )
            greeting_out = ctypes.create_string_buffer(4096)
            with _ctx_lock:
                rc = _lib.api_greet(
                    _ctx, tid, jwt_uid.encode("utf-8"),
                    ctx_json, greet_opts, greeting_out, ctypes.c_size_t(4096),
                )
            if rc == 0:
                greeting_text = greeting_out.value.decode("utf-8", errors="replace").strip()
                if greeting_text:
                    stats = ApiStats()
                    with _ctx_lock:
                        _lib.api_get_stats(_ctx, ctypes.byref(stats))
                    lm = bytes(stats.last_llm_model).split(b"\x00", 1)[0].decode("utf-8", errors="replace").strip()
                    messages.append({
                        "role": "assistant",
                        "content": greeting_text,
                        "source": completion_source_label(stats.last_reply_source, lm or None),
                        "llm_model": lm or None,
                    })
            elif rc == 1:
                print("[M4][python] api_greet: skipped (already chatted today)", file=sys.stderr)

    out = {"messages": messages}
    if jwt_uid:
        out.update(_tenant_json(tid, jwt_uid))
    else:
        out.update(_tenant_json(tid))
    return jsonify(out)


@_app.route("/api/chat", methods=["POST"])
def chat():
    data = request.get_json(silent=True) or {}
    message_raw = (data.get("message") or "").strip()
    if not message_raw:
        return jsonify({"error": "message required"}), 400
    jwt_claims: Optional[Dict[str, Any]] = None
    if _chat_require_jwt():
        jwt_claims = require_jwt()
        uid = str(jwt_claims.get("user_id") or "").strip()
        if not uid:
            raise Unauthorized(description="token has no user_id — sign in again")
        tid = API_DEFAULT_TENANT_ID
        uid_b = uid.encode("utf-8")
    else:
        tid = _tenant_id_bytes(data.get("tenant_id") or data.get("user"))
        user_raw = data.get("user") or data.get("tenant_id") or "default"
        uid_b = (str(user_raw).strip() or "default").encode("utf-8")

    msg_b = message_raw.encode("utf-8")
    ctx_json = _build_context_json(jwt_claims)
    print(
        f"[M4][python] api_chat SYNC: tenant={tid!r} user={uid_b!r} "
        f"message_len={len(msg_b)} context_json={ctx_json!r}",
        file=sys.stderr,
    )
    out = ctypes.create_string_buffer(OL_BUF_SIZE)
    with _ctx_lock:
        rc = _lib.api_chat(_ctx, tid, uid_b, msg_b, ctx_json, out, ctypes.c_size_t(OL_BUF_SIZE), STREAM_TOKEN_CB(), None)
        stats = ApiStats()
        _lib.api_get_stats(_ctx, ctypes.byref(stats))

    if rc != 0:
        return (
            jsonify(
                {
                    "error": "api_chat failed",
                    "hint": "Is Ollama running? For cloud mode set GROQ_API_KEY / CEREBRAS_API_KEY / GEMINI_API_KEY.",
                }
            ),
            502,
        )
    reply = out.value.decode("utf-8", errors="replace").strip() or "(empty)"
    out_json: Dict[str, Any] = {"reply": reply}
    lm = bytes(stats.last_llm_model).split(b"\x00", 1)[0].decode("utf-8", errors="replace").strip()
    last_src_c = stats.last_reply_source
    wire_u = int(stats.last_chat_wire)
    out_json["source"] = completion_source_label(last_src_c, lm or None)
    if lm:
        out_json["llm_model"] = lm
    if wire_u:
        out_json["chat_wire"] = CHAT_WIRE_LABELS.get(wire_u, str(wire_u))
    if jwt_claims is not None:
        uj = str(jwt_claims.get("user_id") or "").strip()
        out_json.update(_tenant_json(tid, uj if uj else None))
    else:
        out_json.update(_tenant_json(tid))
    return jsonify(out_json)


@_app.route("/api/chat/stream", methods=["POST"])
def chat_stream():
    """
    SSE token stream. ``M4_CHAT_STREAM_MODE`` unset/empty → ``router`` (see ``stream_chat_backends.resolved_chat_stream_mode``).
    ``ollama``: c-lib ``api_chat_stream`` only. ``router``: Redis RAG short-circuit, then hosted tiers (``M4_CLOUD_TRY_ORDER``)
    before Ollama (so ``OLLAMA_MODEL`` does not skip Groq/Gemini/Cerebras). Sink contract: provider-agnostic JSON events.
    """
    data = request.get_json(silent=True) or {}
    message_raw = (data.get("message") or "").strip()
    if not message_raw:
        return jsonify({"error": "message required"}), 400
    stream_jwt_claims: Optional[Dict[str, Any]] = None
    if _chat_require_jwt():
        stream_jwt_claims = require_jwt()
        uid = str(stream_jwt_claims.get("user_id") or "").strip()
        if not uid:
            raise Unauthorized(description="token has no user_id — sign in again")
        tid = API_DEFAULT_TENANT_ID
        user_s = uid
    else:
        tid = _tenant_id_bytes(data.get("tenant_id") or data.get("user"))
        user_raw = data.get("user") or data.get("tenant_id") or "default"
        user_s = str(user_raw).strip() or "default"
    tmid = (data.get("temp_message_id") or "").strip()
    stream_mode = resolved_chat_stream_mode(os.environ.get("M4_CHAT_STREAM_MODE"))

    stream_ctx_json = _build_context_json(stream_jwt_claims)
    print(
        f"[M4][python] api_chat STREAM: tenant={tid!r} user={user_s!r} "
        f"message_len={len(message_raw)} context_json={stream_ctx_json!r} mode={stream_mode}",
        file=sys.stderr,
    )

    def sse_stream() -> Generator[str, None, None]:
        # SSE comment: opens the chunked body immediately so proxies/clients see a live stream
        # (some buffers wait for first bytes before forwarding).
        yield ": stream\n\n"
        q: queue.Queue = queue.Queue()

        def worker(jc: Optional[Dict[str, Any]] = stream_jwt_claims):
            try:
                with _ctx_lock:
                    dispatch_chat_stream(
                        mode=stream_mode,
                        lib=_lib,
                        ctx=_ctx,
                        tid=tid,
                        user_s=user_s,
                        message=message_raw,
                        context_json=stream_ctx_json,
                        temp_message_id=tmid,
                        sink=q.put,
                        gil_held_for_c_callback=gil_held_for_c_callback,
                    )
            except Exception as ex:
                q.put(
                    {
                        "token": "",
                        "temp_message_id": tmid or "",
                        "done": True,
                        "error": str(ex),
                    }
                )
            finally:
                q.put(None)

        th = threading.Thread(target=worker, daemon=True)
        th.start()
        try:
            while True:
                item = q.get()
                if item is None:
                    break
                yield "data: " + json.dumps(item) + "\n\n"
        finally:
            th.join(timeout=7200.0)

    return Response(
        stream_with_context(sse_stream()),
        mimetype="text/event-stream; charset=utf-8",
        headers={
            "Cache-Control": "no-cache, no-transform",
            "Connection": "keep-alive",
            "X-Accel-Buffering": "no",
        },
    )


def main():
    _init_engine()
    host = os.environ.get("M4ENGINE_SERVER_HOST", "127.0.0.1")
    port, skipped = _pick_listen_port(host)
    if skipped is not None and not os.environ.get("M4ENGINE_SERVER_PORT", "").strip():
        print(
            f"Note: port {skipped} was in use; using {port}. "
            f"Set M4ENGINE_SERVER_PORT={port} for clients (e.g. VITE_API_URL).",
            file=sys.stderr,
        )
    print(f"M4 chat server http://{host}:{port}")
    # threaded=True so /api/health and other clients work while a long SSE stream runs.
    _app.run(host=host, port=port, threaded=True)


if __name__ == "__main__":
    main()
