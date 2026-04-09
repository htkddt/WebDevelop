# -*- coding: utf-8 -*-
"""Admin desk ABAC spine metadata (pageKey ↔ policy) — no Mongo required."""
from __future__ import annotations

import os
import sys
import unittest

_PYTHON_AI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_SERVER_DIR = os.path.join(_PYTHON_AI, "server")
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)


class TestAccessModelHrSpine(unittest.TestCase):
    def test_hr_desks_link_to_seed_policies(self):
        from access_model import ADMIN_DESK_PAGES

        users = next(d for d in ADMIN_DESK_PAGES if d["pageKey"] == "admin.hr.users")
        self.assertEqual(users.get("abacArea"), "hr_abac")
        self.assertEqual(users.get("policyResource"), "user")
        self.assertEqual(users.get("relatedPolicyIds"), ["pol_user_manage"])

        pol = next(d for d in ADMIN_DESK_PAGES if d["pageKey"] == "admin.abac.policies")
        self.assertEqual(pol.get("abacArea"), "hr_abac")
        self.assertEqual(pol.get("relatedPolicyIds"), ["pol_policy_manage"])

    def test_seed_policies_include_page_keys(self):
        from policy_store import _default_policy_docs, policy_public

        docs = {d["id"]: d for d in _default_policy_docs()}
        self.assertEqual(docs["pol_user_manage"].get("pageKeys"), ["admin.hr.users"])
        self.assertEqual(docs["pol_policy_manage"].get("pageKeys"), ["admin.abac.policies"])
        self.assertEqual(
            docs["pol_cart_finance"].get("pageKeys"),
            ["admin.finance.charts", "admin.finance.orders"],
        )
        pub = policy_public(docs["pol_user_manage"])
        self.assertEqual(pub.get("pageKeys"), ["admin.hr.users"])
        self.assertIn("scopeLabel", pub)
        full = policy_public(docs["pol_system_full"])
        self.assertEqual(full.get("pageKeys"), [])
        self.assertIn("wildcard", full.get("scopeLabel", "").lower())

    def test_custom_policy_gets_scope_label_and_empty_page_keys(self):
        from policy_store import policy_public

        pub = policy_public(
            {"id": "pol_custom_x", "name": "x", "resource": "order", "actions": ["read", "list"], "conditions": None},
        )
        self.assertEqual(pub["pageKeys"], [])
        self.assertIn("Custom", pub["scopeLabel"])
        self.assertIn("order", pub["scopeLabel"])
