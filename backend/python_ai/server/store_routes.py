# -*- coding: utf-8 -*-
"""Storefront API: published catalog + cart (guest or customer ``USER`` only — staff cannot use cart)."""
from __future__ import annotations

import uuid

from flask import Blueprint, jsonify, request
from werkzeug.exceptions import BadRequest, Forbidden

from app_mongo import app_mongo_disabled
from auth_jwt import optional_bearer_jwt, require_jwt
from order_store import checkout_from_cart, customer_confirm_delivered, list_orders_for_customer
from product_cart_store import (
    cart_public,
    get_product_store_view,
    list_categories_published,
    list_store_products,
    save_cart,
)

bp = Blueprint("store_api", __name__, url_prefix="/api/store")

_STAFF_CANNOT_CART = "Company (staff) accounts cannot use the storefront cart."


def _require_app_db():
    if app_mongo_disabled():
        return jsonify({"error": "app catalog unavailable", "hint": "Unset M4_APP_MONGO_DISABLE and configure M4_APP_MONGO_URI"}), 503
    return None


def _store_cart_key() -> str:
    claims = optional_bearer_jwt()
    if claims:
        role = (claims.get("role") or "").upper()
        if role != "USER":
            raise Forbidden(description=_STAFF_CANNOT_CART)
        sub = (claims.get("sub") or "").strip().lower()
        if not sub:
            raise BadRequest(description="invalid token subject")
        return f"user:{sub}"
    raw = (request.headers.get("X-Guest-Cart-Id") or "").strip()
    if not raw:
        raise BadRequest(
            description="missing X-Guest-Cart-Id (send a stable UUID for anonymous carts)",
        )
    try:
        gid = str(uuid.UUID(raw))
    except ValueError as e:
        raise BadRequest(description="X-Guest-Cart-Id must be a UUID") from e
    return f"guest:{gid}"


@bp.route("/products", methods=["GET"])
def store_products_list():
    err = _require_app_db()
    if err:
        return err
    try:
        cat = (request.args.get("category") or "").strip() or None
        products = list_store_products(category=cat)
        categories = list_categories_published()
        return jsonify({"products": products, "categories": categories})
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/products/<product_id>", methods=["GET"])
def store_product_one(product_id: str):
    err = _require_app_db()
    if err:
        return err
    try:
        p = get_product_store_view(product_id)
        if not p:
            return jsonify({"error": "not found"}), 404
        return jsonify({"product": p})
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/cart", methods=["GET"])
def store_cart_get():
    err = _require_app_db()
    if err:
        return err
    try:
        key = _store_cart_key()
        return jsonify(cart_public(key))
    except Forbidden as f:
        return jsonify({"error": f.description}), 403
    except BadRequest as b:
        return jsonify({"error": b.description}), 400
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/cart", methods=["PUT"])
def store_cart_put():
    err = _require_app_db()
    if err:
        return err
    try:
        key = _store_cart_key()
        body = request.get_json(silent=True) or {}
        return jsonify(save_cart(key, body.get("lines")))
    except Forbidden as f:
        return jsonify({"error": f.description}), 403
    except BadRequest as b:
        return jsonify({"error": b.description}), 400
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/checkout", methods=["POST"])
def store_checkout():
    err = _require_app_db()
    if err:
        return err
    try:
        key = _store_cart_key()
    except Forbidden as f:
        return jsonify({"error": f.description}), 403
    except BadRequest as b:
        return jsonify({"error": b.description}), 400
    claims = optional_bearer_jwt()
    body = request.get_json(silent=True) or {}
    delivery_note = body.get("delivery_note")
    contact_email = body.get("contact_email")
    try:
        if claims and (claims.get("role") or "").upper() == "USER":
            sub = (claims.get("sub") or "").strip().lower()
            order = checkout_from_cart(
                key,
                customer_email=sub,
                customer_type="user",
                delivery_note=delivery_note,
                contact_email=contact_email,
            )
        else:
            order = checkout_from_cart(
                key,
                customer_email=None,
                customer_type="guest",
                delivery_note=delivery_note,
                contact_email=contact_email,
            )
        return jsonify({"order": order}), 201
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/orders", methods=["GET"])
def store_my_orders():
    err = _require_app_db()
    if err:
        return err
    try:
        claims = require_jwt()
    except Exception as e:
        return jsonify({"error": getattr(e, "description", None) or str(e)}), 401
    if (claims.get("role") or "").upper() != "USER":
        return jsonify({"error": "only customer (USER) accounts may list storefront orders"}), 403
    sub = (claims.get("sub") or "").strip().lower()
    if not sub:
        return jsonify({"error": "invalid token subject"}), 400
    try:
        return jsonify({"orders": list_orders_for_customer(sub)})
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@bp.route("/orders/<order_id>/confirm-delivery", methods=["POST"])
def store_confirm_order_delivered(order_id: str):
    """Registered customer confirms receipt (``shipped`` → ``delivered``)."""
    err = _require_app_db()
    if err:
        return err
    try:
        claims = require_jwt()
    except Exception as e:
        return jsonify({"error": getattr(e, "description", None) or str(e)}), 401
    if (claims.get("role") or "").upper() != "USER":
        return jsonify({"error": "only customer accounts may confirm delivery"}), 403
    sub = (claims.get("sub") or "").strip().lower()
    if not sub:
        return jsonify({"error": "invalid token subject"}), 400
    try:
        order = customer_confirm_delivered(order_id, customer_email=sub)
        return jsonify({"order": order})
    except ValueError as e:
        msg = str(e)
        code = 403 if "forbidden" in msg.lower() else 400
        return jsonify({"error": msg}), code
    except RuntimeError as e:
        return jsonify({"error": str(e)}), 503
    except Exception as e:
        return jsonify({"error": str(e)}), 500
