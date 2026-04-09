# -*- coding: utf-8 -*-
"""Store orders — checkout from cart.

Lifecycle: pending_confirmation → confirmed/rejected → packed → shipped → **delivered**.

``delivered`` after ``shipped``: the **customer** (registered ``USER`` matching ``customer_email``) confirms
receipt via ``customer_confirm_delivered``. **ADMIN** may still set ``delivered`` from the admin API (guest orders,
disputes). Delivery staff mark ``shipped`` only.
"""
from __future__ import annotations

import uuid
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Set, Tuple

from app_mongo import app_mongo_disabled, get_app_database

STATUSES = (
    "pending_confirmation",
    "confirmed",
    "rejected",
    "packed",
    "shipped",
    "delivered",
)

_TRANSITIONS: Dict[str, Set[str]] = {
    "pending_confirmation": {"confirmed", "rejected"},
    "confirmed": {"packed"},
    "packed": {"shipped"},
    "shipped": {"delivered"},
}

# Department codes treated like ``delivery`` for orders desk + ship/deliver transitions.
_DELIVERY_DEPT_CODES = frozenset({"delivery", "logistics", "courier", "shipping"})


def _is_delivery_dept(dept: Optional[str]) -> bool:
    d = (dept or "").strip().lower()
    return d in _DELIVERY_DEPT_CODES


def is_delivery_staff_user(role: str, department: Optional[str]) -> bool:
    """True for DELIVERY role or delivery-like department codes (orders desk + ship)."""
    r, d, _ = _actor_key(role, department, "")
    return r == "DELIVERY" or _is_delivery_dept(d)


def can_override_delivery_assignee(role: str, department: Optional[str]) -> bool:
    """ADMIN / FINANCE / finance dept may set or clear ``delivery_assignee_user_id`` on any order."""
    r, d, _ = _actor_key(role, department, "")
    if r == "ADMIN":
        return True
    return r == "FINANCE" or d == "finance"


def _db():
    if app_mongo_disabled():
        raise RuntimeError("app Mongo disabled")
    return get_app_database()


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _orders_col():
    return _db()["orders"]


def _actor_key(role: str, department: Optional[str], email: str) -> Tuple[str, str]:
    r = (role or "").upper()
    d = (department or "").strip().lower()
    e = (email or "").strip().lower()
    return r, d, e


def can_access_orders_desk(role: str, department: Optional[str]) -> bool:
    r, d, _ = _actor_key(role, department, "")
    if r == "ADMIN":
        return True
    if r in ("FINANCE", "STORAGE", "DELIVERY"):
        return True
    if d in ("finance", "storage"):
        return True
    if _is_delivery_dept(d):
        return True
    return False


def _can_transition(actor_role: str, actor_department: Optional[str], from_s: str, to_s: str) -> bool:
    if from_s not in _TRANSITIONS or to_s not in _TRANSITIONS[from_s]:
        return False
    r, d, _ = _actor_key(actor_role, actor_department, "")
    if r == "ADMIN":
        return True
    if to_s in ("confirmed", "rejected"):
        return r == "FINANCE" or d == "finance"
    if to_s == "packed":
        return r == "STORAGE" or d == "storage"
    if to_s == "shipped":
        return r == "DELIVERY" or _is_delivery_dept(d)
    if to_s == "delivered" and from_s == "shipped":
        # Customer confirms via ``customer_confirm_delivered``; only ADMIN may force-close here.
        return r == "ADMIN"
    return False


def _iso(dt: Any) -> Any:
    if dt is None:
        return None
    if hasattr(dt, "isoformat"):
        return dt.isoformat()
    return dt


def _lookup_delivery_assignee_display(user_id: str) -> Optional[str]:
    """Display name for assignee: user ``name``, else ``email``."""
    uid = (user_id or "").strip()
    if not uid:
        return None
    from user_store import get_user_by_id

    u = get_user_by_id(uid)
    if not u:
        return None
    nm = (u.get("name") or "").strip()
    if nm:
        return nm
    em = (u.get("email") or "").strip()
    return em or None


def order_public(
    doc: Dict[str, Any],
    *,
    assignee_display_cache: Optional[Dict[str, Optional[str]]] = None,
) -> Dict[str, Any]:
    if not doc:
        return {}
    aid = doc.get("delivery_assignee_user_id")
    aid_str = str(aid) if aid else None
    disp: Optional[str] = None
    if aid_str:
        if assignee_display_cache is not None:
            if aid_str not in assignee_display_cache:
                assignee_display_cache[aid_str] = _lookup_delivery_assignee_display(aid_str)
            disp = assignee_display_cache[aid_str]
        else:
            disp = _lookup_delivery_assignee_display(aid_str)
    return {
        "id": doc.get("id"),
        "status": doc.get("status"),
        "customer_type": doc.get("customer_type"),
        "customer_email": doc.get("customer_email"),
        "contact_email": doc.get("contact_email"),
        "lines": doc.get("lines") or [],
        "subtotal": float(doc.get("subtotal") or 0),
        "delivery_note": doc.get("delivery_note"),
        "delivery_assignee_user_id": aid_str,
        "delivery_assignee_name": disp,
        "created_at": _iso(doc.get("created_at")),
        "updated_at": _iso(doc.get("updated_at")),
    }


def checkout_from_cart(
    cart_key: str,
    *,
    customer_email: Optional[str],
    customer_type: str,
    delivery_note: Optional[str] = None,
    contact_email: Optional[str] = None,
) -> Dict[str, Any]:
    """Create order from current cart, clear cart. Re-validates stock via ``save_cart``."""
    from product_cart_store import cart_public, clear_cart, save_cart

    cart = cart_public(cart_key)
    lines_in = cart.get("lines") or []
    if not lines_in:
        raise ValueError("cart is empty")

    minimal = [{"product_id": ln["product_id"], "qty": int(ln["qty"])} for ln in lines_in]
    save_cart(cart_key, minimal)
    cart = cart_public(cart_key)
    lines = cart.get("lines") or []
    subtotal = float(cart.get("subtotal") or 0)

    oid = str(uuid.uuid4())
    now = _now()
    doc = {
        "id": oid,
        "cart_key": cart_key,
        "customer_type": customer_type,
        "customer_email": (customer_email or "").strip().lower() or None,
        "contact_email": (contact_email or "").strip().lower() or None,
        "delivery_note": (delivery_note or "").strip() or None,
        "lines": lines,
        "subtotal": round(subtotal, 2),
        "status": "pending_confirmation",
        "delivery_assignee_user_id": None,
        "created_at": now,
        "updated_at": now,
    }
    _orders_col().insert_one(doc)
    clear_cart(cart_key)
    return order_public(doc)


def get_order_by_id(order_id: str) -> Optional[Dict[str, Any]]:
    doc = _orders_col().find_one({"id": order_id})
    m = _mongo_order(doc)
    return order_public(m) if m else None


def _mongo_order(doc: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not doc:
        return None
    return {k: v for k, v in doc.items() if k != "_id"}


def list_orders_for_customer(email: str) -> List[Dict[str, Any]]:
    em = (email or "").strip().lower()
    cur = _orders_col().find({"customer_email": em}).sort("created_at", -1)
    cache: Dict[str, Optional[str]] = {}
    return [order_public(_mongo_order(d) or {}, assignee_display_cache=cache) for d in cur if d]


def _status_scope_for_staff(role: str, department: Optional[str]) -> Optional[Set[str]]:
    """``None`` = all statuses (ADMIN / FINANCE). Otherwise Mongo ``$in`` filter."""
    r, d, _ = _actor_key(role, department, "")
    if r == "ADMIN" or r == "FINANCE" or d == "finance":
        return None
    if r == "STORAGE" or d == "storage":
        return {"confirmed", "packed", "shipped", "delivered"}
    if r == "DELIVERY" or _is_delivery_dept(d):
        return {"packed", "shipped", "delivered"}
    return set()


def list_orders_for_staff(
    role: str,
    department: Optional[str],
    *,
    status: Optional[str] = None,
    cross_finance_hr_pending: bool = False,
    viewer_user_id: Optional[str] = None,
) -> List[Dict[str, Any]]:
    """``cross_finance_hr_pending``: HR + ``pol_finance_cross_hr_pending`` — same UI filters as finance; server only returns ``pending_confirmation`` rows.

    ``viewer_user_id``: for delivery staff, restrict to orders with no assignee or assignee equal to this user id.
    """
    if cross_finance_hr_pending:
        st = (status or "").strip() or None
        if st and st != "pending_confirmation":
            return []
        q: Dict[str, Any] = {"status": "pending_confirmation"}
        cur = _orders_col().find(q).sort("created_at", -1)
        cache: Dict[str, Optional[str]] = {}
        return [order_public(_mongo_order(d) or {}, assignee_display_cache=cache) for d in cur if d]
    if not can_access_orders_desk(role, department):
        return []
    scope = _status_scope_for_staff(role, department)
    st = (status or "").strip() or None
    if st:
        if st not in STATUSES:
            raise ValueError("invalid status")
        if scope is not None and st not in scope:
            raise ValueError("status not visible for this role")
        status_filter: Dict[str, Any] = {"status": st}
    elif scope is None:
        status_filter = {}
    else:
        status_filter = {"status": {"$in": sorted(scope)}}

    vu = (viewer_user_id or "").strip()
    if is_delivery_staff_user(role, department):
        if not vu:
            return []
        assign_filter: Dict[str, Any] = {
            "$or": [
                {"delivery_assignee_user_id": None},
                {"delivery_assignee_user_id": {"$exists": False}},
                {"delivery_assignee_user_id": vu},
            ]
        }
        q = {"$and": [status_filter, assign_filter]} if status_filter else assign_filter
    else:
        q = status_filter

    cur = _orders_col().find(q).sort("created_at", -1)
    cache: Dict[str, Optional[str]] = {}
    return [order_public(_mongo_order(d) or {}, assignee_display_cache=cache) for d in cur if d]


# Delivery may self-assign / release only while the order is in these statuses.
_ASSIGNABLE_BY_DELIVERY_STATUSES = frozenset({"packed", "shipped"})


def can_set_delivery_assignee(role: str, department: Optional[str]) -> bool:
    return can_override_delivery_assignee(role, department) or is_delivery_staff_user(role, department)


def set_order_delivery_assignee(
    order_id: str,
    new_assignee_user_id: Optional[str],
    *,
    actor_role: str,
    actor_department: Optional[str],
    actor_user_id: str,
    actor_email: str,
) -> Dict[str, Any]:
    """Set or clear ``delivery_assignee_user_id``. ADMIN/FINANCE may override anytime; delivery may self-assign or self-unassign only."""
    col = _orders_col()
    doc = col.find_one({"id": order_id})
    if not doc:
        raise ValueError("order not found")

    nxt = (new_assignee_user_id or "").strip() or None
    cur_raw = doc.get("delivery_assignee_user_id")
    cur = str(cur_raw).strip() if cur_raw else None
    ost = str(doc.get("status") or "")

    if can_override_delivery_assignee(actor_role, actor_department):
        pass
    elif is_delivery_staff_user(actor_role, actor_department):
        au = (actor_user_id or "").strip()
        if not au:
            raise ValueError("forbidden")
        if ost not in _ASSIGNABLE_BY_DELIVERY_STATUSES:
            raise ValueError("delivery assignee can only be changed for packed or shipped orders")
        if nxt == au:
            if cur:
                raise ValueError("order is already assigned — ask an admin or finance user to reassign")
        elif nxt is None:
            if cur != au:
                raise ValueError("forbidden")
        else:
            raise ValueError("forbidden")
    else:
        raise ValueError("forbidden for this role")

    if cur == nxt:
        return get_order_by_id(order_id) or order_public(_mongo_order(doc) or {})

    now = _now()
    col.update_one(
        {"id": order_id},
        {
            "$set": {"delivery_assignee_user_id": nxt, "updated_at": now},
            "$push": {
                "audit": {
                    "at": now,
                    "delivery_assignee_user_id": nxt,
                    "by": (actor_email or "").strip().lower(),
                    "role": (actor_role or "").upper(),
                    "note": "delivery_assignee",
                }
            },
        },
    )
    return get_order_by_id(order_id) or order_public(_mongo_order(doc) or {})


def transition_order(
    order_id: str,
    new_status: str,
    *,
    actor_role: str,
    actor_department: Optional[str],
    actor_email: str,
    actor_user_id: Optional[str] = None,
) -> Dict[str, Any]:
    from product_cart_store import decrement_product_stock, increment_product_stock

    ns = (new_status or "").strip()
    if ns not in STATUSES:
        raise ValueError("invalid status")

    col = _orders_col()
    doc = col.find_one({"id": order_id})
    if not doc:
        raise ValueError("order not found")
    cur = doc.get("status")
    if cur not in _TRANSITIONS or ns not in _TRANSITIONS[cur]:
        raise ValueError(f"cannot transition from {cur} to {ns}")
    if not _can_transition(actor_role, actor_department, cur, ns):
        raise ValueError("forbidden for this role")

    if ns == "shipped" and cur == "packed":
        r, d, _ = _actor_key(actor_role, actor_department, "")
        if r == "DELIVERY" or _is_delivery_dept(d):
            aid = (actor_user_id or "").strip()
            assigned = doc.get("delivery_assignee_user_id")
            if not aid or not assigned or str(assigned) != aid:
                raise ValueError("assign this order to yourself before marking shipped")

    lines = doc.get("lines") or []

    if ns == "packed":
        rolled: List[Tuple[str, int]] = []
        try:
            for ln in lines:
                pid = str(ln.get("product_id") or "")
                qty = int(ln.get("qty") or 0)
                if not pid or qty < 1:
                    continue
                decrement_product_stock(pid, qty)
                rolled.append((pid, qty))
        except Exception:
            for pid, qty in rolled:
                try:
                    increment_product_stock(pid, qty)
                except Exception:
                    pass
            raise

    now = _now()
    col.update_one(
        {"id": order_id},
        {
            "$set": {"status": ns, "updated_at": now},
            "$push": {
                "audit": {
                    "at": now,
                    "status": ns,
                    "by": (actor_email or "").strip().lower(),
                    "role": (actor_role or "").upper(),
                }
            },
        },
    )
    return get_order_by_id(order_id) or order_public(_mongo_order(doc) or {})


def can_access_finance_analytics(role: str, department: Optional[str]) -> bool:
    """Charts / export — **ADMIN**, **FINANCE** role, or **finance** department."""
    r, d, _ = _actor_key(role, department, "")
    if r == "ADMIN":
        return True
    return r == "FINANCE" or d == "finance"


def _finance_match(
    *,
    status: Optional[str],
    from_day: Optional[str],
    to_day: Optional[str],
    customer_type: Optional[str],
) -> Dict[str, Any]:
    q: Dict[str, Any] = {}
    if status and status in STATUSES:
        q["status"] = status
    if from_day:
        try:
            lo = datetime.strptime(from_day.strip()[:10], "%Y-%m-%d").replace(tzinfo=timezone.utc)
            q.setdefault("created_at", {})["$gte"] = lo
        except ValueError as e:
            raise ValueError("invalid from date (use YYYY-MM-DD)") from e
    if to_day:
        try:
            hi = datetime.strptime(to_day.strip()[:10], "%Y-%m-%d").replace(
                hour=23, minute=59, second=59, microsecond=999999, tzinfo=timezone.utc
            )
            q.setdefault("created_at", {})["$lte"] = hi
        except ValueError as e:
            raise ValueError("invalid to date (use YYYY-MM-DD)") from e
    ct = (customer_type or "").strip().lower()
    if ct in ("guest", "user"):
        q["customer_type"] = ct
    return q


def finance_summary(
    role: str,
    department: Optional[str],
    *,
    from_day: Optional[str] = None,
    to_day: Optional[str] = None,
    status: Optional[str] = None,
    customer_type: Optional[str] = None,
    cross_pending_only: bool = False,
) -> Dict[str, Any]:
    """``cross_pending_only``: HR cross-delegation — only ``pending_confirmation`` orders (ignores ``status`` arg)."""
    eff_status = (status or "").strip() or None
    if cross_pending_only:
        eff_status = "pending_confirmation"
    elif not can_access_finance_analytics(role, department):
        raise ValueError("forbidden")
    q = _finance_match(
        status=eff_status,
        from_day=(from_day or "").strip() or None,
        to_day=(to_day or "").strip() or None,
        customer_type=(customer_type or "").strip() or None,
    )
    col = _orders_col()
    tot = list(
        col.aggregate(
            [
                {"$match": q},
                {"$group": {"_id": None, "n": {"$sum": 1}, "subtotal": {"$sum": "$subtotal"}}},
            ]
        )
    )
    total_orders = int(tot[0]["n"]) if tot else 0
    total_subtotal = float(tot[0]["subtotal"]) if tot else 0.0
    by_status = list(
        col.aggregate(
            [
                {"$match": q},
                {
                    "$group": {
                        "_id": "$status",
                        "count": {"$sum": 1},
                        "subtotal_sum": {"$sum": "$subtotal"},
                    }
                },
                {"$sort": {"_id": 1}},
            ]
        )
    )
    by_day = list(
        col.aggregate(
            [
                {"$match": q},
                {
                    "$group": {
                        "_id": {
                            "$dateToString": {
                                "format": "%Y-%m-%d",
                                "date": "$created_at",
                                "timezone": "UTC",
                            }
                        },
                        "count": {"$sum": 1},
                        "subtotal_sum": {"$sum": "$subtotal"},
                    }
                },
                {"$sort": {"_id": 1}},
            ]
        )
    )
    return {
        "total_orders": total_orders,
        "total_subtotal": round(total_subtotal, 2),
        "by_status": [
            {
                "status": x["_id"],
                "count": int(x["count"]),
                "subtotal_sum": round(float(x["subtotal_sum"] or 0), 2),
            }
            for x in by_status
            if x.get("_id")
        ],
        "by_day": [
            {
                "day": x["_id"],
                "count": int(x["count"]),
                "subtotal_sum": round(float(x["subtotal_sum"] or 0), 2),
            }
            for x in by_day
            if x.get("_id")
        ],
        "filters": {
            "from": (from_day or "").strip() or None,
            "to": (to_day or "").strip() or None,
            "status": eff_status,
            "customer_type": (customer_type or "").strip() or None,
            "cross_pending_only": bool(cross_pending_only),
        },
    }


def finance_export_rows(
    role: str,
    department: Optional[str],
    *,
    from_day: Optional[str] = None,
    to_day: Optional[str] = None,
    status: Optional[str] = None,
    customer_type: Optional[str] = None,
    cross_pending_only: bool = False,
) -> List[Dict[str, Any]]:
    eff_status = (status or "").strip() or None
    if cross_pending_only:
        eff_status = "pending_confirmation"
    elif not can_access_finance_analytics(role, department):
        raise ValueError("forbidden")
    q = _finance_match(
        status=eff_status,
        from_day=(from_day or "").strip() or None,
        to_day=(to_day or "").strip() or None,
        customer_type=(customer_type or "").strip() or None,
    )
    col = _orders_col()
    rows: List[Dict[str, Any]] = []
    cache: Dict[str, Optional[str]] = {}
    for doc in col.find(q).sort("created_at", -1):
        pub = order_public(_mongo_order(doc) or {}, assignee_display_cache=cache)
        rows.append(
            {
                "id": pub.get("id"),
                "status": pub.get("status"),
                "customer_type": pub.get("customer_type"),
                "customer_email": pub.get("customer_email") or "",
                "contact_email": pub.get("contact_email") or "",
                "subtotal": doc.get("subtotal"),
                "line_count": len(doc.get("lines") or []),
                "created_at": pub.get("created_at"),
                "updated_at": pub.get("updated_at"),
                "delivery_assignee_user_id": pub.get("delivery_assignee_user_id"),
                "delivery_assignee_name": pub.get("delivery_assignee_name"),
            }
        )
    return rows


def customer_confirm_delivered(order_id: str, *, customer_email: str) -> Dict[str, Any]:
    """``USER`` only: ``shipped`` → ``delivered`` when JWT ``sub`` matches order ``customer_email``."""
    em = (customer_email or "").strip().lower()
    if not em:
        raise ValueError("invalid customer")

    col = _orders_col()
    doc = col.find_one({"id": order_id})
    if not doc:
        raise ValueError("order not found")
    cur = doc.get("status")
    if cur != "shipped":
        raise ValueError("order is not shipped — only shipped orders can be confirmed as delivered")
    order_em = (doc.get("customer_email") or "").strip().lower()
    if not order_em:
        raise ValueError(
            "this order has no customer account — contact support or ask an admin to mark delivered"
        )
    if order_em != em:
        raise ValueError("forbidden: this order belongs to another customer")

    now = _now()
    col.update_one(
        {"id": order_id},
        {
            "$set": {"status": "delivered", "updated_at": now},
            "$push": {
                "audit": {
                    "at": now,
                    "status": "delivered",
                    "by": em,
                    "role": "USER",
                    "note": "customer_confirmed_receipt",
                }
            },
        },
    )
    return get_order_by_id(order_id) or order_public(_mongo_order(doc) or {})
