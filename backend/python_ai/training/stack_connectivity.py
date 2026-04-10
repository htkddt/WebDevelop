# -*- coding: utf-8 -*-
"""
Lightweight reachability probes for Mongo / Redis / Elasticsearch (c-lib stack).
Used by ``training.full_options.resolve_engine_options_dict`` for HIGH→LOW fallback.

Opt out of probes: ``M4ENGINE_SKIP_STACK_PROBE=1`` (treat all tiers as up).
"""
from __future__ import annotations

import os
import socket
import urllib.error
import urllib.request
from typing import Dict


def probe_stack_connectivity_from_merged(merged: dict) -> Dict[str, bool]:
    """
    Return ``mongo_ok``, ``redis_ok``, ``elk_ok`` for the merged option dict (hosts/ports from
    ``merged`` with env fallbacks). MEMORY mode (0) skips network work and returns all True.
    """
    if os.environ.get("M4ENGINE_SKIP_STACK_PROBE", "").strip().lower() in ("1", "true", "yes"):
        return {"mongo_ok": True, "redis_ok": True, "elk_ok": True}
    try:
        mode = int(merged.get("mode") or 0)
    except (TypeError, ValueError):
        mode = 0
    if mode == 0:
        return {"mongo_ok": True, "redis_ok": True, "elk_ok": True}

    mongo_ok = _probe_mongo(merged)
    redis_ok = True if mode < 2 else _probe_redis(merged)
    elk_ok = True if mode < 3 else _probe_elk(merged)
    return {"mongo_ok": mongo_ok, "redis_ok": redis_ok, "elk_ok": elk_ok}


def _probe_mongo(merged: dict) -> bool:
    uri = merged.get("mongo_uri") or os.environ.get("M4ENGINE_MONGO_URI")
    if not uri:
        return False
    try:
        from pymongo import MongoClient
    except ImportError:
        return False
    try:
        c = MongoClient(str(uri), serverSelectionTimeoutMS=800)
        c.admin.command("ping")
        c.close()
        return True
    except Exception:
        return False


def _probe_redis(merged: dict) -> bool:
    host = merged.get("redis_host") or os.environ.get("M4ENGINE_REDIS_HOST") or "127.0.0.1"
    try:
        port = int(merged.get("redis_port") or os.environ.get("M4ENGINE_REDIS_PORT") or 6379)
    except (TypeError, ValueError):
        port = 6379
    if not str(host).strip():
        return False
    try:
        s = socket.create_connection((str(host).strip(), port), timeout=1.0)
        s.close()
        return True
    except OSError:
        return False


def _probe_elk(merged: dict) -> bool:
    host = merged.get("es_host") or os.environ.get("M4ENGINE_ES_HOST") or "127.0.0.1"
    try:
        port = int(merged.get("es_port") or os.environ.get("M4ENGINE_ES_PORT") or 9200)
    except (TypeError, ValueError):
        port = 9200
    if not str(host).strip():
        return False
    url = f"http://{str(host).strip()}:{port}/"
    try:
        with urllib.request.urlopen(url, timeout=1.5) as r:
            r.read(64)
        return True
    except (urllib.error.URLError, OSError, TimeoutError, ValueError):
        return False
