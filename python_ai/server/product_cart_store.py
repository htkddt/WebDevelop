# -*- coding: utf-8 -*-
"""Products and carts in app MongoDB (``M4_APP_MONGO_*``) — see docs/ABAC_PRODUCT_TEST_PLAN.md."""
from __future__ import annotations

import re
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from bson import ObjectId
from bson.errors import InvalidId

from app_mongo import app_mongo_disabled, ensure_app_data_indexes, get_app_database

_SKU_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$")


def _db():
    if app_mongo_disabled():
        raise RuntimeError("app Mongo disabled")
    return get_app_database()


def _now() -> datetime:
    return datetime.now(timezone.utc)


def ensure_catalog_indexes() -> None:
    """Ensure all app DB indexes (catalog + identity). No-op if app Mongo disabled."""
    if app_mongo_disabled():
        return
    ensure_app_data_indexes()


def _product_out(doc: Dict[str, Any], *, admin: bool = False) -> Dict[str, Any]:
    if not doc:
        return {}
    oid = str(doc["_id"])
    out: Dict[str, Any] = {
        "id": oid,
        "sku": doc.get("sku") or "",
        "name": doc.get("name") or "",
        "price": float(doc.get("price") or 0),
        "quantity_available": int(doc.get("quantity_available") or 0),
        "category": doc.get("category") or "",
        "image_url": doc.get("image_url") or "",
    }
    if admin:
        out["published"] = bool(doc.get("published"))
        out["created_at"] = doc.get("created_at")
        out["updated_at"] = doc.get("updated_at")
    return out


def init_seed_products_if_empty() -> None:
    """Insert a few demo rows when ``products`` is empty (dev convenience)."""
    try:
        db = _db()
    except RuntimeError:
        return
    ensure_catalog_indexes()
    if db.products.estimated_document_count() > 0:
        return
    demo = [
        {
            "sku": "SKU-1001",
            "name": "Sample item A",
            "price": 19.99,
            "quantity_available": 50,
            "category": "general",
            "image_url": "",
            "published": True,
            "created_at": _now(),
            "updated_at": _now(),
        },
        {
            "sku": "SKU-1002",
            "name": "Sample item B",
            "price": 29.5,
            "quantity_available": 30,
            "category": "general",
            "image_url": "",
            "published": True,
            "created_at": _now(),
            "updated_at": _now(),
        },
        {
            "sku": "SKU-1003",
            "name": "Sample item C",
            "price": 9.0,
            "quantity_available": 100,
            "category": "electronics",
            "image_url": "",
            "published": True,
            "created_at": _now(),
            "updated_at": _now(),
        },
    ]
    db.products.insert_many(demo)


def list_categories_published() -> List[str]:
    from org_store import list_product_category_codes_merged

    db = _db()
    raw = [
        str(x)
        for x in db.products.distinct("category", {"published": True})
        if x is not None and str(x).strip()
    ]
    return list_product_category_codes_merged(product_distinct_categories=raw)


def list_store_products(*, category: Optional[str] = None) -> List[Dict[str, Any]]:
    db = _db()
    q: Dict[str, Any] = {"published": True, "quantity_available": {"$gt": 0}}
    if category:
        q["category"] = category.strip()
    cur = db.products.find(q).sort("sku", 1)
    return [_product_out(d, admin=False) for d in cur]


def get_product_by_id(oid: str) -> Optional[Dict[str, Any]]:
    try:
        _id = ObjectId(oid)
    except InvalidId:
        return None
    db = _db()
    return db.products.find_one({"_id": _id})


def admin_get_product(oid: str) -> Optional[Dict[str, Any]]:
    doc = get_product_by_id(oid)
    return _product_out(doc, admin=True) if doc else None


def get_product_store_view(oid: str) -> Optional[Dict[str, Any]]:
    doc = get_product_by_id(oid)
    if not doc or not doc.get("published"):
        return None
    if int(doc.get("quantity_available") or 0) <= 0:
        return None
    return _product_out(doc, admin=False)


def admin_list_products(
    *,
    category: Optional[str] = None,
    published: Optional[str] = None,
) -> List[Dict[str, Any]]:
    db = _db()
    q: Dict[str, Any] = {}
    if category:
        q["category"] = category.strip()
    if published == "true":
        q["published"] = True
    elif published == "false":
        q["published"] = False
    cur = db.products.find(q).sort("sku", 1)
    return [_product_out(d, admin=True) for d in cur]


def admin_categories() -> List[str]:
    from org_store import list_product_category_codes_merged

    db = _db()
    raw = [str(x) for x in db.products.distinct("category") if x is not None and str(x).strip()]
    return list_product_category_codes_merged(product_distinct_categories=raw)


def _validate_sku(sku: str) -> str:
    s = (sku or "").strip()
    if not _SKU_RE.match(s):
        raise ValueError("invalid sku (alphanumeric, dot, hyphen; max 63 chars)")
    return s


def admin_create_product(data: Dict[str, Any]) -> Dict[str, Any]:
    db = _db()
    sku = _validate_sku(str(data.get("sku") or ""))
    if db.products.find_one({"sku": sku}):
        raise ValueError("sku already exists")
    doc = {
        "sku": sku,
        "name": (data.get("name") or "").strip() or sku,
        "price": float(data.get("price") or 0),
        "quantity_available": max(0, int(data.get("quantity_available") or 0)),
        "category": (data.get("category") or "general").strip() or "general",
        "image_url": (data.get("image_url") or "").strip(),
        "published": bool(data.get("published")),
        "created_at": _now(),
        "updated_at": _now(),
    }
    r = db.products.insert_one(doc)
    doc["_id"] = r.inserted_id
    return _product_out(doc, admin=True)


def admin_update_product(oid: str, data: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    try:
        _id = ObjectId(oid)
    except InvalidId:
        return None
    db = _db()
    cur = db.products.find_one({"_id": _id})
    if not cur:
        return None
    patch: Dict[str, Any] = {"updated_at": _now()}
    if "name" in data:
        patch["name"] = (data.get("name") or "").strip() or cur.get("name")
    if "price" in data:
        patch["price"] = float(data.get("price") or 0)
    if "quantity_available" in data:
        patch["quantity_available"] = max(0, int(data.get("quantity_available") or 0))
    if "category" in data:
        patch["category"] = (data.get("category") or "general").strip() or "general"
    if "image_url" in data:
        patch["image_url"] = (data.get("image_url") or "").strip()
    if "published" in data:
        patch["published"] = bool(data.get("published"))
    if "sku" in data:
        new_sku = _validate_sku(str(data.get("sku") or ""))
        if new_sku != cur.get("sku") and db.products.find_one({"sku": new_sku}):
            raise ValueError("sku already exists")
        patch["sku"] = new_sku
    db.products.update_one({"_id": _id}, {"$set": patch})
    doc = db.products.find_one({"_id": _id})
    return _product_out(doc or {}, admin=True) if doc else None


def admin_bulk_upsert(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    db = _db()
    ok = 0
    errors: List[Dict[str, Any]] = []
    for i, raw in enumerate(rows):
        if not isinstance(raw, dict):
            errors.append({"index": i, "error": "not an object"})
            continue
        try:
            sku = _validate_sku(str(raw.get("sku") or ""))
        except ValueError as e:
            errors.append({"index": i, "error": str(e)})
            continue
        doc = {
            "sku": sku,
            "name": (raw.get("name") or "").strip() or sku,
            "price": float(raw.get("price") or 0),
            "quantity_available": max(0, int(raw.get("quantity_available") or 0)),
            "category": (raw.get("category") or "general").strip() or "general",
            "image_url": (raw.get("image_url") or "").strip(),
            "published": bool(raw.get("published", False)),
            "updated_at": _now(),
        }
        existing = db.products.find_one({"sku": sku})
        if existing:
            db.products.update_one({"_id": existing["_id"]}, {"$set": doc})
        else:
            doc["created_at"] = _now()
            db.products.insert_one(doc)
        ok += 1
    return {"ok": True, "imported": ok, "errors": errors}


def get_cart_doc(cart_key: str) -> Dict[str, Any]:
    db = _db()
    doc = db.carts.find_one({"cart_key": cart_key})
    if doc:
        return doc
    return {"cart_key": cart_key, "lines": [], "updated_at": _now()}


def _validate_cart_lines(lines: Any) -> List[Dict[str, Any]]:
    if not isinstance(lines, list):
        raise ValueError("lines must be an array")
    out: List[Dict[str, Any]] = []
    seen: set[str] = set()
    for item in lines:
        if not isinstance(item, dict):
            raise ValueError("each line must be an object")
        pid = str(item.get("product_id") or "").strip()
        qty = int(item.get("qty") or 0)
        if not pid or qty < 1:
            continue
        if pid in seen:
            raise ValueError(f"duplicate product_id {pid}")
        seen.add(pid)
        out.append({"product_id": pid, "qty": qty})
    return out


def save_cart(cart_key: str, lines: Any) -> Dict[str, Any]:
    normalized = _validate_cart_lines(lines)
    db = _db()
    resolved: List[Dict[str, Any]] = []
    for ln in normalized:
        doc = get_product_by_id(ln["product_id"])
        if not doc:
            raise ValueError(f"unknown product_id {ln['product_id']}")
        if not doc.get("published"):
            raise ValueError(f"product {ln['product_id']} is not published")
        avail = int(doc.get("quantity_available") or 0)
        if ln["qty"] > avail:
            raise ValueError(f"qty exceeds stock for {doc.get('sku')}")
        resolved.append(
            {
                "product_id": ln["product_id"],
                "sku": doc.get("sku") or "",
                "name": doc.get("name") or "",
                "qty": ln["qty"],
                "unit_price": float(doc.get("price") or 0),
            }
        )
    now = _now()
    db.carts.update_one(
        {"cart_key": cart_key},
        {"$set": {"lines": resolved, "updated_at": now}},
        upsert=True,
    )
    return cart_public(cart_key)


def cart_public(cart_key: str) -> Dict[str, Any]:
    db = _db()
    doc = db.carts.find_one({"cart_key": cart_key}) or {"lines": []}
    lines = doc.get("lines") or []
    subtotal = sum(float(x.get("unit_price", 0)) * int(x.get("qty", 0)) for x in lines)
    return {
        "cart_key": cart_key,
        "lines": lines,
        "line_count": len(lines),
        "subtotal": round(subtotal, 2),
        "updated_at": doc.get("updated_at"),
    }


def clear_cart(cart_key: str) -> None:
    db = _db()
    db.carts.update_one(
        {"cart_key": cart_key},
        {"$set": {"lines": [], "updated_at": _now()}},
        upsert=True,
    )


def decrement_product_stock(product_id: str, qty: int) -> None:
    if qty < 1:
        return
    try:
        _id = ObjectId(product_id)
    except InvalidId as e:
        raise ValueError("invalid product id") from e
    db = _db()
    r = db.products.update_one(
        {"_id": _id, "quantity_available": {"$gte": qty}},
        {"$inc": {"quantity_available": -qty}, "$set": {"updated_at": _now()}},
    )
    if r.modified_count != 1:
        raise ValueError("insufficient stock or product not found")


def increment_product_stock(product_id: str, qty: int) -> None:
    if qty < 1:
        return
    try:
        _id = ObjectId(product_id)
    except InvalidId as e:
        raise ValueError("invalid product id") from e
    db = _db()
    r = db.products.update_one(
        {"_id": _id},
        {"$inc": {"quantity_available": qty}, "$set": {"updated_at": _now()}},
    )
    if r.matched_count != 1:
        raise ValueError("product not found")
