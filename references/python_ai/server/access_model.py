# -*- coding: utf-8 -*-
"""
Default platform → module → page → **UI methods** definitions.

**Runtime API** reads from MongoDB ``access_pages`` when populated (see ``access_model_store.py``).
This module supplies the **seed** when the collection is empty and the **static fallback** shape.
"""
from __future__ import annotations

from typing import Any, Dict, List, Optional, Set

ACCESS_MODEL_VERSION = 1


def _norm_path(path: str | None) -> str:
    x = (path or "").strip().rstrip("/") or "/"
    return x if x.startswith("/") else f"/{x}"


def build_web_nav_entries(web_pages: List[dict], web_module_order: List[str]) -> List[dict]:
    """
    Top app bar: one item per WEB **module** in ``platforms.WEB.modules`` order
    (first page per module, by ``path``).
    """
    web_only = [p for p in web_pages if p.get("platform") == "WEB"]
    by_mod: Dict[str, List[dict]] = {}
    for p in web_only:
        m = str(p.get("module") or "")
        by_mod.setdefault(m, []).append(p)
    for lst in by_mod.values():
        lst.sort(key=lambda x: x.get("path") or "")
    out: List[dict] = []
    for mod in web_module_order:
        cands = by_mod.get(str(mod), [])
        if not cands:
            continue
        p = cands[0]
        path = _norm_path(p.get("path"))
        name = p.get("label") or path
        out.append(
            {
                "name": name,
                "nav": path,
                "link": path,
                "path": path,
                "pageKey": p.get("pageKey"),
                "module": p.get("module"),
            }
        )
    return out


def build_web_menu_entries(web_pages: List[dict], skip_paths: Optional[Set[str]] = None) -> List[dict]:
    """Account menu: all WEB pages (e.g. register), sorted by ``path``."""
    skip: Set[str] = set(skip_paths) if skip_paths is not None else {"/"}
    xs = [p for p in web_pages if p.get("platform") == "WEB" and _norm_path(p.get("path")) not in skip]
    xs.sort(key=lambda x: x.get("path") or "")
    out: List[dict] = []
    for p in xs:
        path = _norm_path(p.get("path"))
        name = p.get("label") or path
        out.append(
            {
                "name": name,
                "nav": path,
                "link": path,
                "path": path,
                "pageKey": p.get("pageKey"),
                "module": p.get("module"),
            }
        )
    return out

PLATFORMS = {
    "WEB": {"modules": ["Product", "Cart"]},
    "ADMIN": {"modules": ["ADMIN", "BOT_CONFIG"]},
}

# WEB storefront: ``audiences`` = who may open this path (SPA guard). Nav chrome is inferred on the client.

WEB_PAGES = [
    {
        "path": "/productions",
        "platform": "WEB",
        "module": "Product",
        "pageKey": "web.product.catalog",
        "label": "Home",
        "audiences": ["anonymous", "authenticated"],
        "uiMethods": ["viewCatalog", "filterCategory", "refreshCatalog", "openProduct"],
    },
    {
        "path": "/cart",
        "platform": "WEB",
        "module": "Cart",
        "pageKey": "web.cart.checkout",
        "label": "Cart",
        "audiences": ["anonymous", "authenticated"],
        "uiMethods": [
            "viewCart",
            "saveCart",
            "checkout",
            "viewOrderHistory",
            "confirmDelivery",
        ],
    },
    {
        "path": "/register",
        "platform": "WEB",
        "module": "Cart",
        "pageKey": "web.account.register",
        "label": "Register",
        "audiences": ["anonymous", "authenticated"],
        "uiMethods": ["submitRegister"],
    },
]

# ``abacArea`` — stable grep tag (org / HR+ABAC / finance / ops / …).
# ``policyResource`` — typical ABAC ``resource`` string for seed policies (see ``policy_store``).
# ``relatedPolicyIds`` — default seed policy ids that align with this desk (documentation + tooling).

ADMIN_DESK_PAGES = [
    {
        "adminSection": "department",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.org.department",
        "label": "Department",
        "abacArea": "org",
        "uiMethods": ["listDepartments", "createDepartment", "editDepartment"],
    },
    {
        "adminSection": "users",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.hr.users",
        "label": "Users",
        "abacArea": "hr_abac",
        "policyResource": "user",
        "relatedPolicyIds": ["pol_user_manage"],
        "uiMethods": [
            "listUsers",
            "createUser",
            "changeRole",
            "changeDepartment",
            "assignPolicies",
        ],
    },
    {
        "adminSection": "policies",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.abac.policies",
        "label": "Policies",
        "abacArea": "hr_abac",
        "policyResource": "policy",
        "relatedPolicyIds": ["pol_policy_manage"],
        "uiMethods": ["listPolicies", "createPolicy", "reloadCatalog"],
    },
    {
        "adminSection": "finance_charts",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.finance.charts",
        "label": "Finance · Charts & export",
        "abacArea": "finance",
        "policyResource": "cart",
        "relatedPolicyIds": ["pol_cart_finance"],
        "uiMethods": ["viewSummary", "refreshCharts", "exportCsv", "filterRange"],
    },
    {
        "adminSection": "finance_orders",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.finance.orders",
        "label": "Finance · Orders",
        "abacArea": "finance",
        "policyResource": "cart",
        "relatedPolicyIds": ["pol_cart_finance"],
        "uiMethods": ["listOrders", "transitionOrder", "filterStatus"],
    },
    {
        "adminSection": "storage",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.storage.catalog",
        "label": "Storage",
        "abacArea": "storage",
        "uiMethods": [
            "listProducts",
            "createProduct",
            "editStock",
            "bulkImport",
            "manageCategories",
        ],
    },
    {
        "adminSection": "orders",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.ops.orders",
        "label": "Orders",
        "abacArea": "ops",
        "uiMethods": ["listOrders", "transitionOrder", "filterStatus"],
    },
    {
        "adminSection": "delivery",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.ops.delivery",
        "label": "Delivery",
        "abacArea": "ops",
        "uiMethods": ["listOrders", "markShipped"],
    },
    {
        "adminSection": "sale",
        "platform": "ADMIN",
        "module": "ADMIN",
        "pageKey": "admin.sale.catalog",
        "label": "Sale",
        "abacArea": "sale",
        "uiMethods": ["viewProductList"],
    },
]

ADMIN_BOT_PAGE = {
    "path": "/bot",
    "platform": "ADMIN",
    "module": "BOT_CONFIG",
    "pageKey": "admin.bot.workspace",
    "label": "Bot status",
    "abacArea": "bot",
    "relatedPolicyIds": ["pol_bot_use"],
    "uiMethods": ["viewStats", "viewHistory", "sendChat", "geoImport"],
}


def access_model_public() -> Dict[str, Any]:
    """Full catalog (use ``access_model_http_response`` for slim per-platform JSON)."""
    web_nav = build_web_nav_entries(WEB_PAGES, list(PLATFORMS["WEB"]["modules"]))
    web_menu = build_web_menu_entries(WEB_PAGES)
    return {
        "version": ACCESS_MODEL_VERSION,
        "platforms": PLATFORMS,
        "webPages": WEB_PAGES,
        "webNav": web_nav,
        "webMenu": web_menu,
        "adminDeskPages": ADMIN_DESK_PAGES,
        "adminBotPage": ADMIN_BOT_PAGE,
        "staffShellPaths": ["/admin", "/bot"],
        "_webPagesByPlatform": {"WEB": list(WEB_PAGES)},
    }
