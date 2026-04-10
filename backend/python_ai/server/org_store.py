# -*- coding: utf-8 -*-
"""Org units (``departments``) and catalog category registry (``product_categories``) — app Mongo or in-memory."""
from __future__ import annotations

import re
import threading
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from app_mongo import get_app_database, identity_persists_mongo

_lock = threading.Lock()
_mem_departments: Dict[str, Dict[str, Any]] = {}
_mem_product_categories: Dict[str, Dict[str, Any]] = {}

_CODE_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _norm_code(raw: str) -> str:
    return (raw or "").strip().lower()


def _validate_code(code: str) -> str:
    c = _norm_code(code)
    if not _CODE_RE.match(c):
        raise ValueError("invalid code: lowercase letters, digits, hyphen; 1–32 chars")
    return c


def _dept_col():
    return get_app_database()["departments"]


def _pcat_col():
    return get_app_database()["product_categories"]


def _mongo_doc(d: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not d:
        return None
    return {k: v for k, v in d.items() if k != "_id"}


def init_seed_org_catalog() -> None:
    """Idempotent seed: default departments + product categories."""
    if identity_persists_mongo():
        _init_seed_org_mongo()
    else:
        _init_seed_org_memory()


def _default_departments() -> List[Dict[str, Any]]:
    return [
        {"code": "hr", "name": "Human Resources", "description": "HR — users, policies, org structure."},
        {"code": "sale", "name": "Sales", "description": "Sales — catalog read, quotes."},
        {"code": "storage", "name": "Storage / Warehouse", "description": "Inventory and product intake."},
        {"code": "finance", "name": "Finance", "description": "Carts, orders, export."},
        {"code": "delivery", "name": "Delivery", "description": "Ship and deliver confirmed packed orders."},
        {
            "code": "logistics",
            "name": "Logistics & delivery",
            "description": "Last-mile coordination — same order-desk powers as Delivery when role is DELIVERY.",
        },
        {"code": "general", "name": "General", "description": "Default / cross-functional."},
    ]


def _default_product_categories() -> List[Dict[str, Any]]:
    return [
        {"code": "general", "name": "General", "active": True},
        {"code": "electronics", "name": "Electronics", "active": True},
    ]


def _init_seed_org_memory() -> None:
    global _mem_departments, _mem_product_categories
    with _lock:
        if not _mem_departments:
            for d in _default_departments():
                _mem_departments[d["code"]] = {**d}
        if not _mem_product_categories:
            for c in _default_product_categories():
                _mem_product_categories[c["code"]] = {**c}


def _init_seed_org_mongo() -> None:
    col = _dept_col()
    for d in _default_departments():
        if col.find_one({"code": d["code"]}):
            continue
        col.insert_one({**d, "created_at": _now()})
    pcol = _pcat_col()
    for c in _default_product_categories():
        if pcol.find_one({"code": c["code"]}):
            continue
        pcol.insert_one({**c, "created_at": _now()})


def list_departments() -> List[Dict[str, Any]]:
    if identity_persists_mongo():
        out = []
        for d in _dept_col().find({}).sort("code", 1):
            x = _mongo_doc(d)
            if x:
                out.append(
                    {
                        "code": x["code"],
                        "name": x.get("name") or x["code"],
                        "description": x.get("description"),
                    }
                )
        return out
    with _lock:
        _init_seed_org_memory()
        return [
            {
                "code": c,
                "name": v.get("name") or c,
                "description": v.get("description"),
            }
            for c, v in sorted(_mem_departments.items(), key=lambda kv: kv[0])
        ]


def get_department(code: str) -> Optional[Dict[str, Any]]:
    c = _norm_code(code)
    if not c:
        return None
    if identity_persists_mongo():
        return _mongo_doc(_dept_col().find_one({"code": c}))
    with _lock:
        _init_seed_org_memory()
        v = _mem_departments.get(c)
        return dict(v) if v else None


def department_exists(code: Optional[str]) -> bool:
    if not code or not str(code).strip():
        return True
    return get_department(str(code)) is not None


def create_department(code: str, name: str, description: Optional[str] = None) -> Dict[str, Any]:
    c = _validate_code(code)
    nm = (name or "").strip() or c
    desc = (description or "").strip() or None
    if identity_persists_mongo():
        from pymongo.errors import DuplicateKeyError

        doc = {"code": c, "name": nm, "description": desc, "created_at": _now()}
        try:
            _dept_col().insert_one(doc)
        except DuplicateKeyError as e:
            raise ValueError("department code already exists") from e
        return {"code": c, "name": nm, "description": desc}
    with _lock:
        _init_seed_org_memory()
        if c in _mem_departments:
            raise ValueError("department code already exists")
        _mem_departments[c] = {"code": c, "name": nm, "description": desc}
        return {"code": c, "name": nm, "description": desc}


def update_department(code: str, *, name: Optional[str] = None, description: Optional[str] = None) -> Optional[Dict[str, Any]]:
    c = _norm_code(code)
    if not c:
        return None
    patch: Dict[str, Any] = {}
    if name is not None:
        patch["name"] = (name or "").strip() or c
    if description is not None:
        patch["description"] = (description or "").strip() or None
    if identity_persists_mongo():
        r = _dept_col().update_one({"code": c}, {"$set": patch})
        if r.matched_count == 0:
            return None
        return _mongo_doc(_dept_col().find_one({"code": c}))
    with _lock:
        _init_seed_org_memory()
        if c not in _mem_departments:
            return None
        _mem_departments[c].update(patch)
        d = _mem_departments[c]
        return {"code": d["code"], "name": d.get("name"), "description": d.get("description")}


def list_product_categories() -> List[Dict[str, Any]]:
    if identity_persists_mongo():
        out = []
        for d in _pcat_col().find({}).sort("code", 1):
            x = _mongo_doc(d)
            if x:
                out.append(
                    {
                        "code": x["code"],
                        "name": x.get("name") or x["code"],
                        "active": bool(x.get("active", True)),
                    }
                )
        return out
    with _lock:
        _init_seed_org_memory()
        return [
            {
                "code": c,
                "name": v.get("name") or c,
                "active": bool(v.get("active", True)),
            }
            for c, v in sorted(_mem_product_categories.items(), key=lambda kv: kv[0])
        ]


def create_product_category(code: str, name: str, *, active: bool = True) -> Dict[str, Any]:
    c = _validate_code(code)
    nm = (name or "").strip() or c
    if identity_persists_mongo():
        from pymongo.errors import DuplicateKeyError

        doc = {"code": c, "name": nm, "active": bool(active), "created_at": _now()}
        try:
            _pcat_col().insert_one(doc)
        except DuplicateKeyError as e:
            raise ValueError("category code already exists") from e
        return {"code": c, "name": nm, "active": bool(active)}
    with _lock:
        _init_seed_org_memory()
        if c in _mem_product_categories:
            raise ValueError("category code already exists")
        _mem_product_categories[c] = {"code": c, "name": nm, "active": bool(active)}
        return {"code": c, "name": nm, "active": bool(active)}


def update_product_category(
    code: str,
    *,
    name: Optional[str] = None,
    active: Optional[bool] = None,
) -> Optional[Dict[str, Any]]:
    c = _norm_code(code)
    if not c:
        return None
    patch: Dict[str, Any] = {}
    if name is not None:
        patch["name"] = (name or "").strip() or c
    if active is not None:
        patch["active"] = bool(active)
    if identity_persists_mongo():
        if not patch:
            return _mongo_doc(_pcat_col().find_one({"code": c}))
        r = _pcat_col().update_one({"code": c}, {"$set": patch})
        if r.matched_count == 0:
            return None
        return _mongo_doc(_pcat_col().find_one({"code": c}))
    with _lock:
        _init_seed_org_memory()
        if c not in _mem_product_categories:
            return None
        _mem_product_categories[c].update(patch)
        x = _mem_product_categories[c]
        return {"code": x["code"], "name": x.get("name"), "active": bool(x.get("active", True))}


def active_product_category_codes() -> List[str]:
    return sorted(c["code"] for c in list_product_categories() if c.get("active"))


def list_product_category_codes_merged(*, product_distinct_categories: List[str]) -> List[str]:
    """Union of registry (active) + values already used on products (for admin filters)."""
    reg = set(active_product_category_codes())
    reg.update(str(x).strip().lower() for x in product_distinct_categories if x and str(x).strip())
    return sorted(reg)
