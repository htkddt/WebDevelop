# -*- coding: utf-8 -*-
"""
ABAC policies + user ↔ policy assignments.

**MongoDB** (``policies`` + ``user_policies`` on ``M4_APP_MONGO_*``) when ``identity_persists_mongo()``;
otherwise in-memory. See docs/ABAC_PRODUCT_TEST_PLAN.md.

On seed: default policies are created; **ADMIN** user receives full policy set (if not already assigned).
"""
from __future__ import annotations

import re
import threading
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

# Stable id for API + Mongo unique index (letters, digits, underscore, hyphen).
_POLICY_ID_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9_-]{0,62}$")

# Seeded cross-dept HR demo: ``cross-hr1@…`` + ``ensure_cross_hr_finance_demo_seed()``.
POL_FINANCE_CROSS_HR_PENDING = "pol_finance_cross_hr_pending"
# Chat bot UI (floating assistant + ``/bot`` workspace) — assign via HR or seed (see ``seed_bot_chat_policy_assignments``).
POL_BOT_USE = "pol_bot_use"

from app_mongo import get_app_database, identity_persists_mongo
from user_store import bump_auth_rev

_lock = threading.Lock()
_policies: Dict[str, Dict[str, Any]] = {}
_user_policy_ids: Dict[str, List[str]] = {}  # user_id -> [policy_id]


def _policies_collection():
    return get_app_database()["policies"]


def _user_policies_collection():
    return get_app_database()["user_policies"]


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _default_policy_docs() -> List[Dict[str, Any]]:
    # ``pageKeys`` link seed policies to ``access_model`` / ``access_pages`` (grep + future PDP).
    return [
        {
            "id": "pol_system_full",
            "name": "system_full_access",
            "description": "Full access to all resources (default for platform admin).",
            "resource": "*",
            "actions": ["*"],
            "conditions": None,
            "pageKeys": [],
        },
        {
            "id": "pol_policy_manage",
            "name": "policy_management",
            "description": "Create, read, update, delete policies and assign them to users.",
            "resource": "policy",
            "actions": ["create", "read", "update", "delete", "assign"],
            "conditions": None,
            "pageKeys": ["admin.abac.policies"],
        },
        {
            "id": "pol_user_manage",
            "name": "user_management",
            "description": "Manage user accounts (HR / admin scope).",
            "resource": "user",
            "actions": ["create", "read", "update", "delete"],
            "conditions": None,
            "pageKeys": ["admin.hr.users"],
        },
        {
            "id": "pol_cart_finance",
            "name": "cart_finance_view",
            "description": "View and export all carts (finance).",
            "resource": "cart",
            "actions": ["read", "list", "export", "update_status"],
            "conditions": None,
            "pageKeys": ["admin.finance.charts", "admin.finance.orders"],
        },
        {
            "id": POL_FINANCE_CROSS_HR_PENDING,
            "name": "finance_cross_hr_pending_orders",
            "description": "HR cross-support: finance summary/export only for pending_confirmation orders (finance scope).",
            "resource": "cart",
            "actions": ["read", "list", "export"],
            "conditions": {
                "department_any_of": ["finance"],
                "order_status_any_of": ["pending_confirmation"],
            },
            "pageKeys": ["admin.finance.charts", "admin.finance.orders"],
        },
        {
            "id": POL_BOT_USE,
            "name": "bot_chat_use",
            "description": "Use the AI chat assistant (floating widget and /bot workspace). UI checks this policy; assign or revoke in HR.",
            "resource": "chat",
            "actions": ["use"],
            "conditions": None,
            "pageKeys": ["admin.bot.workspace", "web.bot.floating_assistant"],
        },
    ]


# Short lines for ``policy_public`` — helps humans scan ``GET /api/admin/policies`` JSON.
_POLICY_SCOPE_LABELS: Dict[str, str] = {
    "pol_system_full": "All resources — platform ADMIN default (wildcard)",
    "pol_policy_manage": "Admin · Policies & ABAC desk (manage bundles and assignments)",
    "pol_user_manage": "Admin · Users desk (accounts, roles, departments)",
    "pol_cart_finance": "Admin · Finance desks (charts, carts, finance orders)",
    POL_FINANCE_CROSS_HR_PENDING: "Cross-dept HR · Finance desks — pending orders only (demo)",
    POL_BOT_USE: "Chat · Floating assistant + bot workspace (policy-gated in UI)",
}


def _default_policy_by_id() -> Dict[str, Dict[str, Any]]:
    return {d["id"]: d for d in _default_policy_docs()}


def _mongo_policy_from_doc(doc: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not doc:
        return None
    return {k: v for k, v in doc.items() if k != "_id"}


def init_seed_policies(admin_user_id: Optional[str] = None) -> None:
    """Idempotent: default policy documents + link all to the seeded ADMIN user."""
    if identity_persists_mongo():
        _init_seed_policies_mongo(admin_user_id)
    else:
        _init_seed_policies_memory(admin_user_id)
    seed_bot_chat_policy_assignments()


def _init_seed_policies_memory(admin_user_id: Optional[str] = None) -> None:
    from user_store import get_user_by_email, seed_admin_email

    defaults = _default_policy_docs()

    admin_id = admin_user_id
    if not admin_id:
        admin_rec = get_user_by_email(seed_admin_email())
        if admin_rec:
            admin_id = admin_rec["id"]

    with _lock:
        for p in defaults:
            if p["id"] not in _policies:
                _policies[p["id"]] = {**p}
        if admin_id:
            want = [p["id"] for p in defaults]
            cur = list(_user_policy_ids.get(admin_id) or [])
            if not cur:
                _user_policy_ids[admin_id] = want
            else:
                merged = list(dict.fromkeys(cur + [x for x in want if x not in cur]))
                if merged != cur:
                    _user_policy_ids[admin_id] = merged
                    bump_auth_rev(admin_id)


def _init_seed_policies_mongo(admin_user_id: Optional[str] = None) -> None:
    from app_mongo import ensure_app_data_indexes
    from user_store import get_user_by_email, seed_admin_email

    ensure_app_data_indexes()
    pol_col = _policies_collection()
    up_col = _user_policies_collection()

    for p in _default_policy_docs():
        if pol_col.find_one({"id": p["id"]}):
            continue
        pol_col.insert_one({**p})

    # Older DBs: seed rows without ``pageKeys`` — backfill so API/exports stay consistent.
    for p in _default_policy_docs():
        pol_col.update_one(
            {"id": p["id"], "pageKeys": {"$exists": False}},
            {"$set": {"pageKeys": list(p.get("pageKeys") or [])}},
        )

    admin_id = admin_user_id
    if not admin_id:
        admin_rec = get_user_by_email(seed_admin_email())
        if admin_rec:
            admin_id = admin_rec["id"]

    if not admin_id:
        return
    want_ids = [p["id"] for p in _default_policy_docs()]
    existing = up_col.find_one({"user_id": admin_id})
    if existing:
        have = list(existing.get("policy_ids") or [])
        merged = list(dict.fromkeys(have + [x for x in want_ids if x not in have]))
        if merged != have:
            up_col.update_one(
                {"user_id": admin_id},
                {"$set": {"policy_ids": merged, "updated_at": _now(), "granted_by": "seed_merge_defaults"}},
            )
            bump_auth_rev(admin_id)
        return
    up_col.insert_one({"user_id": admin_id, "policy_ids": want_ids, "updated_at": _now()})


def list_policies() -> List[Dict[str, Any]]:
    if identity_persists_mongo():
        return [
            policy_public(_mongo_policy_from_doc(d))
            for d in _policies_collection().find({}).sort("id", 1)
        ]
    with _lock:
        return [policy_public(p) for p in _policies.values()]


def get_policy(policy_id: str) -> Optional[Dict[str, Any]]:
    if identity_persists_mongo():
        return _mongo_policy_from_doc(_policies_collection().find_one({"id": policy_id}))
    with _lock:
        p = _policies.get(policy_id)
        return dict(p) if p else None


def create_policy(
    *,
    policy_id: str,
    name: str,
    resource: str,
    actions: List[str],
    description: Optional[str] = None,
    conditions: Optional[Any] = None,
    page_keys: Optional[List[str]] = None,
) -> Dict[str, Any]:
    """Insert a new policy document. Raises ``ValueError`` on bad input or duplicate ``id``."""
    pid = (policy_id or "").strip()
    if not pid or not _POLICY_ID_RE.match(pid):
        raise ValueError(
            "invalid id: use a letter first, then letters, digits, underscore, or hyphen (max 63 chars)",
        )
    nm = (name or "").strip()
    if not nm or len(nm) > 128:
        raise ValueError("name is required (max 128 characters)")
    res = (resource or "").strip()
    if not res or len(res) > 128:
        raise ValueError("resource is required (max 128 characters)")
    act_list = [str(a).strip() for a in (actions or []) if str(a).strip()]
    if not act_list:
        raise ValueError("actions must be a non-empty list of non-empty strings")
    if len(act_list) > 32:
        raise ValueError("at most 32 actions")
    desc = (description or "").strip() or None
    if desc and len(desc) > 512:
        raise ValueError("description too long (max 512 characters)")
    cond = conditions
    if cond is not None and not isinstance(cond, (dict, list)):
        raise ValueError("conditions must be a JSON object, array, or null")
    pks: Optional[List[str]] = None
    if page_keys is not None:
        if not isinstance(page_keys, list):
            raise ValueError("page_keys must be a list of strings or omitted")
        pks = [str(x).strip() for x in page_keys if str(x).strip()]
        if len(pks) > 32:
            raise ValueError("at most 32 page_keys")

    doc = {
        "id": pid,
        "name": nm,
        "description": desc,
        "resource": res,
        "actions": act_list,
        "conditions": cond,
    }
    if pks is not None:
        doc["pageKeys"] = pks

    if identity_persists_mongo():
        col = _policies_collection()
        if col.find_one({"id": pid}, projection={"id": 1}):
            raise ValueError("policy id already exists")
        col.insert_one({**doc})
        got = _mongo_policy_from_doc(col.find_one({"id": pid}))
        return policy_public(got or doc)

    with _lock:
        if pid in _policies:
            raise ValueError("policy id already exists")
        _policies[pid] = {**doc}
        return policy_public(_policies[pid])


def policy_public(p: Dict[str, Any]) -> Dict[str, Any]:
    pid = str(p.get("id") or "")
    seed = _default_policy_by_id().get(pid)
    raw_pk = p.get("pageKeys")
    if isinstance(raw_pk, list) and any(str(x).strip() for x in raw_pk):
        page_keys = [str(x).strip() for x in raw_pk if str(x).strip()]
    elif seed is not None:
        page_keys = [str(x).strip() for x in (seed.get("pageKeys") or []) if str(x).strip()]
    else:
        page_keys = []

    res = p.get("resource")
    actions = list(p.get("actions") or [])
    scope = _POLICY_SCOPE_LABELS.get(pid)
    if not scope:
        nm = str(p.get("name") or "").strip()
        if nm and "›" in nm:
            scope = nm
        elif seed is None:
            act_preview = ", ".join(actions[:6]) + ("…" if len(actions) > 6 else "")
            scope = f'Custom — resource "{res}" · actions: {act_preview or "(none)"}'
        else:
            scope = _POLICY_SCOPE_LABELS.get(pid, f'Seed policy — resource "{res}"')

    out: Dict[str, Any] = {
        "id": p["id"],
        "name": p["name"],
        "description": p.get("description"),
        "resource": p.get("resource"),
        "actions": actions,
        "conditions": p.get("conditions"),
        "pageKeys": page_keys,
        "scopeLabel": scope,
    }
    return out


def user_has_finance_cross_pending_access(user_id: str) -> bool:
    """True if ``POL_FINANCE_CROSS_HR_PENDING`` is assigned (enforced in finance routes)."""
    for p in list_policies_for_user(user_id):
        if p.get("id") == POL_FINANCE_CROSS_HR_PENDING:
            return True
    return False


def ensure_user_policy_if_missing(
    user_id: str,
    policy_id: str,
    *,
    granted_by: str = "seed",
) -> None:
    """Idempotent policy attach; bumps ``auth_rev`` only when the assignment changes."""
    if not get_policy(policy_id):
        return
    if identity_persists_mongo():
        doc = _user_policies_collection().find_one({"user_id": user_id})
        ids = list((doc or {}).get("policy_ids") or [])
        if policy_id in ids:
            return
        _user_policies_collection().update_one(
            {"user_id": user_id},
            {
                "$addToSet": {"policy_ids": policy_id},
                "$set": {"updated_at": _now(), "granted_by": granted_by},
            },
            upsert=True,
        )
        bump_auth_rev(user_id)
        return
    with _lock:
        cur = list(_user_policy_ids.get(user_id) or [])
        if policy_id in cur:
            return
        if policy_id not in _policies:
            return
        cur.append(policy_id)
        _user_policy_ids[user_id] = cur
    bump_auth_rev(user_id)


def ensure_cross_hr_finance_demo_seed() -> None:
    """Attach cross-finance policy to seeded ``cross-hr1@…`` when that user exists (idempotent)."""
    from user_store import cross_hr1_seed_email, get_user_by_email

    rec = get_user_by_email(cross_hr1_seed_email())
    if not rec:
        return
    ensure_user_policy_if_missing(rec["id"], POL_FINANCE_CROSS_HR_PENDING, granted_by="seed_cross_hr_demo")


def seed_bot_chat_policy_assignments() -> None:
    """
    Attach ``POL_BOT_USE`` to seeded accounts (and the registered-customer email) so policy-gated chat UI works
    after upgrade. Idempotent. New signups get the same policy from ``auth_routes`` after register.
    """
    import os

    from user_store import cross_hr1_seed_email, get_user_by_email, seed_admin_email

    dom = (os.environ.get("AUTH_SEED_EMAIL_DOMAIN", "mailinator.com") or "mailinator.com").strip().lower()
    ce = (os.environ.get("AUTH_SEED_USER_EMAIL", "") or "").strip().lower()
    customer = ce if ce else f"user@{dom}"
    emails = [
        seed_admin_email(),
        customer,
        f"hr@{dom}",
        f"sale@{dom}",
        f"storage@{dom}",
        f"finance@{dom}",
        f"delivery@{dom}",
        f"delivery1@{dom}",
        f"courier@{dom}",
        cross_hr1_seed_email(),
    ]
    seen: set[str] = set()
    for em in emails:
        em = (em or "").strip().lower()
        if not em or em in seen:
            continue
        seen.add(em)
        u = get_user_by_email(em)
        if u:
            ensure_user_policy_if_missing(u["id"], POL_BOT_USE, granted_by="seed_bot_chat")

    # HR-created staff (any email) may have missed pol_bot_use before admin_create_user granted it — idempotent.
    from user_store import list_users_public  # noqa: PLC0415

    _staff_roles = frozenset({"ADMIN", "HR", "SALE", "STORAGE", "FINANCE", "DELIVERY"})
    for u in list_users_public():
        if (u.get("role") or "").upper() in _staff_roles:
            ensure_user_policy_if_missing(u["id"], POL_BOT_USE, granted_by="seed_bot_chat_staff")


def list_policies_for_user(user_id: str) -> List[Dict[str, Any]]:
    if identity_persists_mongo():
        doc = _user_policies_collection().find_one({"user_id": user_id})
        ids = doc.get("policy_ids", []) if doc else []
        out: List[Dict[str, Any]] = []
        for pid in ids:
            p = get_policy(pid)
            if p:
                out.append(policy_public(p))
        return out
    with _lock:
        ids = _user_policy_ids.get(user_id) or []
    out2: List[Dict[str, Any]] = []
    for pid in ids:
        p = get_policy(pid)
        if p:
            out2.append(policy_public(p))
    return out2


def set_user_policies(user_id: str, policy_ids: List[str], *, granted_by: str) -> None:
    """Replace assignments (HR/ADMIN); ``granted_by`` reserved for future audit in Mongo."""
    if identity_persists_mongo():
        for pid in policy_ids:
            if not _policies_collection().find_one({"id": pid}):
                raise ValueError(f"unknown policy_id: {pid}")
        _user_policies_collection().update_one(
            {"user_id": user_id},
            {"$set": {"policy_ids": list(dict.fromkeys(policy_ids)), "updated_at": _now(), "granted_by": granted_by}},
            upsert=True,
        )
        bump_auth_rev(user_id)
        return
    with _lock:
        for pid in policy_ids:
            if pid not in _policies:
                raise ValueError(f"unknown policy_id: {pid}")
        _user_policy_ids[user_id] = list(dict.fromkeys(policy_ids))
    bump_auth_rev(user_id)


def assign_policy_to_user(user_id: str, policy_id: str) -> None:
    if identity_persists_mongo():
        if not _policies_collection().find_one({"id": policy_id}):
            raise ValueError("unknown policy_id")
        _user_policies_collection().update_one(
            {"user_id": user_id},
            {"$addToSet": {"policy_ids": policy_id}, "$set": {"updated_at": _now()}},
            upsert=True,
        )
        bump_auth_rev(user_id)
        return
    with _lock:
        if policy_id not in _policies:
            raise ValueError("unknown policy_id")
        cur = list(_user_policy_ids.get(user_id) or [])
        if policy_id not in cur:
            cur.append(policy_id)
        _user_policy_ids[user_id] = cur
    bump_auth_rev(user_id)


def remove_policy_from_user(user_id: str, policy_id: str) -> None:
    if identity_persists_mongo():
        _user_policies_collection().update_one(
            {"user_id": user_id},
            {"$pull": {"policy_ids": policy_id}, "$set": {"updated_at": _now()}},
        )
        bump_auth_rev(user_id)
        return
    with _lock:
        cur = [x for x in (_user_policy_ids.get(user_id) or []) if x != policy_id]
        _user_policy_ids[user_id] = cur
    bump_auth_rev(user_id)
