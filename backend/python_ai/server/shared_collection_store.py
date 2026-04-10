# -*- coding: utf-8 -*-
"""Mongo ``shared_collections`` — SharedCollection config for store DB (see ``shared_collection_catalog``)."""
from __future__ import annotations

import os
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from app_mongo import app_mongo_disabled, get_app_database

from shared_collection_catalog import catalog_collections_copy

_COLLECTION = "shared_collections"


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _col():
    return get_app_database()[_COLLECTION]


def ensure_shared_collections_indexes() -> None:
    if app_mongo_disabled():
        return
    _col().create_index("collection", unique=True)


def ensure_shared_collections_seeded() -> None:
    """Insert catalog defaults when the collection is empty (idempotent)."""
    if app_mongo_disabled():
        return
    try:
        ensure_shared_collections_indexes()
        col = _col()
        if col.estimated_document_count() > 0:
            return
        for doc in catalog_collections_copy():
            row = dict(doc)
            row["updated_at"] = _now()
            row["seed_source"] = "shared_collection_catalog.DEFAULT_STORE_SHARED_COLLECTIONS"
            col.insert_one(row)
    except Exception:
        pass


def iter_mongo_shared_collections_raw() -> Optional[List[Dict[str, Any]]]:
    """Return raw Mongo docs or ``None`` if app DB disabled, empty, or error."""
    if app_mongo_disabled():
        return None
    try:
        ensure_shared_collections_indexes()
        col = _col()
        if col.estimated_document_count() == 0:
            return None
        return list(col.find({}).sort("collection", 1))
    except Exception:
        return None


def _strip_mongo_doc(doc: Dict[str, Any]) -> Dict[str, Any]:
    out = {k: v for k, v in doc.items() if k not in ("_id", "updated_at", "seed_source")}
    if "_id" in doc:
        out["id"] = str(doc["_id"])
    if "updated_at" in doc:
        out["updated_at"] = doc["updated_at"]
    return out


def list_shared_collections_meta() -> Dict[str, Any]:
    """
    Return ``{ collections, source }`` for ``GET /api/meta/shared-collections``.

    Collections match c-lib registry: :func:`shared_collection_catalog.load_collections_for_registry`.
    ``source`` describes which layers contributed (``store.json`` applied last).
    """
    from shared_collection_catalog import load_collections_for_registry, shared_collections_store_json_path

    cols = load_collections_for_registry()
    has_store = os.path.isfile(shared_collections_store_json_path())
    spec = "c-lib/.cursor/shared_collection.md §2 (store: products, carts, product_categories)"

    if app_mongo_disabled():
        return {"collections": cols, "source": "store" if has_store else "code", "spec": spec}

    try:
        ensure_shared_collections_indexes()
        col = _col()
        mongo_count = col.estimated_document_count()
    except Exception:
        return {"collections": cols, "source": "store" if has_store else "code_fallback", "spec": spec}

    if mongo_count == 0:
        out: Dict[str, Any] = {
            "collections": cols,
            "source": "store" if has_store else "code",
            "spec": spec,
        }
        if not has_store:
            out["note"] = (
                "Mongo enabled but collection empty; call ensure_shared_collections_seeded() or restart app."
            )
        return out

    return {
        "collections": cols,
        "source": "mongo+store" if has_store else "mongo",
        "spec": spec,
    }


def get_shared_collection_by_name(name: str) -> Optional[Dict[str, Any]]:
    """Single config by internal ``collection`` name, or None."""
    n = (name or "").strip()
    if not n:
        return None
    for row in list_shared_collections_meta()["collections"]:
        if row.get("collection") == n:
            return row
    return None
