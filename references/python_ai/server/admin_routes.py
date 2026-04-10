# -*- coding: utf-8 -*-
"""Admin API: policies, users, assignments, products — ABAC. See docs/ABAC_PRODUCT_TEST_PLAN.md."""
from __future__ import annotations

import csv
import io
import os
from datetime import datetime, timezone
from typing import Optional

from flask import Blueprint, Response, jsonify, request

from app_mongo import app_mongo_disabled
from auth_jwt import require_roles
from org_store import (
    create_department,
    create_product_category,
    list_departments,
    list_product_categories,
    update_department,
    update_product_category,
)
from policy_builder import policy_builder_options
from policy_store import (
    POL_FINANCE_CROSS_HR_PENDING,
    create_policy,
    ensure_user_policy_if_missing,
    list_policies,
    list_policies_for_user,
    set_user_policies,
    user_has_finance_cross_pending_access,
)
from order_store import (
    can_access_finance_analytics,
    can_override_delivery_assignee,
    can_set_delivery_assignee,
    finance_export_rows,
    finance_summary,
    is_delivery_staff_user,
    list_orders_for_staff,
    set_order_delivery_assignee,
    transition_order,
)
from product_cart_store import (
    admin_bulk_upsert,
    admin_categories,
    admin_create_product,
    admin_get_product,
    admin_list_products,
    admin_update_product,
)
from user_store import (
    admin_create_user,
    admin_update_user,
    cross_hr1_seed_email,
    get_user_by_email,
    get_user_by_id,
    list_users_public,
    user_public,
)
from bot_c_lib_settings_store import get_bot_c_lib_overrides, save_bot_c_lib_settings
from training.full_options import (
    BOT_C_LIB_MODE_LABELS,
    BOT_C_LIB_OPTION_SCHEMA,
    OPTION_KEYS,
    get_full_options,
    get_max_options,
    merge_options_overrides,
)

bp = Blueprint("admin_api", __name__, url_prefix="/api/admin")


def _claims_user_id() -> Optional[str]:
    c = getattr(request, "jwt_claims", {}) or {}
    uid = (c.get("user_id") or "").strip()
    if uid:
        return uid
    sub = (c.get("sub") or "").strip()
    if not sub:
        return None
    rec = get_user_by_email(sub)
    return str(rec["id"]).strip() if rec else None


def _actor_role() -> str:
    return str((getattr(request, "jwt_claims", {}) or {}).get("role") or "").upper()


def _catalog_unavailable():
    if app_mongo_disabled():
        return jsonify(
            {
                "error": "app catalog unavailable",
                "hint": "Unset M4_APP_MONGO_DISABLE and configure M4_APP_MONGO_URI",
            }
        ), 503
    return None


@bp.route("/policies", methods=["GET"])
@require_roles("ADMIN", "HR")
def policies_list():
    return jsonify({"policies": list_policies()})


@bp.route("/policies/builder-options", methods=["GET"])
@require_roles("ADMIN", "HR")
def policies_builder_options():
    """Platform → module → feature lists for the policy builder UI (from access catalog)."""
    return jsonify(policy_builder_options())


@bp.route("/policies", methods=["POST"])
@require_roles("ADMIN", "HR")
def policies_create():
    data = request.get_json(silent=True) or {}
    pid = (data.get("id") or data.get("policy_id") or "").strip()
    name = (data.get("name") or "").strip()
    resource = (data.get("resource") or "").strip()
    raw_actions = data.get("actions")
    if not isinstance(raw_actions, list):
        raw_actions = []
    description = data.get("description")
    if description is not None:
        description = str(description).strip() or None
    conditions = data.get("conditions")
    if conditions is not None and not isinstance(conditions, (dict, list)):
        return jsonify({"error": "conditions must be a JSON object, array, or omitted"}), 400
    raw_page_keys = data.get("pageKeys") or data.get("page_keys")
    page_keys = None
    if raw_page_keys is not None:
        if not isinstance(raw_page_keys, list):
            return jsonify({"error": "pageKeys must be a string array or omitted"}), 400
        page_keys = [str(x).strip() for x in raw_page_keys if str(x).strip()]
    try:
        p = create_policy(
            policy_id=pid,
            name=name,
            resource=resource,
            actions=[str(x) for x in raw_actions],
            description=description,
            conditions=conditions,
            page_keys=page_keys,
        )
        return jsonify({"policy": p}), 201
    except ValueError as e:
        return jsonify({"error": str(e)}), 400


@bp.route("/users", methods=["GET"])
@require_roles("ADMIN", "HR")
def users_list():
    return jsonify({"users": list_users_public()})


@bp.route("/users", methods=["POST"])
@require_roles("ADMIN", "HR")
def users_create():
    data = request.get_json(silent=True) or {}
    email = (data.get("email") or "").strip()
    password = data.get("password") or ""
    name = (data.get("name") or "").strip() or None
    role = (data.get("role") or "USER").strip()
    department = (data.get("department") or "").strip() or None
    description = (data.get("description") or "").strip() or None
    try:
        u = admin_create_user(
            email=email,
            password=password,
            name=name,
            role=role,
            department=department,
            description=description,
            actor_role=_actor_role(),
        )
        return jsonify({"user": u}), 201
    except ValueError as e:
        return jsonify({"error": str(e)}), 400


@bp.route("/users/<user_id>", methods=["PATCH"])
@require_roles("ADMIN", "HR")
def users_patch(user_id: str):
    data = request.get_json(silent=True) or {}
    updates = {k: data[k] for k in ("name", "department", "role", "description") if k in data}
    try:
        u = admin_update_user(user_id, actor_role=_actor_role(), updates=updates)
        return jsonify({"user": u})
    except ValueError as e:
        msg = str(e)
        code = 404 if "not found" in msg.lower() else 400
        return jsonify({"error": msg}), code


@bp.route("/departments", methods=["GET"])
@require_roles("ADMIN", "HR")
def departments_list():
    return jsonify({"departments": list_departments()})


@bp.route("/departments", methods=["POST"])
@require_roles("ADMIN", "HR")
def departments_create():
    data = request.get_json(silent=True) or {}
    code = (data.get("code") or "").strip()
    name = (data.get("name") or "").strip()
    description = (data.get("description") or "").strip() or None
    try:
        d = create_department(code, name, description)
        return jsonify({"department": d}), 201
    except ValueError as e:
        return jsonify({"error": str(e)}), 400


@bp.route("/departments/<code>", methods=["PATCH"])
@require_roles("ADMIN", "HR")
def departments_patch(code: str):
    data = request.get_json(silent=True) or {}
    try:
        d = update_department(
            code,
            name=data.get("name"),
            description=data.get("description"),
        )
        if not d:
            return jsonify({"error": "not found"}), 404
        return jsonify({"department": d})
    except ValueError as e:
        return jsonify({"error": str(e)}), 400


@bp.route("/product-categories", methods=["GET"])
@require_roles("ADMIN", "STORAGE", "SALE", "FINANCE", "HR")
def product_categories_list():
    return jsonify({"categories": list_product_categories()})


@bp.route("/product-categories", methods=["POST"])
@require_roles("ADMIN", "STORAGE")
def product_categories_create():
    data = request.get_json(silent=True) or {}
    code = (data.get("code") or "").strip()
    name = (data.get("name") or "").strip()
    active = data.get("active")
    if active is not None and not isinstance(active, bool):
        return jsonify({"error": "active must be boolean"}), 400
    try:
        c = create_product_category(code, name, active=bool(active) if active is not None else True)
        return jsonify({"category": c}), 201
    except ValueError as e:
        return jsonify({"error": str(e)}), 400


@bp.route("/product-categories/<code>", methods=["PATCH"])
@require_roles("ADMIN", "STORAGE")
def product_categories_patch(code: str):
    data = request.get_json(silent=True) or {}
    active = data.get("active")
    if active is not None and not isinstance(active, bool):
        return jsonify({"error": "active must be boolean"}), 400
    try:
        c = update_product_category(code, name=data.get("name"), active=active)
        if not c:
            return jsonify({"error": "not found"}), 404
        return jsonify({"category": c})
    except ValueError as e:
        return jsonify({"error": str(e)}), 400


@bp.route("/users/<user_id>/policies", methods=["GET"])
@require_roles("ADMIN", "HR")
def user_policies_get(user_id: str):
    u = get_user_by_id(user_id)
    if not u:
        return jsonify({"error": "user not found"}), 404
    return jsonify(
        {
            "user": user_public(u),
            "policies": list_policies_for_user(user_id),
        }
    )


@bp.route("/products", methods=["GET"])
@require_roles("ADMIN", "STORAGE", "SALE", "FINANCE")
def admin_products_list():
    err = _catalog_unavailable()
    if err:
        return err
    try:
        cat = (request.args.get("category") or "").strip() or None
        pub_raw = (request.args.get("published") or "").strip().lower()
        pub = pub_raw if pub_raw in ("true", "false") else None
        products = admin_list_products(category=cat, published=pub)
        categories = admin_categories()
        return jsonify({"products": products, "categories": categories})
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/products", methods=["POST"])
@require_roles("ADMIN", "STORAGE")
def admin_products_create():
    err = _catalog_unavailable()
    if err:
        return err
    data = request.get_json(silent=True) or {}
    try:
        p = admin_create_product(data)
        return jsonify({"product": p}), 201
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/products/bulk", methods=["POST"])
@require_roles("ADMIN", "STORAGE")
def admin_products_bulk():
    err = _catalog_unavailable()
    if err:
        return err
    data = request.get_json(silent=True) or {}
    rows = data.get("products")
    if not isinstance(rows, list):
        return jsonify({"error": "body must include products: array"}), 400
    try:
        out = admin_bulk_upsert(rows)
        return jsonify(out)
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/products/<product_id>", methods=["GET"])
@require_roles("ADMIN", "STORAGE", "SALE", "FINANCE")
def admin_products_get(product_id: str):
    err = _catalog_unavailable()
    if err:
        return err
    try:
        p = admin_get_product(product_id)
        if not p:
            return jsonify({"error": "not found"}), 404
        return jsonify({"product": p})
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/products/<product_id>", methods=["PATCH"])
@require_roles("ADMIN", "STORAGE")
def admin_products_patch(product_id: str):
    err = _catalog_unavailable()
    if err:
        return err
    data = request.get_json(silent=True) or {}
    try:
        p = admin_update_product(product_id, data)
        if not p:
            return jsonify({"error": "not found"}), 404
        return jsonify({"product": p})
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/users/<user_id>/policies", methods=["PUT"])
@require_roles("ADMIN", "HR")
def user_policies_put(user_id: str):
    u = get_user_by_id(user_id)
    if not u:
        return jsonify({"error": "user not found"}), 404
    target_role = str(u.get("role") or "").upper()
    if _actor_role() == "HR" and target_role == "ADMIN":
        return jsonify({"error": "HR cannot change policies for ADMIN accounts"}), 403
    data = request.get_json(silent=True) or {}
    raw = data.get("policy_ids")
    if not isinstance(raw, list):
        return jsonify({"error": "body must include policy_ids: string[]"}), 400
    policy_ids = [str(x) for x in raw]
    claims = getattr(request, "jwt_claims", {}) or {}
    granted_by = (claims.get("sub") or "").strip() or "unknown"
    try:
        set_user_policies(user_id, policy_ids, granted_by=granted_by)
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    return jsonify(
        {
            "ok": True,
            "user": user_public(u),
            "policies": list_policies_for_user(user_id),
        }
    )


def _finance_claims():
    c = getattr(request, "jwt_claims", {}) or {}
    return str(c.get("role") or ""), c.get("department")


def _finance_cross_pending_scope() -> tuple[bool, str | None]:
    """(cross_pending_only, error_message). ``cross_pending`` forces pending_confirmation-only data."""
    claims = getattr(request, "jwt_claims", {}) or {}
    role = str(claims.get("role") or "").upper()
    dept = claims.get("department")
    sub = (claims.get("sub") or "").strip()
    uid = (claims.get("user_id") or "").strip()
    rec = get_user_by_email(sub) if sub else None
    if not uid and rec:
        uid = str(rec.get("id") or "").strip()
    # Idempotent: DB restores / older seeds may lack user_policies row for the demo account.
    if role == "HR" and uid and rec:
        em = (rec.get("email") or "").strip().lower()
        if em == cross_hr1_seed_email():
            ensure_user_policy_if_missing(uid, POL_FINANCE_CROSS_HR_PENDING)
    if can_access_finance_analytics(role, dept):
        return False, None
    if role == "HR" and uid and user_has_finance_cross_pending_access(str(uid)):
        return True, None
    return False, "forbidden"


@bp.route("/finance/summary", methods=["GET"])
@require_roles("ADMIN", "FINANCE", "HR")
def admin_finance_summary():
    err = _catalog_unavailable()
    if err:
        return err
    role, dept = _finance_claims()
    cross, ferr = _finance_cross_pending_scope()
    if ferr:
        return jsonify(
            {
                "error": ferr,
                "message": "Not allowed for this HR account (assign pol_finance_cross_hr_pending or use a finance role/department).",
            }
        ), 403
    try:
        out = finance_summary(
            role,
            dept,
            from_day=(request.args.get("from") or "").strip() or None,
            to_day=(request.args.get("to") or "").strip() or None,
            status=(request.args.get("status") or "").strip() or None,
            customer_type=(request.args.get("customer_type") or "").strip() or None,
            cross_pending_only=cross,
        )
        return jsonify(out)
    except ValueError as e:
        msg = str(e)
        code = 403 if "forbidden" in msg.lower() else 400
        return jsonify({"error": msg}), code
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/finance/export", methods=["GET"])
@require_roles("ADMIN", "FINANCE", "HR")
def admin_finance_export():
    err = _catalog_unavailable()
    if err:
        return err
    role, dept = _finance_claims()
    cross, ferr = _finance_cross_pending_scope()
    if ferr:
        return jsonify(
            {
                "error": ferr,
                "message": "Not allowed for this HR account (assign pol_finance_cross_hr_pending or use a finance role/department).",
            }
        ), 403
    try:
        rows = finance_export_rows(
            role,
            dept,
            from_day=(request.args.get("from") or "").strip() or None,
            to_day=(request.args.get("to") or "").strip() or None,
            status=(request.args.get("status") or "").strip() or None,
            customer_type=(request.args.get("customer_type") or "").strip() or None,
            cross_pending_only=cross,
        )
    except ValueError as e:
        msg = str(e)
        code = 403 if "forbidden" in msg.lower() else 400
        return jsonify({"error": msg}), code
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500

    buf = io.StringIO()
    fieldnames = [
        "id",
        "status",
        "customer_type",
        "customer_email",
        "contact_email",
        "subtotal",
        "line_count",
        "created_at",
        "updated_at",
        "delivery_assignee_user_id",
        "delivery_assignee_name",
    ]
    w = csv.DictWriter(buf, fieldnames=fieldnames, extrasaction="ignore")
    w.writeheader()
    w.writerows(rows)
    body = "\ufeff" + buf.getvalue()
    fn = f"orders_export_{datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')}.csv"
    return Response(
        body.encode("utf-8"),
        mimetype="text/csv; charset=utf-8",
        headers={"Content-Disposition": f'attachment; filename="{fn}"'},
    )


@bp.route("/delivery-assignee-candidates", methods=["GET"])
@require_roles("ADMIN", "FINANCE", "DELIVERY", "STORAGE", "HR", "SALE")
def delivery_assignee_candidates():
    """Users who may be assigned as delivery on orders — ADMIN/FINANCE override UI + delivery self-assign (same ids as PATCH)."""
    claims = getattr(request, "jwt_claims", {}) or {}
    role = str(claims.get("role") or "")
    dept = claims.get("department")
    if not (can_override_delivery_assignee(role, dept) or is_delivery_staff_user(role, dept)):
        return jsonify({"error": "forbidden"}), 403
    out = []
    for u in list_users_public():
        r = str(u.get("role") or "").upper()
        d = u.get("department")
        if is_delivery_staff_user(r, d):
            out.append(
                {
                    "id": u["id"],
                    "email": u["email"],
                    "name": u.get("name"),
                    "role": r,
                }
            )
    return jsonify({"users": out})


@bp.route("/orders", methods=["GET"])
@require_roles("ADMIN", "FINANCE", "STORAGE", "DELIVERY", "HR")
def admin_orders_list():
    err = _catalog_unavailable()
    if err:
        return err
    claims = getattr(request, "jwt_claims", {}) or {}
    role = str(claims.get("role") or "")
    dept = claims.get("department")
    st = (request.args.get("status") or "").strip() or None
    cross, ferr = _finance_cross_pending_scope()
    try:
        if role.upper() == "HR":
            if ferr:
                return jsonify(
                    {
                        "error": ferr,
                        "message": "HR accounts need the cross-finance policy to list orders here, or a finance department/role for full access.",
                    }
                ), 403
            orders = list_orders_for_staff(role, dept, status=st, cross_finance_hr_pending=cross)
        else:
            vuid = _claims_user_id()
            viewer = vuid if is_delivery_staff_user(str(role or ""), dept) else None
            orders = list_orders_for_staff(role, dept, status=st, viewer_user_id=viewer)
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500
    return jsonify({"orders": orders})


@bp.route("/orders/<order_id>", methods=["PATCH"])
@require_roles("ADMIN", "FINANCE", "STORAGE", "DELIVERY", "HR")
def admin_orders_patch(order_id: str):
    err = _catalog_unavailable()
    if err:
        return err
    cross, _ = _finance_cross_pending_scope()
    if cross:
        return jsonify(
            {
                "error": "forbidden",
                "message": "This account may view finance orders only (pending_confirmation); status changes are not allowed.",
            }
        ), 403
    # ``force=True``: parse JSON even if proxy strips or changes Content-Type (assignee-only PATCH must work).
    raw = request.get_json(force=True, silent=True)
    data = raw if isinstance(raw, dict) else {}
    new_status = data.get("status")
    has_status = bool(new_status and isinstance(new_status, str))
    assign_in_body = "delivery_assignee_user_id" in data
    if assign_in_body:
        raw_a = data.get("delivery_assignee_user_id")
        if raw_a is not None and not isinstance(raw_a, str):
            return jsonify({"error": "delivery_assignee_user_id must be a string or null"}), 400
        new_assignee = None if raw_a is None else (str(raw_a).strip() or None)
    else:
        new_assignee = None

    if not has_status and not assign_in_body:
        return jsonify({"error": "body must include status and/or delivery_assignee_user_id"}), 400

    claims = getattr(request, "jwt_claims", {}) or {}
    role = str(claims.get("role") or "")
    dept = claims.get("department")
    actor_uid = _claims_user_id() or ""

    try:
        order = None
        if assign_in_body:
            if not can_set_delivery_assignee(role, dept):
                return jsonify({"error": "forbidden"}), 403
            order = set_order_delivery_assignee(
                order_id,
                new_assignee,
                actor_role=role,
                actor_department=dept,
                actor_user_id=actor_uid,
                actor_email=str(claims.get("sub") or ""),
            )
        if has_status:
            order = transition_order(
                order_id,
                new_status.strip(),
                actor_role=role,
                actor_department=dept,
                actor_email=str(claims.get("sub") or ""),
                actor_user_id=actor_uid or None,
            )
        return jsonify({"order": order})
    except ValueError as e:
        msg = str(e)
        code = 403 if "forbidden" in msg.lower() else 400
        return jsonify({"error": msg}), code
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


def _server_uses_max_options() -> bool:
    return os.environ.get("M4ENGINE_SERVER_MAX", "1").strip().lower() not in ("0", "false", "no")


@bp.route("/bot-c-lib/schema", methods=["GET"])
@require_roles("ADMIN")
def bot_c_lib_schema():
    """All supported option keys for c-lib ``api_options_t`` (subset wired in ``training/full_options``)."""
    return jsonify(
        {
            "schema": BOT_C_LIB_OPTION_SCHEMA,
            "mode_labels": {
                str(M4ENGINE_MODE_ONLY_MEMORY): "MEMORY",
                str(M4ENGINE_MODE_ONLY_MONGO): "MONGO_ONLY",
                str(M4ENGINE_MODE_MONGO_REDIS): "MONGO_REDIS",
                str(M4ENGINE_MODE_MONGO_REDIS_ELK): "MONGO_REDIS_ELK",
            },
            "use_max_options": _server_uses_max_options(),
        }
    )


@bp.route("/bot-c-lib/settings", methods=["GET"])
@require_roles("ADMIN")
def bot_c_lib_settings_get():
    use_max = _server_uses_max_options()
    base = get_max_options() if use_max else get_full_options()
    saved = get_bot_c_lib_overrides()
    effective = merge_options_overrides(base, saved)
    return jsonify(
        {
            "use_max_options": use_max,
            "schema": BOT_C_LIB_OPTION_SCHEMA,
            "base_from_env": base,
            "saved_overrides": saved,
            "effective_preview": effective,
        }
    )


@bp.route("/bot-c-lib/settings", methods=["PUT"])
@require_roles("ADMIN")
def bot_c_lib_settings_put():
    data = request.get_json(force=True, silent=True)
    if not isinstance(data, dict):
        return jsonify({"error": "JSON object required"}), 400
    values = data.get("values")
    if not isinstance(values, dict):
        return jsonify({"error": "body.values must be an object"}), 400
    replace = bool(data.get("replace"))
    claims = getattr(request, "jwt_claims", {}) or {}
    sub = (claims.get("sub") or "").strip() or "unknown"
    try:
        out = save_bot_c_lib_settings(values, replace=replace, updated_by=sub)
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    return jsonify(
        {
            "values": out,
            "message": "Saved to DB. Restart the server so api_create picks up merged settings (env base + saved overrides).",
        }
    )
