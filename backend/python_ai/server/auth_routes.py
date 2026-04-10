# -*- coding: utf-8 -*-
"""Flask blueprint: POST /api/auth/register, /login, GET /me — see docs/AUTH_JWT.md."""
from __future__ import annotations

import os

from flask import Blueprint, jsonify, request

from auth_jwt import JWT_EXPIRE_HOURS, create_access_token, require_jwt, require_roles
from policy_store import POL_BOT_USE, ensure_user_policy_if_missing, list_policies_for_user
from user_store import (
    default_seed_password,
    get_user_by_email,
    register_customer,
    user_public,
    verify_login,
)

bp = Blueprint("auth_api", __name__, url_prefix="/api/auth")


@bp.route("/register", methods=["POST"])
def register():
    data = request.get_json(silent=True) or {}
    email = (data.get("email") or "").strip()
    password = data.get("password") or ""
    name = (data.get("name") or "").strip() or None
    description = (data.get("description") or "").strip() or None
    try:
        u = register_customer(email, password, name=name, description=description)
    except ValueError as e:
        msg = str(e)
        hint = (
            "Seeded password for dev accounts is in docs/AUTH_JWT.md "
            f"(default `{default_seed_password()}`)."
        )
        if "already registered" in msg:
            return jsonify({"error": msg, "hint": hint}), 409
        return jsonify({"error": msg, "hint": hint}), 400
    ensure_user_policy_if_missing(u["id"], POL_BOT_USE, granted_by="register")
    return jsonify({"user": u, "message": "ok"}), 201


@bp.route("/login", methods=["POST"])
def login():
    data = request.get_json(silent=True) or {}
    email = (data.get("email") or "").strip()
    password = data.get("password") or ""
    rec = verify_login(email, password)
    if not rec:
        pw = default_seed_password()
        hint = (
            f"Use the current seed password (default `{pw}`) for pre-created users — "
            "see docs/AUTH_JWT.md. Restart the Flask process after changing seed env vars."
        )
        if os.environ.get("AUTH_NO_LOGIN_HINT", "").strip() in ("1", "true", "yes"):
            hint = ""
        body = {"error": "invalid email or password"}
        if hint:
            body["hint"] = hint
        return jsonify(body), 401
    token = create_access_token(
        sub=rec["email"],
        role=rec["role"],
        department=rec.get("department"),
        name=rec.get("name"),
        description=rec.get("description"),
        auth_rev=int(rec.get("auth_rev") or 0),
    )
    return jsonify(
        {
            "access_token": token,
            "token_type": "Bearer",
            "expires_in": JWT_EXPIRE_HOURS * 3600,
            "user": user_public(rec),
        }
    )


@bp.route("/me", methods=["GET"])
def me():
    claims = require_jwt()
    sub = (claims.get("sub") or "").strip()
    rec = get_user_by_email(sub)
    if not rec:
        return jsonify({"error": "user not found"}), 404
    policies = list_policies_for_user(rec["id"])
    return jsonify({"user": user_public(rec), "claims": claims, "policies": policies})


@bp.route("/admin/ping", methods=["GET"])
@require_roles("ADMIN", "HR", "SALE", "STORAGE", "FINANCE", "DELIVERY")
def admin_ping():
    """Smoke test: staff JWT only (not ``USER``)."""
    return jsonify({"ok": True, "message": "staff jwt accepted"})
