# -*- coding: utf-8 -*-
"""Store/cart API rules: guest UUID, USER vs staff cart ban. Requires app Mongo (not disabled)."""
from __future__ import annotations

import os
import sys
import unittest
import uuid

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


def _app_mongo_disabled() -> bool:
    return os.environ.get("M4_APP_MONGO_DISABLE", "").strip().lower() in ("1", "true", "yes")


@unittest.skipIf(_app_mongo_disabled(), "M4_APP_MONGO_DISABLE — skip store API tests")
class TestStoreCartAPI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import server.app as srv

        cls._srv = srv
        srv._app.config["TESTING"] = True
        cls.client = srv._app.test_client()

    def test_guest_cart_requires_uuid_header(self):
        r = self.client.get("/api/store/cart")
        self.assertEqual(r.status_code, 400, r.get_json())
        data = r.get_json()
        self.assertIn("error", data)

    def test_guest_cart_ok_with_uuid(self):
        gid = str(uuid.uuid4())
        r = self.client.get("/api/store/cart", headers={"X-Guest-Cart-Id": gid})
        self.assertEqual(r.status_code, 200, r.get_json())
        data = r.get_json()
        self.assertEqual(data.get("cart_key"), f"guest:{gid}")
        self.assertEqual(data.get("lines"), [])

    def test_staff_jwt_cart_forbidden(self):
        from server.user_store import default_seed_password, seed_admin_email

        email = seed_admin_email()
        pw = default_seed_password()
        lr = self.client.post("/api/auth/login", json={"email": email, "password": pw})
        self.assertEqual(lr.status_code, 200, lr.get_json())
        token = lr.get_json().get("access_token")
        self.assertTrue(token)
        r = self.client.get(
            "/api/store/cart",
            headers={"Authorization": f"Bearer {token}"},
        )
        self.assertEqual(r.status_code, 403, r.get_json())
        self.assertIn("Company", r.get_json().get("error", ""))

    def test_store_products_returns_list(self):
        r = self.client.get("/api/store/products")
        if r.status_code == 503:
            self.skipTest("app catalog Mongo unavailable")
        self.assertEqual(r.status_code, 200, r.get_json())
        data = r.get_json()
        self.assertIn("products", data)
        self.assertIsInstance(data["products"], list)

    def test_my_orders_requires_jwt(self):
        r = self.client.get("/api/store/orders")
        self.assertEqual(r.status_code, 401, r.get_json())

    def test_checkout_empty_cart_400(self):
        gid = str(uuid.uuid4())
        h = {"X-Guest-Cart-Id": gid}
        r = self.client.post("/api/store/checkout", json={}, headers=h)
        self.assertEqual(r.status_code, 400, r.get_json())
        self.assertIn("error", r.get_json())

    def test_checkout_guest_creates_order_and_clears_cart(self):
        pr = self.client.get("/api/store/products")
        if pr.status_code == 503:
            self.skipTest("app catalog Mongo unavailable")
        self.assertEqual(pr.status_code, 200, pr.get_json())
        products = pr.get_json().get("products") or []
        if not products:
            self.skipTest("no published products")
        pid = products[0]["id"]
        gid = str(uuid.uuid4())
        h = {"X-Guest-Cart-Id": gid}
        put = self.client.put("/api/store/cart", json={"lines": [{"product_id": pid, "qty": 1}]}, headers=h)
        self.assertEqual(put.status_code, 200, put.get_json())
        co = self.client.post("/api/store/checkout", json={"contact_email": "buyer@example.com"}, headers=h)
        self.assertEqual(co.status_code, 201, co.get_json())
        od = co.get_json().get("order") or {}
        self.assertEqual(od.get("status"), "pending_confirmation")
        self.assertEqual(od.get("customer_type"), "guest")
        cart = self.client.get("/api/store/cart", headers=h)
        self.assertEqual(cart.status_code, 200, cart.get_json())
        self.assertEqual(cart.get_json().get("lines"), [])

    def test_my_orders_user_jwt(self):
        from server.user_store import default_seed_password

        dom = (os.environ.get("AUTH_SEED_EMAIL_DOMAIN", "mailinator.com") or "mailinator.com").strip().lower()
        email = f"user@{dom}"
        lr = self.client.post("/api/auth/login", json={"email": email, "password": default_seed_password()})
        if lr.status_code != 200:
            self.skipTest("seeded customer not available")
        token = lr.get_json().get("access_token")
        self.assertTrue(token)
        r = self.client.get("/api/store/orders", headers={"Authorization": f"Bearer {token}"})
        self.assertEqual(r.status_code, 200, r.get_json())
        self.assertIn("orders", r.get_json())
