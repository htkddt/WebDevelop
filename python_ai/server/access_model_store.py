# -*- coding: utf-8 -*-
"""Persist platform → module → page → ``uiMethods`` in MongoDB (``access_pages`` collection)."""
from __future__ import annotations

from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from access_model import (
    ACCESS_MODEL_VERSION,
    ADMIN_BOT_PAGE,
    ADMIN_DESK_PAGES,
    PLATFORMS,
    WEB_PAGES,
    access_model_public as access_model_public_static,
    build_web_menu_entries,
    build_web_nav_entries,
)
from app_mongo import app_mongo_disabled, get_app_database


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _access_pages_col():
    return get_app_database()["access_pages"]


def _access_platforms_col():
    return get_app_database()["access_platforms"]


def load_platforms_catalog() -> Dict[str, Dict[str, Any]]:
    """
    Platform id (uppercase) → ``{ "modules": [str, ...] }``.

    Mongo ``access_platforms`` doc ``_id: "catalog"`` merges **over** code defaults from
    ``access_model.PLATFORMS`` so new platforms can be added in DB only; WEB/ADMIN stay unless omitted.
    """
    base: Dict[str, Dict[str, Any]] = {k: {"modules": list(v.get("modules", []))} for k, v in PLATFORMS.items()}
    if app_mongo_disabled():
        return base
    try:
        doc = _access_platforms_col().find_one({"_id": "catalog"})
        raw = doc.get("platforms") if doc else None
        if isinstance(raw, dict) and raw:
            merged = dict(base)
            for key, val in raw.items():
                k = str(key).strip().upper()
                if not k:
                    continue
                mods: List[str] = []
                if isinstance(val, dict):
                    ml = val.get("modules")
                    if isinstance(ml, list):
                        mods = [str(m).strip() for m in ml if str(m).strip()]
                elif isinstance(val, list):
                    mods = [str(m).strip() for m in val if str(m).strip()]
                if mods or k not in merged:
                    merged[k] = {"modules": mods}
            return merged
    except Exception:
        pass
    return base


def ensure_access_platforms_seeded() -> None:
    """Insert catalog from code when missing (idempotent)."""
    if app_mongo_disabled():
        return
    try:
        col = _access_platforms_col()
        if col.find_one({"_id": "catalog"}):
            return
        col.insert_one(
            {
                "_id": "catalog",
                "platforms": {k: {"modules": list(v.get("modules", []))} for k, v in PLATFORMS.items()},
                "updated_at": _now(),
            },
        )
    except Exception:
        pass


def _default_page_documents() -> List[Dict[str, Any]]:
    """Shape stored in Mongo (one document per page)."""
    out: List[Dict[str, Any]] = []
    for p in WEB_PAGES:
        row: Dict[str, Any] = {
            "pageKey": p["pageKey"],
            "kind": "web",
            "platform": p["platform"],
            "module": p["module"],
            "path": p["path"],
            "adminSection": None,
            "label": p.get("label"),
            "audiences": list(p.get("audiences") or ["anonymous", "authenticated"]),
            "uiMethods": list(p.get("uiMethods") or []),
        }
        out.append(row)
    for p in ADMIN_DESK_PAGES:
        row_ad: Dict[str, Any] = {
            "pageKey": p["pageKey"],
            "kind": "admin_desk",
            "platform": p["platform"],
            "module": p["module"],
            "path": None,
            "adminSection": p["adminSection"],
            "label": p.get("label"),
            "uiMethods": list(p.get("uiMethods") or []),
        }
        if p.get("abacArea") is not None:
            row_ad["abacArea"] = p["abacArea"]
        if p.get("policyResource"):
            row_ad["policyResource"] = p["policyResource"]
        if p.get("relatedPolicyIds"):
            row_ad["relatedPolicyIds"] = list(p["relatedPolicyIds"])
        out.append(row_ad)
    b = dict(ADMIN_BOT_PAGE)
    row_bot: Dict[str, Any] = {
        "pageKey": b["pageKey"],
        "kind": "admin_bot",
        "platform": b["platform"],
        "module": b["module"],
        "path": b["path"],
        "adminSection": None,
        "label": b.get("label"),
        "uiMethods": list(b.get("uiMethods") or []),
    }
    if b.get("abacArea") is not None:
        row_bot["abacArea"] = b["abacArea"]
    if b.get("relatedPolicyIds"):
        row_bot["relatedPolicyIds"] = list(b["relatedPolicyIds"])
    out.append(row_bot)
    return out


def ensure_access_pages_indexes() -> None:
    if app_mongo_disabled():
        return
    db = get_app_database()
    db.access_pages.create_index("pageKey", unique=True)
    db.access_pages.create_index("kind")


def ensure_access_pages_seeded() -> None:
    """If ``access_pages`` is empty, insert built-in defaults (idempotent for non-empty DB)."""
    if app_mongo_disabled():
        return
    try:
        ensure_access_platforms_seeded()
        ensure_access_pages_indexes()
        col = _access_pages_col()
        if col.estimated_document_count() == 0:
            now = _now()
            for doc in _default_page_documents():
                col.insert_one({**doc, "updated_at": now})
        else:
            # Older rows: add default WEB audiences so SPA guards match platform config.
            col.update_many(
                {"kind": "web", "audiences": {"$exists": False}},
                {
                    "$set": {
                        "audiences": ["anonymous", "authenticated"],
                        "updated_at": _now(),
                    }
                },
            )
            # Catalog path + label: ``/productions`` is the storefront ``Home``; drop legacy ``/`` landing row.
            now = _now()
            col.update_many(
                {"kind": "web", "pageKey": "web.product.catalog"},
                {
                    "$set": {
                        "path": "/productions",
                        "module": "Product",
                        "label": "Home",
                        "updated_at": now,
                    }
                },
            )
            col.delete_many({"kind": "web", "pageKey": "web.home.landing"})
            # Bot workspace: default chat policy ids for UI (HR can change in Mongo).
            col.update_many(
                {"kind": "admin_bot", "relatedPolicyIds": {"$exists": False}},
                {
                    "$set": {
                        "relatedPolicyIds": ["pol_bot_use"],
                        "updated_at": now,
                    }
                },
            )
    except Exception:
        pass


def _doc_to_public_shapes(docs: List[Dict[str, Any]]) -> Dict[str, Any]:
    platforms_cat = load_platforms_catalog()
    web: List[Dict[str, Any]] = []
    web_by_plat: Dict[str, List[Dict[str, Any]]] = {}
    admin_desk: List[Dict[str, Any]] = []
    admin_bot: Optional[Dict[str, Any]] = None
    for raw in docs:
        d = {k: v for k, v in raw.items() if k not in ("_id", "updated_at")}
        kind = d.get("kind")
        if kind == "web":
            shaped = {
                "path": d["path"],
                "platform": d["platform"],
                "module": d["module"],
                "pageKey": d["pageKey"],
                "label": d.get("label"),
                "audiences": list(d.get("audiences") or ["anonymous", "authenticated"]),
                "uiMethods": list(d.get("uiMethods") or []),
            }
            web.append(shaped)
            plat = str(d.get("platform") or "WEB").strip().upper() or "WEB"
            web_by_plat.setdefault(plat, []).append(shaped)
        elif kind == "admin_desk":
            desk_pub: Dict[str, Any] = {
                "adminSection": d["adminSection"],
                "platform": d["platform"],
                "module": d["module"],
                "pageKey": d["pageKey"],
                "label": d.get("label"),
                "uiMethods": list(d.get("uiMethods") or []),
            }
            if d.get("abacArea") is not None:
                desk_pub["abacArea"] = d["abacArea"]
            if d.get("policyResource"):
                desk_pub["policyResource"] = d["policyResource"]
            if d.get("relatedPolicyIds"):
                desk_pub["relatedPolicyIds"] = list(d["relatedPolicyIds"])
            admin_desk.append(desk_pub)
        elif kind == "admin_bot":
            admin_bot = {
                "path": d["path"],
                "platform": d["platform"],
                "module": d["module"],
                "pageKey": d["pageKey"],
                "label": d.get("label"),
                "uiMethods": list(d.get("uiMethods") or []),
            }
            if d.get("abacArea") is not None:
                admin_bot["abacArea"] = d["abacArea"]
            if d.get("relatedPolicyIds"):
                admin_bot["relatedPolicyIds"] = list(d["relatedPolicyIds"])
    admin_desk.sort(key=lambda x: x.get("adminSection") or "")
    web.sort(key=lambda x: x.get("path") or "")
    for lst in web_by_plat.values():
        lst.sort(key=lambda x: x.get("path") or "")
    web_pages_web_only = web_by_plat.get("WEB", [])
    if admin_bot is None:
        admin_bot = dict(ADMIN_BOT_PAGE)
        admin_bot["uiMethods"] = list(admin_bot.get("uiMethods") or [])
    web_mods = list(platforms_cat.get("WEB", PLATFORMS["WEB"]).get("modules", PLATFORMS["WEB"]["modules"]))
    web_nav = build_web_nav_entries(web_pages_web_only, web_mods)
    web_menu = build_web_menu_entries(web_pages_web_only)
    return {
        "version": ACCESS_MODEL_VERSION,
        "source": "mongodb",
        "catalog_source": "mongodb",
        "platforms": platforms_cat,
        "webPages": web_pages_web_only,
        "webNav": web_nav,
        "webMenu": web_menu,
        "adminDeskPages": admin_desk,
        "adminBotPage": admin_bot,
        "staffShellPaths": ["/admin", "/bot"],
        "_webPagesByPlatform": web_by_plat,
    }


def access_model_public() -> Dict[str, Any]:
    """
    Prefer MongoDB ``access_pages`` when app DB is available and non-empty.
    Otherwise same as static ``access_model.access_model_public()`` (and ``source``: ``static``).
    """
    if app_mongo_disabled():
        s = access_model_public_static()
        s = {**s, "source": "static", "catalog_source": "static"}
        if "_webPagesByPlatform" not in s:
            s = {**s, "_webPagesByPlatform": {"WEB": list(s.get("webPages") or [])}}
        return s
    try:
        ensure_access_platforms_seeded()
        ensure_access_pages_indexes()
        col = _access_pages_col()
        docs = list(col.find({}).sort("pageKey", 1))
        if not docs:
            s = access_model_public_static()
            s = {**s, "source": "static", "catalog_source": "static"}
            if "_webPagesByPlatform" not in s:
                s = {**s, "_webPagesByPlatform": {"WEB": list(s.get("webPages") or [])}}
            return s
        return _doc_to_public_shapes(docs)
    except Exception:
        s = access_model_public_static()
        s = {**s, "source": "static_error", "catalog_source": "static_error"}
        if "_webPagesByPlatform" not in s:
            s = {**s, "_webPagesByPlatform": {"WEB": list(s.get("webPages") or [])}}
        return s


def _public_full_for_client(full: Dict[str, Any]) -> Dict[str, Any]:
    """Drop underscore internals except expose ``webPagesByPlatform`` for multi-surface clients."""
    out = {k: v for k, v in full.items() if not str(k).startswith("_")}
    wpb = full.get("_webPagesByPlatform")
    if isinstance(wpb, dict):
        out["webPagesByPlatform"] = wpb
    return out


def access_model_http_response(platform: Optional[str] = None) -> Dict[str, Any]:
    """
    Slim JSON for clients.

    - ``?platform=web`` (default): legacy WEB keys ``webPages``, ``webNav``, ``webMenu``.
    - ``?platform=admin``: admin desk catalog + ``staffShellPaths``.
    - ``?platform=all``: full catalog + ``webPagesByPlatform`` (all ``kind: web`` rows by platform).
    - ``?platform=<OTHER>``: any id present in ``platforms`` (from Mongo ``access_platforms`` + code defaults) —
      returns ``pages``, ``nav``, ``menu`` built like WEB (no code change per platform).
    """
    full = access_model_public()
    raw = (platform or "web").strip()
    p_lower = raw.lower()
    p_upper = raw.strip().upper()
    if p_lower in ("all", "full", "*"):
        return _public_full_for_client(full)

    src = full.get("source", "static")
    cat_src = full.get("catalog_source", src)
    platforms_cat: Dict[str, Any] = full.get("platforms") or PLATFORMS

    if p_lower == "admin":
        return {
            "version": full["version"],
            "source": src,
            "catalog_source": cat_src,
            "platform": "ADMIN",
            "platforms": {"ADMIN": platforms_cat.get("ADMIN", PLATFORMS["ADMIN"])},
            "adminDeskPages": full["adminDeskPages"],
            "adminBotPage": full["adminBotPage"],
            "staffShellPaths": full["staffShellPaths"],
        }

    if p_lower == "web":
        return {
            "version": full["version"],
            "source": src,
            "catalog_source": cat_src,
            "platform": "WEB",
            "platforms": {"WEB": platforms_cat.get("WEB", PLATFORMS["WEB"])},
            "webPages": full["webPages"],
            "webNav": full.get("webNav") or [],
            "webMenu": full.get("webMenu") or [],
        }

    if p_upper not in platforms_cat:
        return {
            "error": "unknown_platform",
            "hint": "Use keys from platforms (see ?platform=all) or web, admin, all.",
            "known_platforms": sorted(platforms_cat.keys()),
        }

    wpb = full.get("_webPagesByPlatform") or {}
    pages = list(wpb.get(p_upper, []))
    pm = platforms_cat.get(p_upper) or {"modules": []}
    modules = list(pm.get("modules", []))
    nav = build_web_nav_entries(pages, modules)
    menu = build_web_menu_entries(pages)
    return {
        "version": full["version"],
        "source": src,
        "catalog_source": cat_src,
        "platform": p_upper,
        "platforms": {p_upper: pm},
        "pages": pages,
        "nav": nav,
        "menu": menu,
    }
