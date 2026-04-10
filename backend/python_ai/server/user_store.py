# -*- coding: utf-8 -*-
"""
Users for JWT auth: **MongoDB** (``users`` collection on ``M4_APP_MONGO_*``) when app Mongo is
available; otherwise in-memory (``M4_APP_MONGO_DISABLE`` or connection failure).

Seeded system users (idempotent, ``mailinator.com`` by default) each have a **description** string
(see ``user_public`` / JWT).

  admin@{domain}     Admin      ADMIN     + platform admin description
  hr@{domain}        HR         HR/hr     + HR description
  cross-hr1@{domain} HR       HR/hr     + cross-dept demo: finance **pending orders** only (see ``pol_finance_cross_hr_pending``)
  … sale, storage, finance, delivery, delivery1, courier likewise (delivery + logistics departments)

Default customer: ``user@mailinator.com`` (name ``user``, role USER) unless ``AUTH_SEED_USER_EMAIL`` is set.

Env:
  AUTH_SEED_EMAIL_DOMAIN       default mailinator.com
  AUTH_SEED_DEFAULT_PASSWORD   default ``12345678@Ab`` (all seeded staff + customer when overrides unset)
  AUTH_SEED_ADMIN_EMAIL        override admin email only
  AUTH_SEED_ADMIN_PASSWORD     override admin password only
  AUTH_SEED_USER_EMAIL         optional: override customer email (default ``user@mailinator.com``)
  AUTH_SEED_USER_PASSWORD      customer password (defaults to ``AUTH_SEED_DEFAULT_PASSWORD``)
"""
from __future__ import annotations

import os
import threading
import uuid
from typing import Any, Dict, List, Optional

from werkzeug.security import check_password_hash, generate_password_hash

from app_mongo import get_app_database, identity_persists_mongo

_lock = threading.Lock()
_users_by_email: Dict[str, Dict[str, Any]] = {}
_users_by_id: Dict[str, str] = {}  # id -> email


def _norm_email(email: str) -> str:
    return (email or "").strip().lower()


def _domain() -> str:
    return (os.environ.get("AUTH_SEED_EMAIL_DOMAIN", "mailinator.com") or "mailinator.com").strip().lower()


def seed_admin_email() -> str:
    """Resolved email for the seeded ADMIN account (matches ``init_seed_users``)."""
    o = os.environ.get("AUTH_SEED_ADMIN_EMAIL", "").strip()
    return _norm_email(o) if o else f"admin@{_domain()}"


def cross_hr1_seed_email() -> str:
    """Seeded HR user for cross-department finance (pending orders only) — matches ``init_seed_users``."""
    return f"cross-hr1@{_domain()}"


_DEFAULT_PASSWORD = "12345678@Ab"


def default_seed_password() -> str:
    """Public: current default seed password (same as ``_default_seed_password()``)."""
    return _default_seed_password()


def _default_seed_password() -> str:
    return (os.environ.get("AUTH_SEED_DEFAULT_PASSWORD", _DEFAULT_PASSWORD) or _DEFAULT_PASSWORD).strip()


def _users_collection():
    return get_app_database()["users"]


def _mongo_user_from_doc(doc: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not doc:
        return None
    return {k: v for k, v in doc.items() if k != "_id"}


def init_seed_users() -> None:
    """Idempotent seed: admin + department staff + optional demo customer."""
    if identity_persists_mongo():
        _init_seed_users_mongo()
    else:
        _init_seed_users_memory()


def _init_seed_users_memory() -> None:
    dom = _domain()
    pw = _default_seed_password()
    admin_pw = os.environ.get("AUTH_SEED_ADMIN_PASSWORD", "").strip() or pw

    staff_rows: List[tuple[str, str, str, Optional[str], str]] = [
        ("admin", "Admin", "ADMIN", None, "Platform administrator — full access; default ABAC policies attached."),
        ("hr", "HR", "HR", "hr", "Human resources — user accounts and policy assignment."),
        ("sale", "Sale", "SALE", "sale", "Sales — product catalog and availability (read)."),
        ("storage", "Storage", "STORAGE", "storage", "Warehouse — stock quantity and inventory updates."),
        ("finance", "Finance", "FINANCE", "finance", "Finance — carts overview, export, order status."),
        ("delivery", "Delivery", "DELIVERY", "delivery", "Delivery — ship packed orders and mark delivered."),
        ("delivery1", "Delivery 1", "DELIVERY", "delivery", "Second delivery account — same queue / assignee rules as delivery@."),
        ("courier", "Courier", "DELIVERY", "logistics", "Courier / logistics — ship packed orders and mark delivered."),
        (
            "cross-hr1",
            "Cross HR1",
            "HR",
            "hr",
            "Cross-dept demo: HR home unit; policy grants finance desk **pending_confirmation** orders only.",
        ),
    ]

    admin_override = os.environ.get("AUTH_SEED_ADMIN_EMAIL", "").strip()
    customer_email_env = os.environ.get("AUTH_SEED_USER_EMAIL", "").strip()
    customer_pw = os.environ.get("AUTH_SEED_USER_PASSWORD", "").strip() or pw

    with _lock:
        for local, name, role, dept, description in staff_rows:
            if local == "admin" and admin_override:
                em = _norm_email(admin_override)
            else:
                em = f"{local}@{dom}"
            if em in _users_by_email:
                continue
            use_pw = admin_pw if local == "admin" else pw
            _put_user_nolock(
                {
                    "id": str(uuid.uuid4()),
                    "email": em,
                    "name": name,
                    "description": description,
                    "password_hash": generate_password_hash(use_pw),
                    "role": role,
                    "department": dept,
                    "auth_rev": 1,
                }
            )

        if customer_email_env:
            c_em = _norm_email(customer_email_env)
        else:
            c_em = "user@mailinator.com"
        if c_em not in _users_by_email:
            _put_user_nolock(
                {
                    "id": str(uuid.uuid4()),
                    "email": c_em,
                    "name": "user",
                    "description": "Registered customer — storefront: browse products, cart, pending confirmation.",
                    "password_hash": generate_password_hash(customer_pw),
                    "role": "USER",
                    "department": None,
                    "auth_rev": 1,
                }
            )


def _init_seed_users_mongo() -> None:
    from app_mongo import ensure_app_data_indexes

    ensure_app_data_indexes()
    col = _users_collection()
    dom = _domain()
    pw = _default_seed_password()
    admin_pw = os.environ.get("AUTH_SEED_ADMIN_PASSWORD", "").strip() or pw

    staff_rows: List[tuple[str, str, str, Optional[str], str]] = [
        ("admin", "Admin", "ADMIN", None, "Platform administrator — full access; default ABAC policies attached."),
        ("hr", "HR", "HR", "hr", "Human resources — user accounts and policy assignment."),
        ("sale", "Sale", "SALE", "sale", "Sales — product catalog and availability (read)."),
        ("storage", "Storage", "STORAGE", "storage", "Warehouse — stock quantity and inventory updates."),
        ("finance", "Finance", "FINANCE", "finance", "Finance — carts overview, export, order status."),
        ("delivery", "Delivery", "DELIVERY", "delivery", "Delivery — ship packed orders and mark delivered."),
        ("delivery1", "Delivery 1", "DELIVERY", "delivery", "Second delivery account — same queue / assignee rules as delivery@."),
        ("courier", "Courier", "DELIVERY", "logistics", "Courier / logistics — ship packed orders and mark delivered."),
        (
            "cross-hr1",
            "Cross HR1",
            "HR",
            "hr",
            "Cross-dept demo: HR home unit; policy grants finance desk **pending_confirmation** orders only.",
        ),
    ]

    admin_override = os.environ.get("AUTH_SEED_ADMIN_EMAIL", "").strip()
    customer_email_env = os.environ.get("AUTH_SEED_USER_EMAIL", "").strip()
    customer_pw = os.environ.get("AUTH_SEED_USER_PASSWORD", "").strip() or pw

    for local, name, role, dept, description in staff_rows:
        if local == "admin" and admin_override:
            em = _norm_email(admin_override)
        else:
            em = f"{local}@{dom}"
        if col.find_one({"email": em}):
            continue
        use_pw = admin_pw if local == "admin" else pw
        col.insert_one(
            {
                "id": str(uuid.uuid4()),
                "email": em,
                "name": name,
                "description": description,
                "password_hash": generate_password_hash(use_pw),
                "role": role,
                "department": dept,
                "auth_rev": 1,
            }
        )

    if customer_email_env:
        c_em = _norm_email(customer_email_env)
    else:
        c_em = "user@mailinator.com"
    if not col.find_one({"email": c_em}):
        col.insert_one(
            {
                "id": str(uuid.uuid4()),
                "email": c_em,
                "name": "user",
                "description": "Registered customer — storefront: browse products, cart, pending confirmation.",
                "password_hash": generate_password_hash(customer_pw),
                "role": "USER",
                "department": None,
                "auth_rev": 1,
            }
        )


def _put_user_nolock(rec: Dict[str, Any]) -> None:
    em = _norm_email(rec["email"])
    if "auth_rev" not in rec:
        rec["auth_rev"] = 1
    _users_by_email[em] = rec
    _users_by_id[rec["id"]] = em


def ensure_cross_hr1_demo_user() -> None:
    """Ensure ``cross-hr1@<AUTH_SEED_EMAIL_DOMAIN>`` exists (idempotent). Call after ``init_seed_users``."""
    em = cross_hr1_seed_email()
    pw = _default_seed_password()
    desc = (
        "Cross-dept demo: HR home unit; policy grants finance desk **pending_confirmation** orders only."
    )
    rec = {
        "id": str(uuid.uuid4()),
        "email": em,
        "name": "Cross HR1",
        "description": desc,
        "password_hash": generate_password_hash(pw),
        "role": "HR",
        "department": "hr",
        "auth_rev": 1,
    }
    if identity_persists_mongo():
        col = _users_collection()
        if col.find_one({"email": em}, projection={"email": 1}):
            return
        try:
            col.insert_one(rec)
        except Exception as e:
            from pymongo.errors import DuplicateKeyError

            if isinstance(e, DuplicateKeyError):
                return
            raise
        return
    with _lock:
        if em in _users_by_email:
            return
        _put_user_nolock(rec)


def bump_auth_rev(user_id: str) -> None:
    """Invalidate outstanding JWTs for this user (policy assignment or similar)."""
    if identity_persists_mongo():
        _users_collection().update_one({"id": user_id}, {"$inc": {"auth_rev": 1}})
        return
    with _lock:
        em = _users_by_id.get(user_id)
        if not em:
            return
        rec = _users_by_email.get(em)
        if not rec:
            return
        rec["auth_rev"] = int(rec.get("auth_rev") or 0) + 1


def register_customer(
    email: str,
    password: str,
    *,
    name: Optional[str] = None,
    description: Optional[str] = None,
) -> Dict[str, Any]:
    """Public signup: role USER only."""
    return register_user(
        email, password, name=name, description=description, role="USER", department=None
    )


def register_user(
    email: str,
    password: str,
    *,
    name: Optional[str],
    role: str,
    department: Optional[str],
    description: Optional[str] = None,
) -> Dict[str, Any]:
    em = _norm_email(email)
    if not em or "@" not in em:
        raise ValueError("invalid email")
    if len(password or "") < 6:
        raise ValueError("password must be at least 6 characters")
    display = (name or "").strip() or em.split("@", 1)[0]
    rec = {
        "id": str(uuid.uuid4()),
        "email": em,
        "name": display,
        "description": (description.strip() if description else None),
        "password_hash": generate_password_hash(password),
        "role": role.upper(),
        "department": (department.strip().lower() if department else None),
        "auth_rev": 1,
    }
    if identity_persists_mongo():
        from pymongo.errors import DuplicateKeyError

        try:
            _users_collection().insert_one(rec)
        except DuplicateKeyError as e:
            raise ValueError(
                "email already registered — sign in instead, or use a different email "
                "(user@mailinator.com is pre-seeded as the demo customer)"
            ) from e
        return user_public(rec)

    with _lock:
        if em in _users_by_email:
            raise ValueError(
                "email already registered — sign in instead, or use a different email "
                "(user@mailinator.com is pre-seeded as the demo customer)"
            )
        _put_user_nolock(rec)
        return user_public(rec)


def _verify_login_normalized(em: str, password: str) -> Optional[Dict[str, Any]]:
    if identity_persists_mongo():
        rec = _mongo_user_from_doc(_users_collection().find_one({"email": em}))
        if not rec:
            return None
        if not check_password_hash(rec["password_hash"], password):
            return None
        return rec
    with _lock:
        rec = _users_by_email.get(em)
        if not rec:
            return None
        if not check_password_hash(rec["password_hash"], password):
            return None
        return rec


def verify_login(email: str, password: str) -> Optional[Dict[str, Any]]:
    em = _norm_email(email)
    rec = _verify_login_normalized(em, password)
    if rec:
        return rec
    # Docs use cross-hr1@mailinator.com; seed email is cross-hr1@{AUTH_SEED_EMAIL_DOMAIN}.
    if em == "cross-hr1@mailinator.com":
        alt = cross_hr1_seed_email()
        if alt != em:
            return _verify_login_normalized(alt, password)
    return None


def get_user_by_email(email: str) -> Optional[Dict[str, Any]]:
    em = _norm_email(email)
    if identity_persists_mongo():
        return _mongo_user_from_doc(_users_collection().find_one({"email": em}))
    with _lock:
        return _users_by_email.get(em)


def get_user_by_id(user_id: str) -> Optional[Dict[str, Any]]:
    if identity_persists_mongo():
        return _mongo_user_from_doc(_users_collection().find_one({"id": user_id}))
    with _lock:
        em = _users_by_id.get(user_id)
        if not em:
            return None
        return _users_by_email.get(em)


def user_public(rec: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = {
        "id": rec["id"],
        "email": rec["email"],
        "name": rec.get("name") or rec["email"].split("@", 1)[0],
        "role": rec["role"],
        "department": rec.get("department"),
    }
    d = rec.get("description")
    if d:
        out["description"] = d
    else:
        out["description"] = None
    return out


def list_users_public() -> List[Dict[str, Any]]:
    if identity_persists_mongo():
        col = _users_collection()
        return [user_public(_mongo_user_from_doc(d)) for d in col.find({}).sort("email", 1)]
    with _lock:
        return [user_public(u) for u in _users_by_email.values()]


# Roles HR may create or assign (never ADMIN).
_STAFF_CREATABLE_BY_HR = frozenset({"USER", "HR", "SALE", "STORAGE", "FINANCE", "DELIVERY"})


def admin_create_user(
    *,
    email: str,
    password: str,
    name: Optional[str],
    role: str,
    department: Optional[str],
    description: Optional[str],
    actor_role: str,
) -> Dict[str, Any]:
    """Create user from admin API — ``actor_role`` from JWT (``ADMIN`` unrestricted, ``HR`` limited)."""
    from org_store import department_exists

    em = _norm_email(email)
    if not em or "@" not in em:
        raise ValueError("invalid email")
    ar = (actor_role or "").upper()
    r = (role or "USER").upper()
    if ar == "HR":
        if r not in _STAFF_CREATABLE_BY_HR:
            raise ValueError("HR may only create USER, HR, SALE, STORAGE, FINANCE, or DELIVERY accounts")
    dept_str = "" if department is None else str(department)
    dept = dept_str.strip().lower() or None
    if dept and not department_exists(dept):
        raise ValueError(f"unknown department code: {dept}")
    desc = (description or "").strip() or None
    display = (name or "").strip() or em.split("@", 1)[0]
    if r == "USER":
        created = register_customer(email, password, name=display, description=desc)
    else:
        created = register_user(email, password, name=display, role=r, department=dept, description=desc)
    # Same as public /api/auth/register: floating assistant + /bot gate on pol_bot_use (idempotent).
    from policy_store import POL_BOT_USE, ensure_user_policy_if_missing  # noqa: PLC0415

    ensure_user_policy_if_missing(created["id"], POL_BOT_USE, granted_by="admin_create_user")
    return created


def admin_update_user(user_id: str, *, actor_role: str, updates: Dict[str, Any]) -> Dict[str, Any]:
    """Apply only keys present in ``updates`` (from JSON body)."""
    from org_store import department_exists

    u = get_user_by_id(user_id)
    if not u:
        raise ValueError("user not found")
    ar = (actor_role or "").upper()
    if ar == "HR" and (u.get("role") or "").upper() == "ADMIN":
        raise ValueError("HR cannot modify ADMIN users")

    patch: Dict[str, Any] = {}
    if "name" in updates:
        patch["name"] = (updates.get("name") or "").strip() or (u.get("email") or "?").split("@", 1)[0]
    if "description" in updates:
        patch["description"] = (updates.get("description") or "").strip() or None
    if "department" in updates:
        v = updates.get("department")
        if v is None or (isinstance(v, str) and not v.strip()):
            patch["department"] = None
        else:
            dept = str(v).strip().lower()
            if dept and not department_exists(dept):
                raise ValueError(f"unknown department code: {dept}")
            patch["department"] = dept
    if "role" in updates:
        nr = (updates.get("role") or "").upper()
        if ar == "HR":
            if nr == "ADMIN":
                raise ValueError("HR cannot assign ADMIN role")
            if nr not in _STAFF_CREATABLE_BY_HR:
                raise ValueError("invalid role for HR assignment")
        patch["role"] = nr

    if not patch:
        return user_public(u)

    need_bump = False
    if "role" in patch:
        if (patch.get("role") or "").upper() != (u.get("role") or "").upper():
            need_bump = True
    if "department" in patch:
        old_d = u.get("department")
        new_d = patch.get("department")
        if old_d != new_d:
            need_bump = True

    if identity_persists_mongo():
        update: Dict[str, Any] = {"$set": patch}
        if need_bump:
            update["$inc"] = {"auth_rev": 1}
        _users_collection().update_one({"id": user_id}, update)
        nu = get_user_by_id(user_id)
        return user_public(nu or u)

    with _lock:
        em = _users_by_id.get(user_id)
        if not em:
            raise ValueError("user not found")
        rec = _users_by_email.get(em)
        if not rec:
            raise ValueError("user not found")
        rec.update(patch)
        if need_bump:
            rec["auth_rev"] = int(rec.get("auth_rev") or 0) + 1
        return user_public(rec)
