# -*- coding: utf-8 -*-
"""POST /api/admin/policies — create policy (HR/ADMIN). Requires app Mongo (not disabled)."""
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


@unittest.skipIf(_app_mongo_disabled(), "M4_APP_MONGO_DISABLE — skip policy API tests")
class TestAdminPoliciesCreate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import server.app as srv

        cls._srv = srv
        srv._app.config["TESTING"] = True
        cls.client = srv._app.test_client()

    def _hr_token(self) -> str:
        from server.user_store import default_seed_password

        dom = (os.environ.get("AUTH_SEED_EMAIL_DOMAIN", "mailinator.com") or "mailinator.com").strip().lower()
        email = f"hr@{dom}"
        lr = self.client.post("/api/auth/login", json={"email": email, "password": default_seed_password()})
        if lr.status_code != 200:
            self.skipTest("seeded HR user not available")
        token = lr.get_json().get("access_token")
        self.assertTrue(token)
        return token

    def test_create_policy_201(self):
        token = self._hr_token()
        h = {"Authorization": f"Bearer {token}"}
        pid = f"pol_test_{uuid.uuid4().hex[:8]}"
        body = {
            "id": pid,
            "name": "test_policy",
            "resource": "order",
            "actions": ["read", "list"],
            "description": "created by test",
        }
        r = self.client.post("/api/admin/policies", json=body, headers=h)
        self.assertEqual(r.status_code, 201, r.get_json())
        data = r.get_json()
        self.assertEqual(data.get("policy", {}).get("id"), pid)

    def test_create_policy_duplicate_400(self):
        token = self._hr_token()
        h = {"Authorization": f"Bearer {token}"}
        pid = f"pol_dup_{uuid.uuid4().hex[:8]}"
        body = {
            "id": pid,
            "name": "dup_a",
            "resource": "x",
            "actions": ["read"],
        }
        r1 = self.client.post("/api/admin/policies", json=body, headers=h)
        self.assertEqual(r1.status_code, 201, r1.get_json())
        r2 = self.client.post("/api/admin/policies", json=body, headers=h)
        self.assertEqual(r2.status_code, 400, r2.get_json())
        self.assertIn("already exists", r2.get_json().get("error", ""))

    def test_policies_builder_options_200(self):
        token = self._hr_token()
        h = {"Authorization": f"Bearer {token}"}
        r = self.client.get("/api/admin/policies/builder-options", headers=h)
        self.assertEqual(r.status_code, 200, r.get_json())
        data = r.get_json()
        self.assertIn("platforms", data)
        self.assertIn("features", data)
        self.assertTrue(any(f.get("pageKey") for f in data.get("features") or []))

    def test_create_policy_with_page_keys(self):
        token = self._hr_token()
        h = {"Authorization": f"Bearer {token}"}
        pid = f"pol_test_pk_{uuid.uuid4().hex[:8]}"
        body = {
            "id": pid,
            "name": "Web › Cart › Test",
            "resource": "cart",
            "actions": ["read"],
            "pageKeys": ["web.cart.checkout"],
        }
        r = self.client.post("/api/admin/policies", json=body, headers=h)
        self.assertEqual(r.status_code, 201, r.get_json())
        pol = r.get_json().get("policy") or {}
        self.assertEqual(pol.get("pageKeys"), ["web.cart.checkout"])
