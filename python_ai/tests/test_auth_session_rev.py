# -*- coding: utf-8 -*-
"""JWT auth_rev: tokens invalidate when role, department, or policies change."""
from __future__ import annotations

import os
import sys
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _PYTHON_AI not in sys.path:
    sys.path.insert(0, _PYTHON_AI)


def _app_mongo_disabled() -> bool:
    return os.environ.get("M4_APP_MONGO_DISABLE", "").strip().lower() in ("1", "true", "yes")


@unittest.skipIf(_app_mongo_disabled(), "M4_APP_MONGO_DISABLE — skip auth_rev tests")
class TestAuthSessionRev(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import server.app as srv

        cls._srv = srv
        srv._app.config["TESTING"] = True
        cls.client = srv._app.test_client()

    def _login(self, email: str, password: str) -> str:
        lr = self.client.post("/api/auth/login", json={"email": email, "password": password})
        self.assertEqual(lr.status_code, 200, lr.get_json())
        t = lr.get_json().get("access_token")
        self.assertTrue(t)
        return t

    def test_old_token_rejected_after_role_change(self):
        from server.user_store import default_seed_password

        dom = (os.environ.get("AUTH_SEED_EMAIL_DOMAIN", "mailinator.com") or "mailinator.com").strip().lower()
        sale_email = f"sale@{dom}"
        pw = default_seed_password()
        admin_email = os.environ.get("AUTH_SEED_ADMIN_EMAIL", "").strip() or f"admin@{dom}"
        admin_email = admin_email.lower()
        token_ad = self._login(admin_email, pw)
        h_ad = {"Authorization": f"Bearer {token_ad}"}

        ur = self.client.get("/api/admin/users", headers=h_ad)
        self.assertEqual(ur.status_code, 200, ur.get_json())
        users = ur.get_json().get("users") or []
        sale_u = next((u for u in users if (u.get("email") or "").lower() == sale_email), None)
        self.assertIsNotNone(sale_u, "seeded sale user missing")
        uid = sale_u["id"]

        # Baseline role so a later "FINANCE" patch always changes role (bumps auth_rev).
        base = self.client.patch(
            f"/api/admin/users/{uid}",
            json={"role": "SALE"},
            headers=h_ad,
        )
        self.assertEqual(base.status_code, 200, base.get_json())
        token_sale = self._login(sale_email, pw)
        h_sale = {"Authorization": f"Bearer {token_sale}"}
        me1 = self.client.get("/api/auth/me", headers=h_sale)
        self.assertEqual(me1.status_code, 200, me1.get_json())

        patch = self.client.patch(
            f"/api/admin/users/{uid}",
            json={"role": "FINANCE"},
            headers=h_ad,
        )
        self.assertEqual(patch.status_code, 200, patch.get_json())

        me2 = self.client.get("/api/auth/me", headers=h_sale)
        self.assertEqual(me2.status_code, 401, me2.get_json())
        err = str((me2.get_json() or {}).get("message") or "")
        self.assertIn("session outdated", err.lower())

        token_sale2 = self._login(sale_email, pw)
        me3 = self.client.get("/api/auth/me", headers={"Authorization": f"Bearer {token_sale2}"})
        self.assertEqual(me3.status_code, 200, me3.get_json())
        self.assertEqual((me3.get_json().get("user") or {}).get("role"), "FINANCE")

        # Restore role for other tests / dev DB
        self.client.patch(
            f"/api/admin/users/{uid}",
            json={"role": "SALE"},
            headers=h_ad,
        )
