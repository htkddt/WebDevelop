# -*- coding: utf-8 -*-
"""Persist c-lib ``api_options_t`` tunables (subset) in app Mongo — see ``training.full_options.OPTION_KEYS``.

Engine startup (``app._init_engine``) merges these overrides on top of env via
``build_*_api_options_with_overrides``.

**Auto-init:** On app import, if there is no ``default`` row yet (Mongo) or no in-memory values yet,
``ensure_bot_c_lib_full_if_no_document()`` writes a full env snapshot (same as
``build_snapshot_bot_c_lib_values_for_db``). Existing rows are never overwritten.

Optional seed env (runs after auto-init):

- ``M4ENGINE_SEED_BOT_C_LIB=1`` — tiny demo row only if ``default`` doc is still missing (Mongo only).
- ``M4ENGINE_SEED_BOT_C_LIB=full`` — replace ``values`` with a fresh full snapshot (forced).

CLI: ``python3 scripts/seed_bot_c_lib_full.py`` (from ``python_ai/``).
"""
from __future__ import annotations

import os
import sys
import threading
from datetime import datetime, timezone
from typing import Any, Dict, Optional

from app_mongo import get_app_database, identity_persists_mongo

from training.full_options import (
    OPTION_KEYS,
    build_snapshot_bot_c_lib_values_for_db,
    normalize_stored_overrides,
    validate_client_bot_c_lib_values,
)

DOC_ID = "default"
_COLLECTION = "bot_c_lib_settings"
_ENV_SEED_DEMO = "M4ENGINE_SEED_BOT_C_LIB"


def _effective_use_max_for_server() -> bool:
    """Match ``USE_MAX_OPTIONS`` / ``admin_routes._server_uses_max_options``."""
    return os.environ.get("M4ENGINE_SERVER_MAX", "1").strip().lower() not in ("0", "false", "no")

_lock = threading.Lock()
_memory_values: Dict[str, Any] = {}


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _collection():
    return get_app_database()[_COLLECTION]


def ensure_bot_c_lib_settings_indexes() -> None:
    if not identity_persists_mongo():
        return
    _collection().create_index("id", unique=True)


def ensure_bot_c_lib_full_if_no_document() -> bool:
    """
    If no persisted ``default`` document exists (Mongo) or in-memory store is still empty, upsert a
    full ``OPTION_KEYS`` snapshot from current env. Returns True when a new row was written.
    """
    if identity_persists_mongo():
        ensure_bot_c_lib_settings_indexes()
        col = _collection()
        if col.find_one({"id": DOC_ID}, projection=["_id"]):
            return False
        out = upsert_bot_c_lib_full_options_snapshot(
            updated_by="system:ensure_bot_c_lib_full_if_no_document",
        )
        print(
            f"[M4] bot_c_lib_settings: created default document ({len(out)} keys) — none existed.",
            file=sys.stderr,
        )
        return True
    with _lock:
        if _memory_values:
            return False
    out = upsert_bot_c_lib_full_options_snapshot(
        updated_by="system:ensure_bot_c_lib_memory_init",
    )
    print(
        f"[M4] bot_c_lib_settings: initialized in-memory defaults ({len(out)} keys).",
        file=sys.stderr,
    )
    return True


def get_bot_c_lib_overrides() -> Dict[str, Any]:
    """Map of saved overrides (may be empty). Unknown keys stripped at save time."""
    if identity_persists_mongo():
        ensure_bot_c_lib_settings_indexes()
        doc = _collection().find_one({"id": DOC_ID})
        raw = (doc or {}).get("values") or {}
        if not isinstance(raw, dict):
            return {}
        return dict(raw)
    with _lock:
        return dict(_memory_values)


def save_bot_c_lib_settings(
    values: Dict[str, Any],
    *,
    replace: bool,
    updated_by: str,
) -> Dict[str, Any]:
    """
    ``replace`` True: stored map becomes ``normalize_stored_overrides(values)``.
    False: shallow-merge; pass ``{"redis_port": null}`` to remove a stored key (env base still used for preview only until you apply env + restart).

    Client ``values`` are validated with ``validate_client_bot_c_lib_values`` (unknown keys rejected).
    """
    validate_client_bot_c_lib_values(values or {})
    if identity_persists_mongo():
        ensure_bot_c_lib_settings_indexes()
        col = _collection()
        if replace:
            new_map = normalize_stored_overrides(values)
        else:
            cur = dict(get_bot_c_lib_overrides())
            for k, v in (values or {}).items():
                if k not in OPTION_KEYS:
                    continue
                if v is None:
                    cur.pop(k, None)
                else:
                    cur[k] = v
            new_map = normalize_stored_overrides(cur)
        col.update_one(
            {"id": DOC_ID},
            {
                "$set": {
                    "values": new_map,
                    "updated_at": _now(),
                    "updated_by": (updated_by or "").strip() or "unknown",
                }
            },
            upsert=True,
        )
        return get_bot_c_lib_overrides()
    with _lock:
        global _memory_values
        if replace:
            _memory_values = normalize_stored_overrides(values)
        else:
            cur = dict(_memory_values)
            for k, v in (values or {}).items():
                if k not in OPTION_KEYS:
                    continue
                if v is None:
                    cur.pop(k, None)
                else:
                    cur[k] = v
            _memory_values = normalize_stored_overrides(cur)
        return dict(_memory_values)


def seed_bot_c_lib_settings_demo_if_missing() -> bool:
    """
    If ``M4ENGINE_SEED_BOT_C_LIB`` is truthy and app Mongo is active and no ``default`` doc exists,
    upsert a small demo ``values`` map (``context_batch_size=35``) so you can confirm DB merge at
    ``api_create`` after restart. Safe no-op if a document already exists.
    """
    if os.environ.get(_ENV_SEED_DEMO, "").strip().lower() not in ("1", "true", "yes"):
        return False
    if not identity_persists_mongo():
        return False
    ensure_bot_c_lib_settings_indexes()
    col = _collection()
    if col.find_one({"id": DOC_ID}, projection=["_id"]):
        return False
    demo = normalize_stored_overrides(
        {
            # Distinct from c-lib default history window when env leaves this unset (0 → 30).
            "context_batch_size": 35,
        }
    )
    col.update_one(
        {"id": DOC_ID},
        {
            "$set": {
                "values": demo,
                "updated_at": _now(),
                "updated_by": f"seed:{_ENV_SEED_DEMO}",
            }
        },
        upsert=True,
    )
    return True


def upsert_bot_c_lib_full_options_snapshot(
    *,
    use_max: Optional[bool] = None,
    replace: bool = True,
    updated_by: str = "python:upsert_bot_c_lib_full_options_snapshot",
) -> Dict[str, Any]:
    """
    Write a full ``OPTION_KEYS`` snapshot (from env) to ``bot_c_lib_settings``. When ``use_max`` is
    None, uses ``M4ENGINE_SERVER_MAX`` the same way the Flask server picks ``get_max_options`` vs
    ``get_full_options`` for its base.
    """
    um = use_max if use_max is not None else _effective_use_max_for_server()
    values = build_snapshot_bot_c_lib_values_for_db(use_max=um)
    return save_bot_c_lib_settings(values, replace=replace, updated_by=updated_by)


def run_bot_c_lib_seed_from_env() -> None:
    """Handle ``M4ENGINE_SEED_BOT_C_LIB`` at process startup (see module docstring)."""
    raw = os.environ.get(_ENV_SEED_DEMO, "").strip().lower()
    if raw in ("1", "true", "yes"):
        if seed_bot_c_lib_settings_demo_if_missing():
            print(
                "[M4] Seeded bot_c_lib_settings demo (context_batch_size=35) for DB-merge smoke test.",
                file=sys.stderr,
            )
        return
    if raw == "full":
        try:
            out = upsert_bot_c_lib_full_options_snapshot(
                updated_by=f"seed:{_ENV_SEED_DEMO}:full",
            )
            print(
                f"[M4] Upserted full bot_c_lib_settings ({len(out)} keys) from env snapshot.",
                file=sys.stderr,
            )
        except Exception as exc:
            print(f"[M4] bot_c_lib full seed failed: {exc}", file=sys.stderr)
