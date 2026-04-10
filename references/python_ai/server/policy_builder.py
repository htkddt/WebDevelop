# -*- coding: utf-8 -*-
"""
Structured options for the **policy builder** UI (platform → module → feature).

Uses the live access model from ``access_model_store.access_model_public()``.
"""
from __future__ import annotations

import re
from typing import Any, Dict, List, Optional

from access_model_store import access_model_public

try:
    from order_store import STATUSES as ORDER_STATUSES
except Exception:
    ORDER_STATUSES = (
        "pending_confirmation",
        "confirmed",
        "rejected",
        "packed",
        "shipped",
        "delivered",
    )

_PLATFORM_LABELS = {
    "WEB": "Web storefront",
    "ADMIN": "Admin & staff",
    "MOBILE": "Mobile",
    "PARTNER": "Partner",
}

_SLUG_SAFE = re.compile(r"[^a-zA-Z0-9]+")


def _slug_part(s: str, max_len: int = 24) -> str:
    x = _SLUG_SAFE.sub("_", (s or "").strip().lower()).strip("_")
    return (x[:max_len] if x else "x").rstrip("_") or "x"


def suggest_policy_id(page_key: str) -> str:
    """Stable id: ``pol_`` + pageKey with dots → underscores (trimmed to policy id rules)."""
    base = "pol_" + _slug_part(page_key.replace(".", "_"), 56)
    if not base[0].isalpha():
        base = "pol_" + base
    return base[:63]


def human_policy_name(platform_id: str, module: str, feature_label: str) -> str:
    plab = _PLATFORM_LABELS.get(platform_id, platform_id)
    return f"{plab} › {module} › {feature_label}".strip()


def ui_methods_to_suggested_actions(ui_methods: List[str]) -> List[str]:
    """Map UI capability names to coarse ABAC-style actions (deduped)."""
    out: List[str] = []
    seen = set()
    for raw in ui_methods or []:
        m = str(raw or "").strip()
        if not m:
            continue
        ml = m.lower()
        if ml.startswith("view") or ml.startswith("open") or ml.startswith("refresh") or ml.startswith("filter"):
            a = "read"
        elif ml.startswith("list"):
            a = "list"
        elif "export" in ml:
            a = "export"
        elif ml.startswith("delete") or ml.startswith("remove"):
            a = "delete"
        elif any(
            ml.startswith(p)
            for p in (
                "create",
                "add",
                "submit",
                "save",
                "edit",
                "update",
                "assign",
                "transition",
                "mark",
                "confirm",
                "checkout",
                "bulk",
                "manage",
                "change",
                "reload",
            )
        ):
            a = "update"
        else:
            a = "read"
        if a not in seen:
            seen.add(a)
            out.append(a)
    return out if out else ["read"]


def suggest_resource(
    page_key: str,
    *,
    kind: str,
    policy_resource_hint: Optional[str] = None,
) -> str:
    if policy_resource_hint and str(policy_resource_hint).strip():
        return str(policy_resource_hint).strip()
    pk = (page_key or "").lower()
    if "cart" in pk or "finance" in pk:
        return "cart"
    if "order" in pk or "ops" in pk or "delivery" in pk or "sale" in pk:
        return "order"
    if "user" in pk or "hr" in pk:
        return "user"
    if "polic" in pk or "abac" in pk:
        return "policy"
    if "product" in pk or "catalog" in pk or "storage" in pk:
        return "product"
    if "department" in pk or "org" in pk:
        return "department"
    if "bot" in pk:
        return "bot_config"
    return "resource"


def policy_builder_options() -> Dict[str, Any]:
    full = access_model_public()
    platforms_raw = full.get("platforms") or {}
    platforms_out: List[Dict[str, Any]] = []
    for pid, meta in platforms_raw.items():
        pidu = str(pid).upper()
        mods = list((meta or {}).get("modules") or [])
        platforms_out.append(
            {
                "id": pidu,
                "label": _PLATFORM_LABELS.get(pidu, pidu),
                "modules": mods,
            },
        )
    platforms_out.sort(key=lambda x: x["id"])

    features: List[Dict[str, Any]] = []

    for wp in full.get("webPages") or []:
        pk = str(wp.get("pageKey") or "")
        plat = str(wp.get("platform") or "WEB").upper()
        mod = str(wp.get("module") or "")
        label = str(wp.get("label") or pk or mod)
        uim = list(wp.get("uiMethods") or [])
        features.append(
            {
                "pageKey": pk,
                "label": label,
                "platform": plat,
                "module": mod,
                "kind": "web",
                "path": wp.get("path"),
                "uiMethods": uim,
                "suggestedResource": suggest_resource(pk, kind="web"),
                "suggestedActions": ui_methods_to_suggested_actions(uim),
                "suggestedPolicyId": suggest_policy_id(pk),
                "humanName": human_policy_name(plat, mod, label),
            },
        )

    for ad in full.get("adminDeskPages") or []:
        pk = str(ad.get("pageKey") or "")
        plat = str(ad.get("platform") or "ADMIN").upper()
        mod = str(ad.get("module") or "ADMIN")
        label = str(ad.get("label") or pk)
        uim = list(ad.get("uiMethods") or [])
        hint = ad.get("policyResource")
        features.append(
            {
                "pageKey": pk,
                "label": label,
                "platform": plat,
                "module": mod,
                "kind": "admin_desk",
                "adminSection": ad.get("adminSection"),
                "uiMethods": uim,
                "suggestedResource": suggest_resource(pk, kind="admin_desk", policy_resource_hint=hint),
                "suggestedActions": ui_methods_to_suggested_actions(uim),
                "suggestedPolicyId": suggest_policy_id(pk),
                "humanName": human_policy_name(plat, mod, label),
            },
        )

    bot = full.get("adminBotPage")
    if isinstance(bot, dict) and bot.get("pageKey"):
        pk = str(bot["pageKey"])
        uim = list(bot.get("uiMethods") or [])
        label = str(bot.get("label") or "Bot")
        features.append(
            {
                "pageKey": pk,
                "label": label,
                "platform": "ADMIN",
                "module": str(bot.get("module") or "BOT_CONFIG"),
                "kind": "admin_bot",
                "path": bot.get("path"),
                "uiMethods": uim,
                "suggestedResource": suggest_resource(pk, kind="admin_bot"),
                "suggestedActions": ui_methods_to_suggested_actions(uim),
                "suggestedPolicyId": suggest_policy_id(pk),
                "humanName": human_policy_name("ADMIN", str(bot.get("module") or "BOT_CONFIG"), label),
            },
        )

    features.sort(key=lambda x: (x.get("platform") or "", x.get("module") or "", x.get("pageKey") or ""))

    return {
        "platforms": platforms_out,
        "features": features,
        "orderStatuses": list(ORDER_STATUSES),
        "hint": "Pick platform → module → feature; id and readable name are generated. Optional order statuses go into conditions.order_status_any_of for documentation.",
    }
