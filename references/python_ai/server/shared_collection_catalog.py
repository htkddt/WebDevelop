# -*- coding: utf-8 -*-
"""
Declarative **SharedCollection** shapes for the app store MongoDB collections (see
``c-lib/.cursor/shared_collection.md`` §2). Used to seed ``shared_collections`` and to export JSON for
``api_options_t.shared_collection_json_path`` (c-lib / tooling).

Internal names match collection names: ``products``, ``carts``, ``product_categories``.
"""
from __future__ import annotations

import copy
import json
import os
from typing import Any, Dict, List, Optional

# Written by ``ensure_shared_collection_registry_json_file`` for ``api_options_t.shared_collection_json_path``.
_REGISTRY_FILENAME = "shared_collection_registry.json"
# Optional overlay: merged after Mongo (when present). Wins on conflicts so ``elk.allow`` etc. match this file.
_STORE_FILENAME = "shared_collections.store.json"

# One object per logical collection; field names align with the spec (collection, alias, public, …).
DEFAULT_STORE_SHARED_COLLECTIONS: List[Dict[str, Any]] = [
    {
        "collection": "products",
        "alias": "store_products",
        "public": [
            "sku",
            "name",
            "price",
            "quantity_available",
            "category",
            "image_url",
            "published",
        ],
        "sensitive": [],
        "metadata": {
            "description": "Published catalog SKUs: pricing, stock, and category code for storefront and admin storage.",
            "allow_prompt": True,
            "field_hints": {
                "sku": "Stable product code (unique).",
                "category": "Category code; aligns with product_categories.code and distinct product.category.",
                "quantity_available": "Stock on hand; guest checkout validates against this.",
                "published": "If false, item is hidden from GET /api/store/products.",
            },
            "sample_injection": 0,
        },
        "elk": {"allow": True, "index": "", "transform": False},
        "vector_engine": {
            "allow": False,
            "source_fields": ["name", "sku", "category"],
            "dim": 0,
            "metric": "cosine",
            "l1_cache": False,
            "fallback_only": False,
        },
    },
    {
        "collection": "carts",
        "alias": "shopping_carts",
        "public": ["cart_key", "lines", "updated_at"],
        "sensitive": [],
        "metadata": {
            "description": "Guest or user cart: resolved line items with sku, name, qty, unit_price.",
            "allow_prompt": True,
            "field_hints": {
                "cart_key": "Stable cart id (guest header or user-derived key).",
                "lines": "Array of {product_id, sku, name, qty, unit_price} after server validation.",
                "updated_at": "Last save time (UTC).",
            },
            "sample_injection": 0,
        },
        "elk": {"allow": True, "index": "", "transform": False},
        "vector_engine": {
            "allow": False,
            "source_fields": [],
            "dim": 0,
            "metric": "cosine",
            "l1_cache": False,
            "fallback_only": False,
        },
        "joins": [
            {
                "from": "lines.product_id",
                "to_collection": "products",
                "to_field": "_id",
                "note": "Line items reference catalog documents by ObjectId string.",
            }
        ],
    },
    {
        "collection": "product_categories",
        "alias": "product_categories",
        "public": ["code", "name", "active"],
        "sensitive": [],
        "metadata": {
            "description": "Registry of catalog category codes (merged with distinct product.category in APIs).",
            "allow_prompt": True,
            "field_hints": {
                "code": "Lowercase slug used on products.category.",
                "active": "Inactive categories may still appear on old products; UI typically filters.",
            },
            "sample_injection": 0,
        },
        "elk": {"allow": True, "index": "", "transform": False},
        "vector_engine": {
            "allow": False,
            "source_fields": ["name", "code"],
            "dim": 0,
            "metric": "cosine",
            "l1_cache": False,
            "fallback_only": False,
        },
    },
]


def catalog_collections_copy() -> List[Dict[str, Any]]:
    """Deep copy for responses / seeding (avoid mutating module defaults)."""
    return copy.deepcopy(DEFAULT_STORE_SHARED_COLLECTIONS)


def shared_collections_store_json_path() -> str:
    """``server/data/shared_collections.store.json`` — merged last so ``elk.allow`` overrides Mongo seed rows."""
    server_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(server_dir, "data", _STORE_FILENAME)


def _merge_overlay_row(dst: Dict[str, Any], src: Dict[str, Any]) -> None:
    skip = {"_id", "updated_at", "seed_source", "id"}
    for k, v in src.items():
        if k in skip:
            continue
        if k in dst and isinstance(dst[k], dict) and isinstance(v, dict):
            _merge_overlay_row(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)


def _load_store_file_collections(path: str) -> Optional[List[Dict[str, Any]]]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return None
    cols = data.get("collections")
    if not isinstance(cols, list):
        return None
    return cols


def load_collections_for_registry() -> List[Dict[str, Any]]:
    """
    Rows for c-lib ``shared_collection_registry.json`` and ``GET /api/meta/shared-collections``.

    Order: catalog → Mongo ``shared_collections`` (if any) → ``shared_collections.store.json`` (if present;
    wins on conflict).
    """
    order = [c["collection"] for c in DEFAULT_STORE_SHARED_COLLECTIONS]
    by_name = {c["collection"]: copy.deepcopy(c) for c in DEFAULT_STORE_SHARED_COLLECTIONS}

    try:
        from shared_collection_store import iter_mongo_shared_collections_raw

        mongo_rows = iter_mongo_shared_collections_raw()
    except ImportError:
        mongo_rows = None
    if mongo_rows:
        for row in mongo_rows:
            name = row.get("collection")
            if isinstance(name, str) and name in by_name:
                _merge_overlay_row(by_name[name], row)

    store_path = shared_collections_store_json_path()
    if os.path.isfile(store_path):
        store_cols = _load_store_file_collections(store_path)
        if store_cols:
            for row in store_cols:
                name = row.get("collection")
                if isinstance(name, str) and name in by_name:
                    _merge_overlay_row(by_name[name], row)

    return [by_name[n] for n in order]


def catalog_json_bytes(*, indent: int = 2) -> bytes:
    """JSON for SharedCollection registry (merged catalog + Mongo + ``shared_collections.store.json``)."""
    payload = {"collections": load_collections_for_registry()}
    return json.dumps(payload, indent=indent, ensure_ascii=False).encode("utf-8")


def shared_collection_registry_json_path() -> str:
    """Absolute path to the generated registry file (``server/data/``)."""
    server_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(server_dir, "data", _REGISTRY_FILENAME)


def ensure_shared_collection_registry_json_file(*, refresh: bool = False) -> str:
    """
    Write registry JSON for c-lib from :func:`load_collections_for_registry`.

    Always overwrites so Mongo and ``shared_collections.store.json`` changes apply on engine init.
    ``refresh`` is retained for callers only.
    """
    _ = refresh
    path = shared_collection_registry_json_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(catalog_json_bytes())
    return os.path.abspath(path)
