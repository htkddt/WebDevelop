# -*- coding: utf-8 -*-
"""App-layer MongoDB — single database (``M4_APP_MONGO_DB``, default ``product``).

Collections used by Flask (not c-lib): ``users``, ``policies``, ``user_policies``, ``departments``,
``product_categories``, ``products``, ``carts``, ``orders``, ``access_pages``, ``shared_collections``
(SharedCollection-style config for store DB — see ``shared_collection_catalog.py``).
See docs/ABAC_PRODUCT_TEST_PLAN.md.

Separate from ``M4ENGINE_MONGO_URI`` / c-lib, which uses the ``bot`` database for chat/geo.
"""
from __future__ import annotations

import os
import threading
from typing import Any, Dict, Optional

_ENV_URI = "M4_APP_MONGO_URI"
_ENV_DB = "M4_APP_MONGO_DB"
_ENV_DISABLE = "M4_APP_MONGO_DISABLE"

_DEFAULT_URI = "mongodb://127.0.0.1:27017/?directConnection=true"
_DEFAULT_DB = "product"

_lock = threading.Lock()
_client: Any = None


def app_mongo_uri() -> str:
    v = os.environ.get(_ENV_URI, "").strip()
    return v if v else _DEFAULT_URI


def app_mongo_db_name() -> str:
    v = os.environ.get(_ENV_DB, "").strip()
    return v if v else _DEFAULT_DB


def app_mongo_disabled() -> bool:
    return os.environ.get(_ENV_DISABLE, "").strip().lower() in ("1", "true", "yes")


def get_app_mongo_client():
    """Lazy singleton pymongo.MongoClient for the app database."""
    global _client
    if app_mongo_disabled():
        raise RuntimeError("app Mongo disabled (set M4_APP_MONGO_DISABLE)")
    from pymongo import MongoClient

    if _client is None:
        with _lock:
            if _client is None:
                _client = MongoClient(app_mongo_uri(), serverSelectionTimeoutMS=8000)
    return _client


def get_app_database():
    """Return Mongo database named by ``M4_APP_MONGO_DB`` (default ``product``)."""
    return get_app_mongo_client()[app_mongo_db_name()]


def ensure_app_data_indexes() -> None:
    """Indexes for app-layer collections: products, carts, users, policies, user_policies, orders."""
    if app_mongo_disabled():
        return
    db = get_app_database()
    db.products.create_index("sku", unique=True)
    db.products.create_index([("published", 1), ("category", 1)])
    db.carts.create_index("cart_key", unique=True)
    db.users.create_index("email", unique=True)
    db.users.create_index("id", unique=True)
    db.policies.create_index("id", unique=True)
    db.user_policies.create_index("user_id", unique=True)
    db.departments.create_index("code", unique=True)
    db.product_categories.create_index("code", unique=True)
    db.orders.create_index("id", unique=True)
    db.orders.create_index("status")
    db.orders.create_index("delivery_assignee_user_id")
    db.orders.create_index("customer_email")
    db.orders.create_index("created_at")
    db.access_pages.create_index("pageKey", unique=True)
    db.access_pages.create_index("kind")
    db.shared_collections.create_index("collection", unique=True)


_identity_mongo_active: Optional[bool] = None


def identity_persists_mongo() -> bool:
    """
    Whether users/policies use MongoDB (``users``, ``policies``, ``user_policies`` collections).

    Cached after first check: app Mongo not disabled, indexes ensured, ping succeeds.
    Otherwise in-memory stores in ``user_store`` / ``policy_store``.
    """
    global _identity_mongo_active
    if _identity_mongo_active is not None:
        return _identity_mongo_active
    if app_mongo_disabled():
        _identity_mongo_active = False
        return False
    try:
        ensure_app_data_indexes()
        get_app_database().command("ping")
        _identity_mongo_active = True
    except Exception:
        _identity_mongo_active = False
    return _identity_mongo_active


def app_mongo_health() -> Dict[str, Any]:
    """Ping for ``GET /api/health``; never raises."""
    if app_mongo_disabled():
        return {"enabled": False, "skip": "M4_APP_MONGO_DISABLE"}

    try:
        from pymongo import MongoClient
    except ImportError:
        return {
            "enabled": False,
            "skip": "pymongo not installed",
        }

    dbn = app_mongo_db_name()
    uri = app_mongo_uri()
    try:
        c = MongoClient(uri, serverSelectionTimeoutMS=3000)
        c.admin.command("ping")
        c.close()
        return {"enabled": True, "reachable": True, "db": dbn}
    except Exception as e:
        return {"enabled": True, "reachable": False, "db": dbn, "error": str(e)}
