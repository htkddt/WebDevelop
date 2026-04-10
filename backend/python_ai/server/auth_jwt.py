# -*- coding: utf-8 -*-
"""HS256 JWT helpers for Flask — used by /api/auth/* (see docs/AUTH_JWT.md)."""
from __future__ import annotations

import os
from datetime import datetime, timedelta, timezone
from functools import wraps
from typing import Any, Callable, Dict, Optional, TypeVar

import jwt
from flask import request
from werkzeug.exceptions import Forbidden, Unauthorized

from user_store import get_user_by_email

JWT_SECRET = os.environ.get("JWT_SECRET", "").strip() or "dev-only-change-JWT_SECRET"
JWT_ALG = "HS256"
JWT_EXPIRE_HOURS = int(os.environ.get("JWT_EXPIRE_HOURS", "72"))


def create_access_token(
    *,
    sub: str,
    role: str,
    department: Optional[str] = None,
    name: Optional[str] = None,
    description: Optional[str] = None,
    auth_rev: int = 0,
) -> str:
    """``sub`` is stable user id (email lower). ``auth_rev`` must match ``users.auth_rev`` or token is rejected."""
    now = datetime.now(timezone.utc)
    payload: Dict[str, Any] = {
        "sub": sub,
        "role": role,
        "auth_rev": int(auth_rev),
        "iat": now,
        "exp": now + timedelta(hours=JWT_EXPIRE_HOURS),
    }
    if department:
        payload["department"] = department
    if name:
        payload["name"] = name
    if description:
        payload["description"] = description
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALG)


def decode_token(token: str) -> Dict[str, Any]:
    return jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALG])


def get_bearer_token() -> Optional[str]:
    h = request.headers.get("Authorization") or ""
    if h.startswith("Bearer "):
        return h[7:].strip() or None
    return None


def _claims_from_request() -> Dict[str, Any]:
    tok = get_bearer_token()
    if not tok:
        raise Unauthorized(description="missing Authorization: Bearer <token>")
    try:
        return decode_token(tok)
    except jwt.ExpiredSignatureError:
        raise Unauthorized(description="token expired") from None
    except jwt.InvalidTokenError:
        raise Unauthorized(description="invalid token") from None


def _token_auth_rev(decoded: Dict[str, Any]) -> int:
    v = decoded.get("auth_rev")
    if v is None:
        return 0
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


def sync_jwt_claims_with_user(decoded: Dict[str, Any]) -> Dict[str, Any]:
    """Reject stale tokens after role, department, or policy assignment changes; return DB-backed claims."""
    sub = (decoded.get("sub") or "").strip()
    if not sub:
        raise Unauthorized(description="invalid token subject")
    user = get_user_by_email(sub)
    if not user:
        raise Unauthorized(description="user not found")
    db_rev = int(user.get("auth_rev") or 0)
    if _token_auth_rev(decoded) != db_rev:
        raise Unauthorized(
            description="session outdated — sign in again (role, department, or policies changed)",
        )
    role = str(user.get("role") or "")
    uid_str = str(user.get("id") or "").strip()
    out: Dict[str, Any] = {
        "sub": sub,
        "role": role,
        "auth_rev": db_rev,
        "iat": decoded.get("iat"),
        "exp": decoded.get("exp"),
    }
    if uid_str:
        out["user_id"] = uid_str
    dept = user.get("department")
    if dept:
        out["department"] = dept
    nm = user.get("name")
    if nm:
        out["name"] = nm
    desc = user.get("description")
    if desc:
        out["description"] = desc
    return out


def require_jwt() -> Dict[str, Any]:
    """Decode Bearer token, verify ``auth_rev`` vs DB, return claims with **current** role/department."""
    raw = _claims_from_request()
    return sync_jwt_claims_with_user(raw)


def optional_bearer_jwt() -> Optional[Dict[str, Any]]:
    """Return claims if ``Authorization: Bearer`` present, valid, and ``auth_rev`` matches DB; ``None`` if header absent."""
    tok = get_bearer_token()
    if not tok:
        return None
    try:
        raw = decode_token(tok)
    except jwt.ExpiredSignatureError:
        raise Unauthorized(description="token expired") from None
    except jwt.InvalidTokenError:
        raise Unauthorized(description="invalid token") from None
    return sync_jwt_claims_with_user(raw)


F = TypeVar("F", bound=Callable[..., Any])


def require_roles(*allowed: str) -> Callable[[F], F]:
    """Decorator: ``@require_roles("ADMIN", "HR")`` — must be used after route registration (claims from Bearer)."""

    def deco(fn: F) -> F:
        @wraps(fn)
        def wrapped(*args: Any, **kwargs: Any):
            raw = _claims_from_request()
            c = sync_jwt_claims_with_user(raw)
            r = (c.get("role") or "").upper()
            allowed_u = {x.upper() for x in allowed}
            if r not in allowed_u:
                raise Forbidden(description="forbidden for this role")
            request.jwt_claims = c  # type: ignore[attr-defined]
            return fn(*args, **kwargs)

        return wrapped  # type: ignore[return-value]

    return deco


def require_staff() -> Dict[str, Any]:
    """Any logged-in user that is not a storefront customer (``USER``)."""
    raw = _claims_from_request()
    c = sync_jwt_claims_with_user(raw)
    r = (c.get("role") or "").upper()
    if r == "USER":
        raise Forbidden(description="staff only")
    return c
